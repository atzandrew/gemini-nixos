//! Nested backend — run gemshell as a Wayland CLIENT under a host
//! compositor (x86_64 development; `GEMSHELL_NESTED=1`).
//!
//! On the PDA gemshell owns the panel directly (the LK framebuffer +
//! compute blit, src/compositor/render.rs). For iterating on the UI on a
//! desktop, this backend presents the SAME scene FBO into an
//! `xdg_toplevel` on the host compositor via `wl_shm`, and forwards the
//! host's pointer/touch/keyboard into the compositor's own seat.
//!
//! This is a development aid, not a product path: it is CPU-readback
//! (glReadPixels + an optional nearest-neighbour downscale), so it is
//! fine for static UI but not for 60 fps demos. `GEMSHELL_NESTED_SCALE`
//! (default 0.5) sets the host window size relative to the logical
//! scene W×H.
//!
//! Design + receipts: docs/gemshell.md ("Nested mode on x86_64").

use wayland_client::globals::{registry_queue_init, GlobalListContents};
use wayland_client::protocol::{
    wl_buffer, wl_compositor, wl_keyboard, wl_pointer, wl_registry, wl_seat, wl_shm, wl_shm_pool,
    wl_surface, wl_touch,
};
use wayland_client::{Connection, Dispatch, EventQueue, QueueHandle, WEnum};
use wayland_protocols::xdg::shell::client::{xdg_surface, xdg_toplevel, xdg_wm_base};

use std::os::fd::{AsFd, AsRawFd};

/// A normalised input event, in WINDOW coordinates (the caller converts
/// to scene coordinates by scaling with scene/window).
#[derive(Debug, Clone, Copy)]
pub enum NestedInput {
    Key { code: u32, pressed: bool },
    PointerDown { x: f64, y: f64 },
    PointerMotion { x: f64, y: f64 },
    PointerUp,
}

pub struct Nested {
    conn: Connection,
    /// `Option` so `pump`/`new` can move it out while dispatching into
    /// `&mut self` (a `queue.field.dispatch(&mut self)` would double-borrow).
    queue: Option<EventQueue<Nested>>,
    surface: wl_surface::WlSurface,
    xdg_surface: xdg_surface::XdgSurface,
    toplevel: xdg_toplevel::XdgToplevel,
    /// kept alive; sub-devices created from its capabilities event
    #[allow(dead_code)]
    seat: Option<wl_seat::WlSeat>,
    buffer: wl_buffer::WlBuffer,
    mmap: memmap2::MmapMut,
    width: u32,
    height: u32,
    configured: bool,
    pointer: Option<wl_pointer::WlPointer>,
    keyboard: Option<wl_keyboard::WlKeyboard>,
    touch: Option<wl_touch::WlTouch>,
    pointer_pos: (f64, f64),
    events: Vec<NestedInput>,
}

impl Nested {
    pub fn new(scene_w: u32, scene_h: u32) -> Result<Self, String> {
        let scale: f64 = std::env::var("GEMSHELL_NESTED_SCALE")
            .ok()
            .and_then(|s| s.parse().ok())
            .unwrap_or(0.5);
        let width = ((scene_w as f64 * scale).round() as u32).max(64);
        let height = ((scene_h as f64 * scale).round() as u32).max(64);

        let conn = Connection::connect_to_env().map_err(|e| format!("connect: {e}"))?;
        let (globals, mut queue) =
            registry_queue_init(&conn).map_err(|e| format!("registry: {e}"))?;
        let qh = queue.handle();

        let compositor = globals
            .bind::<wl_compositor::WlCompositor, _, _>(&qh, 1..=4, ())
            .map_err(|e| format!("wl_compositor: {e}"))?;
        let shm = globals
            .bind::<wl_shm::WlShm, _, _>(&qh, 1..=1, ())
            .map_err(|e| format!("wl_shm: {e}"))?;
        let wm_base = globals
            .bind::<xdg_wm_base::XdgWmBase, _, _>(&qh, 1..=1, ())
            .map_err(|e| format!("xdg_wm_base: {e}"))?;
        // Bind the seat up front: the initial blocking_dispatch below
        // then processes both the first xdg configure and the seat
        // capabilities event that spawns the pointer/keyboard/touch.
        let seat = globals.bind::<wl_seat::WlSeat, _, _>(&qh, 1..=5, ()).ok();

        let surface = compositor.create_surface(&qh, ());
        let xdg_surface = wm_base.get_xdg_surface(&surface, &qh, ());
        let toplevel = xdg_surface.get_toplevel(&qh, ());
        toplevel.set_title("gemshell (nested)".into());
        toplevel.set_app_id("gemshell-nested".into());
        toplevel.set_min_size(320, 160);

        // shm buffer
        let size = (width * height * 4) as usize;
        let file = memfd(size).map_err(|e| format!("memfd: {e}"))?;
        let pool = shm.create_pool(file.as_fd(), size as i32, &qh, ());
        let buffer = pool.create_buffer(
            0,
            width as i32,
            height as i32,
            (width * 4) as i32,
            wl_shm::Format::Argb8888,
            &qh,
            (),
        );
        let mmap = unsafe {
            memmap2::MmapOptions::new()
                .len(size)
                .map_mut(&file)
                .map_err(|e| format!("mmap: {e}"))?
        };
        std::mem::forget(pool);
        std::mem::forget(file);

        let mut n = Nested {
            conn,
            queue: None,
            surface,
            xdg_surface,
            toplevel,
            seat,
            buffer,
            mmap,
            width,
            height,
            configured: false,
            pointer: None,
            keyboard: None,
            touch: None,
            pointer_pos: (0.0, 0.0),
            events: Vec::new(),
        };
        n.surface.commit();
        n.conn.flush().ok();
        // Wait for the first configure + seat capabilities (the queue is
        // still a local here — see the field comment).
        queue
            .blocking_dispatch(&mut n)
            .map_err(|e| format!("initial dispatch: {e}"))?;
        n.conn.flush().ok();
        n.queue = Some(queue);
        Ok(n)
    }

    /// The host fd to poll for parent events.
    pub fn fd(&self) -> i32 {
        self.conn.backend().poll_fd().as_raw_fd()
    }

    /// Process whatever the host sent (call when `fd()` is readable).
    pub fn pump(&mut self) {
        if let Some(guard) = self.conn.prepare_read() {
            let _ = guard.read();
        }
        if let Some(mut q) = self.queue.take() {
            let _ = q.dispatch_pending(self);
            self.queue = Some(q);
        }
        let _ = self.conn.flush();
    }

    pub fn take_input(&mut self) -> Vec<NestedInput> {
        std::mem::take(&mut self.events)
    }

    pub fn configured(&self) -> bool {
        self.configured
    }

    pub fn size(&self) -> (u32, u32) {
        (self.width, self.height)
    }

    /// Publish a scene frame (top-down RGBA8, `sw`×`sh`) into the host
    /// window with a nearest-neighbour downscale.
    pub fn submit(&mut self, rgba: &[u8], sw: u32, sh: u32) {
        let (dw, dh) = (self.width, self.height);
        if sw != dw || sh != dh {
            for y in 0..dh as usize {
                let sy = (y * sh as usize / dh as usize).min(sh as usize - 1);
                for x in 0..dw as usize {
                    let sx = (x * sw as usize / dw as usize).min(sw as usize - 1);
                    let s = (sy * sw as usize + sx) * 4;
                    let d = (y * dw as usize + x) * 4;
                    // wl_shm ARGB8888 little-endian = [B, G, R, A]
                    self.mmap[d] = rgba[s + 2];
                    self.mmap[d + 1] = rgba[s + 1];
                    self.mmap[d + 2] = rgba[s];
                    self.mmap[d + 3] = 255;
                }
            }
        } else {
            for i in 0..(dw * dh) as usize {
                self.mmap[i * 4] = rgba[i * 4 + 2];
                self.mmap[i * 4 + 1] = rgba[i * 4 + 1];
                self.mmap[i * 4 + 2] = rgba[i * 4];
                self.mmap[i * 4 + 3] = 255;
            }
        }
        self.surface.attach(Some(&self.buffer), 0, 0);
        self.surface.damage(0, 0, dw as i32, dh as i32);
        self.surface.commit();
        let _ = self.conn.flush();
    }
}

fn memfd(size: usize) -> Result<std::fs::File, std::io::Error> {
    // Platform shm helper: memfd_create on Linux, shm_open elsewhere
    // (keeps this module compiling on the macOS dev host).
    crate::platform::shm::anonymous_file("gemshell-nested-shm", size)
}

// ---------------------------------------------------------------------------
// dispatch

impl Dispatch<wl_registry::WlRegistry, GlobalListContents> for Nested {
    fn event(
        _state: &mut Self,
        _: &wl_registry::WlRegistry,
        _: wl_registry::Event,
        _: &GlobalListContents,
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

macro_rules! empty_dispatch {
    ($iface:ty) => {
        impl Dispatch<$iface, ()> for Nested {
            fn event(
                _state: &mut Self,
                _: &$iface,
                _: <$iface as wayland_client::Proxy>::Event,
                _: &(),
                _: &Connection,
                _: &QueueHandle<Self>,
            ) {
            }
        }
    };
}

empty_dispatch!(wl_compositor::WlCompositor);
empty_dispatch!(wl_shm::WlShm);
empty_dispatch!(wl_shm_pool::WlShmPool);
empty_dispatch!(wl_buffer::WlBuffer);
empty_dispatch!(wl_surface::WlSurface);

impl Dispatch<wl_seat::WlSeat, ()> for Nested {
    fn event(
        state: &mut Self,
        seat: &wl_seat::WlSeat,
        event: wl_seat::Event,
        _: &(),
        _: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        if let wl_seat::Event::Capabilities { capabilities } = event {
            let WEnum::Value(caps) = capabilities else {
                return;
            };
            if caps.contains(wl_seat::Capability::Pointer) && state.pointer.is_none() {
                state.pointer = Some(seat.get_pointer(qh, ()));
            }
            if caps.contains(wl_seat::Capability::Keyboard) && state.keyboard.is_none() {
                state.keyboard = Some(seat.get_keyboard(qh, ()));
            }
            if caps.contains(wl_seat::Capability::Touch) && state.touch.is_none() {
                state.touch = Some(seat.get_touch(qh, ()));
            }
        }
    }
}

impl Dispatch<wl_pointer::WlPointer, ()> for Nested {
    fn event(
        state: &mut Self,
        _: &wl_pointer::WlPointer,
        event: wl_pointer::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            wl_pointer::Event::Motion {
                surface_x,
                surface_y,
                ..
            } => {
                state.pointer_pos = (surface_x, surface_y);
                state.events.push(NestedInput::PointerMotion {
                    x: surface_x,
                    y: surface_y,
                });
            }
            wl_pointer::Event::Button {
                button, state: bs, ..
            } => {
                let WEnum::Value(bs) = bs else { return };
                // BTN_LEFT = 0x110
                if button == 0x110 {
                    match bs {
                        wl_pointer::ButtonState::Pressed => {
                            state.events.push(NestedInput::PointerDown {
                                x: state.pointer_pos.0,
                                y: state.pointer_pos.1,
                            });
                        }
                        wl_pointer::ButtonState::Released => {
                            state.events.push(NestedInput::PointerUp);
                        }
                        _ => {}
                    }
                }
            }
            _ => {}
        }
    }
}

impl Dispatch<wl_touch::WlTouch, ()> for Nested {
    fn event(
        state: &mut Self,
        _: &wl_touch::WlTouch,
        event: wl_touch::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            wl_touch::Event::Down { x, y, .. } => {
                state.events.push(NestedInput::PointerDown { x, y });
            }
            wl_touch::Event::Motion { x, y, .. } => {
                state.events.push(NestedInput::PointerMotion { x, y });
            }
            wl_touch::Event::Up { .. } => state.events.push(NestedInput::PointerUp),
            _ => {}
        }
    }
}

impl Dispatch<wl_keyboard::WlKeyboard, ()> for Nested {
    fn event(
        state: &mut Self,
        _: &wl_keyboard::WlKeyboard,
        event: wl_keyboard::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let wl_keyboard::Event::Key { key, state: ks, .. } = event {
            let WEnum::Value(ks) = ks else { return };
            let pressed = matches!(ks, wl_keyboard::KeyState::Pressed);
            state.events.push(NestedInput::Key { code: key, pressed });
        }
    }
}

impl Dispatch<xdg_wm_base::XdgWmBase, ()> for Nested {
    fn event(
        _state: &mut Self,
        wm: &xdg_wm_base::XdgWmBase,
        event: xdg_wm_base::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let xdg_wm_base::Event::Ping { serial } = event {
            wm.pong(serial);
        }
    }
}

impl Dispatch<xdg_surface::XdgSurface, ()> for Nested {
    fn event(
        state: &mut Self,
        surf: &xdg_surface::XdgSurface,
        event: xdg_surface::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let xdg_surface::Event::Configure { serial } = event {
            surf.ack_configure(serial);
            state.configured = true;
        }
    }
}

impl Dispatch<xdg_toplevel::XdgToplevel, ()> for Nested {
    fn event(
        _state: &mut Self,
        _: &xdg_toplevel::XdgToplevel,
        _: xdg_toplevel::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}
