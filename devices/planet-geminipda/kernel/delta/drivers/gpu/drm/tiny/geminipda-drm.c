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
 * CORE RULE 5: this driver never initialises the panel.  LK does that.
 * The shadow blit only writes the already-initialised scanout region, so
 * there is no path from here to the "uninitialised panel" flicker.  The
 * banned mediatek-drm/mtk-mmsys/DSI-PHY stack (which WOULD re-drive the
 * panel) stays excluded — see devices/planet-geminipda/kernel/default.nix.
 */

#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <asm/unaligned.h>

#include <drm/drm_atomic.h>
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

#define DRIVER_NAME	"geminipda-drm"
#define DRIVER_DESC	"Gemini PDA (MT6797) LK framebuffer DRM/KMS driver"
#define DRIVER_DATE	"20260910"
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
	int ret, idx;

	if (!fb)
		return;

	ret = drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE);
	if (ret)
		return;

	if (!drm_dev_enter(dev, &idx))
		goto out;

	drm_atomic_helper_damage_iter_init(&iter, old_plane_state, plane_state);
	drm_atomic_for_each_plane_damage(&iter, &damage) {
		struct drm_rect dst_clip = plane_state->dst;
		struct iosys_map dst = IOSYS_MAP_INIT_VADDR_IOMEM(sdev->screen_base);
		unsigned int offset;

		if (!drm_rect_intersect(&dst_clip, &damage))
			continue;

		offset = drm_fb_clip_offset(sdev->pitch, sdev->format, &dst_clip);
		iosys_map_incr(&dst, offset);

		/*
		 * fb->format is XRGB8888 (the plane format list is built by
		 * drm_fb_build_fourcc_list(), which strips alpha from the
		 * native ARGB8888), while sdev->format is the scanout's
		 * ARGB8888.  drm_fb_blit() therefore runs the XRGB8888 ->
		 * ARGB8888 conversion, which fills the alpha byte with 0xff
		 * during the copy — exactly what the LK/OVL scanout needs (see
		 * the file header).  Do NOT add a separate per-pixel alpha
		 * pass here: that pinned the DRM commit worker at 100 % CPU.
		 */
		drm_fb_blit(&dst, &sdev->pitch, sdev->format->format,
			    shadow_plane_state->data, fb, &damage);
	}

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
 * The CRTC is always enabled.  Screen updates are performed by the
 * primary plane's atomic_update function; disabling clears the screen in
 * the primary plane's atomic_disable function.
 */
static const struct drm_crtc_helper_funcs geminipda_drm_crtc_helper_funcs = {
	.mode_valid = geminipda_drm_crtc_helper_mode_valid,
	.atomic_check = drm_crtc_helper_atomic_check,
};

static const struct drm_crtc_funcs geminipda_drm_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
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

	return 0;
}

static void geminipda_drm_shutdown(struct platform_device *pdev)
{
	struct geminipda_drm_device *sdev = platform_get_drvdata(pdev);

	drm_atomic_helper_shutdown(&sdev->dev);
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
