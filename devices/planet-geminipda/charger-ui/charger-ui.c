/*
 * charger-ui — draws the Gemini PDA off-mode-charging screen on /dev/fb0.
 *
 * Called by charger.sh (initrd) each time something visible changes; it is
 * stateless: every run composes the whole landscape frame in RAM (background
 * PNG + battery circle + text), then copies only the rectangles that can
 * change (or the whole screen with --full) to the framebuffer.
 *
 * Why this shape (project doc claude/charger-mode.md):
 *  - The framebuffer is LK's free-running scanout: no vblank, no second
 *    buffer, so every write can tear. Composing off-screen and copying one
 *    small rectangle in a single pass keeps any tear to a brief seam inside
 *    that rectangle (no visible erase/redraw).
 *  - The panel is portrait (1080x2160); the UI is landscape (2160x1080),
 *    rotated on the copy the same way fbcon is (/sys/class/graphics/fbcon/rotate).
 *  - --full also switches tty0 to KD_GRAPHICS so fbcon stops drawing over
 *    us; --text-mode switches it back before the normal boot continues.
 *
 * Usage:
 *   charger-ui --full --pct 80 --line "Charging at 1.5 A"   first frame
 *   charger-ui --pct 81 --line "Charging at 1.4 A"          later frames
 *   charger-ui --text-mode                                  give tty0 back
 *   charger-ui --out frame.ppm --pct 80 --line ...          host preview
 *   options: --bg FILE  --font FILE  --fb DEV  --rot 0|1|2|3  --pct -1 (unknown)
 *
 * Layout constants are in `struct layout` below (measured from the design
 * mockup at 2160x1080). Uses stb_image (PNG) and stb_truetype (public domain).
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define STBI_ONLY_PNG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

/* ---- layout (logical landscape pixels) ---------------------------------- */
static const struct layout {
	int w, h;                 /* logical screen */
	double cx, cy;            /* circle centre */
	double r_out, ring;       /* outer radius, ring width */
	uint32_t ring_rgb, fill_rgb, fill_low_rgb, text_rgb;
	int low_pct;              /* fill_low_rgb below this */
	double pct_px;            /* % text font size (em, px) */
	double line_px;           /* bottom line font size (em, px) */
	double line_cx, line_base;/* bottom line centre x, baseline y */
	int line_halfw;           /* half width of the area the line may use */
	int draw_ring;            /* 0 if the background already has the outline */
} L = {
	.w = 2160, .h = 1080,
	.cx = 545.5, .cy = 566.5, .r_out = 263.5, .ring = 3.0,
	.ring_rgb = 0xffffff, .fill_rgb = 0x343434, .fill_low_rgb = 0x5a2222,
	.text_rgb = 0xffffff, .low_pct = 15,
	.pct_px = 130.0,
	.line_px = 33.0, .line_cx = 545.5, .line_base = 908.0, .line_halfw = 420,
	.draw_ring = 1,
};

/* ---- frame buffer in RAM: 0x00RRGGBB per pixel -------------------------- */
static uint32_t *frame;

static inline uint32_t mix(uint32_t a, uint32_t b, double t)
{
	if (t <= 0) return a;
	if (t >= 1) return b;
	int ar = a >> 16 & 255, ag = a >> 8 & 255, ab = a & 255;
	int br = b >> 16 & 255, bg = b >> 8 & 255, bb = b & 255;
	return (uint32_t)(ar + (br - ar) * t + .5) << 16 |
	       (uint32_t)(ag + (bg - ag) * t + .5) << 8 |
	       (uint32_t)(ab + (bb - ab) * t + .5);
}

static inline double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

struct rect { int x0, y0, x1, y1; };   /* half-open */

static struct rect clip(struct rect r)
{
	if (r.x0 < 0) r.x0 = 0;
	if (r.y0 < 0) r.y0 = 0;
	if (r.x1 > L.w) r.x1 = L.w;
	if (r.y1 > L.h) r.y1 = L.h;
	return r;
}

static int load_background(const char *path)
{
	int w, h, n;
	unsigned char *px = stbi_load(path, &w, &h, &n, 3);

	for (int i = 0; i < L.w * L.h; i++) frame[i] = 0;
	if (!px) {
		fprintf(stderr, "charger-ui: %s: %s (black background)\n", path, stbi_failure_reason());
		return -1;
	}
	/* centre/crop anything that isn't exactly the screen size */
	int ox = (L.w - w) / 2, oy = (L.h - h) / 2;
	for (int y = 0; y < h; y++) {
		int fy = y + oy;
		if (fy < 0 || fy >= L.h) continue;
		for (int x = 0; x < w; x++) {
			int fx = x + ox;
			if (fx < 0 || fx >= L.w) continue;
			unsigned char *p = px + 3 * (y * w + x);
			frame[fy * L.w + fx] = (uint32_t)p[0] << 16 | p[1] << 8 | p[2];
		}
	}
	stbi_image_free(px);
	return 0;
}

/* Battery circle: disc filled from the bottom to pct, anti-aliased edges. */
static struct rect draw_circle(int pct)
{
	double r_in = L.r_out - (L.draw_ring ? L.ring : 0);
	double level = (pct < 0 ? 0 : pct > 100 ? 100 : pct) / 100.0;
	double level_y = L.cy + r_in - 2 * r_in * level;   /* fill above this y */
	uint32_t fill = (pct >= 0 && pct < L.low_pct) ? L.fill_low_rgb : L.fill_rgb;
	struct rect r = clip((struct rect){ (int)(L.cx - L.r_out) - 2, (int)(L.cy - L.r_out) - 2,
					    (int)(L.cx + L.r_out) + 3, (int)(L.cy + L.r_out) + 3 });

	for (int y = r.y0; y < r.y1; y++) {
		/* fraction of this pixel row below the fill line (AA'd level edge) */
		double fill_cov = clamp01(y + 1 - level_y);
		for (int x = r.x0; x < r.x1; x++) {
			double d = hypot(x + .5 - L.cx, y + .5 - L.cy);
			double in_cov = clamp01(r_in - d + .5);
			double out_cov = clamp01(L.r_out - d + .5);
			uint32_t *p = &frame[y * L.w + x];
			uint32_t inside = mix(0x000000, fill, fill_cov);
			uint32_t c = mix(*p, inside, in_cov);
			if (L.draw_ring)
				c = mix(c, L.ring_rgb, out_cov - in_cov);
			*p = c;
		}
	}
	return r;
}

/* ---- text ---------------------------------------------------------------- */
static stbtt_fontinfo font;
static unsigned char *font_data;

static int load_font(const char *path)
{
	FILE *f = fopen(path, "rb");
	long n;

	if (!f) { fprintf(stderr, "charger-ui: %s: %s\n", path, strerror(errno)); return -1; }
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	font_data = malloc(n);
	if (!font_data || fread(font_data, 1, n, f) != (size_t)n) { fclose(f); return -1; }
	fclose(f);
	if (!stbtt_InitFont(&font, font_data, stbtt_GetFontOffsetForIndex(font_data, 0))) {
		fprintf(stderr, "charger-ui: %s: not a usable font\n", path);
		return -1;
	}
	return 0;
}

static double text_width(const char *s, double scale)
{
	double w = 0;
	for (; *s; s++) {
		int adv, lsb;
		stbtt_GetCodepointHMetrics(&font, (unsigned char)*s, &adv, &lsb);
		w += adv * scale;
		if (s[1]) w += scale * stbtt_GetCodepointKernAdvance(&font, (unsigned char)s[0], (unsigned char)s[1]);
	}
	return w;
}

/* draw s centred on cx with its baseline at base; blends text_rgb over frame */
static void draw_text(const char *s, double px, double cx, double base, struct rect lim)
{
	double scale = stbtt_ScaleForMappingEmToPixels(&font, px);
	double x = cx - text_width(s, scale) / 2;

	lim = clip(lim);
	for (; *s; s++) {
		int cp = (unsigned char)*s, adv, lsb, x0, y0, x1, y1, bw, bh;
		double fx = x - floor(x);
		stbtt_GetCodepointHMetrics(&font, cp, &adv, &lsb);
		stbtt_GetCodepointBitmapBoxSubpixel(&font, cp, scale, scale, fx, 0, &x0, &y0, &x1, &y1);
		bw = x1 - x0; bh = y1 - y0;
		if (bw > 0 && bh > 0) {
			unsigned char *bm = calloc(bw * bh, 1);
			stbtt_MakeCodepointBitmapSubpixel(&font, bm, bw, bh, bw, scale, scale, fx, 0, cp);
			int ox = (int)floor(x) + x0, oy = (int)floor(base + .5) + y0;
			for (int j = 0; j < bh; j++) {
				int fy = oy + j;
				if (fy < lim.y0 || fy >= lim.y1) continue;
				for (int i = 0; i < bw; i++) {
					int fxp = ox + i;
					if (fxp < lim.x0 || fxp >= lim.x1 || !bm[j * bw + i]) continue;
					uint32_t *p = &frame[fy * L.w + fxp];
					*p = mix(*p, L.text_rgb, bm[j * bw + i] / 255.0);
				}
			}
			free(bm);
		}
		x += adv * scale;
		if (s[1]) x += scale * stbtt_GetCodepointKernAdvance(&font, cp, (unsigned char)s[1]);
	}
}

static double digit_height(double px)
{
	int x0, y0, x1, y1;
	stbtt_GetCodepointBox(&font, '0', &x0, &y0, &x1, &y1);
	return (y1 - y0) * stbtt_ScaleForMappingEmToPixels(&font, px);
}

/* ---- output -------------------------------------------------------------- */
static int write_ppm(const char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f) return -1;
	fprintf(f, "P6\n%d %d\n255\n", L.w, L.h);
	for (int i = 0; i < L.w * L.h; i++) {
		unsigned char p[3] = { frame[i] >> 16, frame[i] >> 8, frame[i] };
		fwrite(p, 1, 3, f);
	}
	return fclose(f);
}

static int read_rot(void)
{
	FILE *f = fopen("/sys/class/graphics/fbcon/rotate", "r");
	int r = 3;   /* our kernel cmdline: fbcon=rotate:3 */
	if (f) { if (fscanf(f, "%d", &r) != 1) r = 3; fclose(f); }
	return r & 3;
}

static void set_kd_mode(int mode)
{
	int fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);
	if (fd < 0) return;
	if (ioctl(fd, KDSETMODE, mode))
		fprintf(stderr, "charger-ui: KDSETMODE: %s\n", strerror(errno));
	close(fd);
}

static inline uint32_t pack(uint32_t rgb, const struct fb_var_screeninfo *v)
{
	uint32_t r = rgb >> 16 & 255, g = rgb >> 8 & 255, b = rgb & 255;
	return (r >> (8 - v->red.length)) << v->red.offset |
	       (g >> (8 - v->green.length)) << v->green.offset |
	       (b >> (8 - v->blue.length)) << v->blue.offset |
	       (v->transp.length ? ((0xffu >> (8 - v->transp.length)) << v->transp.offset) : 0);
}

/* copy logical rects to the (rotated) framebuffer */
static int blit(const char *dev, int rot, const struct rect *rs, int n)
{
	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo fx;
	int fd = open(dev, O_RDWR | O_CLOEXEC);

	if (fd < 0) { fprintf(stderr, "charger-ui: %s: %s\n", dev, strerror(errno)); return -1; }
	if (ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &fx)) {
		fprintf(stderr, "charger-ui: fb ioctl: %s\n", strerror(errno)); close(fd); return -1;
	}
	if (v.bits_per_pixel != 32) {
		fprintf(stderr, "charger-ui: %u bpp unsupported\n", v.bits_per_pixel); close(fd); return -1;
	}
	int pw = v.xres, ph = v.yres, stride = fx.line_length / 4;
	int lw = (rot & 1) ? ph : pw, lh = (rot & 1) ? pw : ph;
	if (lw != L.w || lh != L.h)
		fprintf(stderr, "charger-ui: fb %dx%d rot %d = logical %dx%d (layout %dx%d)\n",
			pw, ph, rot, lw, lh, L.w, L.h);
	size_t len = fx.smem_len ? fx.smem_len : (size_t)fx.line_length * v.yres_virtual;
	uint32_t *fb = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED) { fprintf(stderr, "charger-ui: mmap: %s\n", strerror(errno)); close(fd); return -1; }
	fb += v.yoffset * stride + v.xoffset;

	for (int k = 0; k < n; k++) {
		struct rect r = rs[k];
		if (r.x1 > lw) r.x1 = lw;
		if (r.y1 > lh) r.y1 = lh;
		if (r.x0 >= r.x1 || r.y0 >= r.y1) continue;
		/* the same rect in physical coordinates (half-open) */
		int qx0, qy0, qx1, qy1;
		switch (rot) {
		case 1:  qx0 = pw - r.y1; qx1 = pw - r.y0; qy0 = r.x0;      qy1 = r.x1;      break;
		case 2:  qx0 = pw - r.x1; qx1 = pw - r.x0; qy0 = ph - r.y1; qy1 = ph - r.y0; break;
		case 3:  qx0 = r.y0;      qx1 = r.y1;      qy0 = ph - r.x1; qy1 = ph - r.x0; break;
		default: qx0 = r.x0;      qx1 = r.x1;      qy0 = r.y0;      qy1 = r.y1;      break;
		}
		/* walk in physical scanout order: sequential writes per panel row
		 * (fbdev memory is usually write-combined; strided writes are slow) */
		for (int py = qy0; py < qy1; py++) {
			uint32_t *row = fb + py * stride;
			for (int px = qx0; px < qx1; px++) {
				int x, y;   /* logical pixel shown at (px, py) */
				switch (rot) {
				case 1:  x = py;          y = pw - 1 - px; break; /* CW  */
				case 2:  x = pw - 1 - px; y = ph - 1 - py; break; /* UD  */
				case 3:  x = ph - 1 - py; y = px;          break; /* CCW: fbcon_ccw.c */
				default: x = px;          y = py;          break;
				}
				row[px] = pack(frame[y * L.w + x], &v);
			}
		}
	}
	munmap(fb - (v.yoffset * stride + v.xoffset), len);
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	const char *bg = "/share/charger/bg.png", *fontp = "/share/charger/Nunito-Regular.ttf";
	const char *dev = "/dev/fb0", *out = NULL, *line = "";
	int pct = -1, full = 0, rot = -1;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
		if (!strcmp(a, "--text-mode")) { set_kd_mode(KD_TEXT); return 0; }
		else if (!strcmp(a, "--full")) full = 1;
		else if (!strcmp(a, "--pct") && v) { pct = atoi(v); i++; }
		else if (!strcmp(a, "--line") && v) { line = v; i++; }
		else if (!strcmp(a, "--bg") && v) { bg = v; i++; }
		else if (!strcmp(a, "--font") && v) { fontp = v; i++; }
		else if (!strcmp(a, "--fb") && v) { dev = v; i++; }
		else if (!strcmp(a, "--out") && v) { out = v; i++; }
		else if (!strcmp(a, "--rot") && v) { rot = atoi(v) & 3; i++; }
		else { fprintf(stderr, "charger-ui: bad argument %s\n", a); return 2; }
	}

	frame = malloc(sizeof(*frame) * L.w * L.h);
	if (!frame) return 1;
	load_background(bg);
	int have_font = load_font(fontp) == 0;

	struct rect rs[3];
	int n = 0;
	rs[n++] = draw_circle(pct);
	if (have_font) {
		char buf[16];
		if (pct >= 0) snprintf(buf, sizeof(buf), "%d%%", pct > 100 ? 100 : pct);
		else snprintf(buf, sizeof(buf), "--%%");
		double r_in = L.r_out - L.ring;
		draw_text(buf, L.pct_px, L.cx, L.cy + digit_height(L.pct_px) / 2,
			  (struct rect){ (int)(L.cx - r_in), (int)(L.cy - r_in), (int)(L.cx + r_in) + 1, (int)(L.cy + r_in) + 1 });
		struct rect lr = clip((struct rect){ (int)(L.line_cx - L.line_halfw), (int)(L.line_base - L.line_px * 1.1),
						     (int)(L.line_cx + L.line_halfw), (int)(L.line_base + L.line_px * 0.45) });
		draw_text(line, L.line_px, L.line_cx, L.line_base, lr);
		rs[n++] = lr;
	}

	if (out) return write_ppm(out) ? 1 : 0;
	if (full) {
		set_kd_mode(KD_GRAPHICS);
		rs[0] = (struct rect){ 0, 0, L.w, L.h };
		n = 1;
	}
	return blit(dev, rot >= 0 ? rot : read_rot(), rs, n) ? 1 : 0;
}
