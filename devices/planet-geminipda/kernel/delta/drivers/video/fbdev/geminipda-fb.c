// SPDX-License-Identifier: GPL-2.0
/*
 * Gemini PDA (MT6797) "screen console" framebuffer driver.
 *
 * LK (Little Kernel, dguidipc/gemini-lk) initializes the NT36672 FHD DSI
 * panel and leaves a framebuffer in DRAM, announcing it in the DTB
 * /chosen node. On real hardware (non-FPGA build) LK writes a single
 * raw little-endian blob:
 *
 *	atag,videolfb:
 *		u64  fb phys base (LE)
 *		u32  is_lcm_found
 *		u32  fps
 *		u32  vram size
 *		char lcm name[] (NUL-terminated, 4-byte padded)
 *
 * (platform/mt6797/atags.c target_atag_videolfb(), invoked from
 * mt_boot.c under `#ifndef MACH_FPGA_NO_DISPLAY`.)  On FPGA builds LK
 * instead writes three separate big-endian properties:
 *
 *	atag,videolfb-fb_base_h / atag,videolfb-fb_base_l / atag,videolfb-vramSize
 *
 * (mt_disp_drv.c mt_disp_config_frame_buffer(), the `#else` branch.)
 *
 * The fb base is chosen by LK at runtime (mblock_reserve at/above
 * 0xC0000000, 64 KiB aligned — platform.c:674), so it must be read from
 * the DTB rather than hardcoded. The board DTS reserves the whole range
 * (0xC0000000..0xC4000000, no-map) because LK only shrinks /memory by
 * fb_size from the TOP of the last DRAM bank, which does not cover the
 * fb region.
 *
 * Panel geometry (LK LCM driver aeon_nt36672_fhd_dsi_vdo_x600_xinli):
 * 1080x2160. LK drives the fb layer in eBGRA8888 format (redoffset_32bit
 * = 1, mt_disp_drv.c:30,209,228). MediaTek names formats LSB-first, so
 * eBGRA8888 == byte0=B, byte1=G, byte2=R, byte3=A == Linux a8r8g8b8.
 * This driver declares the same (see PX() and the var offsets below).
 * If the console text appears rotated on a given unit, add
 * fbcon=rotate:<1|2|3> to the cmdline.
 *
 * CRITICAL — row pitch (verified on hardware 2026-08-31, build 272):
 * LK configures the OVL FB layer with
 *   input.src_pitch = ALIGN_TO(CFG_DISPLAY_WIDTH, MTK_FB_ALIGNMENT)*4
 * (mt_disp_drv.c:234; MTK_FB_ALIGNMENT=32, disp_drv_platform.h:31),
 * i.e. 1088 px = 4352 bytes per row, NOT 1080*4. Writing rows at a
 * 4320-byte stride shears the image: each displayed row is shifted
 * left 8 px per row and wraps every 135 rows — console text appears as
 * ~9 tilted yellow streaks wrapping edge-to-edge (observed on glass).
 * The 8 trailing pixels per row are never displayed (OVL dst_w=1080).
 */

#include <linux/dma-buf.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pfn.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/uaccess.h>
#include <asm/unaligned.h>

#define GEMINIPDA_FB_WIDTH	1080
#define GEMINIPDA_FB_HEIGHT	2160
/* LK OVL reads the fb at ALIGN(1080,32) px/row = 1088 (see header) */
#define GEMINIPDA_FB_ALIGN	1088
#define GEMINIPDA_FB_STRIDE	(GEMINIPDA_FB_ALIGN * 4) /* a8r8g8b8 */

/*
 * Pixel layout: LK drives the OVL input as eBGRA8888 (mt_disp_drv.c:30
 * redoffset_32bit=1, :209 input.fmt = eBGRA8888). MediaTek names this
 * format LSB-first, so "eBGRA8888" == byte0=B, byte1=G, byte2=R,
 * byte3=A — which is Linux's a8r8g8b8 (red.offset 16, green.offset 8,
 * blue.offset 0, transp.offset 24). LK enables per-pixel alpha
 * (input.aen=1), so byte3 MUST be 0xff (opaque) or the OVL blends the
 * pixel to fully transparent. Evidence (hardware probe 2026-08-31,
 * read bottom-to-top due to the landscape rotation):
 *   [255,0,0,255]->blue   [0,255,0,255]->green   [0,0,255,255]->red
 *   [255,255,255,255]->white   [255,255,255,0]->transparent (black)
 *   - ixoo/gemini-pda-mainline boots this same hardware with simplefb
 *     format a8r8g8b8 ({16,8},{8,8},{0,8},{24,8} — simplefb.h).
 *   - BSP mtkfb.c:537 maps blue.offset==0 -> DISP_FORMAT_BGRA8888.
 * As a 32-bit little-endian value: (0xff<<24)|(R<<16)|(G<<8)|(B).
 *
 * fbcon's 1-bit font blits (sys_imageblit in sysimgblt.c) take their
 * fg/bg colors from info->pseudo_palette for truecolor frames. Without
 * a populated pseudo_palette + fb_set_cmap handler, every character is
 * drawn 0x00000000 (black on black — the "invisible console" bug,
 * verified on hardware 2026-08-31: the whole boot log rendered
 * zero-valued pixels).
 */
#define PX(r, g, b) (0xFF000000UL | ((u32)(r) << 16) | ((u32)(g) << 8) | (u32)(b))

static u32 geminipda_fb_pseudo_palette[16] = {
	PX(0, 0, 0),		/* 0: black */
	PX(170, 0, 0),		/* 1: red */
	PX(0, 170, 0),		/* 2: green */
	PX(170, 85, 0),		/* 3: brown */
	PX(0, 0, 170),		/* 4: blue */
	PX(170, 0, 170),	/* 5: magenta */
	PX(0, 170, 170),	/* 6: cyan */
	PX(170, 170, 170),	/* 7: light gray */
	PX(85, 85, 85),		/* 8: dark gray */
	PX(255, 85, 85),	/* 9: bright red */
	PX(85, 255, 85),	/* 10: bright green */
	PX(255, 255, 85),	/* 11: bright yellow */
	PX(85, 85, 255),	/* 12: bright blue */
	PX(255, 85, 255),	/* 13: bright magenta */
	PX(85, 255, 255),	/* 14: bright cyan */
	PX(255, 255, 255),	/* 15: white */
};

/*
 * fbcon pushes the 16-color console palette through fb_set_cmap as u16
 * entries of the (v<<8)|v pattern; normalize to 8-bit channels and
 * keep pseudo_palette in sync so the 1-bit font blits get real colors.
 */
static u8 geminipda_fb_cmap8(u32 v)
{
	return v ? ((v & 0xff00) ? (v >> 8) & 0xff : v & 0xff) : 0;
}

static int geminipda_fb_set_cmap(struct fb_cmap *cmap, struct fb_info *info)
{
	int i;

	if (!cmap || !cmap->red || !cmap->green || !cmap->blue ||
	    cmap->start > 16 || cmap->start + cmap->len > 16)
		return 0;

	for (i = 0; i < cmap->len; i++) {
		u32 r = cmap->red[cmap->start + i];
		u32 g = cmap->green[cmap->start + i];
		u32 b = cmap->blue[cmap->start + i];

		((u32 *)info->pseudo_palette)[cmap->start + i] =
			PX(geminipda_fb_cmap8(r), geminipda_fb_cmap8(g),
			   geminipda_fb_cmap8(b));
	}
	return 0;
}

static void geminipda_fb_fillrect(struct fb_info *info,
				  const struct fb_fillrect *rect)
{
	sys_fillrect(info, rect);
}

static void geminipda_fb_copyarea(struct fb_info *info,
				  const struct fb_copyarea *area)
{
	sys_copyarea(info, area);
}

/*
 * The boot logo (fb_show_logo) is blitted as an 8-bit image through a
 * temporary truecolor palette that fbmem builds from the red/green/blue
 * fields only, so every logo pixel lands with alpha 0 = transparent on
 * LK's alpha-enabled OVL layer (the logo was invisible and left a blank
 * band above the console text; 2026-10-05). Font glyphs (depth 1) use
 * our opaque pseudo_palette and are fine. Force the alpha byte opaque
 * over any non-1-bit image after blitting it.
 */
static void geminipda_fb_imageblit(struct fb_info *info,
				   const struct fb_image *image)
{
	u32 x, y, w, h;

	sys_imageblit(info, image);

	if (image->depth == 1)
		return;

	x = image->dx;
	y = image->dy;
	w = min_t(u32, image->width, info->var.xres_virtual - min(x, info->var.xres_virtual));
	h = min_t(u32, image->height, info->var.yres_virtual - min(y, info->var.yres_virtual));
	for (; h; h--, y++) {
		u32 *p = (u32 *)((u8 __force *)info->screen_base +
				 y * info->fix.line_length) + x;
		u32 i;

		for (i = 0; i < w; i++)
			p[i] |= 0xff000000U;
	}
}

/*
 * Fixed-geometry framebuffer: the LK OVL reads this buffer with a fixed
 * mode (1080x2160 a8r8g8b8, 1088-px rows), so the physical geometry and
 * layout are normalized to the hardware reality. The virtual sizes are
 * kept >= the request (as well as >= the hardware): X.Org's fbdev driver
 * swaps virtualX/virtualY when it rotates CCW/CW, so it asks for
 * xres_virtual=2160, yres_virtual=1080 and compares the ioctl result
 * with >= ("FBIOPUT_VSCREENINFO succeeded but modified mode" otherwise,
 * observed 2026-08-31). Without fb_check_var at all, fb_set_var()
 * wholesale replaces the caller's var with info->var and the same
 * comparison fails. The timing fields are left untouched — meaningless
 * to the LK OVL, but X compares them verbatim.
 */
static int geminipda_fb_check_var(struct fb_var_screeninfo *var,
				  struct fb_info *info)
{
	u32 xv = max(var->xres_virtual, GEMINIPDA_FB_ALIGN);
	u32 yv = max(var->yres_virtual, GEMINIPDA_FB_HEIGHT);

	var->xres = GEMINIPDA_FB_WIDTH;
	var->yres = GEMINIPDA_FB_HEIGHT;
	var->xres_virtual = xv;
	var->yres_virtual = yv;
	var->xoffset = 0;
	var->yoffset = 0;
	var->bits_per_pixel = 32;
	var->red.offset = 16;	var->red.length = 8;
	var->green.offset = 8;	var->green.length = 8;
	var->blue.offset = 0;	var->blue.length = 8;
	var->transp.offset = 24; var->transp.length = 8;
	var->nonstd = 0;
	var->grayscale = 0;
	return 0;
}

static const struct fb_ops geminipda_fb_ops = {
	.owner		= THIS_MODULE,
	.fb_check_var	= geminipda_fb_check_var,
	.fb_setcmap	= geminipda_fb_set_cmap,
	.fb_read	= fb_sys_read,
	.fb_write	= fb_sys_write,
	.fb_fillrect	= geminipda_fb_fillrect,
	.fb_copyarea	= geminipda_fb_copyarea,
	.fb_imageblit	= geminipda_fb_imageblit,
};

/*
 * =====================================================================
 * dma-buf export of the LK framebuffer (GPU-direct render target)
 * =====================================================================
 *
 * The Mali-T880 (panfrost) can render directly into any physical DRAM
 * region: kernel panfrost's mmu_map_sg() only consumes sg_dma_address()
 * / sg_dma_len() of the imported sg_table (drivers/gpu/drm/panfrost/
 * panfrost_mmu.c mmu_map_sg), so no struct page is needed for the
 * region — it stays a no-map reserved region (no linear mapping,
 * untouched by the page allocator). This exporter hands the fb region
 * to the GPU via the standard prime dma-buf path:
 *
 *   /dev/gemfb ioctl(GEMFB_IOC_EXPORT) -> dma-buf fd
 *     -> userspace: eglCreateImageKHR(EGL_EXT_image_dma_buf_import)
 *        (Mesa panfrost: panfrost_resource_from_handle ->
 *         drmPrimeFDToHandle -> panfrost_gem_prime_import_sg_table)
 *     -> GPU renders into it; every panfrost job is submitted with
 *        JS_CONFIG_END_FLUSH_CLEAN_INVALIDATE so L2 is flushed to
 *        DRAM on job end (panfrost_job.c panfrost_job_start) — the
 *        OVL scan-out then sees the fresh frame in DRAM.
 *
 * The region is physically contiguous (one LK mblock reservation),
 * so the sg_table is a single entry covering the whole area.
 */
#define GEMFB_IOC_MAGIC 'G'
#define GEMFB_IOC_EXPORT _IOW(GEMFB_IOC_MAGIC, 1, int)

static u64 gemfb_base;
static u32 gemfb_size;
static void __iomem *gemfb_wc;
static struct scatterlist gemfb_sgl;
static struct sg_table gemfb_sgt;
static bool gemfb_ready;

static int gemfb_attach(struct dma_buf *db, struct dma_buf_attachment *attach)
{
	return 0;
}

static void gemfb_detach(struct dma_buf *db, struct dma_buf_attachment *attach)
{
}

static struct sg_table *gemfb_map_dma_buf(struct dma_buf_attachment *attach,
					   enum dma_data_direction dir)
{
	return &gemfb_sgt;
}

static void gemfb_unmap_dma_buf(struct dma_buf_attachment *attach,
				 struct sg_table *sgt, enum dma_data_direction dir)
{
}

/* CPU access is fine: the fb driver already ioremap_wc's the region
 * (non-cached, no coherency to manage). */
static int gemfb_begin_cpu_access(struct dma_buf *db, enum dma_data_direction dir)
{
	return 0;
}

static int gemfb_end_cpu_access(struct dma_buf *db, enum dma_data_direction dir)
{
	return 0;
}

/* Optional user-space mapping (debugging); WC like the fbdev path.
 *
 * Bound the mapping to the VMA length: the original always remapped the
 * WHOLE gemfb_size starting at vma->vm_start, so a mmap() shorter than
 * the region wrote page tables past vm_end (and, worse, `gemfb_size` may
 * exceed the visible fb). On glass 2026-09-11 a CPU mmap+read of this
 * dma-buf via gemshell's debug screenshot hung the system until the WDT
 * reset it — this bound is the guard (the path is otherwise unused). */
static int gemfb_mmap(struct dma_buf *db, struct vm_area_struct *vma)
{
	unsigned long size = vma->vm_end - vma->vm_start;

	if (vma->vm_pgoff != 0 || size > gemfb_size)
		return -EINVAL;
	return remap_pfn_range(vma, vma->vm_start, gemfb_base >> PAGE_SHIFT,
			       size, pgprot_noncached(vma->vm_page_prot));
}

static void gemfb_release(struct dma_buf *db)
{
	/* region lives for the lifetime of the driver; nothing to free */
}

static const struct dma_buf_ops gemfb_ops = {
	.cache_sgt_mapping = true,
	.attach = gemfb_attach,
	.detach = gemfb_detach,
	.map_dma_buf = gemfb_map_dma_buf,
	.unmap_dma_buf = gemfb_unmap_dma_buf,
	.begin_cpu_access = gemfb_begin_cpu_access,
	.end_cpu_access = gemfb_end_cpu_access,
	.mmap = gemfb_mmap,
	.release = gemfb_release,
};

static int gemfb_export_fd(void)
{
	struct dma_buf_export_info exp = {
		.exp_name = "geminipda-lkfb",
		.owner = THIS_MODULE,
		.ops = &gemfb_ops,
		.size = gemfb_size,
		.priv = &gemfb_sgt,
	};
	struct dma_buf *db;
	int fd;

	db = dma_buf_export(&exp);
	if (IS_ERR(db))
		return PTR_ERR(db);
	/* 6.6: dma_buf_fd() installs the export's file into the fd table and
	 * TRANSFERS the reference (fd_install, no ref bump — see drm_prime.c
	 * drm_prime_handle_to_fd: dma_buf_put only on error). Putting here
	 * would free the file while the fd still points at it (kernel WARN
	 * "VFS: Close: file count is 0"). */
	fd = dma_buf_fd(db, 0);
	if (fd < 0)
		dma_buf_put(db); /* export ref still ours on failure */
	return fd;
}

static long gemfb_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int fd;

	if (cmd != GEMFB_IOC_EXPORT)
		return -ENOTTY;
	if (!gemfb_ready)
		return -ENODEV;

	fd = gemfb_export_fd();
	if (fd < 0)
		return fd;
	return fd; /* fd ownership passes to the caller via the ioctl return */
}

static const struct file_operations gemfb_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = gemfb_ioctl,
};

static struct miscdevice gemfb_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "gemfb",
	.fops = &gemfb_fops,
};

static int gemfb_export_setup(u64 base, u32 size, void __iomem *wc)
{
	gemfb_base = base;
	gemfb_size = size;
	gemfb_wc = wc;

	gemfb_sgt.sgl = &gemfb_sgl;
	sg_init_one(&gemfb_sgl, NULL, size);
	sg_dma_address(&gemfb_sgl) = base;
	/* CONFIG_NEED_SG_DMA_LENGTH=y: sg_dma_len() reads the real dma_length
	 * field — leaving it 0 makes mmu_map_sg map nothing (GPU
	 * TRANSLATION_FAULT on the imported BO). */
	sg_dma_len(&gemfb_sgl) = size;
	gemfb_sgt.nents = gemfb_sgt.orig_nents = 1;

	if (misc_register(&gemfb_miscdev)) {
		pr_warn("geminipda-fb: /dev/gemfb misc_register failed\n");
		return -ENOMEM; /* non-fatal for the console; reported above */
	}
	gemfb_ready = true;
	pr_info("geminipda-fb: dma-buf export ready (/dev/gemfb, 0x%llx+0x%x)\n",
		base, size);
	return 0;
}

/*
 * Read the LK framebuffer geometry from /chosen. Returns 0 and fills
 * the fb_base and fb_size outputs on success, -ENODEV/-EINVAL otherwise.
 */
static int geminipda_fb_get_geometry(u64 *fb_base, u32 *fb_size)
{
	struct device_node *chosen;
	const __be32 *val;
	const u8 *b;
	int len;

	chosen = of_find_node_by_path("/chosen");
	if (!chosen)
		return -ENODEV;

	/* Hardware path: raw LE blob "atag,videolfb". */
	val = of_get_property(chosen, "atag,videolfb", &len);
	if (val && len >= 8 + 4 + 4 + 4) {
		b = (const u8 *)val;
		*fb_base = get_unaligned_le64(b);
		*fb_size = get_unaligned_le32(b + 16);
		of_node_put(chosen);
		return 0;
	}

	/* FPGA-only fallback: separate big-endian props. */
	*fb_base = 0;
	val = of_get_property(chosen, "atag,videolfb-fb_base_h", &len);
	if (!val || len != 4)
		goto err;
	*fb_base = (u64)be32_to_cpu(*val) << 32;

	val = of_get_property(chosen, "atag,videolfb-fb_base_l", &len);
	if (!val || len != 4)
		goto err;
	*fb_base |= be32_to_cpu(*val);

	val = of_get_property(chosen, "atag,videolfb-vramSize", &len);
	if (!val || len != 4)
		goto err;
	*fb_size = be32_to_cpu(*val);

	of_node_put(chosen);
	return 0;
err:
	of_node_put(chosen);
	return -ENODEV;
}

static int geminipda_fb_probe(struct platform_device *pdev)
{
	struct fb_fix_screeninfo fix = { 0 };
	struct fb_var_screeninfo var = { 0 };
	struct fb_info *info;
	void __iomem *screen;
	u64 fb_base;
	u32 fb_size;
	int ret;

	ret = geminipda_fb_get_geometry(&fb_base, &fb_size);
	if (ret)
		return ret;

	/* Sanity: the framebuffer must live inside DRAM (0x40000000..0x140000000) */
	if (!fb_base || !fb_size ||
	    fb_base < 0x40000000ULL || fb_base + fb_size > 0x140000000ULL) {
		pr_warn("geminipda-fb: implausible LK framebuffer 0x%llx+0x%x\n",
			fb_base, fb_size);
		return -ENODEV;
	}

	pr_info("geminipda-fb: LK framebuffer 0x%llx size 0x%x\n",
		fb_base, fb_size);

	screen = ioremap_wc(fb_base, fb_size);
	if (!screen)
		return -ENOMEM;

	/*
	 * Pass the platform device as the fb's parent: X.Org's fbdev
	 * driver (fbdevhw fbdev_open()) refuses any fb whose
	 * /sys/class/graphics/fbN/device/subsystem link is missing or
	 * resolves to bus/pci. With a parent device the link appears
	 * and points at bus/platform, so Xorg can use this fb as its
	 * screen (verified requirement 2026-08-31).
	 */
	info = framebuffer_alloc(0, &pdev->dev);
	if (!info) {
		iounmap(screen);
		return -ENOMEM;
	}

	info->screen_base = screen;
	info->screen_size = fb_size;
	info->fbops = &geminipda_fb_ops;
	info->pseudo_palette = geminipda_fb_pseudo_palette;

	strscpy(fix.id, "geminipda-lcd", sizeof(fix.id));
	fix.smem_start = fb_base;
	fix.smem_len = fb_size;
	fix.type = FB_TYPE_PACKED_PIXELS;
	fix.visual = FB_VISUAL_TRUECOLOR;
	fix.line_length = GEMINIPDA_FB_STRIDE;
	fix.accel = FB_ACCEL_NONE;
	info->fix = fix;

	var.xres = GEMINIPDA_FB_WIDTH;
	var.yres = GEMINIPDA_FB_HEIGHT;
	var.xres_virtual = GEMINIPDA_FB_ALIGN;
	var.yres_virtual = GEMINIPDA_FB_HEIGHT;
	var.xoffset = 0;
	var.yoffset = 0;
	/* 6.6 has no var.stride; the row length comes from fix.line_length */
	var.bits_per_pixel = 32;
	/* a8r8g8b8 — LK OVL input is MediaTek "eBGRA8888" (LSB-first:
	 * byte0=B, byte1=G, byte2=R, byte3=A). Per-pixel alpha is enabled,
	 * so byte3 (transp) is the alpha lane: 0xff = opaque (PX() sets
	 * it). */
	var.red.offset = 16;	var.red.length = 8;
	var.green.offset = 8;	var.green.length = 8;
	var.blue.offset = 0;	var.blue.length = 8;
	var.transp.offset = 24;	var.transp.length = 8;
	var.nonstd = 0;
	var.activate = FB_ACTIVATE_NOW;
	var.height = -1;
	var.width = -1;
	info->var = var;

	if (register_framebuffer(info) < 0) {
		iounmap(screen);
		framebuffer_release(info);
		return -EINVAL;
	}

	/* GPU-direct render target: export the same region as a dma-buf.
	 * Failure here only loses the /dev/gemfb export (GPU can no longer
	 * render straight into the LK fb); the console keeps working. */
	gemfb_export_setup(fb_base, fb_size, screen);

	pr_info("geminipda-fb: registered %dx%d %dbpp screen console\n",
		GEMINIPDA_FB_WIDTH, GEMINIPDA_FB_HEIGHT, var.bits_per_pixel);
	return 0;
}

static const struct of_device_id geminipda_fb_of_match[] = {
	{ .compatible = "planet,geminipda-fb" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, geminipda_fb_of_match);

static struct platform_driver geminipda_fb_driver = {
	.probe	= geminipda_fb_probe,
	.driver	= {
		.name		= "geminipda-fb",
		.of_match_table	= geminipda_fb_of_match,
	},
};
builtin_platform_driver(geminipda_fb_driver);
