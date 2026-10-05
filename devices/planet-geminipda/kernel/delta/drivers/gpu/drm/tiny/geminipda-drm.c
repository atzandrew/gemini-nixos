// SPDX-License-Identifier: GPL-2.0
/*
 * geminipda-drm - expose the Gemini PDA's bootloader framebuffer as DRM/KMS
 *
 * The LK (Little Kernel) bootloader initialises the NT36672 FHD DSI panel
 * and leaves a framebuffer in DRAM, announced via /chosen atag,videolfb
 * (see geminipda-fb.c for the full atag description).  That buffer is a
 * plain 1080x2160 portrait surface with a 1088-pixel (4352-byte) row
 * pitch, scanned out by the MediaTek OVL.  Until now the only userspace
 * access was the fbdev/dma-buf device (/dev/gemfb) that gemwl composites
 * into.
 *
 * This driver instead presents the same memory as a normal KMS device:
 * /dev/dri/card0 with one CRTC, one primary plane and one DSI connector.
 * That makes the display stack standard, so ordinary Wayland compositors
 * (GNOME Shell/mutter, KWin, …) can drive it without a bespoke
 * framebuffer client.  Modelled on drivers/gpu/drm/tiny/simpledrm.c.
 *
 * Design notes / receipts:
 *
 *  - The panel is physically mounted so that a landscape desktop must be
 *    rendered rotated 90 degrees into this portrait buffer.  Rather than
 *    rotate in the kernel, the driver advertises the standard DRM
 *    "panel orientation" connector property (default Left Side Up = 90,
 *    module parameter panel_orientation).  Mutter's native backend maps
 *    LEFT_UP -> MTK_MONITOR_TRANSFORM_90 and, because our CRTC advertises
 *    no hardware rotation, renders the rotated scene itself — the same
 *    transform gemwl applies with -t 90 today.  Receipts:
 *    mutter-50.4 src/backends/native/meta-kms-connector.c
 *    set_panel_orientation() and meta-renderer-native.c
 *    calculate_view_transform().
 *
 *  - The connector is DRM_MODE_CONNECTOR_DSI so that mutter treats it as
 *    a built-in panel (meta_output_info_is_builtin(), mutter
 *    src/backends/meta-output.c).
 *
 *  - The plane is a shadow plane (DRM_GEM_SHADOW_PLANE_HELPER_FUNCS):
 *    the compositor renders into its own GEM buffer and this driver
 *    blits that into the fixed scanout memory on every atomic update,
 *    exactly like simpledrm.
 *
 *  - The alpha byte is made opaque by the blit itself.  LK leaves the
 *    buffer in eBGRA8888 (byte3 = alpha) and the OVL/scanout path has
 *    been observed to render ARGB8888 content black when the alpha byte
 *    is not 0xff (pkgs/gemwl/gemwl.c "ARGB8888 renders BLACK … panfrost
 *    silently fails").  The primary plane advertises XRGB8888, because
 *    the DRM core's drm_fb_build_fourcc_list() strips the alpha channel
 *    from the native ARGB8888 format ("primary planes usually don't
 *    support alpha").  fb->format is therefore always XRGB8888 while the
 *    scanout format here is ARGB8888, so drm_fb_blit() takes its
 *    XRGB8888 -> ARGB8888 conversion path, which fills alpha with 0xff
 *    as part of the copy (drm_fb_xrgb8888_to_argb8888_line()).  No
 *    separate alpha pass is needed or wanted: the original per-pixel
 *    writeb() loop ran 2.3M barriered byte stores per full-screen update
 *    in the DRM commit worker and pinned it at ~100 % CPU.  [2026-09-10;
 *    rewritten 2026-09-10k]
 *
 *  - Fast single-pass copy + damage-scoped cache sync.  [2026-10-03,
 *    gemini-debian Plasma investigation]  perf under KWin showed the
 *    generic drm_fb_blit() path costing ~20 ms per commit for a small
 *    window (es2gears ~28 fps on glass): __drm_fb_xfrm ~45 %, its line
 *    buffer memmove + __memcpy_toio ~35 %, and the whole-object cache
 *    clean+invalidate of 509bd9e ~20 %.  For XRGB/ARGB8888 sources the
 *    copy is now one loop per damaged row: 8-byte loads, alpha forced to
 *    0xff with one OR per two pixels, 8-byte stores straight into the WC
 *    scanout (memcpy-speed; plain copy of a full frame ≈ 7 ms at the
 *    measured 2.8 GB/s).  The cache sync from 509bd9e (reverted in
 *    063585d) is back but limited to the byte range actually copied:
 *    the GPU renders into our shmem objects with non-coherent DMA while
 *    the blit reads them through a cached vmap, so stale lines are
 *    possible in principle (the X "residue" turned out to be X damage,
 *    not this).  Live switches (/sys/module/geminipda_drm/parameters):
 *    fast_copy, cache_sync; per-commit stats stat_* (copy time incl.
 *    sync, pixels copied, full-frame count) for measuring without
 *    ftrace.  No hardware register is touched (CORE RULE 5).
 *
 *  - Software vblank at refresh_hz (default 60) so flips complete at
 *    panel rate instead of copy rate.  [2026-10-03] See the comment above
 *    geminipda_drm_vblank_tick().
 *
 * CORE RULE 5: this driver never initialises the panel.  LK does that.
 * The shadow blit only writes the already-initialised scanout region, so
 * there is no path from here to the "uninitialised panel" flicker.  The
 * banned mediatek-drm/mtk-mmsys/DSI-PHY stack (which WOULD re-drive the
 * panel) stays excluded — see devices/planet-geminipda/kernel/default.nix.
 */

#include <linux/backlight.h>
#include <linux/dma-mapping.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <asm/unaligned.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_generic.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#define DRIVER_NAME	"geminipda-drm"
#define DRIVER_DESC	"Gemini PDA (MT6797) LK framebuffer DRM/KMS driver"
#define DRIVER_DATE	"20261003"
#define DRIVER_MAJOR	1
#define DRIVER_MINOR	0

/*
 * LK panel geometry.  1080x2160 native, ALIGN(1080,32) = 1088 px
 * (4352 B) row pitch, eBGRA8888 == Linux a8r8g8b8 == DRM ARGB8888.
 * Receipts: geminipda-fb.c header (verified on glass 2026-08-31).
 */
#define GEMINIPDA_DRM_WIDTH	1080
#define GEMINIPDA_DRM_HEIGHT	2160
#define GEMINIPDA_DRM_PITCH	(1088 * 4)
#define GEMINIPDA_DRM_FORMAT	DRM_FORMAT_ARGB8888
/* ~5.99" diagonal -> 68 x 136 mm (portrait native). */
#define GEMINIPDA_DRM_WIDTH_MM	68
#define GEMINIPDA_DRM_HEIGHT_MM	136

/*
 * Physical mounting of the panel.  DRM_MODE_PANEL_ORIENTATION_LEFT_UP
 * reproduces gemwl's default output transform of 90 degrees.  Override
 * on the kernel command line with geminipda-drm.panel_orientation=<0..3>
 * (0=normal, 1=upside-down, 2=left-up, 3=right-up) if the glass is
 * rotated/flipped on a given unit.
 */
static int panel_orientation = DRM_MODE_PANEL_ORIENTATION_LEFT_UP;
module_param(panel_orientation, int, 0444);
MODULE_PARM_DESC(panel_orientation,
	"Panel mounting: 0=normal, 1=upside-down, 2=left-up, 3=right-up");

/* Live A/B switches (see the 2026-10-03 note in the file header). */
static bool fast_copy = true;
module_param(fast_copy, bool, 0644);
MODULE_PARM_DESC(fast_copy,
	"Single-pass XRGB/ARGB8888 scanout copy (0 = generic drm_fb_blit)");

static bool cache_sync = true;
module_param(cache_sync, bool, 0644);
MODULE_PARM_DESC(cache_sync,
	"Clean+invalidate the CPU cache over the copied range before the blit");

/*
 * Per-commit statistics, read-only except stat_copy_us_max (write 0 to
 * reset).  Average copy time over an interval = delta(stat_copy_us_total)
 * / delta(stat_commits).  "Copy" includes the cache sync.
 */
static unsigned long stat_commits;
module_param(stat_commits, ulong, 0444);
static unsigned long stat_copy_us_total;
module_param(stat_copy_us_total, ulong, 0444);
static unsigned int stat_copy_us_last;
module_param(stat_copy_us_last, uint, 0444);
static unsigned int stat_copy_us_max;
module_param(stat_copy_us_max, uint, 0644);
static unsigned int stat_px_last;
module_param(stat_px_last, uint, 0444);
static unsigned long stat_full_frames;
module_param(stat_full_frames, ulong, 0444);

struct geminipda_drm_device {
	struct drm_device dev;
	struct drm_plane primary_plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
	const struct drm_format_info *format;
	unsigned int pitch;
	void __iomem *screen_base;
	struct drm_display_mode mode;
	u32 formats[4];
	/* Software vblank (see geminipda_drm_vblank_tick()). */
	struct hrtimer vblank_timer;
	u64 vblank_period_ns;
	bool vblank_on;
};

static struct geminipda_drm_device *
geminipda_drm_device_of_dev(struct drm_device *dev)
{
	return container_of(dev, struct geminipda_drm_device, dev);
}

/*
 * Geometry from /chosen.  Kept in sync with geminipda_fb_get_geometry()
 * in drivers/video/fbdev/geminipda-fb.c (that driver and this one are
 * alternative views of the same LK handover).
 */
static int geminipda_drm_get_geometry(u64 *fb_base, u32 *fb_size)
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

static const uint64_t geminipda_drm_primary_plane_format_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID
};

/*
 * Make GPU writes in [start, start + len) of plane 0's object visible to
 * the CPU blit.  Our own (non-imported) shmem objects are rendered into
 * by panfrost via kmsro with non-coherent DMA but read here through a
 * cached vmap; drm_gem_fb_begin_cpu_access() only syncs IMPORTED objects.
 * Clean first (keeps CPU-written lines from software-rendering clients
 * that mmap the dumb buffer), then invalidate.  Only the byte range the
 * blit reads is touched — 509bd9e did the whole ~9 MB object per commit.
 *
 * The ranges are synced with dma_sync_single_range_*() on the segments
 * of the object's sg mapping; with dma-direct (no IOMMU on this device)
 * DMA segments == CPU segments, so object offsets map 1:1.
 */
static void geminipda_drm_sync_range_for_cpu(struct drm_framebuffer *fb,
					     size_t start, size_t len)
{
	struct device *dmadev = fb->dev->dev;
	struct drm_gem_object *obj = drm_gem_fb_get_obj(fb, 0);
	struct sg_table *sgt;
	struct scatterlist *sg;
	size_t end = start + len, seg_start = 0;
	unsigned int i;

	if (!obj || obj->import_attach || !len)
		return;	/* imported: begin_cpu_access synced it */

	sgt = drm_gem_shmem_get_pages_sgt(to_drm_gem_shmem_obj(obj));
	if (IS_ERR(sgt))
		return;

	for_each_sgtable_dma_sg(sgt, sg, i) {
		size_t seg_end = seg_start + sg_dma_len(sg);

		if (seg_start >= end)
			break;
		if (seg_end > start) {
			size_t a = max(start, seg_start);
			size_t b = min(end, seg_end);

			dma_sync_single_range_for_device(dmadev, sg_dma_address(sg),
							 a - seg_start, b - a,
							 DMA_TO_DEVICE);
			dma_sync_single_range_for_cpu(dmadev, sg_dma_address(sg),
						      a - seg_start, b - a,
						      DMA_FROM_DEVICE);
		}
		seg_start = seg_end;
	}
}

/*
 * One-pass XRGB/ARGB8888 -> scanout ARGB8888 copy of a w x h rectangle:
 * 8-byte loads from the (cached) shadow mapping, alpha forced to 0xff
 * for both pixels with one OR, 8-byte stores into the write-combined
 * scanout.  Pixels are little-endian u32 0xAARRGGBB, so two pixels in a
 * u64 have their alpha bytes at bits 31:24 and 63:56.
 */
#define GEMINIPDA_ALPHA2	0xff000000ff000000ULL

static void geminipda_drm_copy_rect(u8 __iomem *dst, unsigned int dpitch,
				    const u8 *src, unsigned int spitch,
				    unsigned int w, unsigned int h)
{
	while (h--) {
		u8 __iomem *d = dst;
		const u8 *s = src;
		unsigned int n = w;

		/* Align the destination to 8 bytes (odd start column). */
		if (n && ((unsigned long)(__force void *)d & 7)) {
			__raw_writel(get_unaligned((const u32 *)s) | 0xff000000U, d);
			d += 4;
			s += 4;
			n--;
		}
		while (n >= 8) {
			u64 p0 = get_unaligned((const u64 *)(s + 0));
			u64 p1 = get_unaligned((const u64 *)(s + 8));
			u64 p2 = get_unaligned((const u64 *)(s + 16));
			u64 p3 = get_unaligned((const u64 *)(s + 24));

			__raw_writeq(p0 | GEMINIPDA_ALPHA2, d + 0);
			__raw_writeq(p1 | GEMINIPDA_ALPHA2, d + 8);
			__raw_writeq(p2 | GEMINIPDA_ALPHA2, d + 16);
			__raw_writeq(p3 | GEMINIPDA_ALPHA2, d + 24);
			d += 32;
			s += 32;
			n -= 8;
		}
		while (n >= 2) {
			__raw_writeq(get_unaligned((const u64 *)s) | GEMINIPDA_ALPHA2, d);
			d += 8;
			s += 8;
			n -= 2;
		}
		if (n)
			__raw_writel(get_unaligned((const u32 *)s) | 0xff000000U, d);

		dst += dpitch;
		src += spitch;
	}
}

static void
geminipda_drm_primary_plane_helper_atomic_update(struct drm_plane *plane,
						 struct drm_atomic_state *state)
{
	struct drm_plane_state *plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_plane_state *old_plane_state = drm_atomic_get_old_plane_state(state, plane);
	struct drm_shadow_plane_state *shadow_plane_state = to_drm_shadow_plane_state(plane_state);
	struct drm_framebuffer *fb = plane_state->fb;
	struct drm_device *dev = plane->dev;
	struct geminipda_drm_device *sdev = geminipda_drm_device_of_dev(dev);
	struct drm_atomic_helper_damage_iter iter;
	struct drm_rect damage;
	bool fast, synced_all = false;
	unsigned int px = 0, us;
	u64 t0;
	int ret, idx;

	if (!fb)
		return;

	ret = drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE);
	if (ret)
		return;

	if (!drm_dev_enter(dev, &idx))
		goto out;

	t0 = ktime_get_ns();

	fast = fast_copy && !shadow_plane_state->data[0].is_iomem &&
	       (fb->format->format == DRM_FORMAT_XRGB8888 ||
		fb->format->format == DRM_FORMAT_ARGB8888);

	drm_atomic_helper_damage_iter_init(&iter, old_plane_state, plane_state);
	drm_atomic_for_each_plane_damage(&iter, &damage) {
		struct drm_rect dst_clip = plane_state->dst;
		unsigned int w, h;

		if (!drm_rect_intersect(&dst_clip, &damage))
			continue;

		w = drm_rect_width(&dst_clip);
		h = drm_rect_height(&dst_clip);
		if (!w || !h)
			continue;
		px += w * h;

		if (fast) {
			/*
			 * The plane is unscaled and full-screen (fixed mode,
			 * drm_plane_helper_atomic_check), so source and
			 * destination share coordinates.
			 */
			size_t soff = fb->offsets[0] +
				      (size_t)dst_clip.y1 * fb->pitches[0] +
				      (size_t)dst_clip.x1 * 4;
			size_t slen = (size_t)(h - 1) * fb->pitches[0] +
				      (size_t)w * 4;

			if (cache_sync)
				geminipda_drm_sync_range_for_cpu(fb, soff, slen);

			geminipda_drm_copy_rect((u8 __iomem *)sdev->screen_base +
						(size_t)dst_clip.y1 * sdev->pitch +
						(size_t)dst_clip.x1 * 4,
						sdev->pitch,
						(const u8 *)shadow_plane_state->data[0].vaddr + soff,
						fb->pitches[0], w, h);
		} else {
			struct iosys_map dst = IOSYS_MAP_INIT_VADDR_IOMEM(sdev->screen_base);
			unsigned int offset;

			if (cache_sync && !synced_all) {
				struct drm_gem_object *obj = drm_gem_fb_get_obj(fb, 0);

				if (obj)
					geminipda_drm_sync_range_for_cpu(fb, 0, obj->size);
				synced_all = true;
			}

			offset = drm_fb_clip_offset(sdev->pitch, sdev->format, &dst_clip);
			iosys_map_incr(&dst, offset);

			/*
			 * Generic path: drm_fb_blit() runs the XRGB8888 ->
			 * ARGB8888 conversion, which fills alpha with 0xff (see
			 * the file header).  Do NOT add a separate per-pixel
			 * alpha pass: that pinned the commit worker at 100 %.
			 */
			drm_fb_blit(&dst, &sdev->pitch, sdev->format->format,
				    shadow_plane_state->data, fb, &damage);
		}
	}

	/* Drain the write-combining buffer before the (fake) vblank event. */
	wmb();

	us = div_u64(ktime_get_ns() - t0, 1000);
	stat_commits++;
	stat_copy_us_total += us;
	stat_copy_us_last = us;
	if (us > stat_copy_us_max)
		stat_copy_us_max = us;
	stat_px_last = px;
	if (px >= GEMINIPDA_DRM_WIDTH * GEMINIPDA_DRM_HEIGHT)
		stat_full_frames++;

	drm_dev_exit(idx);
out:
	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
}

static void
geminipda_drm_primary_plane_helper_atomic_disable(struct drm_plane *plane,
						  struct drm_atomic_state *state)
{
	struct drm_device *dev = plane->dev;
	struct geminipda_drm_device *sdev = geminipda_drm_device_of_dev(dev);
	int idx;

	if (!drm_dev_enter(dev, &idx))
		return;

	/* Clear screen to black if disabled. */
	memset_io(sdev->screen_base, 0, (size_t)sdev->pitch * sdev->mode.vdisplay);

	drm_dev_exit(idx);
}

static const struct drm_plane_helper_funcs geminipda_drm_primary_plane_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = drm_plane_helper_atomic_check,
	.atomic_update = geminipda_drm_primary_plane_helper_atomic_update,
	.atomic_disable = geminipda_drm_primary_plane_helper_atomic_disable,
};

static const struct drm_plane_funcs geminipda_drm_primary_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	DRM_GEM_SHADOW_PLANE_FUNCS,
};

static enum drm_mode_status
geminipda_drm_crtc_helper_mode_valid(struct drm_crtc *crtc,
				     const struct drm_display_mode *mode)
{
	struct geminipda_drm_device *sdev = geminipda_drm_device_of_dev(crtc->dev);

	return drm_crtc_helper_mode_valid_fixed(crtc, mode, &sdev->mode);
}

/*
 * Software vblank.  [2026-10-03]
 *
 * The LK scanout is free-running and this driver never touches the
 * display hardware (CORE RULE 5), so there is no real vblank interrupt.
 * Without one, the DRM core completed every page flip as soon as the
 * blit finished, compositors believed the panel refreshed at copy speed
 * (labwc: ~157 commits/s on a 60 Hz panel — ~100 wasted frames/s) and
 * KWin's frame scheduler worked from irregular timestamps.  An hrtimer
 * now ticks at refresh_hz and drives drm_crtc_handle_vblank(); flip
 * events are armed in atomic_flush and delivered on the next tick, with
 * exact periodic timestamps (as in vkms).  The tick is NOT synchronised
 * to the panel's real scan (that needs the OVL/RDMA frame IRQ), so
 * tearing is unchanged.
 *
 * refresh_hz (0644): 60 = pace like the panel (default; 1..240 accepted);
 * 0 = uncapped: flip events are sent straight from atomic_flush and the
 * commit tail skips its wait-for-vblank (the old behaviour, for measuring
 * headroom) while the tick itself keeps running at 60 Hz for anything
 * that waits on vblank counts.  Changes apply from the next commit/tick.
 *
 * disable_vblank must not hrtimer_cancel(): the core calls it under
 * vblank_time_lock, which drm_handle_vblank() in the callback also takes.
 * Instead the callback checks vblank_on after handling and stops itself.
 */
static unsigned int refresh_hz = 60;
module_param(refresh_hz, uint, 0644);
MODULE_PARM_DESC(refresh_hz,
	"Software vblank rate in Hz (60 = panel; 0 = complete flips immediately)");

static u64 geminipda_drm_vblank_period_ns(void)
{
	unsigned int hz = READ_ONCE(refresh_hz);

	/* 0 = uncapped flips; the tick itself stays at the panel's 60 Hz. */
	return div_u64(NSEC_PER_SEC, hz ? clamp(hz, 1U, 240U) : 60U);
}

/*
 * Keep the vblank core's frame duration equal to the tick period: with no
 * hardware counter it derives vblank counts from timestamp deltas divided
 * by framedur_ns, so a mismatch would make it drop or double-count ticks.
 */
static void geminipda_drm_set_period(struct geminipda_drm_device *sdev, u64 period)
{
	struct drm_vblank_crtc *vblank = &sdev->dev.vblank[drm_crtc_index(&sdev->crtc)];

	WRITE_ONCE(sdev->vblank_period_ns, period);
	WRITE_ONCE(vblank->framedur_ns, (int)period);
}

static enum hrtimer_restart geminipda_drm_vblank_tick(struct hrtimer *timer)
{
	struct geminipda_drm_device *sdev =
		container_of(timer, struct geminipda_drm_device, vblank_timer);
	u64 period = geminipda_drm_vblank_period_ns();

	if (!READ_ONCE(sdev->vblank_on))
		return HRTIMER_NORESTART;

	/* Forward first (as real hw latches before the IRQ); see timestamp. */
	if (period != READ_ONCE(sdev->vblank_period_ns))
		geminipda_drm_set_period(sdev, period);
	hrtimer_forward_now(timer, ns_to_ktime(period));

	drm_crtc_handle_vblank(&sdev->crtc);

	return READ_ONCE(sdev->vblank_on) ? HRTIMER_RESTART : HRTIMER_NORESTART;
}

static int geminipda_drm_enable_vblank(struct drm_crtc *crtc)
{
	struct geminipda_drm_device *sdev = geminipda_drm_device_of_dev(crtc->dev);
	u64 period = geminipda_drm_vblank_period_ns();

	drm_calc_timestamping_constants(crtc, &crtc->mode);
	geminipda_drm_set_period(sdev, period);
	WRITE_ONCE(sdev->vblank_on, true);
	hrtimer_start(&sdev->vblank_timer, ns_to_ktime(period), HRTIMER_MODE_REL);

	return 0;
}

static void geminipda_drm_disable_vblank(struct drm_crtc *crtc)
{
	struct geminipda_drm_device *sdev = geminipda_drm_device_of_dev(crtc->dev);

	WRITE_ONCE(sdev->vblank_on, false);
	hrtimer_try_to_cancel(&sdev->vblank_timer);
}

static bool geminipda_drm_get_vblank_timestamp(struct drm_crtc *crtc,
					       int *max_error,
					       ktime_t *vblank_time,
					       bool in_vblank_irq)
{
	struct geminipda_drm_device *sdev = geminipda_drm_device_of_dev(crtc->dev);
	struct drm_vblank_crtc *vblank = &crtc->dev->vblank[drm_crtc_index(crtc)];

	if (!READ_ONCE(vblank->enabled)) {
		*vblank_time = ktime_get();
		return true;
	}

	/* The timer was forwarded before handling: back up one period. */
	*vblank_time = ktime_sub_ns(READ_ONCE(sdev->vblank_timer.node.expires),
				    READ_ONCE(sdev->vblank_period_ns));
	return true;
}

/*
 * Display "off" (DPMS / KWin screen blanking, 2026-10-05): the panel
 * itself must stay powered (only LK can initialise it again, CORE RULE
 * 5), so CRTC disable used to leave the LCD backlight lit behind a black
 * frame. Blank the backlight instead (backlight_disable(): pwm-backlight
 * drives 0 while keeping the user's brightness setting) and unblank it
 * on enable. The backlight is looked up by its sysfs name ("backlight",
 * the DT pwm-backlight node) when needed, so no DT link is required.
 */
static bool blank_backlight = true;
module_param(blank_backlight, bool, 0644);
MODULE_PARM_DESC(blank_backlight, "switch the LCD backlight off while the display is off (default on)");

static void geminipda_drm_set_backlight(bool on)
{
	struct backlight_device *bd;

	bd = backlight_device_get_by_name("backlight");
	if (!bd)
		return;
	if (on)
		backlight_enable(bd);
	else
		backlight_disable(bd);
	put_device(&bd->dev);
}

static void geminipda_drm_crtc_helper_atomic_enable(struct drm_crtc *crtc,
						    struct drm_atomic_state *state)
{
	drm_crtc_vblank_on(crtc);
	/* always unblank: the param may have changed while off */
	geminipda_drm_set_backlight(true);
}

static void geminipda_drm_crtc_helper_atomic_disable(struct drm_crtc *crtc,
						     struct drm_atomic_state *state)
{
	drm_crtc_vblank_off(crtc);
	if (READ_ONCE(blank_backlight))
		geminipda_drm_set_backlight(false);
}

/*
 * Runs after the primary plane's atomic_update (the blit), so the flip
 * event fires on the first tick after the copy is complete.
 */
static void geminipda_drm_crtc_helper_atomic_flush(struct drm_crtc *crtc,
						   struct drm_atomic_state *state)
{
	struct drm_pending_vblank_event *event = crtc->state->event;

	if (!event)
		return;
	crtc->state->event = NULL;

	spin_lock_irq(&crtc->dev->event_lock);
	if (READ_ONCE(refresh_hz) && drm_crtc_vblank_get(crtc) == 0)
		drm_crtc_arm_vblank_event(crtc, event);
	else
		drm_crtc_send_vblank_event(crtc, event);
	spin_unlock_irq(&crtc->dev->event_lock);
}

/*
 * Screen updates are performed by the primary plane's atomic_update
 * function; disabling clears the screen in the primary plane's
 * atomic_disable function.  CRTC enable/disable only switch the software
 * vblank on and off.
 */
static const struct drm_crtc_helper_funcs geminipda_drm_crtc_helper_funcs = {
	.mode_valid = geminipda_drm_crtc_helper_mode_valid,
	.atomic_check = drm_crtc_helper_atomic_check,
	.atomic_flush = geminipda_drm_crtc_helper_atomic_flush,
	.atomic_enable = geminipda_drm_crtc_helper_atomic_enable,
	.atomic_disable = geminipda_drm_crtc_helper_atomic_disable,
};

static const struct drm_crtc_funcs geminipda_drm_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.enable_vblank = geminipda_drm_enable_vblank,
	.disable_vblank = geminipda_drm_disable_vblank,
	.get_vblank_timestamp = geminipda_drm_get_vblank_timestamp,
};

static const struct drm_encoder_funcs geminipda_drm_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static int
geminipda_drm_connector_helper_get_modes(struct drm_connector *connector)
{
	struct geminipda_drm_device *sdev = geminipda_drm_device_of_dev(connector->dev);

	return drm_connector_helper_get_modes_fixed(connector, &sdev->mode);
}

static const struct drm_connector_helper_funcs geminipda_drm_connector_helper_funcs = {
	.get_modes = geminipda_drm_connector_helper_get_modes,
};

static const struct drm_connector_funcs geminipda_drm_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

/*
 * drm_atomic_helper_commit_tail() minus the wait-for-vblank when
 * refresh_hz == 0 (uncapped; see the software vblank comment).
 */
static void geminipda_drm_commit_tail(struct drm_atomic_state *old_state)
{
	struct drm_device *dev = old_state->dev;

	drm_atomic_helper_commit_modeset_disables(dev, old_state);
	drm_atomic_helper_commit_planes(dev, old_state, 0);
	drm_atomic_helper_commit_modeset_enables(dev, old_state);
	drm_atomic_helper_fake_vblank(old_state);
	drm_atomic_helper_commit_hw_done(old_state);
	if (READ_ONCE(refresh_hz))
		drm_atomic_helper_wait_for_vblanks(dev, old_state);
	drm_atomic_helper_cleanup_planes(dev, old_state);
}

static const struct drm_mode_config_helper_funcs geminipda_drm_mode_config_helper_funcs = {
	.atomic_commit_tail = geminipda_drm_commit_tail,
};

static const struct drm_mode_config_funcs geminipda_drm_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static struct drm_display_mode geminipda_drm_mode(unsigned int width,
						  unsigned int height,
						  unsigned int width_mm,
						  unsigned int height_mm)
{
	const struct drm_display_mode mode = {
		DRM_MODE_INIT(60, width, height, width_mm, height_mm)
	};

	return mode;
}

static struct geminipda_drm_device *
geminipda_drm_device_create(struct drm_driver *drv, struct platform_device *pdev)
{
	struct geminipda_drm_device *sdev;
	struct drm_device *dev;
	struct drm_plane *primary_plane;
	struct drm_crtc *crtc;
	struct drm_encoder *encoder;
	struct drm_connector *connector;
	const struct drm_format_info *format;
	u64 fb_base;
	u32 fb_size;
	unsigned long max_width, max_height;
	size_t nformats;
	int ret;

	sdev = devm_drm_dev_alloc(&pdev->dev, drv, struct geminipda_drm_device, dev);
	if (IS_ERR(sdev))
		return ERR_CAST(sdev);
	dev = &sdev->dev;
	platform_set_drvdata(pdev, sdev);

	/*
	 * 64-bit DMA mask for geminipda_drm_sync_range_for_cpu(): RAM runs
	 * to 0x140000000, and the OF default (32-bit) would bounce shmem
	 * pages above 4 GiB through swiotlb (from 509bd9e).
	 */
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		drm_warn(dev, "no 64-bit DMA mask (%d); GPU frames may show stale pixels\n", ret);

	/* --- Hardware settings ------------------------------------- */
	ret = geminipda_drm_get_geometry(&fb_base, &fb_size);
	if (ret) {
		drm_err(dev, "no LK framebuffer geometry in /chosen\n");
		return ERR_PTR(ret);
	}

	/* Sanity: the framebuffer must live inside DRAM. */
	if (!fb_base || fb_base < 0x40000000ULL ||
	    fb_base + fb_size > 0x140000000ULL) {
		drm_err(dev, "implausible LK framebuffer 0x%llx+0x%x\n",
			fb_base, fb_size);
		return ERR_PTR(-ENODEV);
	}

	format = drm_format_info(GEMINIPDA_DRM_FORMAT);
	if (!format)
		return ERR_PTR(-EINVAL);

	sdev->pitch = GEMINIPDA_DRM_PITCH;
	if ((u64)sdev->pitch * GEMINIPDA_DRM_HEIGHT > fb_size) {
		drm_err(dev, "framebuffer too small for 1080x2160x32 (%u bytes)\n",
			fb_size);
		return ERR_PTR(-EINVAL);
	}

	sdev->mode = geminipda_drm_mode(GEMINIPDA_DRM_WIDTH,
					GEMINIPDA_DRM_HEIGHT,
					GEMINIPDA_DRM_WIDTH_MM,
					GEMINIPDA_DRM_HEIGHT_MM);
	sdev->format = format;

	drm_dbg(dev, "display mode={" DRM_MODE_FMT "}\n", DRM_MODE_ARG(&sdev->mode));
	drm_dbg(dev, "framebuffer 0x%llx+0x%x, stride=%u bytes\n",
		fb_base, fb_size, sdev->pitch);

	/* --- Scanout memory ---------------------------------------- */
	/*
	 * A plain ioremap_wc, like geminipda-fb: the region is reserved
	 * by LK's own /reserved-memory node and is not claimed through the
	 * aperture API by any other driver here, so generic
	 * drm_aperture_remove_conflicting_* is unnecessary.
	 */
	sdev->screen_base = devm_ioremap_wc(dev->dev, fb_base, fb_size);
	if (!sdev->screen_base)
		return ERR_PTR(-ENOMEM);

	/* --- Modesetting ------------------------------------------- */
	ret = drmm_mode_config_init(dev);
	if (ret)
		return ERR_PTR(ret);

	max_width = max_t(unsigned long, GEMINIPDA_DRM_WIDTH, DRM_SHADOW_PLANE_MAX_WIDTH);
	max_height = max_t(unsigned long, GEMINIPDA_DRM_HEIGHT, DRM_SHADOW_PLANE_MAX_HEIGHT);

	dev->mode_config.min_width = GEMINIPDA_DRM_WIDTH;
	dev->mode_config.max_width = max_width;
	dev->mode_config.min_height = GEMINIPDA_DRM_HEIGHT;
	dev->mode_config.max_height = max_height;
	dev->mode_config.preferred_depth = format->depth;
	dev->mode_config.funcs = &geminipda_drm_mode_config_funcs;
	dev->mode_config.helper_private = &geminipda_drm_mode_config_helper_funcs;

	/* Primary plane */
	nformats = drm_fb_build_fourcc_list(dev, &format->format, 1,
					    sdev->formats, ARRAY_SIZE(sdev->formats));

	primary_plane = &sdev->primary_plane;
	ret = drm_universal_plane_init(dev, primary_plane, 0,
				       &geminipda_drm_primary_plane_funcs,
				       sdev->formats, nformats,
				       geminipda_drm_primary_plane_format_modifiers,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ERR_PTR(ret);
	drm_plane_helper_add(primary_plane, &geminipda_drm_primary_plane_helper_funcs);
	drm_plane_enable_fb_damage_clips(primary_plane);

	/* CRTC */
	crtc = &sdev->crtc;
	ret = drm_crtc_init_with_planes(dev, crtc, primary_plane, NULL,
					&geminipda_drm_crtc_funcs, NULL);
	if (ret)
		return ERR_PTR(ret);
	drm_crtc_helper_add(crtc, &geminipda_drm_crtc_helper_funcs);

	/* Encoder */
	encoder = &sdev->encoder;
	ret = drm_encoder_init(dev, encoder, &geminipda_drm_encoder_funcs,
			       DRM_MODE_ENCODER_DSI, NULL);
	if (ret)
		return ERR_PTR(ret);
	encoder->possible_crtcs = drm_crtc_mask(crtc);

	/* Connector */
	connector = &sdev->connector;
	ret = drm_connector_init(dev, connector, &geminipda_drm_connector_funcs,
				 DRM_MODE_CONNECTOR_DSI);
	if (ret)
		return ERR_PTR(ret);
	drm_connector_helper_add(connector, &geminipda_drm_connector_helper_funcs);

	ret = drm_connector_set_panel_orientation(connector, panel_orientation);
	if (ret) {
		drm_err(dev, "failed to set panel orientation: %d\n", ret);
		return ERR_PTR(ret);
	}

	ret = drm_connector_attach_encoder(connector, encoder);
	if (ret)
		return ERR_PTR(ret);

	/* Software vblank: one CRTC, timer armed by enable_vblank. */
	hrtimer_init(&sdev->vblank_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	sdev->vblank_timer.function = geminipda_drm_vblank_tick;
	ret = drm_vblank_init(dev, 1);
	if (ret)
		return ERR_PTR(ret);

	drm_mode_config_reset(dev);

	return sdev;
}

DEFINE_DRM_GEM_FOPS(geminipda_drm_fops);

static struct drm_driver geminipda_drm_driver = {
	DRM_GEM_SHMEM_DRIVER_OPS,
	.name			= DRIVER_NAME,
	.desc			= DRIVER_DESC,
	.date			= DRIVER_DATE,
	.major			= DRIVER_MAJOR,
	.minor			= DRIVER_MINOR,
	.driver_features	= DRIVER_ATOMIC | DRIVER_GEM | DRIVER_MODESET,
	.fops			= &geminipda_drm_fops,
};

static int geminipda_drm_probe(struct platform_device *pdev)
{
	struct geminipda_drm_device *sdev;
	struct drm_device *dev;
	int ret;

	sdev = geminipda_drm_device_create(&geminipda_drm_driver, pdev);
	if (IS_ERR(sdev))
		return PTR_ERR(sdev);
	dev = &sdev->dev;

	ret = drm_dev_register(dev, 0);
	if (ret)
		return ret;

	drm_fbdev_generic_setup(dev, 0);

	return 0;
}

static int geminipda_drm_remove(struct platform_device *pdev)
{
	struct geminipda_drm_device *sdev = platform_get_drvdata(pdev);
	struct drm_device *dev = &sdev->dev;

	drm_dev_unplug(dev);
	drm_atomic_helper_shutdown(dev);
	sdev->vblank_on = false;
	hrtimer_cancel(&sdev->vblank_timer);

	return 0;
}

static void geminipda_drm_shutdown(struct platform_device *pdev)
{
	struct geminipda_drm_device *sdev = platform_get_drvdata(pdev);

	drm_atomic_helper_shutdown(&sdev->dev);
	sdev->vblank_on = false;
	hrtimer_cancel(&sdev->vblank_timer);
}

static const struct of_device_id geminipda_drm_of_match[] = {
	{ .compatible = "planet,geminipda-drm" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, geminipda_drm_of_match);

static struct platform_driver geminipda_drm_platform_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = geminipda_drm_of_match,
	},
	.probe = geminipda_drm_probe,
	.remove = geminipda_drm_remove,
	.shutdown = geminipda_drm_shutdown,
};
module_platform_driver(geminipda_drm_platform_driver);

MODULE_AUTHOR("gemini-nixos");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
