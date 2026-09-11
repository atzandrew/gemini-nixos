//! GL rendering — EGL (Mesa, via libglvnd) on the gbm device, GLES.
//!
//! Immediate-mode quad drawing: one program (textured quad, y-down
//! pixel coords), every draw call renders one shape (a quad, or a fan
//! of quads for circles/arcs/lines). ~100 small draw calls per frame —
//! trivial for the T880.
//!
//! - solid shapes use a 4x4 white texture, straight-alpha blend
//! - window content: straight RGBA (converted from SHM XRGB/ARGB)
//! - glyph atlas: premultiplied white (r=g=b=a=coverage)
//! - icons: premultiplied RGBA
//!
//! The blend function is switched per category (premultiplied shapes
//! use (ONE, ONE_MINUS_SRC_ALPHA); straight uses (SRC_ALPHA,
//! ONE_MINUS_SRC_ALPHA)).

use std::collections::HashMap;
use std::ffi::CString;
use std::os::raw::{c_char, c_int, c_void};

// The EGL/GBM setup, the LK-framebuffer import and the compute present
// path are DEVICE-side concerns and live in the Linux backend
// (`crate::platform::linux`). This module is platform neutral: it renders
// into the scene FBO and hands the frame to a [`Presenter`].
//
// GL
const GL_FRAMEBUFFER: u32 = 0x8D40;
const GL_COLOR_ATTACHMENT0: u32 = 0x8CE0;
const GL_TEXTURE0: u32 = 0x84C0;
// GL_BLEND 0x0BE0 was hallucinated (is 0x0BE2 — 0xBE0 is GL_BLEND_DST,
// so glEnable silently failed and NOTHING alpha-blended; re-audited
// against the Khronos headers 2026-09-11).

// GL enums
const GL_FLOAT: u32 = 0x1406;
const GL_UNSIGNED_BYTE: u32 = 0x1401;
const GL_TEXTURE_2D: u32 = 0x0DE1;
const GL_RGBA: u32 = 0x1908;
const GL_NEAREST: u32 = 0x2600;
const GL_LINEAR: u32 = 0x2601;
const GL_TEXTURE_MAG_FILTER: u32 = 0x2800;
const GL_TEXTURE_MIN_FILTER: u32 = 0x2801;
const GL_TEXTURE_WRAP_S: u32 = 0x2802;
const GL_TEXTURE_WRAP_T: u32 = 0x2803;
const GL_CLAMP_TO_EDGE: u32 = 0x812F;
const GL_BLEND: u32 = 0x0BE2;
const GL_ONE: u32 = 1;
const GL_ONE_MINUS_SRC_ALPHA: u32 = 0x0303;
const GL_SRC_ALPHA: u32 = 0x0302;
const GL_DEPTH_TEST: u32 = 0x0B71;
const GL_CULL_FACE: u32 = 0x0B44;
const GL_COLOR_BUFFER_BIT: u32 = 0x4000;
const GL_ARRAY_BUFFER: u32 = 0x8892;
const GL_DYNAMIC_DRAW: u32 = 0x88E8;
const GL_TRIANGLE_STRIP: u32 = 5;
const GL_VERTEX_SHADER: u32 = 0x8B31;
const GL_FRAGMENT_SHADER: u32 = 0x8B30;
const GL_COMPILE_STATUS: u32 = 0x8B81;
const GL_LINK_STATUS: u32 = 0x8B82;
const GL_VERSION: u32 = 0x1F02;
// egui mesh drawing (glDrawElements + scissor + premultiplied blend)
const GL_ELEMENT_ARRAY_BUFFER: u32 = 0x8893;
const GL_TRIANGLES: u32 = 0x0004;
const GL_UNSIGNED_INT: u32 = 0x1405;
const GL_SCISSOR_TEST: u32 = 0x0C11;
const GL_ONE_MINUS_DST_ALPHA: u32 = 0x0305;
const GL_BLEND_EQUATION: u32 = 0x8009;
const GL_FUNC_ADD: u32 = 0x8006;

extern "C" {
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
    fn glUseProgram(p: u32);
    fn glGetUniformLocation(p: u32, name: *const c_char) -> c_int;
    fn glUniform2f(l: c_int, x: f32, y: f32);
    fn glUniform4f(l: c_int, r: f32, g: f32, b: f32, a: f32);
    fn glGenTextures(n: c_int, t: *mut u32);
    fn glDeleteTextures(n: c_int, t: *const u32);
    fn glBindTexture(t: u32, tex: u32);
    fn glTexImage2D(
        t: u32,
        level: c_int,
        internal: c_int,
        w: c_int,
        h: c_int,
        border: c_int,
        format: u32,
        ty: u32,
        data: *const c_void,
    );
    fn glTexSubImage2D(
        t: u32,
        level: c_int,
        x: c_int,
        y: c_int,
        w: c_int,
        h: c_int,
        format: u32,
        ty: u32,
        data: *const c_void,
    );
    fn glTexParameteri(t: u32, p: u32, v: c_int);
    fn glGenBuffers(n: c_int, b: *mut u32);
    fn glBindBuffer(t: u32, b: u32);
    fn glBufferData(t: u32, size: i64, data: *const c_void, usage: u32);
    fn glVertexAttribPointer(
        i: c_int,
        size: c_int,
        ty: u32,
        normalized: u32,
        stride: u32,
        offset: u32,
    );
    fn glEnableVertexAttribArray(i: c_int);
    fn glGetAttribLocation(p: u32, name: *const c_char) -> c_int;
    fn glDeleteBuffers(n: c_int, b: *const u32);
    fn glDrawArrays(mode: u32, first: c_int, count: c_int);
    fn glClearColor(r: f32, g: f32, b: f32, a: f32);
    fn glClear(mask: u32);
    fn glEnable(cap: u32);
    fn glDisable(cap: u32);
    fn glBlendFunc(s: u32, d: u32);
    fn glViewport(x: c_int, y: c_int, w: c_int, h: c_int);
    fn glGetString(name: u32) -> *const c_char;
    // FBO (the scene render target) + vertex-array state.
    fn glGenFramebuffers(n: c_int, f: *mut u32);
    fn glBindFramebuffer(t: u32, f: u32);
    fn glFramebufferTexture2D(t: u32, a: u32, tt: u32, tex: u32, level: c_int);
    fn glActiveTexture(t: u32);
    fn glFinish();
    fn glUniform1i(l: c_int, v: c_int);
    fn glDeleteFramebuffers(n: c_int, f: *const u32);
    fn glDeleteShader(s: u32);
    fn glDeleteProgram(p: u32);
    // Core-profile VAOs (the macOS desktop-GL path needs one; GLES 3.0
    // tolerates the implicit VAO 0 that the device path has always used).
    fn glGenVertexArrays(n: c_int, a: *mut u32);
    fn glBindVertexArray(a: u32);
    fn glDeleteVertexArrays(n: c_int, a: *const u32);
    // Framebuffer blit — the macOS window present (scene FBO -> default
    // framebuffer). GL 3.0 / GLES 3.0 core, so also available on device.
    fn glBlitFramebuffer(
        sx0: c_int,
        sy0: c_int,
        sx1: c_int,
        sy1: c_int,
        dx0: c_int,
        dy0: c_int,
        dx1: c_int,
        dy1: c_int,
        mask: u32,
        filter: u32,
    );
    fn glReadPixels(
        x: c_int,
        y: c_int,
        w: c_int,
        h: c_int,
        format: u32,
        ty: u32,
        data: *mut c_void,
    );
    // egui meshes: indexed triangles + clip rects
    fn glDrawElements(mode: u32, count: c_int, ty: u32, indices: *const c_void);
    fn glScissor(x: c_int, y: c_int, w: c_int, h: c_int);
    fn glBlendEquation(mode: u32);
    fn glBlendFuncSeparate(sr: u32, dr: u32, sa: u32, da: u32);
}

// ---------------------------------------------------------------------------
// GL dialect
// ---------------------------------------------------------------------------

/// The GLSL dialect a platform targets. The device (Mesa/panfrost) and
/// the x86_64 Wayland-nested build speak **GLSL ES 3.0**; the macOS
/// preview window uses **desktop GL 3.3 core**, because macOS has no
/// GLES and its CGL context runs a core profile.
///
/// Chosen by the platform backend and threaded into [`Renderer::build`],
/// so the shader sources are selected by data, not by `#[cfg]`.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Glsl {
    /// GLSL ES 3.00 — attribute/varying, `gl_FragColor`.
    Es300,
    /// GLSL 3.30 core — in/out, explicit fragment output, VAO required.
    Core330,
}

/// The renderer's two programs (four shaders).
pub enum Shader {
    MainVert,
    MainFrag,
    EguiVert,
    EguiFrag,
}

impl Glsl {
    /// Core profiles have no default VAO; a VAO must be bound before any
    /// vertex-attribute setup. GLES 3.0 also requires one in spec, but
    /// every driver the device path has run on tolerates VAO 0, so we
    /// only pay for it where it is mandatory.
    pub fn needs_vao(self) -> bool {
        matches!(self, Glsl::Core330)
    }

    /// The source for one of the renderer's shaders.
    pub fn source(self, shader: Shader) -> &'static str {
        match (self, shader) {
            (Glsl::Es300, Shader::MainVert) => VERT_ES,
            (Glsl::Es300, Shader::MainFrag) => FRAG_ES,
            (Glsl::Es300, Shader::EguiVert) => EGUI_VERT_ES,
            (Glsl::Es300, Shader::EguiFrag) => EGUI_FRAG_ES,
            (Glsl::Core330, Shader::MainVert) => VERT_CORE,
            (Glsl::Core330, Shader::MainFrag) => FRAG_CORE,
            (Glsl::Core330, Shader::EguiVert) => EGUI_VERT_CORE,
            (Glsl::Core330, Shader::EguiFrag) => EGUI_FRAG_CORE,
        }
    }
}

// --- shaders: GLSL ES 3.00 (device + x86_64 nested) ----------------------

/// egui mesh shader: pixel-space triangles -> NDC (Y flipped, scene is
/// top-left origin), vertex colour (egui's premultiplied sRGBA, 0-255
/// bytes normalized by the VS) times the sampled texture.
const EGUI_VERT_ES: &str = r#"
attribute vec2 aPos;
attribute vec2 aUv;
attribute vec4 aColor;     // 0-255; GL normalizes when normalized=0
uniform vec2 uRes;         // scene size in pixels
varying vec2 vUv;
varying vec4 vColor;
void main() {
    vec2 ndc = (aPos / uRes) * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
    vUv = aUv;
    vColor = aColor / 255.0;
}
"#;

const EGUI_FRAG_ES: &str = r#"
precision mediump float;
varying vec2 vUv;
varying vec4 vColor;
uniform sampler2D uTex;
void main() {
    gl_FragColor = vColor * texture2D(uTex, vUv);
}
"#;

const VERT_ES: &str = r#"
attribute vec2 aPos;
attribute vec2 aUv;
uniform vec2 uRes;
varying vec2 vUv;
void main() {
    vec2 ndc = (aPos / uRes) * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
    vUv = aUv;
}
"#;

const FRAG_ES: &str = r#"
precision mediump float;
varying vec2 vUv;
uniform sampler2D uTex;
uniform vec4 uColor;
void main() {
    gl_FragColor = texture2D(uTex, vUv) * uColor;
}
"#;

// --- shaders: desktop GLSL 3.30 core (the macOS preview window) ---------
//
// Same maths as the ES variants, translated for a core profile: no
// `precision` qualifiers, `attribute`/`varying` -> `in`/`out`, an
// explicit fragment output instead of `gl_FragColor`.

const EGUI_VERT_CORE: &str = r#"#version 330 core
in vec2 aPos;
in vec2 aUv;
in vec4 aColor;            // 0-255; normalized=0, divided in the shader
uniform vec2 uRes;
out vec2 vUv;
out vec4 vColor;
void main() {
    vec2 ndc = (aPos / uRes) * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
    vUv = aUv;
    vColor = aColor / 255.0;
}
"#;

const EGUI_FRAG_CORE: &str = r#"#version 330 core
in vec2 vUv;
in vec4 vColor;
uniform sampler2D uTex;
out vec4 fragColor;
void main() {
    fragColor = vColor * texture(uTex, vUv);
}
"#;

const VERT_CORE: &str = r#"#version 330 core
in vec2 aPos;
in vec2 aUv;
uniform vec2 uRes;
out vec2 vUv;
void main() {
    vec2 ndc = (aPos / uRes) * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
    vUv = aUv;
}
"#;

const FRAG_CORE: &str = r#"#version 330 core
in vec2 vUv;
uniform sampler2D uTex;
uniform vec4 uColor;
out vec4 fragColor;
void main() {
    fragColor = texture(uTex, vUv) * uColor;
}
"#;

/// An rgba color, 0..=1.
#[derive(Clone, Copy, Debug)]
pub struct Color(pub [f32; 4]);

impl Color {
    pub const fn rgba(r: f32, g: f32, b: f32, a: f32) -> Self {
        Color([r, g, b, a])
    }
}

/// pos.xy + uv.xy per vertex, y-down pixel coordinates.
#[derive(Clone, Copy)]
struct Vert {
    x: f32,
    y: f32,
    u: f32,
    v: f32,
}

impl Vert {
    fn as_f32(&self) -> [f32; 4] {
        [self.x, self.y, self.u, self.v]
    }
}

/// A recorded draw operation (the compositor records a frame into a
/// Vec<Op> while immutably borrowing window state, then replays it).
#[derive(Clone)]
pub enum Op {
    Rect {
        x: f32,
        y: f32,
        w: f32,
        h: f32,
        c: Color,
    },
    RectTex {
        x: f32,
        y: f32,
        w: f32,
        h: f32,
        u0: f32,
        v0: f32,
        u1: f32,
        v1: f32,
        tex: u32,
        c: Color,
        premult: bool,
    },
    Circle {
        cx: f32,
        cy: f32,
        r: f32,
        c: Color,
    },
    Arc {
        cx: f32,
        cy: f32,
        r0: f32,
        r1: f32,
        a0: f32,
        a1: f32,
        c: Color,
    },
    Line {
        x0: f32,
        y0: f32,
        x1: f32,
        y1: f32,
        t: f32,
        c: Color,
    },
    Text {
        x: f32,
        baseline: f32,
        s: String,
        c: Color,
    },
    TextCentered {
        x: f32,
        w: f32,
        baseline: f32,
        s: String,
        c: Color,
    },
    TextClipped {
        x: f32,
        w: f32,
        baseline: f32,
        s: String,
        c: Color,
    },
}

/// Where a rendered scene frame goes.
///
/// The device presents by compute-blitting the scene FBO into the LK
/// framebuffer (`platform::linux`); the macOS preview window blits it to
/// the window's default framebuffer (`platform::macos`). The renderer
/// only knows the trait.
pub trait Presenter {
    /// `scene_fbo`/`scene_tex` are the scene render target, `w`×`h` its
    /// size in physical pixels.
    fn present(&mut self, scene_fbo: u32, scene_tex: u32, w: u32, h: u32) -> Result<(), String>;
    /// The output surface changed size (window resize).
    fn resize(&mut self, _width: u32, _height: u32) {}
}

pub struct Renderer {
    pub width: u32,
    pub height: u32,
    /// UI scale (1.0 / 1.5 / 2.0). The compositor lays out in LOGICAL
    /// units (`width/ui_scale` × `height/ui_scale`) and this shader maps
    /// them across the full physical viewport. See `Compositor::ui_scale`.
    pub ui_scale: f32,
    /// The GLSL dialect this context speaks (chosen by the platform).
    glsl: Glsl,
    /// Core-profile vertex-array object (0 on GLES, where VAO 0 works).
    vao: u32,
    program: u32,
    a_pos: c_int,
    a_uv: c_int,
    u_res: c_int,
    u_color: c_int,
    vbo: u32,
    white_tex: u32,
    pub glyph_tex: u32,
    /// window id -> (texture, width, height)
    win_tex: HashMap<u32, (u32, u32, u32)>,
    verts: Vec<Vert>,
    colors: Vec<Color>,
    /// The scene render target (fullscreen RGBA8). Every backend renders
    /// into this; the [`Presenter`] decides where it lands.
    fbo: u32,
    fbo_tex: u32,
    /// The platform's present target (LK fb / window).
    presenter: Option<Box<dyn Presenter>>,
    // egui mesh drawing (the shell UI): one program + an element buffer;
    // texture deltas are uploaded into `egui_textures`.
    egui_prog: u32,
    egui_a_pos: c_int,
    egui_a_uv: c_int,
    egui_a_color: c_int,
    egui_u_res: c_int,
    egui_u_tex: c_int,
    egui_ebo: u32,
    egui_textures: HashMap<egui::TextureId, u32>,
}

impl Renderer {
    /// Build the renderer's GL objects (programs, textures, scene FBO).
    ///
    /// The caller must already have a **current GL context** whose GLSL
    /// dialect is `glsl` — the Linux backend creates it with EGL on the
    /// GBM/surfaceless platform, the macOS backend with glutin/CGL. The
    /// GL entry points themselves resolve at link time (the `gl*`
    /// externs below are linked against libGLESv2/the OpenGL framework),
    /// so there is nothing to load here.
    pub fn build(
        glsl: Glsl,
        width: u32,
        height: u32,
        glyph_pixels: &[u8],
        glyph_size: u32,
    ) -> Result<Self, String> {
        let version = unsafe { glGetString(GL_VERSION) };
        let version = if version.is_null() {
            "unknown".to_string()
        } else {
            // GL owns this string until the next call — do NOT from_raw
            unsafe { std::ffi::CStr::from_ptr(version) }
                .to_string_lossy()
                .into_owned()
        };
        log::info!("GL: {version} ({glsl:?})");

        let program = build_program(glsl.source(Shader::MainVert), glsl.source(Shader::MainFrag))?;
        log::info!("main program built");
        // ATTRIBUTES need glGetAttribLocation, uniforms glGetUniformLocation
        // — the first cut used the uniform query for aPos/aUv too, so both
        // came back -1, every glEnableVertexAttribArray(-1) raised
        // GL_INVALID_VALUE and NO geometry was drawn (the scene stayed the
        // clear colour; found via the GEMSHELL_SCREENSHOT readback
        // 2026-09-11).
        let attr = |name: &str| unsafe {
            glGetAttribLocation(program, CString::new(name).unwrap().as_ptr())
        };
        let loc = |name: &str| unsafe {
            glGetUniformLocation(program, CString::new(name).unwrap().as_ptr())
        };
        let a_pos = attr("aPos");
        let a_uv = attr("aUv");
        let u_res = loc("uRes");
        let u_color = loc("uColor");

        // egui program + element buffer (built here so a shader error
        // surfaces at startup, not on the first settings tap).
        let egui_prog =
            build_program(glsl.source(Shader::EguiVert), glsl.source(Shader::EguiFrag))?;
        let egui_attr = |name: &str| unsafe {
            glGetAttribLocation(egui_prog, CString::new(name).unwrap().as_ptr())
        };
        let egui_uniform = |name: &str| unsafe {
            glGetUniformLocation(egui_prog, CString::new(name).unwrap().as_ptr())
        };
        let egui_a_pos = egui_attr("aPos");
        let egui_a_uv = egui_attr("aUv");
        let egui_a_color = egui_attr("aColor");
        let egui_u_res = egui_uniform("uRes");
        let egui_u_tex = egui_uniform("uTex");
        let mut egui_ebo = 0u32;
        unsafe { glGenBuffers(1, &mut egui_ebo) };
        log::info!("egui program built (aPos={egui_a_pos} aUv={egui_a_uv} aColor={egui_a_color})");

        let mut vbo = 0u32;
        unsafe { glGenBuffers(1, &mut vbo) };

        // A core profile has no default VAO — create and bind one before
        // any attribute setup (the macOS path). GLES tolerates VAO 0.
        let mut vao = 0u32;
        if glsl.needs_vao() {
            unsafe {
                glGenVertexArrays(1, &mut vao);
                glBindVertexArray(vao);
            }
        }

        let mut r = Renderer {
            width,
            height,
            ui_scale: 1.0,
            glsl,
            vao,
            program,
            a_pos,
            a_uv,
            u_res,
            u_color,
            vbo,
            white_tex: 0,
            glyph_tex: 0,
            win_tex: HashMap::new(),
            verts: Vec::new(),
            colors: Vec::new(),
            fbo: 0,
            fbo_tex: 0,
            presenter: None,
            egui_prog,
            egui_a_pos,
            egui_a_uv,
            egui_a_color,
            egui_u_res,
            egui_u_tex,
            egui_ebo,
            egui_textures: HashMap::new(),
        };

        let white = [255u8; 16 * 4];
        r.white_tex = r.make_texture(4, 4, &white)?;
        if !glyph_pixels.is_empty() {
            r.glyph_tex = r.make_texture(glyph_size, glyph_size, glyph_pixels)?;
        }
        log::info!("base textures created (glyph {glyph_size}px)");

        // The scene FBO is needed by EVERY backend (device + nested).
        r.init_scene_fbo()?;
        log::info!("scene FBO ready ({}x{})", r.width, r.height);
        Ok(r)
    }

    /// Install the platform's present target.
    pub fn set_presenter(&mut self, presenter: Box<dyn Presenter>) {
        self.presenter = Some(presenter);
    }

    /// Forward an output-surface resize to the presenter.
    pub fn resize_presenter(&mut self, width: u32, height: u32) {
        if let Some(p) = self.presenter.as_mut() {
            p.resize(width, height);
        }
    }

    /// The scene FBO (fullscreen RGBA8 render target), shared by the
    /// device and nested backends (context must be current).
    fn init_scene_fbo(&mut self) -> Result<(), String> {
        // The scene FBO: a plain RGBA8 fullscreen texture. (gemwl's shadow
        // is a panfrost dma-buf BO because wlroots' swapchain needs a
        // wlr_buffer; a GL texture is the equivalent for our compute
        // texelFetch source.)
        let mut fbo_tex = 0u32;
        let mut fbo = 0u32;
        unsafe {
            glGenTextures(1, &mut fbo_tex);
            glBindTexture(GL_TEXTURE_2D, fbo_tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST as c_int);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST as c_int);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE as c_int);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE as c_int);
            glTexImage2D(
                GL_TEXTURE_2D,
                0,
                GL_RGBA as c_int,
                self.width as c_int,
                self.height as c_int,
                0,
                GL_RGBA,
                GL_UNSIGNED_BYTE,
                std::ptr::null(),
            );
            glGenFramebuffers(1, &mut fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(
                GL_FRAMEBUFFER,
                GL_COLOR_ATTACHMENT0,
                GL_TEXTURE_2D,
                fbo_tex,
                0,
            );
        }
        self.fbo_tex = fbo_tex;
        self.fbo = fbo;

        // T880 tiler first-batch bug (gemwl receipt, 2026-09-02): the
        // FIRST tiler draw into a fresh fullscreen-size target rasterizes
        // only ~1024x1024 — warm the FBO so the first real frame is
        // clean. Panfrost/GLES only; the desktop-GL preview host has no
        // such tiler (and the warmup shader is GLSL ES).
        if self.glsl == Glsl::Es300 {
            tiler_warmup(self.fbo, self.width, self.height);
        }
        Ok(())
    }

    /// Read the scene FBO back as top-down RGBA8 (the nested present;
    /// same pixels `screenshot` writes, without the PNG).
    #[allow(dead_code)] // the Linux nested backend is its only caller
    pub fn read_scene_rgba(&self) -> Vec<u8> {
        let (w, h) = (self.width as usize, self.height as usize);
        let mut buf = vec![0u8; w * h * 4];
        unsafe {
            glBindFramebuffer(GL_FRAMEBUFFER, self.fbo);
            glReadPixels(
                0,
                0,
                self.width as c_int,
                self.height as c_int,
                GL_RGBA,
                GL_UNSIGNED_BYTE,
                buf.as_mut_ptr() as *mut c_void,
            );
        }
        // glReadPixels returns rows bottom-up; flip to top-down.
        let mut flipped = vec![0u8; buf.len()];
        for y in 0..h {
            let src = (h - 1 - y) * w * 4;
            flipped[y * w * 4..(y + 1) * w * 4].copy_from_slice(&buf[src..src + w * 4]);
        }
        flipped
    }

    fn make_texture(&mut self, w: u32, h: u32, rgba8: &[u8]) -> Result<u32, String> {
        let mut tex = 0u32;
        unsafe {
            glGenTextures(1, &mut tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST as c_int);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST as c_int);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE as c_int);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE as c_int);
            glTexImage2D(
                GL_TEXTURE_2D,
                0,
                GL_RGBA as c_int,
                w as c_int,
                h as c_int,
                0,
                GL_RGBA,
                GL_UNSIGNED_BYTE,
                rgba8.as_ptr() as *const c_void,
            );
        }
        Ok(tex)
    }

    /// (Re)create a window content texture from an SHM buffer
    /// (XRGB8888 or ARGB8888, little-endian byte order) and return the
    /// texture id.
    pub fn window_texture(
        &mut self,
        id: u32,
        w: u32,
        h: u32,
        stride: u32,
        format: u32,
        data: &[u8],
    ) -> u32 {
        let mut rgba = vec![0u8; (w * h * 4) as usize];
        for y in 0..h {
            for x in 0..w {
                let s = (y * stride + x * 4) as usize;
                let d = ((y * w + x) * 4) as usize;
                // wl_shm XRGB8888/ARGB8888 memory layout (little-endian)
                // is [B, G, R, X/A] — build a standard [R, G, B, A]
                let b = data[s];
                let g = data[s + 1];
                let r = data[s + 2];
                let a = if format == 0x34325258 {
                    255
                } else {
                    data[s + 3]
                }; // XRGB8888
                rgba[d] = r;
                rgba[d + 1] = g;
                rgba[d + 2] = b;
                rgba[d + 3] = a;
            }
        }
        match self.win_tex.get(&id) {
            Some(&(tex, tw, th)) if tw == w && th == h => {
                unsafe {
                    glBindTexture(GL_TEXTURE_2D, tex);
                    glTexSubImage2D(
                        GL_TEXTURE_2D,
                        0,
                        0,
                        0,
                        w as c_int,
                        h as c_int,
                        GL_RGBA,
                        GL_UNSIGNED_BYTE,
                        rgba.as_ptr() as *const c_void,
                    );
                }
                tex
            }
            _ => {
                if let Some(&(tex, _, _)) = self.win_tex.get(&id) {
                    unsafe { glDeleteTextures(1, &tex) };
                }
                let tex = match self.make_texture(w, h, &rgba) {
                    Ok(t) => t,
                    Err(e) => {
                        log::error!("window texture: {e}");
                        return self.white_tex;
                    }
                };
                self.win_tex.insert(id, (tex, w, h));
                tex
            }
        }
    }

    pub fn drop_window_texture(&mut self, id: u32) {
        if let Some((tex, _, _)) = self.win_tex.remove(&id) {
            unsafe { glDeleteTextures(1, &tex) };
        }
    }

    // ---------- immediate drawing ----------

    fn frame_setup(&mut self) {
        unsafe {
            glViewport(0, 0, self.width as c_int, self.height as c_int);
            glBindFramebuffer(GL_FRAMEBUFFER, self.fbo);
            glClearColor(0.055, 0.07, 0.085, 1.0);
            glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glEnable(GL_BLEND);
            glUseProgram(self.program);
            // Core profile: a VAO must be bound for the attribute setup
            // below to be recorded (macOS). GLES tolerates VAO 0.
            if self.glsl.needs_vao() {
                glBindVertexArray(self.vao);
            }
            let s = if self.ui_scale > 0.01 {
                self.ui_scale
            } else {
                1.0
            };
            glUniform2f(self.u_res, self.width as f32 / s, self.height as f32 / s);
            glBindBuffer(GL_ARRAY_BUFFER, self.vbo);
            glEnableVertexAttribArray(self.a_pos);
            glEnableVertexAttribArray(self.a_uv);
            glVertexAttribPointer(self.a_pos, 2, GL_FLOAT, 0, 16, 0);
            glVertexAttribPointer(self.a_uv, 2, GL_FLOAT, 0, 16, 8);
        }
    }

    fn draw(&mut self, tex: u32, c: Color, premult: bool) {
        if self.verts.is_empty() {
            return;
        }
        unsafe {
            glBlendFunc(
                if premult { GL_ONE } else { GL_SRC_ALPHA },
                GL_ONE_MINUS_SRC_ALPHA,
            );
            glBindTexture(GL_TEXTURE_2D, tex);
            glUniform4f(self.u_color, c.0[0], c.0[1], c.0[2], c.0[3]);
            let bytes: Vec<f32> = self.verts.iter().flat_map(|v: &Vert| v.as_f32()).collect();
            glBufferData(
                GL_ARRAY_BUFFER,
                (bytes.len() * 4) as i64,
                bytes.as_ptr() as *const c_void,
                GL_DYNAMIC_DRAW,
            );
            glDrawArrays(GL_TRIANGLE_STRIP, 0, self.verts.len() as c_int);
        }
        self.verts.clear();
    }

    fn quad(&mut self, x: f32, y: f32, w: f32, h: f32, u0: f32, v0: f32, u1: f32, v1: f32) {
        self.verts.extend([
            Vert { x, y, u: u0, v: v0 },
            Vert {
                x: x + w,
                y,
                u: u1,
                v: v0,
            },
            Vert {
                x,
                y: y + h,
                u: u0,
                v: v1,
            },
            Vert {
                x: x + w,
                y: y + h,
                u: u1,
                v: v1,
            },
        ]);
    }

    /// Solid rectangle (straight alpha).
    pub fn rect(&mut self, x: f32, y: f32, w: f32, h: f32, c: Color) {
        self.quad(x, y, w, h, 0.0, 0.0, 1.0, 1.0);
        self.draw(self.white_tex, c, false);
    }

    /// Textured rectangle (glyphs: premultiplied; window content:
    /// straight). `premult` selects the blend mode.
    pub fn rect_tex(
        &mut self,
        x: f32,
        y: f32,
        w: f32,
        h: f32,
        u0: f32,
        v0: f32,
        u1: f32,
        v1: f32,
        tex: u32,
        c: Color,
        premult: bool,
    ) {
        self.quad(x, y, w, h, u0, v0, u1, v1);
        self.draw(tex, c, premult);
    }

    /// Filled circle (fan of quads, straight alpha).
    pub fn circle(&mut self, cx: f32, cy: f32, r: f32, c: Color) {
        const SEG: usize = 24;
        for i in 0..SEG {
            let a0 = i as f32 / SEG as f32 * std::f32::consts::TAU;
            let a1 = (i + 1) as f32 / SEG as f32 * std::f32::consts::TAU;
            let p0 = (cx + r * a0.cos(), cy + r * a0.sin());
            let p1 = (cx + r * a1.cos(), cy + r * a1.sin());
            self.verts.extend([
                Vert {
                    x: cx,
                    y: cy,
                    u: 0.0,
                    v: 0.0,
                },
                Vert {
                    x: p0.0,
                    y: p0.1,
                    u: 0.0,
                    v: 0.0,
                },
                Vert {
                    x: cx,
                    y: cy,
                    u: 0.0,
                    v: 0.0,
                },
                Vert {
                    x: p1.0,
                    y: p1.1,
                    u: 0.0,
                    v: 0.0,
                },
            ]);
        }
        self.draw(self.white_tex, c, false);
    }

    /// Annular arc (ring segment), straight alpha. Angles in radians
    /// (y-down: 0 = right, PI/2 = down).
    pub fn arc(&mut self, cx: f32, cy: f32, r0: f32, r1: f32, a0: f32, a1: f32, c: Color) {
        const SEG: usize = 10;
        for i in 0..SEG {
            let t0 = a0 + (a1 - a0) * i as f32 / SEG as f32;
            let t1 = a0 + (a1 - a0) * (i + 1) as f32 / SEG as f32;
            let p = |a: f32, r: f32| (cx + r * a.cos(), cy + r * a.sin());
            let (q0o, q1o, q1i, q0i) = (p(t0, r0), p(t1, r0), p(t1, r1), p(t0, r1));
            self.verts.extend([
                Vert {
                    x: q0o.0,
                    y: q0o.1,
                    u: 0.0,
                    v: 0.0,
                },
                Vert {
                    x: q1o.0,
                    y: q1o.1,
                    u: 0.0,
                    v: 0.0,
                },
                Vert {
                    x: q0i.0,
                    y: q0i.1,
                    u: 0.0,
                    v: 0.0,
                },
                Vert {
                    x: q1i.0,
                    y: q1i.1,
                    u: 0.0,
                    v: 0.0,
                },
            ]);
        }
        self.draw(self.white_tex, c, false);
    }

    /// Thick line (a quad along the segment), straight alpha.
    pub fn line(&mut self, x0: f32, y0: f32, x1: f32, y1: f32, t: f32, c: Color) {
        let dx = x1 - x0;
        let dy = y1 - y0;
        let len = (dx * dx + dy * dy).sqrt().max(1e-6);
        let nx = -dy / len * t * 0.5;
        let ny = dx / len * t * 0.5;
        self.verts.extend([
            Vert {
                x: x0 + nx,
                y: y0 + ny,
                u: 0.0,
                v: 0.0,
            },
            Vert {
                x: x1 + nx,
                y: y1 + ny,
                u: 0.0,
                v: 0.0,
            },
            Vert {
                x: x0 - nx,
                y: y0 - ny,
                u: 0.0,
                v: 0.0,
            },
            Vert {
                x: x1 - nx,
                y: y1 - ny,
                u: 0.0,
                v: 0.0,
            },
        ]);
        self.draw(self.white_tex, c, false);
    }

    /// Draw `s` at pen (x = left, baseline = y) in color c.
    pub fn text(
        &mut self,
        font: &crate::common::font::Font,
        x: f32,
        baseline: f32,
        s: &str,
        c: Color,
    ) {
        let mut pen = x;
        for ch in s.chars() {
            if let Some(g) = font.glyph(ch) {
                let top = baseline - g.y_top;
                self.rect_tex(
                    pen + g.x_off,
                    top,
                    g.w as f32 / crate::common::font::SUP,
                    g.h as f32 / crate::common::font::SUP,
                    g.u0,
                    g.v0,
                    g.u1,
                    g.v1,
                    self.glyph_tex,
                    c,
                    true,
                );
            }
            pen += font
                .glyph(ch)
                .map(|g| g.advance)
                .unwrap_or_else(|| font.size * 0.6);
        }
    }

    /// Centered text inside [x, x+w], baseline at y.
    pub fn text_centered(
        &mut self,
        font: &crate::common::font::Font,
        x: f32,
        w: f32,
        baseline: f32,
        s: &str,
        c: Color,
    ) {
        let tw = font.text_width(s);
        self.text(font, x + (w - tw) / 2.0, baseline, s, c);
    }

    /// Text clipped to [x, x+w] (left aligned) — for window titles.
    pub fn text_clipped(
        &mut self,
        font: &crate::common::font::Font,
        x: f32,
        w: f32,
        baseline: f32,
        s: &str,
        c: Color,
    ) {
        let mut pen = x;
        for ch in s.chars() {
            let Some(g) = font.glyph(ch) else { break };
            let gx = pen + g.x_off;
            if gx >= x + w {
                break;
            }
            let gw = (g.w as f32 / crate::common::font::SUP).min(x + w - gx);
            let top = baseline - g.y_top;
            self.rect_tex(
                gx,
                top,
                gw,
                g.h as f32 / crate::common::font::SUP,
                g.u0,
                g.v0,
                g.u0 + (g.u1 - g.u0) * gw / (g.w as f32 / crate::common::font::SUP),
                g.v1,
                self.glyph_tex,
                c,
                true,
            );
            pen += g.advance;
            if pen > x + w {
                break;
            }
        }
    }

    /// Begin the frame (bind the scene FBO, viewport + clear).
    pub fn begin_frame(&mut self) {
        unsafe { glBindFramebuffer(GL_FRAMEBUFFER, self.fbo) };
        self.frame_setup();
    }

    /// Replay recorded ops.
    pub fn replay(&mut self, font: &crate::common::font::Font, ops: &[Op]) {
        for op in ops {
            match op {
                Op::Rect { x, y, w, h, c } => self.rect(*x, *y, *w, *h, *c),
                Op::RectTex {
                    x,
                    y,
                    w,
                    h,
                    u0,
                    v0,
                    u1,
                    v1,
                    tex,
                    c,
                    premult,
                } => self.rect_tex(*x, *y, *w, *h, *u0, *v0, *u1, *v1, *tex, *c, *premult),
                Op::Circle { cx, cy, r, c } => self.circle(*cx, *cy, *r, *c),
                Op::Arc {
                    cx,
                    cy,
                    r0,
                    r1,
                    a0,
                    a1,
                    c,
                } => self.arc(*cx, *cy, *r0, *r1, *a0, *a1, *c),
                Op::Line {
                    x0,
                    y0,
                    x1,
                    y1,
                    t,
                    c,
                } => self.line(*x0, *y0, *x1, *y1, *t, *c),
                Op::Text { x, baseline, s, c } => self.text(font, *x, *baseline, s, *c),
                Op::TextCentered {
                    x,
                    w,
                    baseline,
                    s,
                    c,
                } => self.text_centered(font, *x, *w, *baseline, s, *c),
                Op::TextClipped {
                    x,
                    w,
                    baseline,
                    s,
                    c,
                } => self.text_clipped(font, *x, *w, *baseline, s, *c),
            }
        }
    }

    /// Draw the egui shell UI (settings panel) on top of the scene.
    ///
    /// egui emits premultiplied-sRGBA triangle meshes + a font-atlas
    /// texture delta; this uploads the atlas/textures and draws the
    /// meshes with the indexed `EGUI_VERT` shader. All GPU-side: no CPU
    /// rasterization (2026-09-12). Clip rects become GL scissor rects
    /// (note GL's bottom-left origin vs egui's top-left).
    pub fn draw_egui(
        &mut self,
        primitives: &[egui::ClippedPrimitive],
        textures: &egui::TexturesDelta,
        pixels_per_point: f32,
    ) {
        if self.egui_prog == 0 {
            return;
        }
        // egui vertices are in POINTS; the scene is physical pixels.
        let ppp = if pixels_per_point > 0.0 {
            pixels_per_point
        } else {
            1.0
        };
        for id in &textures.free {
            if let Some(tex) = self.egui_textures.remove(id) {
                unsafe { glDeleteTextures(1, &tex) };
            }
        }
        for (id, delta) in &textures.set {
            if let Err(e) = self.egui_upload_texture(*id, delta) {
                log::warn!("egui texture {id:?}: {e}");
            }
        }

        unsafe {
            if self.glsl.needs_vao() {
                glBindVertexArray(self.vao);
            }
            glUseProgram(self.egui_prog);
            glUniform2f(
                self.egui_u_res,
                self.width as f32 / ppp,
                self.height as f32 / ppp,
            );
            glUniform1i(self.egui_u_tex, 0);
            glActiveTexture(GL_TEXTURE0);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glEnable(GL_BLEND);
            glBlendEquation(GL_FUNC_ADD);
            glBlendFuncSeparate(
                GL_ONE,
                GL_ONE_MINUS_SRC_ALPHA,
                GL_ONE_MINUS_DST_ALPHA,
                GL_ONE,
            );
            glEnable(GL_SCISSOR_TEST);
            glBindBuffer(GL_ARRAY_BUFFER, self.vbo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, self.egui_ebo);
            glEnableVertexAttribArray(self.egui_a_pos);
            glVertexAttribPointer(self.egui_a_pos, 2, GL_FLOAT, 0, 20, 0);
            glEnableVertexAttribArray(self.egui_a_uv);
            glVertexAttribPointer(self.egui_a_uv, 2, GL_FLOAT, 0, 20, 8);
            glEnableVertexAttribArray(self.egui_a_color);
            glVertexAttribPointer(self.egui_a_color, 4, GL_UNSIGNED_BYTE, 0, 20, 16);
        }

        for prim in primitives {
            let egui::epaint::Primitive::Mesh(mesh) = &prim.primitive else {
                continue;
            };
            if mesh.indices.is_empty() || mesh.vertices.is_empty() {
                continue;
            }
            let r = prim.clip_rect;
            let x = (r.min.x * ppp).max(0.0) as i32;
            let y = (r.min.y * ppp).max(0.0) as i32;
            let x1 = (r.max.x * ppp).min(self.width as f32).max(0.0) as i32;
            let y1 = (r.max.y * ppp).min(self.height as f32).max(0.0) as i32;
            let (cw, ch) = ((x1 - x).max(0), (y1 - y).max(0));
            if cw == 0 || ch == 0 {
                continue;
            }
            unsafe {
                // GL scissor origin is bottom-left; egui's is top-left.
                glScissor(x, self.height as i32 - (y + ch), cw, ch);
                let tex = self
                    .egui_textures
                    .get(&mesh.texture_id)
                    .copied()
                    .unwrap_or(self.white_tex);
                glBindTexture(GL_TEXTURE_2D, tex);
                glBufferData(
                    GL_ARRAY_BUFFER,
                    (mesh.vertices.len() * std::mem::size_of::<egui::epaint::Vertex>()) as i64,
                    mesh.vertices.as_ptr() as *const c_void,
                    GL_DYNAMIC_DRAW,
                );
                glBufferData(
                    GL_ELEMENT_ARRAY_BUFFER,
                    (mesh.indices.len() * std::mem::size_of::<u32>()) as i64,
                    mesh.indices.as_ptr() as *const c_void,
                    GL_DYNAMIC_DRAW,
                );
                glDrawElements(
                    GL_TRIANGLES,
                    mesh.indices.len() as c_int,
                    GL_UNSIGNED_INT,
                    std::ptr::null(),
                );
            }
        }
        unsafe {
            glDisable(GL_SCISSOR_TEST);
        }
    }

    fn egui_upload_texture(
        &mut self,
        id: egui::TextureId,
        delta: &egui::epaint::ImageDelta,
    ) -> Result<(), String> {
        let [w, h] = delta.image.size();
        // egui only ever emits premultiplied sRGBA; Color32 is [u8;4].
        let mut rgba: Vec<u8> = Vec::with_capacity(w * h * 4);
        match &delta.image {
            egui::epaint::ImageData::Color(img) => {
                for p in &img.pixels {
                    rgba.extend_from_slice(&p.to_array());
                }
            }
            egui::epaint::ImageData::Font(img) => {
                for p in img.srgba_pixels(None) {
                    rgba.extend_from_slice(&p.to_array());
                }
            }
        }
        let ptr = rgba.as_ptr() as *const c_void;
        match delta.pos {
            None => {
                let tex = match self.egui_textures.get(&id) {
                    Some(&t) => {
                        unsafe {
                            glBindTexture(GL_TEXTURE_2D, t);
                            glTexImage2D(
                                GL_TEXTURE_2D,
                                0,
                                GL_RGBA as c_int,
                                w as c_int,
                                h as c_int,
                                0,
                                GL_RGBA,
                                GL_UNSIGNED_BYTE,
                                ptr,
                            );
                        }
                        t
                    }
                    None => self.egui_alloc_texture(w, h, ptr)?,
                };
                self.egui_textures.insert(id, tex);
            }
            Some([x, y]) => {
                let Some(&tex) = self.egui_textures.get(&id) else {
                    return Err("partial update for an unknown texture".into());
                };
                unsafe {
                    glBindTexture(GL_TEXTURE_2D, tex);
                    glTexSubImage2D(
                        GL_TEXTURE_2D,
                        0,
                        x as c_int,
                        y as c_int,
                        w as c_int,
                        h as c_int,
                        GL_RGBA,
                        GL_UNSIGNED_BYTE,
                        ptr,
                    );
                }
            }
        }
        Ok(())
    }

    /// A LINEAR-filtered texture (egui wants smooth font scaling).
    fn egui_alloc_texture(
        &mut self,
        w: usize,
        h: usize,
        data: *const c_void,
    ) -> Result<u32, String> {
        let mut tex = 0u32;
        unsafe {
            glGenTextures(1, &mut tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR as c_int);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR as c_int);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE as c_int);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE as c_int);
            glTexImage2D(
                GL_TEXTURE_2D,
                0,
                GL_RGBA as c_int,
                w as c_int,
                h as c_int,
                0,
                GL_RGBA,
                GL_UNSIGNED_BYTE,
                data,
            );
        }
        Ok(tex)
    }

    /// Public texture creation (app icons).
    pub fn make_texture_pub(&mut self, w: u32, h: u32, rgba8: &[u8]) -> Result<u32, String> {
        self.make_texture(w, h, rgba8)
    }

    /// The window content texture id (for switcher thumbnails).
    pub fn window_texture_id(&self, id: u32) -> Option<u32> {
        self.win_tex.get(&id).map(|t| t.0)
    }

    /// Read the scene FBO back and write it as a PNG (debug/
    /// verification — `GEMSHELL_SCREENSHOT=/path`, see the compositor).
    /// glReadPixels returns rows bottom-up, so flip to top-down for PNG.
    pub fn screenshot(&self, path: &str) -> Result<(), String> {
        let (w, h) = (self.width as usize, self.height as usize);
        let mut buf = vec![0u8; w * h * 4];
        unsafe {
            glBindFramebuffer(GL_FRAMEBUFFER, self.fbo);
            glReadPixels(
                0,
                0,
                self.width as c_int,
                self.height as c_int,
                GL_RGBA,
                GL_UNSIGNED_BYTE,
                buf.as_mut_ptr() as *mut c_void,
            );
        }
        let mut flipped = vec![0u8; buf.len()];
        for y in 0..h {
            let src = (h - 1 - y) * w * 4;
            flipped[y * w * 4..(y + 1) * w * 4].copy_from_slice(&buf[src..src + w * 4]);
        }
        write_png(path, self.width, self.height, &flipped)
    }

    /// Present the frame through the platform's [`Presenter`] (the LK
    /// framebuffer on device, the preview window on macOS). No-op if no
    /// presenter is installed (e.g. a headless screenshot run).
    pub fn present(&mut self) -> Result<(), String> {
        let (fbo, tex, w, h) = (self.fbo, self.fbo_tex, self.width, self.height);
        match self.presenter.as_mut() {
            Some(p) => p.present(fbo, tex, w, h),
            None => Ok(()),
        }
    }
}

/// Blit a scene FBO into the current default framebuffer, scaled to
/// `dw`×`dh` — the window present (macOS). Used by the platform's
/// [`Presenter`] so all GL calls stay in one module.
pub fn blit_to_default_fb(scene_fbo: u32, sw: u32, sh: u32, dw: u32, dh: u32) {
    const GL_READ_FRAMEBUFFER: u32 = 0x8CA8;
    const GL_DRAW_FRAMEBUFFER: u32 = 0x8CA9;
    unsafe {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, scene_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(
            0,
            0,
            sw as c_int,
            sh as c_int,
            0,
            0,
            dw as c_int,
            dh as c_int,
            GL_COLOR_BUFFER_BIT,
            GL_LINEAR,
        );
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}

/// Encode an RGBA8 pixel buffer as a PNG.
fn write_png(path: &str, w: u32, h: u32, rgba: &[u8]) -> Result<(), String> {
    let file = std::fs::File::create(path).map_err(|e| e.to_string())?;
    let mut enc = png::Encoder::new(std::io::BufWriter::new(file), w, h);
    enc.set_color(png::ColorType::Rgba);
    enc.set_depth(png::BitDepth::Eight);
    let mut writer = enc.write_header().map_err(|e| e.to_string())?;
    writer.write_image_data(rgba).map_err(|e| e.to_string())?;
    Ok(())
}

/// T880 tiler warmup (gemwl receipt 2026-09-02): the first tiler batch
/// into a fresh fullscreen-size target clips to ~1024x1024; a
/// throwaway full-frame draw into the FBO makes the first real frame
/// clean.
fn tiler_warmup(fbo: u32, width: u32, height: u32) {
    let wvs =
        CString::new("attribute vec2 pos; void main(){ gl_Position=vec4(pos,0.0,1.0); }").unwrap();
    let wfs = CString::new("precision mediump float; void main(){ gl_FragColor=vec4(0,0,0,1); }")
        .unwrap();
    unsafe {
        let vs = glCreateShader(GL_VERTEX_SHADER);
        // Array-of-pointers (see build_program).
        let vsp = wvs.as_ptr();
        glShaderSource(vs, 1, &vsp, std::ptr::null());
        glCompileShader(vs);
        let fs = glCreateShader(GL_FRAGMENT_SHADER);
        let fsp = wfs.as_ptr();
        glShaderSource(fs, 1, &fsp, std::ptr::null());
        glCompileShader(fs);
        let prog = glCreateProgram();
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        glLinkProgram(prog);
        glDeleteShader(vs);
        glDeleteShader(fs);
        // The attribute NAME, not the shader source (the old code passed
        // wvs.as_ptr() — the whole source string — so the lookup returned
        // -1 and the draw used attrib -1).
        let attr_name = CString::new("pos").unwrap();
        let a_pos = glGetAttribLocation(prog, attr_name.as_ptr());
        // Warm the REAL scene FBO (the exact target the first frame
        // uses) — its attachment stays in place.
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, width as c_int, height as c_int);
        glClearColor(0.0, 0.0, 0.0, 1.0);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(prog);
        let q: [f32; 8] = [-1.0, -1.0, 1.0, -1.0, -1.0, 1.0, 1.0, 1.0];
        let mut vbo = 0u32;
        glGenBuffers(1, &mut vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(
            GL_ARRAY_BUFFER,
            (q.len() * 4) as i64,
            q.as_ptr() as *const c_void,
            GL_DYNAMIC_DRAW,
        );
        glVertexAttribPointer(a_pos, 2, GL_FLOAT, 0, 0, 0);
        glEnableVertexAttribArray(a_pos);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glFinish();
        glDeleteBuffers(1, &vbo);
        glDeleteProgram(prog);
        log::info!("tiler warmup: full-frame draw into the scene FBO done");
    }
}

fn build_program(vert: &str, frag: &str) -> Result<u32, String> {
    unsafe {
        let make = |src: &str, ty: u32| -> Result<u32, String> {
            let s = glCreateShader(ty);
            let c = CString::new(src).unwrap();
            let len = [c.as_bytes().len() as c_int];
            // glShaderSource wants an ARRAY OF POINTERS to the strings
            // (`const GLchar *const*`), not the string pointer itself.
            // Passing `c.as_ptr()` directly made Mesa read the first 8
            // bytes of the shader source as a pointer and dereference it
            // — the on-glass SEGV right after "GL: ..." (2026-09-11).
            let sp = c.as_ptr();
            glShaderSource(s, 1, &sp, len.as_ptr());
            glCompileShader(s);
            let mut ok = 0i32;
            glGetShaderiv(s, GL_COMPILE_STATUS, &mut ok);
            if ok == 0 {
                let mut log = vec![0u8; 512];
                let mut l = 0i32;
                glGetShaderInfoLog(s, 512, &mut l, log.as_mut_ptr() as *mut c_void);
                let msg = String::from_utf8_lossy(&log[..l.max(0) as usize]).into_owned();
                return Err(format!("shader compile: {msg}"));
            }
            Ok(s)
        };
        // NOTE: use the `vert` PARAMETER. A copy-paste made this build
        // every program from the global VERT, so the egui program linked
        // VERT with EGUI_FRAG and failed "fragment input `vColor' has no
        // matching output" (found by the nested run 2026-09-12).
        let vs = make(vert, GL_VERTEX_SHADER)?;
        let fs = make(frag, GL_FRAGMENT_SHADER)?;
        let p = glCreateProgram();
        glAttachShader(p, vs);
        glAttachShader(p, fs);
        glLinkProgram(p);
        let mut ok = 0i32;
        glGetProgramiv(p, GL_LINK_STATUS, &mut ok);
        if ok == 0 {
            let mut log = vec![0u8; 512];
            let mut l = 0i32;
            glGetProgramInfoLog(p, 512, &mut l, log.as_mut_ptr() as *mut c_void);
            return Err(format!(
                "program link: {}",
                String::from_utf8_lossy(&log[..l.max(0) as usize])
            ));
        }
        Ok(p)
    }
}
