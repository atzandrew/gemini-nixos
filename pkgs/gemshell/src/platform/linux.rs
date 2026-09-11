//! Linux backend — the product path *and* the x86_64 nested dev loop.
//!
//! Owns everything device/EGL specific that used to be threaded through
//! the compositor:
//!
//! * the **EGL context** (GBM platform on the panfrost render node);
//! * the **LK-framebuffer present**: `/dev/gemfb` -> dma-buf -> EGLImage
//!   -> a compute shader that rotates the scene FBO into the panel buffer
//!   (the gemwl chain — see `docs/gemshell.md`);
//! * the **poll() loop** over the Wayland display, the parent/compositor
//!   sockets, the GBM flip fd and the two evdev nodes.
//!
//! Nested mode (`GEMSHELL_NESTED=1`) reuses all of it except the present:
//! the scene FBO is read back and published to the host compositor over
//! `wl_shm` by `compositor::nested`.
//!
//! Design + receipts: docs/gemshell.md ("Platform backends").

use std::ffi::CString;
use std::os::fd::{AsFd, AsRawFd};
use std::os::raw::{c_char, c_int, c_void};
use std::sync::Arc;

use wayland_server::{Display, ListeningSocket};

use crate::common::{font, util};
use crate::compositor::gbm::Gbm;
use crate::compositor::input::{self, Input};
use crate::compositor::nested::{Nested, NestedInput};
use crate::compositor::render::{Glsl, Presenter, Renderer};
use crate::compositor::wayland::ClientState;
use crate::compositor::{Compositor, H, W};

use super::{Backend, InputEvent, InputSource};

// ---------------------------------------------------------------------------
// EGL

type EGLDisplay = *mut c_void;
type EGLContext = *mut c_void;
type EGLSurface = *mut c_void;
// EGLConfig is an opaque HANDLE (a pointer in Mesa), not the pointed-to
// object. Declaring it as `c_void` (1 byte) made `[EGLConfig; 64]` a
// 64-byte array while eglChooseConfig wrote 64 * 8 bytes of handles into
// it — stack corruption, the SEGV right after "config selected"
// (2026-09-11). It also made the eglCreateContext argument the address
// of the slot rather than the handle.
type EGLConfig = *mut c_void;
type EglImage = *mut c_void;
type EglCreateImageFn =
    unsafe extern "C" fn(EGLDisplay, EGLContext, u32, *const c_void, *const c_int) -> EglImage;
type GlEglImageTargetFn = unsafe extern "C" fn(u32, EglImage);

extern "C" {
    fn eglInitialize(d: EGLDisplay, major: *mut c_int, minor: *mut c_int) -> u32;
    fn eglBindAPI(api: u32) -> u32;
    fn eglChooseConfig(
        d: EGLDisplay,
        attrib_list: *const c_int,
        configs: *mut EGLConfig,
        config_count: c_int,
        num_configs: *mut c_int,
    ) -> u32;
    fn eglCreateContext(
        d: EGLDisplay,
        config: EGLConfig,
        share_context: EGLContext,
        attrib_list: *const c_int,
    ) -> EGLContext;
    fn eglMakeCurrent(d: EGLDisplay, draw: EGLSurface, read: EGLSurface, ctx: EGLContext) -> u32;
    fn eglQueryString(d: EGLDisplay, name: u32) -> *const c_char;
    fn eglGetError() -> c_int;
    fn eglDestroyContext(d: EGLDisplay, c: EGLContext) -> u32;
    fn eglTerminate(d: EGLDisplay) -> u32;
    fn eglGetProcAddress(name: *const c_char) -> *mut c_void;
}

// EGL enums — values from the Khronos EGL headers (verified against
// libglvnd 1.7.0 `include/EGL/egl.h`/`eglext.h`, 2026-09-11).
//
// These were ALL WRONG in the first cut (a hall of hallucinated
// constants: EGL_NONE = 0 instead of 0x3038, EGL_RENDERABLE_TYPE =
// 0x3095 instead of 0x3040, dma_buf attrs 0x32D5.. instead of
// 0x3270..). That single class of bug was the real cause of the
// on-glass crash loop: `eglChooseConfig` was handed the list [0],
// 0 is NOT EGL_NONE (0x3038), so the display parsed it as an unknown
// attribute and returned EGL_BAD_ATTRIBUTE (0x3004) with 0 configs —
// not a driver/ICD problem at all.
const EGL_PLATFORM_GBM_MESA: u32 = 0x31D7;
const EGL_SURFACE_TYPE: u32 = 0x3033;
const EGL_OPENGL_ES_API: u32 = 0x30A0;
const EGL_OPENGL_ES3_BIT: c_int = 0x00000040;
const EGL_RENDERABLE_TYPE: c_int = 0x3040;
const EGL_RED_SIZE: c_int = 0x3024;
const EGL_WIDTH: c_int = 0x3057;
const EGL_HEIGHT: c_int = 0x3056;
const EGL_CONTEXT_CLIENT_VERSION: c_int = 0x3098;
const EGL_NONE: c_int = 0x3038;
const EGL_TRUE: u32 = 1;
const EGL_EXTENSIONS: u32 = 0x3055;
// EGL_EXT_image_dma_buf_import (resolved through eglGetProcAddress —
// libglvnd's libEGL exports no extension entry points, verified
// 2026-09-11; the mesa-geminipda ICD answers them).
const EGL_LINUX_DMA_BUF_EXT: u32 = 0x3270;
const EGL_LINUX_DRM_FOURCC_EXT: c_int = 0x3271;
const EGL_DMA_BUF_PLANE0_FD_EXT: c_int = 0x3272;
const EGL_DMA_BUF_PLANE0_OFFSET_EXT: c_int = 0x3273;
const EGL_DMA_BUF_PLANE0_PITCH_EXT: c_int = 0x3274;
const DRM_FORMAT_ABGR8888: u32 = 0x34324241; // 'ABGR'

// The LK framebuffer geometry (geminipda-fb.c receipts, verified on
// glass 2026-08-31 / gemwl 2026-09-01): 1080x2160 portrait, 1088-px
// (4352-byte) row pitch — the trailing 8 px/row are never scanned out.
const FB_W: u32 = 1080;
const FB_H: u32 = 2160;
const FB_PITCH: u32 = 1088 * 4;
// /dev/gemfb ioctl(GEMFB_IOC_EXPORT) -> dma-buf fd of the LK fb region
// (_IOW('G', 1, int); geminipda-fb.c).
const GEMFB_IOC_EXPORT: u64 = 0x4004_4701;

// GL bits used only by the LK-fb compute present.
const GL_TEXTURE_2D: u32 = 0x0DE1;
const GL_NEAREST: u32 = 0x2600;
const GL_TEXTURE_MAG_FILTER: u32 = 0x2800;
const GL_TEXTURE_MIN_FILTER: u32 = 0x2801;
const GL_TEXTURE0: u32 = 0x84C0;
const GL_TEXTURE1: u32 = 0x84C1;
const GL_FRAMEBUFFER: u32 = 0x8D40;
const GL_COMPUTE_SHADER: u32 = 0x91B9;
const GL_WRITE_ONLY: u32 = 0x88B9;
const GL_RGBA8: u32 = 0x8058;
const GL_FRAMEBUFFER_BARRIER_BIT: u32 = 0x00000400;
const GL_COMPILE_STATUS: u32 = 0x8B81;
const GL_LINK_STATUS: u32 = 0x8B82;

extern "C" {
    fn glGenTextures(n: c_int, t: *mut u32);
    fn glBindTexture(t: u32, tex: u32);
    fn glTexParameteri(t: u32, p: u32, v: c_int);
    fn glCreateShader(t: u32) -> u32;
    fn glShaderSource(s: u32, n: c_int, strings: *const *const c_char, lengths: *const c_int);
    fn glCompileShader(s: u32);
    fn glGetShaderiv(s: u32, p: u32, v: *mut c_int);
    fn glGetShaderInfoLog(s: u32, max: c_int, len: *mut c_int, log: *mut c_void);
    fn glCreateProgram() -> u32;
    fn glAttachShader(p: u32, s: u32);
    fn glLinkProgram(p: u32);
    fn glGetProgramiv(p: u32, p2: u32, v: *mut c_int);
    fn glGetProgramInfoLog(p: u32, max: c_int, len: *mut c_int, log: *mut c_void);
    fn glDeleteShader(s: u32);
    fn glGetUniformLocation(p: u32, name: *const c_char) -> c_int;
    fn glUseProgram(p: u32);
    fn glUniform1i(l: c_int, v: c_int);
    fn glUniform2i(l: c_int, x: c_int, y: c_int);
    fn glActiveTexture(t: u32);
    fn glBindImageTexture(
        unit: u32,
        tex: u32,
        level: c_int,
        layered: u8,
        layer: i32,
        access: u32,
        format: u32,
    );
    fn glDispatchCompute(x: u32, y: u32, z: u32);
    fn glMemoryBarrier(bits: u32);
    fn glFinish();
}

/// The GPU blit shader: scene FBO texture -> LK fb image (the gemwl
/// GEMFB_COPY_CS, verified on glass 2026-09-01). The `.bgra` swizzle is
/// load-bearing: the LK OVL scans the fb as a8r8g8b8 (byte0 = B), so the
/// imageStore must land B in byte0; texelFetch decodes the GL texture to
/// correct RGBA. (gemwl receipt: without it the whole desktop is R/B
/// swapped.) The COMPUTE path is also load-bearing: fragment draws/blits
/// into the 1088x2160 LINEAR fb target clip to ~1024x1024 on this
/// tiler (gemwl A/B, 2026-09-01).
const COPY_CS: &str = r#"
#version 310 es
layout(local_size_x = 16, local_size_y = 8) in;
layout(rgba8, binding = 0) uniform highp writeonly image2D dst;
layout(binding = 1) uniform highp sampler2D src;
uniform ivec2 fbSize;   // destination (LK fb) size
uniform ivec2 srcSize;  // source (logical scene) size
uniform int rotation;   // 0/90/180/270 (counter-rotation of the panel)
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= fbSize.x || p.y >= fbSize.y) return;
    // Map the fb pixel back to a scene texel (inverse of the panel
    // counter-rotation; see docs/gemshell.md "Orientation").
    ivec2 s;
    if (rotation == 90) {
        s = ivec2(p.y, fbSize.x - 1 - p.x);
    } else if (rotation == 180) {
        s = ivec2(srcSize.x - 1 - p.x, srcSize.y - 1 - p.y);
    } else if (rotation == 270) {
        s = ivec2(fbSize.y - 1 - p.y, p.x);
    } else {
        s = p;
    }
    if (s.x < 0 || s.y < 0 || s.x >= srcSize.x || s.y >= srcSize.y) return;
    // The scene FBO texture is stored BOTTOM-UP (a GL framebuffer's
    // origin is lower-left, so scene top row is the last texel row).
    // texelFetch indexes that storage directly, so flip the scene Y.
    // Omitting this turned the intended 90-degree rotation into a
    // transpose — the on-glass left-right mirror (2026-09-12).
    ivec2 t = ivec2(s.x, srcSize.y - 1 - s.y);
    imageStore(dst, p, texelFetch(src, t, 0).bgra);
}
"#;

/// Open /dev/gemfb and export the LK framebuffer as a dma-buf fd
/// (GEMFB_IOC_EXPORT; geminipda-fb.c — the gemwl access path).
fn open_gemfb() -> Result<c_int, String> {
    let path = CString::new("/dev/gemfb").unwrap();
    let fd = unsafe { libc::open(path.as_ptr(), libc::O_RDWR | libc::O_CLOEXEC) };
    if fd < 0 {
        return Err(format!(
            "open /dev/gemfb: {}",
            std::io::Error::last_os_error()
        ));
    }
    // The driver RETURNS the dma-buf fd as the ioctl result (it ignores
    // the arg; geminipda-fb.c gemfb_ioctl + gemwl.c:1500
    // `int dma_fd = ioctl(gfd, GEMFB_IOC_EXPORT)`). The old code expected
    // the fd in the pointer arg, so it treated the positive return as an
    // error and reported a stale errno (ENOENT from the render-node
    // fallback opens).
    let dma_fd = unsafe { libc::ioctl(fd, GEMFB_IOC_EXPORT as _) };
    unsafe { libc::close(fd) };
    if dma_fd < 0 {
        return Err(format!(
            "GEMFB_IOC_EXPORT: {}",
            std::io::Error::last_os_error()
        ));
    }
    log::info!("gemfb: LK fb dma-buf exported (fd {dma_fd})");
    Ok(dma_fd)
}

/// The EGL_EXT_image_dma_buf_import attribute list for the LK fb
/// (ABGR8888 at the 1088-px pitch — the gemwl import attrs).
fn fb_image_attrs(fb_fd: c_int) -> [c_int; 13] {
    [
        EGL_WIDTH,
        FB_W as c_int,
        EGL_HEIGHT,
        FB_H as c_int,
        EGL_LINUX_DRM_FOURCC_EXT,
        DRM_FORMAT_ABGR8888 as c_int,
        EGL_DMA_BUF_PLANE0_FD_EXT,
        fb_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,
        0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT,
        FB_PITCH as c_int,
        EGL_NONE,
    ]
}

/// An EGL display + context, kept alive for the whole session (the
/// renderer's GL objects belong to it). Dropping it tears EGL down.
struct EglContext {
    display: EGLDisplay,
    context: EGLContext,
}

impl EglContext {
    /// Create a GLES 3 context on the GBM platform for `gbm_device`.
    fn new(gbm_device: *mut c_void) -> Result<Self, String> {
        let display = unsafe { platform_display(EGL_PLATFORM_GBM_MESA, gbm_device) };
        if display.is_null() {
            return Err(format!(
                "eglGetPlatformDisplay(GBM) failed (error 0x{:x})",
                unsafe { eglGetError() }
            ));
        }
        // The config: ES3-renderable; the context is created surfaceless
        // (makeCurrent with EGL_NO_SURFACE, legal in EGL 1.5) because we
        // render into a GL FBO. At one point EGL_SURFACE_TYPE/
        // EGL_WINDOW_BIT were requested too; the GBM platform has no
        // window/pbuffer surface type and that was the 0x3004
        // (EGL_BAD_MATCH) from eglChooseConfig (on glass, 2026-09-11),
        // so only renderable_type is requested.
        let attrs = [EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE];
        let (mut maj, mut min) = (0i32, 0i32);
        if unsafe { eglInitialize(display, &mut maj, &mut min) } != EGL_TRUE {
            return Err(format!("eglInitialize failed (error 0x{:x})", unsafe {
                eglGetError()
            }));
        }
        log::info!("EGL {maj}.{min}");
        // Vendor + API strings — distinguish the libglvnd STUB display
        // (no ICD loaded) from a real ICD display (diagnostic, 2026-09-11).
        let v = egl_string(display, 0x3053);
        let a = egl_string(display, 0x3054);
        log::info!("EGL vendor: {v} — api: {a}");
        let dev0 = egl_string(display, EGL_EXTENSIONS);
        log::info!(
            "EGL extensions: {}",
            if dev0.len() > 400 {
                format!("{}...", &dev0[..400])
            } else {
                dev0
            }
        );
        if unsafe { eglBindAPI(EGL_OPENGL_ES_API) } != EGL_TRUE {
            return Err("eglBindAPI(ES) failed".into());
        }
        // Diagnostic (added 2026-09-11, the 0x3004 hunt): how many
        // configs does this display have AT ALL, and what do they say?
        // (eglGetConfigAttrib is NOT exported by libglvnd's libEGL —
        // runtime-resolved, like the other EGL ext entry points. Note the
        // real name is Attr, not Attribute.)
        type GetConfigAttrFn =
            Option<unsafe extern "C" fn(EGLDisplay, *const c_void, c_int, *mut c_int) -> u32>;
        let get_config_attr: GetConfigAttrFn = unsafe {
            std::mem::transmute(eglGetProcAddress(
                b"eglGetConfigAttrib\0".as_ptr() as *const c_char
            ))
        };
        let mut all: [EGLConfig; 64] = unsafe { std::mem::zeroed() };
        let mut nall = 0i32;
        let none = [EGL_NONE];
        let ok_all =
            unsafe { eglChooseConfig(display, none.as_ptr(), all.as_mut_ptr(), 64, &mut nall) };
        log::info!(
            "eglChooseConfig(EGL_NONE): {} (n={nall})",
            if ok_all == EGL_TRUE { "ok" } else { "FAIL" }
        );
        if let Some(get_config_attr) = get_config_attr {
            if ok_all == EGL_TRUE {
                for (i, cfg) in all.iter().take(nall as usize).enumerate() {
                    let (mut rt, mut st, mut r) = (0i32, 0i32, 0i32);
                    unsafe { get_config_attr(display, *cfg, EGL_RENDERABLE_TYPE, &mut rt) };
                    unsafe { get_config_attr(display, *cfg, EGL_SURFACE_TYPE as c_int, &mut st) };
                    unsafe { get_config_attr(display, *cfg, EGL_RED_SIZE, &mut r) };
                    log::info!(
                        "  config[{i}]: renderable_type=0x{rt:x} surface_type=0x{st:x} red={r}"
                    );
                }
            }
        }
        log::info!("selecting config (ES3 renderable + window surface type)...");
        let mut config: EGLConfig = std::ptr::null_mut();
        let mut nconf = 0i32;
        if unsafe {
            eglChooseConfig(
                display,
                attrs.as_ptr(),
                &mut config as *mut EGLConfig,
                1,
                &mut nconf,
            )
        } != EGL_TRUE
            || nconf < 1
        {
            return Err(format!("eglChooseConfig failed (error 0x{:x})", unsafe {
                eglGetError()
            }));
        }
        log::info!("config selected (n={nconf})");
        let ctx_attrs = [EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE];
        let context =
            unsafe { eglCreateContext(display, config, std::ptr::null_mut(), ctx_attrs.as_ptr()) };
        if context.is_null() {
            return Err(format!("eglCreateContext failed (error 0x{:x})", unsafe {
                eglGetError()
            }));
        }
        log::info!("context created");
        // Surfaceless context (no pbuffer on the GBM platform): the FBO
        // is the render target.
        if unsafe { eglMakeCurrent(display, std::ptr::null_mut(), std::ptr::null_mut(), context) }
            != EGL_TRUE
        {
            return Err(format!(
                "eglMakeCurrent(surfaceless) failed (error 0x{:x})",
                unsafe { eglGetError() }
            ));
        }
        log::info!("context current (surfaceless)");
        Ok(EglContext { display, context })
    }
}

impl Drop for EglContext {
    fn drop(&mut self) {
        unsafe {
            eglMakeCurrent(
                self.display,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                std::ptr::null_mut(),
            );
            eglDestroyContext(self.display, self.context);
            eglTerminate(self.display);
        }
    }
}

/// Resolve `eglGetPlatformDisplayEXT` through `eglGetProcAddress`
/// (libglvnd's libEGL exports no extension entry points — verified
/// 2026-09-11 — the vendor ICD answers it) and create a platform
/// display. `native` is the GBM device pointer for the GBM platform.
unsafe fn platform_display(platform: u32, native: *mut c_void) -> EGLDisplay {
    type PlatformDisplayFn = unsafe extern "C" fn(u32, *mut c_void, *const c_int) -> EGLDisplay;
    let f: PlatformDisplayFn = std::mem::transmute(eglGetProcAddress(
        b"eglGetPlatformDisplayEXT\0".as_ptr() as *const c_char,
    ));
    f(platform, native, std::ptr::null())
}

fn egl_string(display: EGLDisplay, name: u32) -> String {
    let s = unsafe { eglQueryString(display, name) };
    if s.is_null() {
        "<null>".to_string()
    } else {
        unsafe { std::ffi::CStr::from_ptr(s) }
            .to_string_lossy()
            .into_owned()
    }
}

// ---------------------------------------------------------------------------
// LK-framebuffer present

/// Compute-blits the scene FBO into the LK framebuffer (the gemwl chain —
/// zero CPU pixel movement, no KMS, no page flip; the panel scans the LK
/// OVL memory directly). Ends with `glFinish`, so on return the panel has
/// the frame.
struct LkFbPresenter {
    fb_tex: u32, // texture over the imported dma-buf (imageStore target)
    cprog: u32,
    c_src: c_int,
    c_size: c_int,
    c_srcsize: c_int,
    c_rot: c_int,
    /// Present rotation into the LK fb, degrees (0/90/180/270). The LK fb
    /// is a PORTRAIT 1080x2160 buffer while the product is a landscape
    /// clamshell; gemwl uses WL_OUTPUT_TRANSFORM_90 for the same reason.
    /// Default 270 (changed from 90 on glass 2026-09-12: 90 rendered the
    /// scene 180° out). MUST match `GEMSHELL_TOUCH_ROTATE` in input.rs.
    /// Override with GEMSHELL_ROTATE for on-glass calibration.
    rotation: i32,
}

impl LkFbPresenter {
    /// Import the LK fb dma-buf as an EGLImage + texture and build the
    /// compute copy program (context must be current).
    fn new(egl: &EglContext) -> Result<Self, String> {
        // The extension entry points (runtime-resolved). The ICD must
        // have EGL_EXT_image_dma_buf_import + GL_OES_EGL_image — without
        // them the whole GPU-direct present path is dead.
        let exts = egl_string(egl.display, EGL_EXTENSIONS);
        if !exts.contains("EGL_EXT_image_dma_buf_import") {
            log::warn!(
                "ICD lacks EGL_EXT_image_dma_buf_import (EGL extensions: {exts}) — the GPU-direct present will fail"
            );
        }
        let egl_create_image: EglCreateImageFn = unsafe {
            std::mem::transmute(eglGetProcAddress(
                b"eglCreateImageKHR\0".as_ptr() as *const c_char
            ))
        };
        let egl_image_target_texture: GlEglImageTargetFn = unsafe {
            std::mem::transmute(eglGetProcAddress(
                b"glEGLImageTargetTexture2DOES\0".as_ptr() as *const c_char,
            ))
        };

        let fb_fd = open_gemfb()?;
        log::info!("gemfb opened (fd={fb_fd})");
        let attrs = fb_image_attrs(fb_fd);
        let fb_image = unsafe {
            egl_create_image(
                egl.display,
                std::ptr::null_mut(),
                EGL_LINUX_DMA_BUF_EXT,
                std::ptr::null(),
                attrs.as_ptr(),
            )
        };
        if fb_image.is_null() {
            return Err(format!(
                "eglCreateImageKHR(LK fb dma-buf) failed (error 0x{:x}) — is the ICD's EGL_EXT_image_dma_buf_import available?",
                unsafe { eglGetError() }
            ));
        }
        log::info!("LK fb dma-buf imported as EGLImage");

        let mut fb_tex = 0u32;
        unsafe {
            glGenTextures(1, &mut fb_tex);
            glBindTexture(GL_TEXTURE_2D, fb_tex);
            egl_image_target_texture(GL_TEXTURE_2D, fb_image);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST as c_int);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST as c_int);
        }
        log::info!("LK fb image bound to texture {fb_tex}");

        // The compute copy program.
        let cs = unsafe { glCreateShader(GL_COMPUTE_SHADER) };
        let src = CString::new(COPY_CS).unwrap();
        unsafe {
            // Array-of-pointers (see build_program).
            let sp = src.as_ptr();
            glShaderSource(cs, 1, &sp, std::ptr::null());
            glCompileShader(cs);
        }
        let mut ok = 0i32;
        unsafe { glGetShaderiv(cs, GL_COMPILE_STATUS, &mut ok) };
        if ok == 0 {
            let mut log = vec![0u8; 512];
            let mut l = 0i32;
            unsafe {
                glGetShaderInfoLog(cs, 512, &mut l, log.as_mut_ptr() as *mut c_void);
            }
            let msg = String::from_utf8_lossy(&log[..l.max(0) as usize]).into_owned();
            return Err(format!("copy compute shader: {msg}"));
        }
        let p = unsafe { glCreateProgram() };
        unsafe {
            glAttachShader(p, cs);
            glLinkProgram(p);
        }
        unsafe { glGetProgramiv(p, GL_LINK_STATUS, &mut ok) };
        if ok == 0 {
            let mut log = vec![0u8; 512];
            let mut l = 0i32;
            unsafe {
                glGetProgramInfoLog(p, 512, &mut l, log.as_mut_ptr() as *mut c_void);
            }
            let msg = String::from_utf8_lossy(&log[..l.max(0) as usize]).into_owned();
            return Err(format!("copy compute program: {msg}"));
        }
        unsafe { glDeleteShader(cs) };
        let loc =
            |name: &str| unsafe { glGetUniformLocation(p, CString::new(name).unwrap().as_ptr()) };
        log::info!("GPU-direct present target ready");
        Ok(LkFbPresenter {
            fb_tex,
            cprog: p,
            c_src: loc("src"),
            c_size: loc("fbSize"),
            c_srcsize: loc("srcSize"),
            c_rot: loc("rotation"),
            rotation: std::env::var("GEMSHELL_ROTATE")
                .ok()
                .and_then(|s| s.parse().ok())
                .unwrap_or(270),
        })
    }
}

impl Presenter for LkFbPresenter {
    fn present(&mut self, _scene_fbo: u32, scene_tex: u32, w: u32, h: u32) -> Result<(), String> {
        unsafe {
            glUseProgram(self.cprog);
            glUniform1i(self.c_src, 1);
            glUniform2i(self.c_size, FB_W as c_int, FB_H as c_int);
            glUniform2i(self.c_srcsize, w as c_int, h as c_int);
            glUniform1i(self.c_rot, self.rotation);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, scene_tex);
            glBindImageTexture(0, self.fb_tex, 0, 0, 0, GL_WRITE_ONLY, GL_RGBA8);
            glDispatchCompute((FB_W + 15) / 16, (FB_H + 7) / 8, 1);
            glMemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT);
            glFinish();
            glBindImageTexture(0, 0, 0, 0, 0, GL_WRITE_ONLY, GL_RGBA8);
            glActiveTexture(GL_TEXTURE0);
        }
        Ok(())
    }
}

// ---------------------------------------------------------------------------
// Backend

pub struct LinuxBackend {
    comp: Compositor,
    display: Display<Compositor>,
    socket: ListeningSocket,
    nested: Option<Nested>,
    gbm: Option<Gbm>,
    /// Kept alive for the renderer's GL objects.
    _egl: EglContext,
}

impl LinuxBackend {
    /// Build the Linux backend: GL context + renderer + input source,
    /// then the compositor. `nested` selects the x86_64 Wayland-client
    /// dev loop (`GEMSHELL_NESTED=1`).
    pub fn new(nested: bool) -> Result<Self, String> {
        let font = load_font()?;
        let glyph_size = font.w;

        // GBM device on the PANFROST RENDER NODE (renderD128) — headless
        // rendering; the LK panel is driven by the compute blit into the
        // /dev/gemfb LK framebuffer, NOT by KMS on card0 (the gemwl chain,
        // gemwl.c header + docs/gemshell.md). card0 (geminipda-drm) is
        // left alone. Nested mode uses the same node.
        let gbm = Gbm::new("/dev/dri/renderD128")?;
        let egl = EglContext::new(gbm.ptr)?;

        let mut renderer = Renderer::build(Glsl::Es300, W, H, &font.pixels, glyph_size)?;
        if !nested {
            renderer.set_presenter(Box::new(LkFbPresenter::new(&egl)?));
        }

        let input: Box<dyn InputSource> = if nested {
            // x86_64 dev: no evdev, no /dev/gemfb — headless GL on the
            // HOST render node and a parent Wayland connection.
            Box::new(Input::new_virtual()?)
        } else {
            let (kbd_opt, touch_opt, kbd_name, touch_name) = input::find_nodes();
            let (kbd_path, touch_path) = match (kbd_opt, touch_opt) {
                (Some(k), Some(t)) => (k, t),
                _ => return Err("no keyboard/touch evdev node (need `input` group)".to_string()),
            };
            Box::new(Input::open(&kbd_path, &touch_path, &kbd_name, &touch_name)?)
        };

        let nested_client = if nested {
            Some(Nested::new(W, H)?)
        } else {
            None
        };

        let (display, comp) = Compositor::new(renderer, input, font, nested)?;
        Ok(LinuxBackend {
            comp,
            display,
            socket: bind_socket(nested)?,
            nested: nested_client,
            gbm: Some(gbm),
            _egl: egl,
        })
    }

    fn run_loop(self) -> i32 {
        let LinuxBackend {
            mut comp,
            mut display,
            socket,
            mut nested,
            mut gbm,
            _egl,
        } = self;

        // Nested: the host compositor already owns `wayland-0`, so the
        // sockets must not collide — our clients use wayland-gemshell.
        let sock_name = socket
            .socket_name()
            .map(|s| s.to_string_lossy().into_owned())
            .unwrap_or_else(|| {
                if nested.is_some() {
                    "wayland-gemshell".into()
                } else {
                    "wayland-0".into()
                }
            });
        log::info!("wayland socket: {sock_name}");
        // Children spawned from the launcher inherit this and therefore
        // connect to US, not the host compositor.
        std::env::set_var("WAYLAND_DISPLAY", &sock_name);

        comp.startup();

        let wl_fd = display.as_fd().as_raw_fd();
        let sock_fd = socket.as_raw_fd();
        let mut pfd: Vec<libc::pollfd> = vec![pollfd(wl_fd, libc::POLLIN)];
        let parent_idx = nested.as_ref().map(|n| {
            pfd.push(pollfd(n.fd(), libc::POLLIN));
            pfd.len() - 1
        });
        let gbm_idx = gbm.as_ref().map(|g| {
            pfd.push(pollfd(g.fd, libc::POLLIN));
            pfd.len() - 1
        });
        let (kbd_idx, touch_idx) = if nested.is_none() {
            let (kbd_fd, touch_fd) = comp.input_fds();
            pfd.push(pollfd(kbd_fd, libc::POLLIN));
            let k = pfd.len() - 1;
            pfd.push(pollfd(touch_fd, libc::POLLIN));
            (Some(k), Some(pfd.len() - 1))
        } else {
            (None, None)
        };
        pfd.push(pollfd(sock_fd, libc::POLLIN));
        let sock_idx = pfd.len() - 1;

        loop {
            let timeout = comp.poll_timeout_ms();
            let rc = unsafe {
                libc::poll(
                    pfd.as_mut_ptr(),
                    pfd.len() as libc::nfds_t,
                    timeout as libc::c_int,
                )
            };
            if rc < 0 && std::io::Error::last_os_error().raw_os_error() != Some(libc::EINTR) {
                log::error!("poll: {}", std::io::Error::last_os_error());
                return 1;
            }

            comp.drain_background();

            if pfd[0].revents & libc::POLLIN != 0 {
                if let Err(e) = display.dispatch_clients(&mut comp) {
                    log::error!("dispatch: {e}");
                }
                let _ = display.flush_clients();
            }

            // Nested: drain host input and forward it into our seat.
            if let Some(pi) = parent_idx {
                if pfd[pi].revents & libc::POLLIN != 0 {
                    let (ww, wh) = nested.as_ref().map(|n| n.size()).unwrap_or((W, H));
                    let events = if let Some(n) = nested.as_mut() {
                        n.pump();
                        n.take_input()
                    } else {
                        Vec::new()
                    };
                    for ev in events {
                        match ev {
                            NestedInput::Key { code, pressed } => {
                                comp.feed_evdev_key(code, pressed)
                            }
                            NestedInput::PointerDown { x, y } => {
                                let (sx, sy) = scale_pt(x, y, ww, wh);
                                comp.feed_input(
                                    InputEvent::TouchDown {
                                        id: 0,
                                        x: sx,
                                        y: sy,
                                    },
                                    comp.ui_scale,
                                );
                            }
                            NestedInput::PointerMotion { x, y } => {
                                let (sx, sy) = scale_pt(x, y, ww, wh);
                                comp.feed_input(
                                    InputEvent::TouchMotion {
                                        id: 0,
                                        x: sx,
                                        y: sy,
                                    },
                                    comp.ui_scale,
                                );
                            }
                            NestedInput::PointerUp => {
                                comp.feed_input(InputEvent::TouchUp { id: 0 }, comp.ui_scale);
                            }
                        }
                    }
                }
            }

            if pfd[sock_idx].revents & libc::POLLIN != 0 {
                while let Ok(Some(stream)) = socket.accept() {
                    match display
                        .handle()
                        .insert_client(stream, Arc::new(ClientState::default()))
                    {
                        Ok(client) => {
                            log::info!("client connected");
                            comp.note_client(client);
                            comp.mark_dirty();
                        }
                        Err(e) => {
                            log::error!("client insert: {e}");
                            break;
                        }
                    }
                }
            }

            if let Some(gi) = gbm_idx {
                if pfd[gi].revents & libc::POLLIN != 0 {
                    if let Some(g) = gbm.as_mut() {
                        g.consume_flip();
                    }
                    comp.flip_done();
                }
            }

            if kbd_idx.is_some()
                && (pfd[kbd_idx.unwrap()].revents & libc::POLLIN != 0
                    || pfd[touch_idx.unwrap()].revents & libc::POLLIN != 0)
            {
                // NOTE: `poll` maps touch into the full-resolution scene;
                // `feed_input` converts to the logical layout on the way in.
                comp.poll_input();
            }

            comp.advance();

            if comp.wants_frame() {
                comp.render();
                // Nested: publish the scene FBO to the host window (the
                // device path already presented inside render() via the
                // compute blit into the LK fb).
                if nested.is_some() {
                    let rgba = comp.renderer.read_scene_rgba();
                    let (sw, sh) = (comp.renderer.width, comp.renderer.height);
                    if let Some(n) = nested.as_mut() {
                        n.submit(&rgba, sw, sh);
                    }
                }
                let _ = display.flush_clients();
            }
        }
    }
}

impl Backend for LinuxBackend {
    fn run(self: Box<Self>) -> i32 {
        self.run_loop()
    }
}

fn bind_socket(nested: bool) -> Result<ListeningSocket, String> {
    let name = if nested {
        "wayland-gemshell"
    } else {
        "wayland-0"
    };
    ListeningSocket::bind(name).map_err(|e| format!("wayland socket bind: {e}"))
}

fn load_font() -> Result<font::Font, String> {
    let font_path = util::find_font().ok_or_else(|| "no system TTF font found".to_string())?;
    log::info!("font: {}", font_path.display());
    font::Font::load(
        font_path
            .to_str()
            .ok_or_else(|| "non-unicode font path".to_string())?,
        26.0,
        2048,
    )
    .map_err(|e| format!("font: {e}"))
}

/// Map a point in nested-window pixels to scene coordinates.
fn scale_pt(x: f64, y: f64, win_w: u32, win_h: u32) -> (f32, f32) {
    (
        (x * W as f64 / win_w.max(1) as f64) as f32,
        (y * H as f64 / win_h.max(1) as f64) as f32,
    )
}

fn pollfd(fd: std::os::fd::RawFd, events: libc::c_short) -> libc::pollfd {
    libc::pollfd {
        fd,
        events,
        revents: 0,
    }
}
