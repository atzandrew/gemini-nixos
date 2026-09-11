//! Platform abstraction — the OS-specific half of gemshell.
//!
//! The compositor core (`crate::compositor`) is deliberately platform
//! neutral: it owns the scene, the UI and the renderer, and asks the
//! platform for three things through traits —
//!
//! * [`InputSource`] — where keyboard/touch events come from
//!   (evdev on the device, the host window on a dev workstation);
//! * [`Presenter`](crate::compositor::render::Presenter) — where a
//!   rendered frame goes (the LK framebuffer on the device, a native
//!   window on macOS);
//! * [`Backend`] — the event loop that drives the whole thing.
//!
//! Two implementations ship: [`linux`] (the product: EGL/GBM + evdev +
//! `/dev/gemfb`, x86_64 nested dev loop as a Wayland client) and
//! [`macos`] (a native preview window for UI iteration — no Wayland at
//! all). The single `#[cfg]` fork is [`create_backend`]; everything else
//! is selected through these traits.
//!
//! Design + receipts: docs/gemshell.md ("Platform backends").

use std::os::fd::RawFd;

/// Anonymous shared-memory files (`memfd_create` on Linux, `shm_open`
/// elsewhere) — used for the wl_shm keymap fd and the nested shm buffer.
pub mod shm {
    use std::fs::File;
    use std::io;

    /// Create a fresh anonymous file of `size` bytes.
    #[cfg(target_os = "linux")]
    pub fn anonymous_file(name: &str, size: usize) -> io::Result<File> {
        use std::os::fd::FromRawFd;
        let c = std::ffi::CString::new(name).unwrap();
        let fd = unsafe { libc::memfd_create(c.as_ptr(), libc::MFD_CLOEXEC) };
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }
        let file = unsafe { File::from_raw_fd(fd) };
        file.set_len(size as u64)?;
        Ok(file)
    }

    /// macOS/other: `shm_open` + immediate `shm_unlink` yields an
    /// anonymous fd of the requested size (the POSIX equivalent of
    /// memfd). The name is per-process to avoid collisions.
    #[cfg(not(target_os = "linux"))]
    pub fn anonymous_file(name: &str, size: usize) -> io::Result<File> {
        use std::os::fd::FromRawFd;
        let posix = format!("/gemshell-{name}-{}", std::process::id());
        let c = std::ffi::CString::new(posix.clone()).unwrap();
        let fd = unsafe {
            libc::shm_open(
                c.as_ptr(),
                libc::O_RDWR | libc::O_CREAT | libc::O_EXCL,
                0o600,
            )
        };
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }
        // Unlink immediately: the fd keeps the object alive, nothing is
        // left on disk if we crash.
        unsafe { libc::shm_unlink(c.as_ptr()) };
        let file = unsafe { File::from_raw_fd(fd) };
        file.set_len(size as u64)?;
        Ok(file)
    }
}

// The GLSL dialect enum lives in `crate::compositor::render::Glsl`:
// GLES 3.0 on the device (Mesa/panfrost, and the x86_64 nested build),
// desktop GL 3.3 core for the macOS preview window (which has no GLSL ES
// support at all).

/// A normalised input event, produced by an [`InputSource`] and consumed
/// by `Compositor::feed_input`.
///
/// Some variants are only produced by the evdev backend (the macOS
/// preview feeds keys directly), so allow the platform-specific dead code.
#[allow(dead_code)]
pub enum InputEvent {
    /// A key press/release (or a repeat tick, `pressed` true).
    Key {
        /// the evdev keycode (xkb keycode = this + 8)
        code: u32,
        keysym: u32,
        pressed: bool,
        /// wl-style depressed mods (bit i = modmap slot i)
        mods: u32,
        /// the pressed keysyms (for wl_keyboard.enter)
        pressed_keysyms: Vec<u32>,
        /// set on the first event: the compiled keymap string
        keymap: Option<String>,
    },
    TouchDown {
        id: u32,
        x: f32,
        y: f32,
    },
    TouchUp {
        id: u32,
    },
    TouchMotion {
        id: u32,
        x: f32,
        y: f32,
    },
}

/// Where the compositor's keyboard/touch events come from.
#[allow(dead_code)]
pub trait InputSource {
    /// Drain events, converting touch coordinates into the scene's
    /// full-resolution space (`scene_w`×`scene_h`).
    fn poll(&mut self, scene_w: f32, scene_h: f32) -> Vec<InputEvent>;
    /// Feed one raw evdev scancode and return (keysym, wl mods mask).
    fn process_key(&mut self, code: u32, pressed: bool) -> (u32, u32);
    /// The compiled xkb keymap (for wl_keyboard.keymap), if any.
    fn keymap_string(&self) -> Option<String>;
    /// (depressed, latched, locked) wl mods masks.
    fn mods_masks(&self) -> (u32, u32, u32);
    /// fds to poll for input (evdev); (-1, -1) for event-loop backends.
    fn poll_fds(&self) -> (RawFd, RawFd) {
        (-1, -1)
    }
}

/// A platform event loop that owns the window/display and drives the
/// compositor.
pub trait Backend {
    fn run(self: Box<Self>) -> i32;
}

#[cfg(any(target_os = "linux", gemshell_check_all))]
pub mod linux;
#[cfg(any(target_os = "macos", gemshell_check_all))]
pub mod macos;

/// Build the platform backend. **This is the only OS fork in the whole
/// program** — everything else goes through the traits above.
///
/// `nested` requests the x86_64 Wayland-client dev loop
/// (`GEMSHELL_NESTED=1`); it is ignored on macOS, whose backend is
/// always a native window.
#[cfg(target_os = "macos")]
pub fn create_backend(nested: bool) -> Result<Box<dyn Backend>, String> {
    let _ = nested;
    macos::MacBackend::new().map(|b| Box::new(b) as Box<dyn Backend>)
}

#[cfg(not(target_os = "macos"))]
pub fn create_backend(nested: bool) -> Result<Box<dyn Backend>, String> {
    linux::LinuxBackend::new(nested).map(|b| Box::new(b) as Box<dyn Backend>)
}
