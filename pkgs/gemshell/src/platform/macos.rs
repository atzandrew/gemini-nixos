//! macOS backend — a native preview window for the gemshell UI.
//!
//! macOS has **no Wayland** (the protocol libraries, the compositor
//! socket, the whole session model), so the "nested under a host
//! compositor" trick the x86_64 dev loop uses is impossible here. This
//! backend instead gives the compositor a Cocoa window to render into:
//!
//! * a `winit` window + a `glutin`/CGL **desktop OpenGL 3.3 core**
//!   context (macOS ships no GLES; the renderer picks its desktop-GL
//!   shader set via [`Glsl::Core330`]);
//! * the scene FBO is blitted into the window's default framebuffer each
//!   frame (the [`WindowPresenter`]);
//! * window input (mouse + keyboard) is mapped onto the compositor's own
//!   gestures/shortcuts — it is a **UI test harness**, so there are no
//!   Wayland clients to serve.
//!
//! Run it with `bash bin/gemshell-nested.sh` (which builds this for
//! `aarch64-darwin` with the host Rust toolchain and forwards the env
//! knobs below).
//!
//! Env knobs (same names as the Linux nested loop):
//! * `GEMSHELL_NESTED_SCALE` — window size vs the logical scene (0.5);
//! * `GEMSHELL_OPEN_SETTINGS` / `GEMSHELL_OPEN_LAUNCHER` — open a panel
//!   at startup;
//! * `GEMSHELL_SCREENSHOT` / `_DELAY_MS` — write the scene FBO to a PNG.
//!
//! Design + receipts: docs/gemshell.md ("Platform backends").

use std::time::{Duration, Instant};

use glutin::config::ConfigTemplateBuilder;
use glutin::context::{
    ContextApi, ContextAttributesBuilder, GlProfile, PossiblyCurrentContext, Version,
};
use glutin::display::GetGlDisplay;
use glutin::prelude::*;
use glutin::surface::{Surface, WindowSurface};
use glutin_winit::{DisplayBuilder, GlWindow};
use raw_window_handle::HasRawWindowHandle;
use winit::dpi::LogicalSize;
use winit::event::{ElementState, Event, MouseButton, MouseScrollDelta, WindowEvent};
use winit::event_loop::{ControlFlow, EventLoop};
use winit::keyboard::{KeyCode, PhysicalKey};
use winit::window::{Window, WindowBuilder};

use crate::common::{font, util};
use crate::compositor::render::{self, Glsl, Presenter, Renderer};
use crate::compositor::{Compositor, H, W};

use super::{Backend, InputEvent, InputSource};

/// Presents the scene FBO into the window's default framebuffer (a scaled
/// `glBlitFramebuffer`; GL handles the resize).
struct WindowPresenter {
    width: u32,
    height: u32,
}

impl WindowPresenter {
    fn new(width: u32, height: u32) -> Self {
        WindowPresenter {
            width: width.max(1),
            height: height.max(1),
        }
    }
}

impl Presenter for WindowPresenter {
    fn present(&mut self, scene_fbo: u32, _scene_tex: u32, sw: u32, sh: u32) -> Result<(), String> {
        render::blit_to_default_fb(scene_fbo, sw, sh, self.width, self.height);
        Ok(())
    }

    fn resize(&mut self, width: u32, height: u32) {
        self.width = width.max(1);
        self.height = height.max(1);
    }
}

/// The macOS input source. Unlike evdev there are no fds to poll — the
/// winit event loop feeds the compositor directly — so this is a no-op
/// that satisfies [`InputSource`] (keyboard translation is done in
/// [`key_keysym`]).
struct NativeInput;

impl InputSource for NativeInput {
    fn poll(&mut self, _scene_w: f32, _scene_h: f32) -> Vec<InputEvent> {
        Vec::new()
    }

    fn process_key(&mut self, _code: u32, _pressed: bool) -> (u32, u32) {
        (0, 0)
    }

    fn keymap_string(&self) -> Option<String> {
        None
    }

    fn mods_masks(&self) -> (u32, u32, u32) {
        (0, 0, 0)
    }
}

pub struct MacBackend {
    comp: Compositor,
    /// Kept alive so the Wayland resources in `Window` stay valid; there
    /// are no clients on macOS.
    _display: wayland_server::Display<Compositor>,
    event_loop: EventLoop<()>,
    window: Window,
    surface: Surface<WindowSurface>,
    context: PossiblyCurrentContext,
}

impl MacBackend {
    /// Open the preview window, create the GL context and build the
    /// compositor.
    pub fn new() -> Result<Self, String> {
        let scale: f64 = std::env::var("GEMSHELL_NESTED_SCALE")
            .ok()
            .and_then(|s| s.parse().ok())
            .unwrap_or(0.5);
        // Logical points, so a 0.5-scale window is 1080x540 *points*
        // (2160x1080 physical on a Retina display — a 1:1, crisp blit).
        let win_w = ((W as f64 * scale).round() as u32).max(320);
        let win_h = ((H as f64 * scale).round() as u32).max(160);

        let event_loop = EventLoop::new().map_err(|e| format!("event loop: {e}"))?;
        let window_builder = WindowBuilder::new()
            .with_title("gemshell (macOS preview)")
            .with_inner_size(LogicalSize::new(win_w as f64, win_h as f64));
        let template = ConfigTemplateBuilder::new();
        let display_builder = DisplayBuilder::new().with_window_builder(Some(window_builder));
        let (window, gl_config) = display_builder
            .build(&event_loop, template, |configs| {
                configs
                    .reduce(|a, b| {
                        if b.num_samples() > a.num_samples() {
                            b
                        } else {
                            a
                        }
                    })
                    .expect("no GL config")
            })
            .map_err(|e| format!("display: {e}"))?;
        let window = window.ok_or_else(|| "no window created".to_string())?;

        let gl_display = gl_config.display();
        let raw = window.raw_window_handle();
        let ctx_attrs = ContextAttributesBuilder::new()
            .with_context_api(ContextApi::OpenGl(Some(Version::new(3, 3))))
            .with_profile(GlProfile::Core)
            .build(Some(raw));
        let not_current = unsafe {
            gl_display
                .create_context(&gl_config, &ctx_attrs)
                .map_err(|e| format!("gl context: {e}"))?
        };
        let attrs = window.build_surface_attributes(Default::default());
        let surface = unsafe {
            gl_display
                .create_window_surface(&gl_config, &attrs)
                .map_err(|e| format!("gl surface: {e}"))?
        };
        let context = not_current
            .make_current(&surface)
            .map_err(|e| format!("make current: {e}"))?;

        let size = window.inner_size();
        log::info!("window {}x{}", size.width, size.height);

        // Context is current: build the renderer for desktop GLSL.
        let font = load_font()?;
        let glyph_size = font.w;
        let mut renderer = Renderer::build(Glsl::Core330, W, H, &font.pixels, glyph_size)?;
        renderer.set_presenter(Box::new(WindowPresenter::new(size.width, size.height)));

        let (display, comp) = Compositor::new(renderer, Box::new(NativeInput), font, true)?;
        Ok(MacBackend {
            comp,
            _display: display,
            event_loop,
            window,
            surface,
            context,
        })
    }

    fn run_loop(self) -> i32 {
        let MacBackend {
            mut comp,
            _display,
            event_loop,
            window,
            surface,
            context,
        } = self;

        comp.startup();

        // Physical-pixel window size (Retina: 2x the logical size); the
        // scene is mapped to it 1:1 so blits stay crisp.
        let (mut win_w, mut win_h) = {
            let s = window.inner_size();
            (s.width.max(1), s.height.max(1))
        };
        let mut last_pos = (0.0f64, 0.0f64);
        let mut left_down = false;

        let result = event_loop.run(move |event, elwt| {
            match event {
                Event::WindowEvent { event, .. } => match event {
                    WindowEvent::CloseRequested => elwt.exit(),
                    WindowEvent::Resized(size) => {
                        win_w = size.width.max(1);
                        win_h = size.height.max(1);
                        if let (Some(w), Some(h)) = (
                            std::num::NonZeroU32::new(size.width),
                            std::num::NonZeroU32::new(size.height),
                        ) {
                            let _ = surface.resize(&context, w, h);
                        }
                        comp.renderer.resize_presenter(win_w, win_h);
                        comp.mark_dirty();
                    }
                    WindowEvent::RedrawRequested => {
                        if comp.wants_frame() {
                            let _ = context.make_current(&surface);
                            // render() runs the presenter (blit into the
                            // default framebuffer) and updates the frame
                            // pacing.
                            comp.render();
                            let _ = surface.swap_buffers(&context);
                        }
                    }
                    WindowEvent::CursorMoved { position, .. } => {
                        last_pos = (position.x, position.y);
                        let (sx, sy) = to_scene(position.x, position.y, win_w, win_h);
                        let iscale = if comp.ui_scale > 0.01 {
                            comp.ui_scale
                        } else {
                            1.0
                        };
                        if left_down {
                            comp.feed_input(
                                InputEvent::TouchMotion {
                                    id: 0,
                                    x: sx,
                                    y: sy,
                                },
                                iscale,
                            );
                        } else {
                            comp.pointer_hover(sx / iscale, sy / iscale);
                        }
                    }
                    WindowEvent::MouseInput { state, button, .. } => {
                        if button == MouseButton::Left {
                            let (sx, sy) = to_scene(last_pos.0, last_pos.1, win_w, win_h);
                            let iscale = if comp.ui_scale > 0.01 {
                                comp.ui_scale
                            } else {
                                1.0
                            };
                            match state {
                                ElementState::Pressed => {
                                    left_down = true;
                                    comp.feed_input(
                                        InputEvent::TouchDown {
                                            id: 0,
                                            x: sx,
                                            y: sy,
                                        },
                                        iscale,
                                    );
                                }
                                ElementState::Released => {
                                    left_down = false;
                                    comp.feed_input(InputEvent::TouchUp { id: 0 }, iscale);
                                }
                            }
                        }
                    }
                    WindowEvent::MouseWheel { delta, .. } => {
                        let dy = match delta {
                            MouseScrollDelta::LineDelta(_, y) => y * 48.0,
                            MouseScrollDelta::PixelDelta(p) => p.y as f32,
                        };
                        comp.scroll_panel(dy);
                    }
                    WindowEvent::KeyboardInput { event, .. } => {
                        if let Some(keysym) = key_keysym(&event.physical_key) {
                            let pressed = event.state == ElementState::Pressed;
                            comp.feed_key(0, keysym, pressed, 0);
                        }
                    }
                    _ => {}
                },
                Event::AboutToWait => {
                    comp.drain_background();
                    comp.advance();
                    if comp.wants_frame() {
                        window.request_redraw();
                        elwt.set_control_flow(ControlFlow::Poll);
                    } else {
                        // Background channels (status/clock) cannot wake the
                        // loop, so tick at the pace the compositor asks for.
                        let ms = comp.poll_timeout_ms().max(16) as u64;
                        elwt.set_control_flow(ControlFlow::WaitUntil(
                            Instant::now() + Duration::from_millis(ms),
                        ));
                    }
                }
                _ => {}
            }
        });

        // `_display` is held until here so the Wayland resources outlive
        // the loop (they are never used on macOS).
        drop(_display);
        match result {
            Ok(()) => 0,
            Err(e) => {
                log::error!("event loop: {e}");
                1
            }
        }
    }
}

impl Backend for MacBackend {
    fn run(self: Box<Self>) -> i32 {
        self.run_loop()
    }
}

/// Map window physical pixels to the scene's full-resolution space.
fn to_scene(x: f64, y: f64, win_w: u32, win_h: u32) -> (f32, f32) {
    (
        (x * W as f64 / win_w.max(1) as f64) as f32,
        (y * H as f64 / win_h.max(1) as f64) as f32,
    )
}

/// Map a macOS key to an X11 keysym. There is no xkbcommon here, so the
/// mapping is deliberately small — it covers the shell's own shortcuts
/// (Escape/arrows/media keys, with F1-F8 standing in for the Gemini's Fn
/// layer) and the few keys the settings panel needs.
fn key_keysym(key: &PhysicalKey) -> Option<u32> {
    let PhysicalKey::Code(code) = key else {
        return None;
    };
    let sym = match code {
        KeyCode::Escape => 0xff1b,
        KeyCode::Enter => 0xff0d,
        KeyCode::Backspace => 0xff08,
        KeyCode::Tab => 0xff09,
        KeyCode::Space => 0x20,
        KeyCode::ArrowLeft => 0xff51,
        KeyCode::ArrowUp => 0xff52,
        KeyCode::ArrowRight => 0xff53,
        KeyCode::ArrowDown => 0xff54,
        KeyCode::Home => 0xff50,
        KeyCode::End => 0xff57,
        KeyCode::PageUp => 0xff55,
        KeyCode::PageDown => 0xff56,
        KeyCode::Delete => 0xffff,
        // F1-F8 stand in for the Gemini Fn layer on the preview host:
        // launcher / app switcher / brightness / volume / sleep.
        KeyCode::F1 => 0x1008ff7f, // XF86TaskPane  (Fn+A, launcher)
        KeyCode::F2 => 0x1008ffa2, // XF86TopMenu   (Fn+S, switcher)
        KeyCode::F3 => 0x1008ff03, // brightness down (Fn+B)
        KeyCode::F4 => 0x1008ff02, // brightness up   (Fn+N)
        KeyCode::F5 => 0x1008ff11, // volume down     (Fn+C)
        KeyCode::F6 => 0x1008ff13, // volume up       (Fn+V)
        KeyCode::F7 => 0x1008ff12, // mute            (Fn+T)
        KeyCode::F8 => 0x1008ff2f, // sleep           (Fn+Esc)
        _ => return None,
    };
    Some(sym)
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
