//! The gemshell compositor core — the main loop, window management,
//! gestures and input. See docs/gemshell.md for the design.

pub mod data;
pub mod render;
pub mod status;
pub mod ui;
pub mod wayland;
// Device/nested-only modules: raw evdev input, the GBM device, and the
// x86_64 Wayland-client present path. They are compiled by the Linux
// backend (`crate::platform::linux`).
#[cfg(any(target_os = "linux", gemshell_check_all))]
pub mod gbm;
#[cfg(any(target_os = "linux", gemshell_check_all))]
pub mod input;
#[cfg(any(target_os = "linux", gemshell_check_all))]
pub mod nested;

use std::collections::HashMap;
use std::os::fd::AsFd;
use std::sync::{mpsc, Arc};

use wayland_protocols::xdg::shell::server::xdg_popup::XdgPopup;
use wayland_protocols::xdg::shell::server::xdg_surface::XdgSurface;
use wayland_protocols::xdg::shell::server::xdg_toplevel::{State as ToplevelState, XdgToplevel};
use wayland_server::protocol::wl_keyboard::{KeyState, KeymapFormat, WlKeyboard};
use wayland_server::protocol::wl_output::WlOutput;
use wayland_server::protocol::wl_surface::WlSurface;
use wayland_server::protocol::wl_touch::WlTouch;
use wayland_server::{Client, Display, Resource};

use crate::common::{apps, font, icons, util};
use crate::platform::{InputEvent, InputSource};
use crate::shell;
use gemdata::DataProvider;
use gemdata_device::DeviceData;
use gemdata_dummy::DummyData;

use wayland::{
    keymap_fd, BufferData, ClientState, CompositorData, DataDeviceManagerData,
    DecorationManagerData, OutputData, ShmData, SurfaceData, WmBaseData,
};
use wayland_protocols::xdg::decoration::zv1::server::zxdg_toplevel_decoration_v1::{
    Mode as DecorationMode, ZxdgToplevelDecorationV1,
};

/// GEMSHELL_SCREENSHOT fired once (see render_frame).
static SHOT_DONE: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

// scene geometry
// LOGICAL scene = LANDSCAPE (the Gemini is a landscape clamshell). The
// LK framebuffer is a PORTRAIT 1080x2160 buffer; present() counter-rotates
// this scene into it (gemwl does the same with WL_OUTPUT_TRANSFORM_90) and
// touch is un-rotated in input.rs. See docs/gemshell.md "Orientation".
pub const W: u32 = 2160;
pub const H: u32 = 1080;
pub const STATUS_H: f32 = 44.0;
pub const TASKBAR_H: f32 = 88.0;
pub const TITLEBAR_H: f32 = 40.0;
pub const N_WORKSPACES: u32 = 3;

// xkb keysyms — the Fn layer of layout "gemini" (config/xkb/symbols/gemini)
// emits exactly these on level 3; values from xkeysym 0.2 (X keysyms).
const XK_Escape: u32 = 0xff1b;
const XK_grave: u32 = 0x60;
const XK_Home: u32 = 0xff50;
const XK_End: u32 = 0xff57;
const XK_Prior: u32 = 0xff55; // page up
const XK_Next: u32 = 0xff56; // page down
const XK_Delete: u32 = 0xffff;
const XF86_Sleep: u32 = 0x1008ff2f; // Fn+Esc
const XF86_Tools: u32 = 0x1008ff81; // Fn+Backspace
const XF86_TaskPane: u32 = 0x1008ff7f; // Fn+A — launcher
const XF86_TopMenu: u32 = 0x1008ffa2; // Fn+S — app switcher
const XF86_AudioLower: u32 = 0x1008ff11; // Fn+C
const XF86_AudioMute: u32 = 0x1008ff12; // Fn+T
const XF86_AudioRaise: u32 = 0x1008ff13; // Fn+V
const XF86_MonBrightDown: u32 = 0x1008ff03; // Fn+B
const XF86_MonBrightUp: u32 = 0x1008ff02; // Fn+N

const MOD5: u32 = 1 << 8;

#[derive(Clone, Copy, PartialEq, Debug)]
pub enum Snap {
    None,
    Left,
    Right,
    Top,
    Bottom,
    Max,
}

pub struct Window {
    pub id: u32,
    /// set once the client's toplevel/popup resource exists (adopt_*)
    pub toplevel: Option<XdgToplevel>,
    /// the xdg_surface resource — needed to send the xdg_surface.configure
    /// that MUST follow every xdg_toplevel.configure (clients wait for it
    /// before attaching their first buffer; without it nothing maps).
    pub xdg_surface: Option<XdgSurface>,
    pub surface: WlSurface,
    pub title: String,
    pub app_id: String,
    pub x: f32,
    pub y: f32,
    pub w: f32,
    pub h: f32,
    pub buffer: Option<std::sync::Arc<BufferData>>,
    pub workspace: u32,
    pub minimized: bool,
    pub maximized: bool,
    pub snap: Snap,
    /// the launcher app index (matched by title/app_id), if known
    pub app: Option<usize>,
    pub is_popup: bool,
    pub parent: Option<u32>,
    /// Client-side decoration (xdg-decoration negotiated): the client
    /// draws its own titlebar/header bar, so the compositor must NOT draw
    /// the SSD titlebar (the "2 close buttons" report, 2026-09-11).
    pub csd: bool,
    /// The client's `zxdg_toplevel_decoration_v1` object, if any (used to
    /// send the mode configure).
    pub decoration: Option<ZxdgToplevelDecorationV1>,
}

impl Window {
    /// The compositor-drawn titlebar height for this window (0 with CSD).
    pub fn chrome_h(&self) -> f32 {
        if self.csd {
            0.0
        } else {
            TITLEBAR_H
        }
    }

    /// The client area (below the titlebar), in scene coords. With CSD
    /// there is no compositor titlebar, so the content fills the window.
    pub fn content_rect(&self) -> (f32, f32, f32, f32) {
        if self.csd {
            (self.x, self.y, self.w, self.h)
        } else {
            (self.x, self.y + TITLEBAR_H, self.w, self.h - TITLEBAR_H)
        }
    }

    /// Where the client buffer is drawn (letterboxed inside the content).
    pub fn buffer_rect(&self) -> (f32, f32, f32, f32) {
        let (cx, cy, cw, ch) = self.content_rect();
        let Some(buf) = &self.buffer else {
            return (cx, cy, cw, ch);
        };
        let bw = buf.w as f32;
        let bh = buf.h as f32;
        if bw <= 0.0 || bh <= 0.0 {
            return (cx, cy, cw, ch);
        }
        let sx = cw / bw;
        let sy = ch / bh;
        // GTK/GNOME CSD buffers reserve a transparent shadow margin, so a
        // maximized window's buffer is slightly smaller than the configure
        // we sent (e.g. 2062x939 vs 2160x948 at 100%). Centring that buffer
        // left a visible strip of window background around every app — the
        // "padding on apps" report (2026-09-11). When the aspect mismatch
        // is small, stretch to fill: the distortion is sub-pixel-ish and
        // the app actually reaches the screen edges. Larger mismatches
        // (video, a portrait dialog) still letterbox so content is not
        // cropped or badly stretched.
        let aspect = sx / sy;
        if (0.87..=1.15).contains(&aspect) {
            return (cx, cy, cw, ch);
        }
        let scale = sx.min(sy);
        let dw = bw * scale;
        let dh = bh * scale;
        (cx + (cw - dw) / 2.0, cy + (ch - dh) / 2.0, dw, dh)
    }
}

struct Finger {
    x: f32,
    y: f32,
    start_x: f32,
    start_y: f32,
    moved: bool,
    down_ms: u64,
}

#[derive(PartialEq, Clone)]
enum Gesture {
    None,
    /// one finger forwarded to the focused client (click/drag)
    Forward {
        win: u32,
        finger: u32,
    },
    /// one finger moving a window (from its titlebar)
    Move {
        win: u32,
        finger: u32,
        off_dx: f32,
        off_dy: f32,
    },
    /// two fingers swiping workspaces horizontally
    WorkspaceSwipe {
        f1: u32,
        f2: u32,
        last_x: f32,
        active: bool,
    },
    /// two fingers swiping up = next app
    NextApp {
        f1: u32,
        f2: u32,
        last_y: f32,
        active: bool,
    },
    /// three fingers: home (close overlays)
    Home,
    /// one finger scrolling the launcher
    LauncherScroll {
        finger: u32,
        last_y: f32,
    },
}

pub struct Compositor {
    pub renderer: render::Renderer,
    /// Where keyboard/touch events come from — evdev (device/nested) or
    /// the host window (macOS). See `platform::InputSource`.
    input: Box<dyn InputSource>,
    pub font: font::Font,
    icons: icons::IconSet,
    windows: Vec<Window>,
    pub focus: Option<u32>,
    /// continuous workspace camera (integer = that workspace)
    ws_pos: f32,
    ws_target: f32,
    fingers: HashMap<u32, Finger>,
    gesture: Gesture,
    /// a UI tap awaiting resolution on finger-up (status/taskbar/titlebar)
    pending_tap: Option<(f32, f32)>,
    /// the client surface currently receiving wl_touch
    touch_focus: Option<u32>,
    pub launcher_open: bool,
    pub launcher_scroll: f32,
    pub switcher_open: bool,
    pub switcher_index: usize,
    pub apps: Vec<apps::App>,
    /// app index -> GL texture (premultiplied icon)
    app_icons: HashMap<usize, u32>,
    pub settings_open: bool,
    pub status: status::Status,
    status_rx: mpsc::Receiver<status::Status>,
    dirty: bool,
    in_flight: bool,
    start_ms: u64,
    serial: u32,
    /// monotonic serial for xdg_surface.configure
    configure_serial: u32,
    keymap_str: Option<String>,
    /// Logical UI size in scene units: `W/ui_scale` × `H/ui_scale`. All
    /// layout (chrome, launcher, windows, snap rects) is in these units;
    /// the renderer maps them across the full physical viewport.
    pub lw: f32,
    pub lh: f32,
    /// UI scale (1.0 / 1.5 / 2.0), changed from Settings > Display.
    pub ui_scale: f32,
    /// Bound `wl_output` resources (for `wl_output.scale` broadcasts).
    outputs: Vec<WlOutput>,
    /// An in-progress client-requested window move (`xdg_toplevel.move`
    /// from a CSD header-bar drag). We keep forwarding the touch to the
    /// client AND move the window, so the client's gesture completes
    /// normally (2026-09-11).
    client_move: Option<(u32, u32, f32, f32)>,
    pub snap_preview: Snap,
    pub snap_win: Option<u32>,
    present_time_ms: u64,
    /// connected clients (keyboard fan-out; the backend prunes dead
    /// clients from the protocol state, this list is best-effort)
    tracked_clients: Vec<Client>,
    /// the currently pressed keysyms (for wl_keyboard.enter's keys list)
    pressed_keysyms: Vec<u32>,
    /// last titlebar double-tap bookkeeping
    last_titlebar_tap_ms: u64,
    last_titlebar_win: Option<u32>,
    /// The system-data provider — the ONE place gemshell reads/writes
    /// Wi-Fi, Bluetooth, audio, battery and brightness. Device by
    /// default; the in-memory dummy in nested mode. See `gemdata`.
    /// Wrapped in [`data::CachedData`]: reads are instant (cached) and
    /// mutations/refreshes run on a worker thread, so the compositor
    /// never blocks on nmcli/bluetoothctl/wpctl. See compositor/data.rs.
    data: Arc<data::CachedData>,
    /// Fires whenever the background data worker publishes a snapshot.
    data_rx: mpsc::Receiver<()>,
    /// The egui shell UI (settings panel). GPU-tessellated; see shell.rs.
    shell: shell::ShellUi,
    /// egui input accumulated between frames (points space).
    egui_events: Vec<egui::Event>,
    /// The last pointer point fed to egui (for click/drag events).
    egui_pointer: Option<egui::Pos2>,
    /// When the settings snapshots were last reloaded (ms since start).
    settings_snapshot_ms: u64,
    /// Touch test mode (`GEMSHELL_TOUCH_TRAIL=1`): every finger draws a
    /// trail on the scene and normal gestures are bypassed, so touch can
    /// be verified on the glass by drawing. A 3-finger touch clears it.
    touch_trail: bool,
    /// Trail points: (finger id, scene x, scene y). `id == u32::MAX` is a
    /// pen-up break (never drawn).
    trail: Vec<(u32, f32, f32)>,
}

impl Compositor {
    /// Build the compositor on top of the platform's renderer + input
    /// source. The platform backend owns the GL context/window and the
    /// event loop; this is the shared half.
    ///
    /// `dummy_data` selects the in-memory [`DummyData`] provider (nested
    /// dev loop / macOS preview) over the real device one.
    ///
    /// The display lives OUTSIDE the struct: `dispatch_clients(&mut self)`
    /// needs the display borrowed separately from the state.
    pub fn new(
        renderer: render::Renderer,
        input: Box<dyn InputSource>,
        font: font::Font,
        dummy_data: bool,
    ) -> Result<(Display<Compositor>, Self), String> {
        env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();

        let keymap_str = input.keymap_string();
        let ui_scale = Self::load_ui_scale();

        let display = Display::new().map_err(|e| format!("wayland display: {e}"))?;
        let handle = display.handle();
        let _ = handle.create_global::<Compositor, _, _>(7, CompositorData {}); // wl_compositor
        let _ = handle.create_global::<Compositor, _, _>(3, ShmData {}); // wl_shm
        let _ = handle.create_global::<Compositor, _, _>(11, ()); // wl_seat
        let _ = handle.create_global::<Compositor, _, _>(4, OutputData {}); // wl_output
        let _ = handle.create_global::<Compositor, _, _>(3, WmBaseData {}); // xdg_wm_base
                                                                            // GTK4 REQUIRES wl_data_device_manager to even open the display
                                                                            // (see the import comment in wayland.rs). Version 3 (not 4): we
                                                                            // do not implement wl_data_device_manager.release.
        let _ = handle.create_global::<Compositor, _, _>(3, DataDeviceManagerData {}); // wl_data_device_manager
                                                                                       // xdg-decoration: see wayland.rs. Version 2 (mode configure,
                                                                                       // set_mode/unset_mode, ToplevelDecorationError).
        let _ = handle.create_global::<Compositor, _, _>(2, DecorationManagerData {}); // zxdg_decoration_manager_v1

        // The system-data provider: the real device one, or the in-memory
        // dummy for the nested x86_64 dev loop / macOS preview (no
        // nmcli/bluetoothctl there). Both back the same `gemdata::DataProvider`.
        let real: Arc<dyn DataProvider> = if dummy_data {
            Arc::new(DummyData::new())
        } else {
            Arc::new(DeviceData::new())
        };
        let (tx, rx) = mpsc::channel();
        status::spawn(real.clone(), tx);
        let status = status::read(&*real);
        // Wrap the real provider in the async cache: the UI thread reads
        // the last snapshot instantly and mutations go via a worker, so
        // a slow nmcli/bluetoothctl call can't freeze the compositor
        // (2026-09-11). The cheap status poller above keeps using `real`.
        let (data, data_rx) = data::CachedData::new(real);

        let home = std::env::var("HOME").unwrap_or_else(|_| "/root".into());
        let apps = apps::scan(&home);
        log::info!("{} apps from .desktop files", apps.len());
        let icons = icons::load(&home);
        log::info!(
            "{} PNG + {} SVG icons indexed",
            icons.map.len(),
            icons.svg.len()
        );

        let compositor = Compositor {
            renderer,
            input,
            font,
            icons,
            windows: Vec::new(),
            focus: None,
            ws_pos: 0.0,
            ws_target: 0.0,
            fingers: HashMap::new(),
            gesture: Gesture::None,
            pending_tap: None,
            touch_focus: None,
            launcher_open: false,
            launcher_scroll: 0.0,
            switcher_open: false,
            switcher_index: 0,
            apps,
            app_icons: HashMap::new(),
            settings_open: false,
            configure_serial: 0,
            status,
            status_rx: rx,
            dirty: true,
            in_flight: false,
            start_ms: util::now_ms(),
            serial: 1,
            keymap_str,
            lw: W as f32 / ui_scale,
            lh: H as f32 / ui_scale,
            ui_scale,
            outputs: Vec::new(),
            client_move: None,
            snap_preview: Snap::None,
            snap_win: None,
            present_time_ms: util::now_ms(),
            tracked_clients: Vec::new(),
            pressed_keysyms: Vec::new(),
            last_titlebar_tap_ms: 0,
            last_titlebar_win: None,
            data,
            data_rx,
            shell: shell::ShellUi::new(),
            egui_events: Vec::new(),
            egui_pointer: None,
            settings_snapshot_ms: 0,
            touch_trail: util::env_flag("GEMSHELL_TOUCH_TRAIL"),
            trail: Vec::new(),
        };
        Ok((display, compositor))
    }

    pub fn ws_target_f(&self) -> f32 {
        self.ws_target
    }

    /// Upload app icon textures (premultiplied) once at startup.
    pub fn upload_app_icons(&mut self) {
        // snapshot the app list: the loop below mutably borrows
        // self.renderer/self.icons while reading app data
        let apps = self.apps.clone();
        for (i, app) in apps.iter().enumerate() {
            let Some(icon) = icons::for_app(&mut self.icons, &app.icon, &app.path) else {
                continue;
            };
            let scale = 96.0f32 / icon.w.max(icon.h) as f32;
            let tw = (icon.w as f32 * scale).round().max(2.0) as u32;
            let th = (icon.h as f32 * scale).round().max(2.0) as u32;
            let mut rgba = vec![0u8; (tw * th * 4) as usize];
            for y in 0..th {
                for x in 0..tw {
                    let sx = (x as f32 / scale).min(icon.w as f32 - 1.0) as u32;
                    let sy = (y as f32 / scale).min(icon.h as f32 - 1.0) as u32;
                    let s = ((sy * icon.w + sx) * 4) as usize;
                    let d = ((y * tw + x) * 4) as usize;
                    let a = icon.rgba[s + 3] as f32 / 255.0;
                    rgba[d] = (icon.rgba[s] as f32 * a) as u8;
                    rgba[d + 1] = (icon.rgba[s + 1] as f32 * a) as u8;
                    rgba[d + 2] = (icon.rgba[s + 2] as f32 * a) as u8;
                    rgba[d + 3] = (a * 255.0) as u8;
                }
            }
            if let Ok(tex) = self.renderer.make_texture_pub(tw, th, &rgba) {
                self.app_icons.insert(i, tex);
            }
        }
        self.dirty = true;
    }

    pub fn app_icon_tex(&self, app: usize) -> Option<u32> {
        self.app_icons.get(&app).copied()
    }

    // -----------------------------------------------------------------
    // Platform-facing interface (the `platform::Backend` implementations
    // drive the compositor through these; nothing else should reach into
    // the private state).
    // -----------------------------------------------------------------

    /// Shared startup: upload app icons and apply the dev env knobs
    /// (`GEMSHELL_OPEN_SETTINGS` / `_OPEN_LAUNCHER` / `_AUTOSTART`).
    pub fn startup(&mut self) {
        self.upload_app_icons();
        if util::env_flag("GEMSHELL_OPEN_SETTINGS") {
            self.open_settings();
        }
        if util::env_flag("GEMSHELL_OPEN_LAUNCHER") {
            self.launcher_open = true;
        }
        if let Ok(app) = std::env::var("GEMSHELL_AUTOSTART") {
            if !app.is_empty() {
                log::info!("autostart: {app}");
                spawn_cmd(app, Vec::<String>::new());
            }
        }
    }

    /// Drain the background channels (status poller + data worker). They
    /// do not wake a poll()/event loop, so the backend calls this every
    /// tick — and sets a 1 s ceiling when idle.
    pub(crate) fn drain_background(&mut self) {
        while let Ok(s) = self.status_rx.try_recv() {
            if s != self.status {
                self.status = s;
                self.dirty = true;
            }
            // While the settings panel is open, keep a slow repaint alive:
            // the frame clock is dirty-driven, and the panel's data-refresh
            // request lives in `run_egui` (which only runs when a frame is
            // drawn). The status poller ticks every 2 s, so this yields a
            // ~0.5 Hz panel repaint that services the refresh.
            if self.settings_open {
                self.dirty = true;
            }
        }
        while self.data_rx.try_recv().is_ok() {
            self.shell.state.need_refresh = true;
            self.dirty = true;
        }
    }

    /// Animation + frame-pacing bookkeeping (once per tick, before
    /// deciding whether to render).
    pub(crate) fn advance(&mut self) {
        self.step_animation();
        if self.in_flight && util::now_ms().saturating_sub(self.present_time_ms) >= 16 {
            self.in_flight = false;
        }
    }

    /// A frame is due and none is in flight.
    pub(crate) fn wants_frame(&self) -> bool {
        self.dirty && !self.in_flight
    }

    /// The backend's poll timeout (ms): 0 when a frame is due, else the
    /// remainder of the ~60 Hz interval, else a 1 s idle tick so the
    /// status/clock channel is drained.
    pub(crate) fn poll_timeout_ms(&self) -> i32 {
        if self.in_flight {
            let elapsed = util::now_ms().saturating_sub(self.present_time_ms);
            if elapsed >= 16 {
                0
            } else {
                (16 - elapsed) as i32
            }
        } else if self.dirty || self.animating() {
            0
        } else {
            1000
        }
    }

    /// Render one frame into the scene FBO and run the platform presenter.
    pub(crate) fn render(&mut self) {
        self.render_frame();
        // Keep rendering while egui asks for another frame (animations/
        // hover). Cheap under the modal: the hidden scene is skipped.
        self.dirty = self.settings_open && self.shell.wants_repaint();
        self.in_flight = true;
        self.present_time_ms = util::now_ms();
    }

    /// A device page-flip/GBM event completed the present.
    #[allow(dead_code)] // Linux backend only
    pub(crate) fn flip_done(&mut self) {
        self.in_flight = false;
        self.present_time_ms = util::now_ms();
        self.dirty = true;
    }

    pub(crate) fn mark_dirty(&mut self) {
        self.dirty = true;
    }

    /// Drain the platform input source into the compositor's gestures.
    #[allow(dead_code)] // evdev-backed backends only
    pub(crate) fn poll_input(&mut self) {
        let iscale = if self.ui_scale > 0.01 {
            self.ui_scale
        } else {
            1.0
        };
        for ev in self.input.poll(W as f32, H as f32) {
            self.feed_input(ev, iscale);
        }
    }

    /// Feed one input event (window coords already mapped to the scene's
    /// full-resolution space; `iscale` converts to the logical layout).
    pub(crate) fn feed_input(&mut self, ev: InputEvent, iscale: f32) {
        self.serial += 1;
        match ev {
            InputEvent::Key {
                code,
                keysym,
                pressed,
                mods,
                ..
            } => self.handle_key(code, keysym, pressed, mods),
            InputEvent::TouchDown { id, x, y } => self.touch_down(id, x / iscale, y / iscale),
            InputEvent::TouchMotion { id, x, y } => self.touch_motion(id, x / iscale, y / iscale),
            InputEvent::TouchUp { id } => self.touch_up(id),
        }
    }

    /// Feed a synthesised key (macOS maps its window events to keysyms
    /// without xkbcommon).
    pub(crate) fn feed_key(&mut self, code: u32, keysym: u32, pressed: bool, mods: u32) {
        self.serial += 1;
        self.handle_key(code, keysym, pressed, mods);
    }

    /// Feed one raw evdev scancode through the input source's xkb state
    /// (the nested Wayland-client path forwards host key events this way).
    #[allow(dead_code)] // Linux backend only
    pub(crate) fn feed_evdev_key(&mut self, code: u32, pressed: bool) {
        let (keysym, mods) = self.input.process_key(code, pressed);
        self.serial += 1;
        self.handle_key(code, keysym, pressed, mods);
    }

    /// The input source's poll fds (evdev); (-1, -1) for event-loop
    /// backends. The Linux backend polls these.
    #[allow(dead_code)] // Linux backend only
    pub(crate) fn input_fds(&self) -> (std::os::fd::RawFd, std::os::fd::RawFd) {
        self.input.poll_fds()
    }

    /// Pointer hover (macOS): only the egui settings panel cares.
    pub(crate) fn pointer_hover(&mut self, x: f32, y: f32) {
        if self.settings_open {
            self.egui_move_pointer(x, y);
            self.dirty = true;
        }
    }

    /// Scroll the egui settings panel (macOS mouse wheel).
    pub(crate) fn scroll_panel(&mut self, dy: f32) {
        if self.settings_open {
            self.egui_events.push(egui::Event::MouseWheel {
                unit: egui::MouseWheelUnit::Point,
                delta: egui::vec2(0.0, dy),
                modifiers: self.egui_mods_now(),
            });
            self.dirty = true;
        }
    }

    fn animating(&self) -> bool {
        (self.ws_pos - self.ws_target).abs() > 0.002 || self.screenshot_pending()
    }

    /// A one-shot `GEMSHELL_SCREENSHOT` is still pending: keep the frame
    /// clock alive until it fires (otherwise the new idle-blocking loop
    /// would never render the delayed shot).
    fn screenshot_pending(&self) -> bool {
        std::env::var_os("GEMSHELL_SCREENSHOT").is_some()
            && !SHOT_DONE.load(std::sync::atomic::Ordering::Relaxed)
    }

    fn step_animation(&mut self) {
        if self.screenshot_pending() {
            self.dirty = true;
        }
        if (self.ws_pos - self.ws_target).abs() > 0.002 {
            self.ws_pos += (self.ws_target - self.ws_pos) * 0.35;
            if (self.ws_pos - self.ws_target).abs() <= 0.002 {
                self.ws_pos = self.ws_target;
            }
            self.dirty = true;
        }
        // NOTE: launcher_scroll is deliberately NOT decayed here. It is a
        // persisted list position, not a spring; the old `*= 0.8` snapped
        // the grid back to the top the moment the finger lifted, so apps
        // past row ~3 could never be reached. (It is also kept out of
        // `animating()` so a scrolled launcher does not hold the frame
        // clock at 60 Hz forever.) [2026-09-12]
    }

    #[allow(dead_code)] // Linux backend only
    pub(crate) fn note_client(&mut self, client: Client) {
        self.tracked_clients.push(client);
        if self.tracked_clients.len() > 32 {
            self.tracked_clients.remove(0);
        }
    }

    // -----------------------------------------------------------------
    // keys

    fn handle_key(&mut self, code: u32, keysym: u32, pressed: bool, mods: u32) {
        // While the settings panel is open it owns the keyboard (text
        // entry, Enter/Esc). Fn media keys are not shortcuts here.
        if self.settings_open {
            self.egui_key(keysym, pressed, mods);
            if pressed && keysym == XK_Escape && self.shell.state.password_for.is_none() {
                self.shell.state.close = true;
            }
            return;
        }
        if !pressed {
            self.pressed_keysyms.retain(|k| *k != keysym);
            if keysym == XF86_TopMenu && self.switcher_open {
                // release = activate the selection
                self.switcher_open = false;
                self.activate_switcher();
                self.dirty = true;
            }
            // key up: forward to the focused client
            self.forward_key(code, keysym, false);
            return;
        }
        if !self.pressed_keysyms.contains(&keysym) {
            self.pressed_keysyms.push(keysym);
        }
        // ---- compositor shortcuts (consume) ----
        let consumed = match keysym {
            XF86_TaskPane => {
                self.launcher_open = !self.launcher_open;
                self.switcher_open = false;
                true
            }
            XF86_TopMenu => {
                if self.switcher_open {
                    // each press cycles the selection
                    let n = self.switcher_windows().len();
                    if n > 0 {
                        self.switcher_index = (self.switcher_index + 1) % n;
                    }
                } else {
                    self.switcher_open = true;
                    self.launcher_open = false;
                    self.switcher_index = self
                        .switcher_windows()
                        .iter()
                        .position(|w| Some(w.id) == self.focus)
                        .unwrap_or(0);
                }
                self.dirty = true;
                true
            }
            XF86_Tools | XK_Delete if self.switcher_open == false => {
                if let Some(f) = self.focus {
                    self.request_close(f);
                }
                true
            }
            XF86_AudioLower => {
                self.volume_step(-10);
                true
            }
            XF86_AudioRaise => {
                self.volume_step(10);
                true
            }
            XF86_AudioMute => {
                self.volume_mute();
                true
            }
            XF86_MonBrightDown => {
                self.brightness_step(-10);
                true
            }
            XF86_MonBrightUp => {
                self.brightness_step(10);
                true
            }
            XF86_Sleep => {
                self.launcher_open = false;
                self.switcher_open = false;
                true
            }
            XK_Prior if mods & MOD5 != 0 => {
                self.switch_workspace(self.ws_target - 1.0);
                true
            }
            XK_Next if mods & MOD5 != 0 => {
                self.switch_workspace(self.ws_target + 1.0);
                true
            }
            XK_Home if mods & MOD5 == MOD5 => {
                self.snap_focused(Snap::Left);
                true
            }
            XK_End if mods & MOD5 == MOD5 => {
                self.snap_focused(Snap::Right);
                true
            }
            XK_grave if mods & MOD5 == MOD5 => {
                self.toggle_maximize_focused();
                true
            }
            XK_Escape => {
                if self.launcher_open || self.switcher_open {
                    self.launcher_open = false;
                    self.switcher_open = false;
                    self.dirty = true;
                }
                true
            }
            _ => false,
        };
        if consumed {
            return;
        }
        self.forward_key(code, keysym, true);
    }

    /// Forward a key event to the focused client's wl_keyboard.
    fn forward_key(&mut self, code: u32, _keysym: u32, pressed: bool) {
        let Some(focus) = self.focus else { return };
        let Some(win) = self.windows.iter().find(|w| w.id == focus) else {
            return;
        };
        let Some(kbd) = self.kbd_res_for(win) else {
            return;
        };
        // wl_keyboard.key carries the raw evdev scancode; CLIENTS add 8
        // for xkbcommon (wayland.xml). The old +8 here double-offset the
        // key for every client (fixed 2026-09-11, nested-mode bring-up).
        let state = if pressed {
            KeyState::Pressed
        } else {
            KeyState::Released
        };
        let _ = kbd.key(self.serial, self.present_time_ms as u32, code, state);
        let (d, l, k) = self.input.mods_masks();
        let _ = kbd.modifiers(self.serial, d, l, k, 0);
    }

    fn volume_step(&mut self, delta: i32) {
        let target = (self.status.volume + delta).clamp(0, 100);
        spawn_cmd(
            "wpctl",
            vec![
                "set-volume".to_string(),
                "@DEFAULT_AUDIO_SINK@".to_string(),
                format!("{target}%"),
            ],
        );
        self.status.volume = target;
        self.dirty = true;
    }

    fn volume_mute(&mut self) {
        let m = !self.status.muted;
        spawn_cmd(
            "wpctl",
            vec![
                "set-mute".to_string(),
                "@DEFAULT_AUDIO_SINK@".to_string(),
                if m { "1".to_string() } else { "0".to_string() },
            ],
        );
        self.status.muted = m;
        self.dirty = true;
    }

    fn brightness_step(&mut self, delta: i32) {
        let max = self.status.brightness_max.max(1);
        let cur_pct = self.status.brightness * 100 / max;
        let target = (cur_pct + delta).clamp(0, 100);
        if self.data.set_brightness(target).is_ok() {
            self.status.brightness = target * max / 100;
            self.dirty = true;
        }
    }

    // -----------------------------------------------------------------
    // egui input plumbing

    fn egui_time(&self) -> f64 {
        self.present_time_ms.saturating_sub(self.start_ms) as f64 / 1000.0
    }

    /// Feed a scene-pixel point into egui's pointer (egui is in points).
    fn egui_move_pointer(&mut self, x: f32, y: f32) {
        // `x`/`y` are LOGICAL scene units; egui points are logical / PPP
        // (the screen rect passed to egui is `lw/PPP` × `lh/PPP`).
        let p = egui::pos2(x / shell::PPP, y / shell::PPP);
        self.egui_pointer = Some(p);
        self.egui_events.push(egui::Event::PointerMoved(p));
        self.dirty = true;
    }

    fn egui_pointer_button(&mut self, x: f32, y: f32, pressed: bool) {
        let p = egui::pos2(x / shell::PPP, y / shell::PPP);
        self.egui_pointer = Some(p);
        self.egui_events.push(egui::Event::PointerMoved(p));
        self.egui_events.push(egui::Event::PointerButton {
            pos: p,
            button: egui::PointerButton::Primary,
            pressed,
            modifiers: self.egui_mods_now(),
        });
        self.dirty = true;
    }

    fn egui_mods_now(&self) -> egui::Modifiers {
        let (down, _, _) = self.input.mods_masks();
        egui_mods(down)
    }

    /// Forward a key event to egui (both the logical key and, for
    /// printable symbols, a Text event so the password field receives
    /// the layout's real characters — including the Fn level-3 glyphs).
    fn egui_key(&mut self, keysym: u32, pressed: bool, mods: u32) {
        let modifiers = egui_mods(mods);
        if let Some(key) = egui_key_from_keysym(keysym) {
            self.egui_events.push(egui::Event::Key {
                key,
                physical_key: None,
                pressed,
                repeat: false,
                modifiers,
            });
        }
        if pressed && !modifiers.ctrl {
            if let Some(text) = keysym_text(keysym) {
                self.egui_events.push(egui::Event::Text(text));
            }
        }
        self.dirty = true;
    }

    /// Run one egui pass for the settings panel. Returns the meshes for
    /// the renderer (None when the panel is closed).
    fn run_egui(&mut self) -> Option<shell::UiFrame> {
        if !self.settings_open {
            return None;
        }
        let now = self.present_time_ms.saturating_sub(self.start_ms);
        // 8 s, not 3 s: a full snapshot is ~1–2 s of nmcli/bluetoothctl/
        // wpctl on this SoC, and mutations are queued ahead of it, so a
        // too-frequent poll is felt as input lag. The panel also requests
        // a snapshot on open and after state-changing buttons, so this
        // only needs to catch external changes (2026-09-11).
        if now.saturating_sub(self.settings_snapshot_ms) > 8000 {
            // Ask the worker for a fresh snapshot (non-blocking) and show
            // the cached one immediately; the new one arrives via
            // `data_rx` + `need_refresh` (2026-09-11).
            self.data.refresh();
            self.shell.refresh(&*self.data, self.ui_scale);
            self.settings_snapshot_ms = now;
        }
        let screen = egui::Rect::from_min_size(
            egui::Pos2::ZERO,
            egui::vec2(self.lw / shell::PPP, self.lh / shell::PPP),
        );
        // egui's font atlas is rasterized at `pixels_per_point`; include
        // the UI scale so 150/200% text is crisp, not upscaled.
        self.shell.set_ppp(self.egui_ppp());
        let events = std::mem::take(&mut self.egui_events);
        let input = egui::RawInput {
            screen_rect: Some(screen),
            time: Some(self.egui_time()),
            events,
            ..Default::default()
        };
        let frame = self.shell.run(&*self.data, input);
        if let Some(scale) = frame.requested_scale {
            self.set_ui_scale(scale);
        }
        if self.shell.state.close {
            // Close on THIS frame: clear the modal and return a frame with
            // no panel meshes, so the next present is already panel-free.
            // (Returning the drawn panel made Close feel ~1 frame slow; the
            // atlas delta is still applied by the renderer.)
            self.settings_open = false;
            self.shell.state.close = false;
            let mut frame = frame;
            frame.primitives.clear();
            self.dirty = true;
            return Some(frame);
        }
        // Repaint on demand only: input sets `dirty`, and the main loop
        // re-arms it while egui asks for another frame (animations/hover).
        Some(frame)
    }

    // -----------------------------------------------------------------
    // touch + gestures

    fn touch_down(&mut self, id: u32, x: f32, y: f32) {
        // Touch test mode: draw, don't gesture. 3 fingers = clear.
        if self.touch_trail {
            if self.fingers.len() >= 2 {
                self.trail.clear();
                log::info!("touch-trail: cleared");
            }
            self.fingers.insert(
                id,
                Finger {
                    x,
                    y,
                    start_x: x,
                    start_y: y,
                    moved: false,
                    down_ms: util::now_ms(),
                },
            );
            log::info!("touch-trail: down id={id} scene=({x:.0},{y:.0})");
            self.trail.push((id, x, y));
            if self.trail.len() > 8000 {
                self.trail.drain(0..2000);
            }
            self.dirty = true;
            return;
        }
        // The settings panel is modal: every finger is an egui pointer.
        if self.settings_open {
            self.egui_move_pointer(x, y);
            self.egui_pointer_button(x, y, true);
            return;
        }
        self.dismiss_popups_outside(x, y);

        let f = Finger {
            x,
            y,
            start_x: x,
            start_y: y,
            moved: false,
            down_ms: util::now_ms(),
        };
        let n = self.fingers.len();
        self.fingers.insert(id, f);

        match n {
            0 => self.gesture_begin_single(id, x, y),
            1 => self.gesture_begin_double(id),
            2 => {
                self.cancel_forwarded();
                if self.launcher_open || self.switcher_open {
                    self.launcher_open = false;
                    self.switcher_open = false;
                    self.dirty = true;
                }
                self.gesture = Gesture::Home;
            }
            _ => {
                self.cancel_forwarded();
            }
        }
    }

    fn gesture_begin_single(&mut self, id: u32, x: f32, y: f32) {
        if self.launcher_open {
            self.gesture = Gesture::LauncherScroll {
                finger: id,
                last_y: y,
            };
            return;
        }
        if self.switcher_open {
            self.gesture = Gesture::None; // tap resolved on up
            return;
        }
        if y < STATUS_H || y > self.lh - TASKBAR_H {
            self.pending_tap = Some((x, y));
            self.gesture = Gesture::None;
            return;
        }
        let ws = self.ws_target.round() as u32;
        // copy hit info out first: the branches below mutably borrow self
        let mut hit = None;
        for win in self.windows.iter().rev() {
            if win.minimized || win.workspace != ws {
                continue;
            }
            if x >= win.x && x <= win.x + win.w && y >= win.y && y <= win.y + win.h {
                hit = Some((win.id, win.is_popup, win.x, win.y, win.w, win.csd));
                break;
            }
        }
        let Some((wid, is_popup, wx, wy, ww, csd)) = hit else {
            self.launcher_open = false;
            self.switcher_open = false;
            self.gesture = Gesture::None;
            return;
        };
        self.raise(wid);
        if !is_popup && !csd && y - wy < TITLEBAR_H {
            // close button (top-right 44px)
            if x >= wx + ww - 44.0 {
                self.request_close(wid);
                return;
            }
            // double-tap = toggle maximize
            let now = util::now_ms();
            let double =
                self.last_titlebar_win == Some(wid) && now - self.last_titlebar_tap_ms < 320;
            self.last_titlebar_tap_ms = now;
            self.last_titlebar_win = Some(wid);
            if double {
                let was_max = self
                    .windows
                    .iter()
                    .find(|w| w.id == wid)
                    .map(|w| w.maximized)
                    .unwrap_or(false);
                self.set_maximized(wid, !was_max);
                self.gesture = Gesture::None;
                return;
            }
            self.gesture = Gesture::Move {
                win: wid,
                finger: id,
                off_dx: x - wx,
                off_dy: y - wy,
            };
            self.snap_preview = Snap::None;
            self.snap_win = Some(wid);
        } else {
            self.begin_forward(wid, id);
            self.gesture = Gesture::Forward {
                win: wid,
                finger: id,
            };
        }
    }

    fn gesture_begin_double(&mut self, id: u32) {
        let first = self
            .fingers
            .iter()
            .find(|(k, _)| **k != id)
            .map(|(k, _)| *k);
        self.cancel_forwarded();
        self.snap_preview = Snap::None;
        self.snap_win = None;
        let Some(f1) = first else {
            self.gesture = Gesture::None;
            return;
        };
        let (f1d, f2d) = match (self.fingers.get(&f1), self.fingers.get(&id)) {
            (Some(a), Some(b)) => (a, b),
            _ => {
                self.gesture = Gesture::None;
                return;
            }
        };
        let dx = f2d.start_x - f1d.start_x;
        let dy = f2d.start_y - f1d.start_y;
        if dx.abs() > dy.abs() {
            self.gesture = Gesture::WorkspaceSwipe {
                f1,
                f2: id,
                last_x: (f1d.start_x + f2d.start_x) / 2.0,
                active: true,
            };
        } else {
            self.gesture = Gesture::NextApp {
                f1,
                f2: id,
                last_y: (f1d.start_y + f2d.start_y) / 2.0,
                active: true,
            };
        }
    }

    fn touch_motion(&mut self, id: u32, x: f32, y: f32) {
        if self.touch_trail {
            if let Some(f) = self.fingers.get_mut(&id) {
                f.x = x;
                f.y = y;
            }
            self.trail.push((id, x, y));
            if self.trail.len() > 8000 {
                self.trail.drain(0..2000);
            }
            self.dirty = true;
            return;
        }
        if self.settings_open {
            self.egui_move_pointer(x, y);
            return;
        }
        let Some(f) = self.fingers.get_mut(&id) else {
            return;
        };
        if (x - f.start_x).abs() > 18.0 || (y - f.start_y).abs() > 18.0 {
            f.moved = true;
        }
        f.x = x;
        f.y = y;

        // Client-requested move (a CSD header-bar drag): move the window
        // with this finger while STILL forwarding the touch below, so the
        // client's drag gesture keeps receiving motion/up.
        if let Some((win, finger, off_dx, off_dy)) = self.client_move {
            if finger == id {
                let (lw, lh) = (self.lw, self.lh);
                if let Some(w) = self.windows.iter_mut().find(|w| w.id == win) {
                    w.x = (x - off_dx).clamp(0.0, (lw - w.w).max(0.0));
                    w.y = (y - off_dy).clamp(STATUS_H, (lh - TASKBAR_H - 40.0).max(STATUS_H));
                }
                self.snap_preview = self.snap_zone(x, y);
                self.dirty = true;
            }
        }

        // clone the gesture: the arms below mutably borrow self
        match self.gesture.clone() {
            Gesture::Move {
                win,
                finger,
                off_dx,
                off_dy,
            } if finger == id => {
                let needs_unsnap = self
                    .windows
                    .iter()
                    .find(|w| w.id == win)
                    .map(|w| w.maximized || w.snap != Snap::None)
                    .unwrap_or(false);
                if let Some(w) = self.windows.iter_mut().find(|w| w.id == win) {
                    w.x = (x - off_dx).clamp(0.0, self.lw - w.w);
                    w.y = (y - off_dy).clamp(STATUS_H, self.lh - TASKBAR_H - 40.0);
                }
                if needs_unsnap {
                    self.unmaximize(win);
                }
                self.snap_preview = self.snap_zone(x, y);
                self.dirty = true;
            }
            Gesture::WorkspaceSwipe {
                f1,
                f2,
                last_x,
                active,
            } if f2 == id || f1 == id => {
                let Some(cx) = self.pair_center_x(f1, f2) else {
                    return;
                };
                if active {
                    self.ws_pos = (self.ws_pos - (cx - last_x) / self.lw)
                        .clamp(0.0, (N_WORKSPACES - 1) as f32);
                }
                if let Gesture::WorkspaceSwipe { last_x: lx, .. } = &mut self.gesture {
                    *lx = cx;
                }
                self.dirty = true;
            }
            Gesture::NextApp { f1, f2, last_y, .. } if f2 == id || f1 == id => {
                let Some(cy) = self.pair_center_y(f1, f2) else {
                    return;
                };
                let mut should_next = false;
                if let Gesture::NextApp {
                    active: a,
                    last_y: ly,
                    ..
                } = &mut self.gesture
                {
                    if *a && cy - *ly < -80.0 {
                        should_next = true;
                        *a = false;
                    }
                    *ly = cy;
                }
                if should_next {
                    self.next_app();
                }
                self.dirty = true;
            }
            Gesture::LauncherScroll { finger, last_y } if finger == id => {
                self.launcher_scroll -= (y - last_y) * 0.9;
                // Clamp to the actual content height so the last row is
                // always reachable. `scroll` is the downward content
                // offset, so dragging up (y decreasing) increases it; the
                // old hard clamp was `[-600, 0]` — the wrong sign, so a
                // swipe-up was clamped straight back to 0 and only three
                // rows were ever reachable (2026-09-12).
                let max = (ui::launcher_content_h(self.apps.len()) - self.lh).max(0.0);
                self.launcher_scroll = self.launcher_scroll.clamp(0.0, max);
                if let Gesture::LauncherScroll { last_y: ly, .. } = &mut self.gesture {
                    *ly = y;
                }
                self.dirty = true;
            }
            Gesture::Forward { win, .. } => {
                self.forward_motion(win, x, y);
            }
            _ => {}
        }
    }

    fn touch_up(&mut self, id: u32) {
        if self.touch_trail {
            self.fingers.remove(&id);
            // Pen-up break so the next stroke is not connected to this one.
            self.trail.push((u32::MAX, 0.0, 0.0));
            self.dirty = true;
            return;
        }
        if self.settings_open {
            let p = self.egui_pointer.unwrap_or(egui::Pos2::ZERO);
            self.egui_pointer_button(p.x * shell::PPP, p.y * shell::PPP, false);
            return;
        }
        let was_moved = self.fingers.get(&id).map(|f| f.moved).unwrap_or(false);
        let pos = self.fingers.get(&id).map(|f| (f.x, f.y));
        self.fingers.remove(&id);
        // End a client-requested move with the finger that started it.
        if self.client_move.map(|(_, f, _, _)| f) == Some(id) {
            self.client_move = None;
        }

        match self.gesture.clone() {
            Gesture::None => {
                if !was_moved && self.fingers.is_empty() {
                    if let Some((x, y)) = pos {
                        self.handle_tap(x, y);
                    }
                }
                self.pending_tap = None;
            }
            Gesture::Move { win, .. } => {
                if self.snap_preview != Snap::None && self.snap_win == Some(win) {
                    self.apply_snap(win, self.snap_preview);
                }
                self.snap_preview = Snap::None;
                self.snap_win = None;
                if self.fingers.is_empty() {
                    self.gesture = Gesture::None;
                }
            }
            Gesture::WorkspaceSwipe { .. } => {
                self.ws_target = self.ws_pos.round().clamp(0.0, (N_WORKSPACES - 1) as f32);
                if self.fingers.len() < 2 {
                    self.gesture = Gesture::None;
                }
            }
            Gesture::NextApp { .. } | Gesture::Home => {
                if self.fingers.len() < 2 {
                    self.gesture = Gesture::None;
                }
            }
            Gesture::LauncherScroll { .. } => {
                // A tap (no drag) on a tile launches; a tap elsewhere
                // closes the drawer. Without this the launcher could only
                // be closed by a second power-button/taskbar tap and taps
                // on icons did nothing (2026-09-12).
                if !was_moved && self.fingers.is_empty() {
                    if let Some((x, y)) = pos {
                        self.handle_tap(x, y);
                    }
                }
                if self.fingers.is_empty() {
                    self.gesture = Gesture::None;
                }
            }
            Gesture::Forward { win, .. } => {
                self.forward_up(win, id);
                if self.fingers.is_empty() {
                    self.gesture = Gesture::None;
                }
            }
        }
    }

    fn pair_center_x(&self, a: u32, b: u32) -> Option<f32> {
        let (fa, fb) = (self.fingers.get(&a)?, self.fingers.get(&b)?);
        Some((fa.x + fb.x) / 2.0)
    }
    fn pair_center_y(&self, a: u32, b: u32) -> Option<f32> {
        let (fa, fb) = (self.fingers.get(&a)?, self.fingers.get(&b)?);
        Some((fa.y + fb.y) / 2.0)
    }

    fn snap_zone(&self, x: f32, y: f32) -> Snap {
        let edge = 26.0;
        if x < edge {
            return Snap::Left;
        }
        if x > self.lw - edge {
            return Snap::Right;
        }
        if y < STATUS_H + edge {
            return Snap::Top;
        }
        if y > self.lh - TASKBAR_H - edge {
            return Snap::Bottom;
        }
        Snap::None
    }

    // -----------------------------------------------------------------
    // taps (UI hit-testing)

    fn handle_tap(&mut self, x: f32, y: f32) {
        if self.launcher_open {
            // Explicit Close button (top-right).
            let (bx, by) = ui::launcher_close_pos(self.lw);
            if (x - bx).abs() < 100.0 && (y - by).abs() < 50.0 {
                self.launcher_open = false;
                self.dirty = true;
                return;
            }
            let n = self.apps.len();
            for i in 0..n {
                let (cx, cy) = ui::launcher_tile_pos(i, self.launcher_scroll, self.lw);
                if x > cx - 70.0 && x < cx + 70.0 && y > cy - 70.0 && y < cy + 80.0 {
                    self.launch_app(i);
                    self.launcher_open = false;
                    return;
                }
            }
            // Tap outside any tile closes the drawer.
            self.launcher_open = false;
            self.dirty = true;
            return;
        }
        if self.switcher_open {
            let n = self.switcher_windows().len().max(1);
            let cw = 220.0;
            let gap = 24.0;
            let total = n as f32 * cw + (n - 1) as f32 * gap;
            let x0 = (self.lw - total) / 2.0;
            let y0 = self.lh / 2.0 - 120.0;
            for i in 0..n {
                let tx = x0 + i as f32 * (cw + gap);
                if x > tx - 6.0 && x < tx + cw + 6.0 && y > y0 - 6.0 && y < y0 + 252.0 {
                    self.switcher_index = i;
                    self.switcher_open = false;
                    self.activate_switcher();
                    self.dirty = true;
                    return;
                }
            }
            return;
        }
        if y < STATUS_H {
            // Focused-window controls (left of the status bar): a
            // close / restore / minimize affordance that does not depend
            // on the app's own CSD buttons. New toplevels open maximized
            // and GTK's CSD buttons were the only way back, so maximized
            // apps could not be shrunk or closed (on-glass report
            // 2026-09-12).
            if let Some(win) = self.focus {
                for (name, cx) in ui::window_control_zones() {
                    if (x - cx).abs() < ui::ZONE_HALF {
                        match name {
                            "minimize" => self.minimize(win),
                            "maximize" => {
                                self.focus = Some(win);
                                self.toggle_maximize_focused();
                            }
                            "close" => self.request_close(win),
                            _ => {}
                        }
                        self.dirty = true;
                        return;
                    }
                }
            }
            for (name, cx) in ui::status_zones(self.lw) {
                if (x - cx).abs() < ui::ZONE_HALF {
                    match name {
                        "settings" | "sound" | "wifi" | "bluetooth" => self.open_settings(),
                        "battery" => {}
                        _ => {}
                    }
                    return;
                }
            }
            return;
        }
        if y > self.lh - TASKBAR_H {
            if x < ui::LAUNCHER_X + ui::LAUNCHER_S + 8.0 {
                self.launcher_open = !self.launcher_open;
                self.switcher_open = false;
                self.dirty = true;
                return;
            }
            let tiles: Vec<(u32, u32)> = self
                .visible_windows()
                .iter()
                .map(|w| (w.id, w.workspace))
                .collect();
            let mut tx = ui::TILE_X0;
            for (wid, ws) in tiles {
                if tx + ui::TILE_S > self.lw {
                    break;
                }
                if x >= tx && x < tx + ui::TILE_S {
                    if self.focus == Some(wid) {
                        self.minimize(wid);
                    } else {
                        self.ws_target = ws as f32;
                        self.raise(wid);
                    }
                    return;
                }
                tx += ui::TILE_S + ui::TILE_GAP;
            }
            return;
        }
        // (window titlebars are handled by the Move gesture on touch-down)
    }

    // -----------------------------------------------------------------
    // client touch forwarding

    fn touch_res_for(&self, win: &Window) -> Option<WlTouch> {
        let client = win.toplevel.as_ref()?.client()?;
        let cs = client.get_data::<ClientState>()?;
        let out = cs.inner.lock().ok()?.touch.clone();
        out
    }

    fn kbd_res_for(&self, win: &Window) -> Option<WlKeyboard> {
        let client = win.toplevel.as_ref()?.client()?;
        let cs = client.get_data::<ClientState>()?;
        let out = cs.inner.lock().ok()?.keyboard.clone();
        out
    }

    fn begin_forward(&mut self, win: u32, finger: u32) {
        let Some(w) = self.windows.iter().find(|w| w.id == win) else {
            return;
        };
        let Some(touch) = self.touch_res_for(w) else {
            return;
        };
        self.touch_focus = Some(win);
        let (x, y) = self.finger_pos(finger).unwrap_or((0.0, 0.0));
        let (lx, ly) = self.local_coords(w, x, y);
        let _ = touch.down(
            self.serial,
            self.present_time_ms as u32,
            &w.surface,
            finger as i32,
            lx as f64,
            ly as f64,
        );
        let _ = touch.frame();
        self.dirty = true;
    }

    fn forward_motion(&mut self, win: u32, x: f32, y: f32) {
        let Some(w) = self.windows.iter().find(|w| w.id == win) else {
            return;
        };
        let Some(touch) = self.touch_res_for(w) else {
            return;
        };
        let finger = self.gesture_finger() as i32;
        let (lx, ly) = self.local_coords(w, x, y);
        let _ = touch.motion(self.present_time_ms as u32, finger, lx as f64, ly as f64);
        let _ = touch.frame();
        self.dirty = true;
    }

    fn forward_up(&mut self, win: u32, finger: u32) {
        let Some(w) = self.windows.iter().find(|w| w.id == win) else {
            return;
        };
        let Some(touch) = self.touch_res_for(w) else {
            return;
        };
        let _ = touch.up(self.serial, self.present_time_ms as u32, finger as i32);
        let _ = touch.frame();
        if self.fingers.is_empty() {
            self.touch_focus = None;
        }
        self.dirty = true;
    }

    fn cancel_forwarded(&mut self) {
        if let Gesture::Forward { win, finger } = self.gesture.clone() {
            let Some(w) = self.windows.iter().find(|w| w.id == win) else {
                return;
            };
            let Some(touch) = self.touch_res_for(w) else {
                return;
            };
            let _ = touch.cancel();
            let _ = touch.frame();
            self.touch_focus = None;
        }
    }

    fn gesture_finger(&self) -> u32 {
        if let Gesture::Forward { finger, .. } = self.gesture {
            return finger;
        }
        self.fingers.keys().next().copied().unwrap_or(0)
    }

    fn finger_pos(&self, finger: u32) -> Option<(f32, f32)> {
        self.fingers.get(&finger).map(|f| (f.x, f.y))
    }

    /// The pressed keysyms as a wl_keyboard enter `keys` array (u32
    /// little-endian bytes — the 0.31 codegen maps the array to Vec<u8>).
    fn pressed_keys_bytes(&self) -> Vec<u8> {
        let mut out = Vec::new();
        for k in &self.pressed_keysyms {
            out.extend_from_slice(&k.to_le_bytes());
        }
        out
    }

    /// Surface-local logical coords (scene buffer rect -> client pixels;
    /// clients with 1:1 buffers get exact pixels).
    fn local_coords(&self, w: &Window, x: f32, y: f32) -> (f32, f32) {
        let (bx, by, bw, _bh) = w.buffer_rect();
        let Some(buf) = &w.buffer else {
            let ty = if w.csd { 0.0 } else { TITLEBAR_H };
            return (x - w.x, y - w.y - ty);
        };
        let scale = (bw / buf.w as f32).max(1e-6);
        ((x - bx) / scale, (y - by) / scale)
    }

    // -----------------------------------------------------------------
    // app launching

    fn launch_app(&mut self, idx: usize) {
        let Some(app) = self.apps.get(idx) else {
            return;
        };
        let Some(argv) = apps::exec_argv(&app.exec) else {
            log::warn!("no Exec= for {}", app.name);
            return;
        };
        log::info!("launch: {} ({})", app.name, app.exec);
        let prog = argv[0].clone();
        spawn_cmd(&prog, &argv[1..]);
    }

    /// Open the in-process egui settings panel. No client is launched:
    /// the shell UI is drawn by this compositor (GPU-tessellated meshes)
    /// from the shared `DataProvider` snapshots (2026-09-12).
    fn open_settings(&mut self) {
        self.settings_open = true;
        self.shell.state.close = false;
        self.shell.state.status.clear();
        // Instant: read the cache + queue a fresh snapshot on the worker.
        self.shell.refresh(&*self.data, self.ui_scale);
        self.data.refresh();
        self.settings_snapshot_ms = util::now_ms().saturating_sub(self.start_ms);
        self.dirty = true;
    }

    // -----------------------------------------------------------------
    // window management

    pub fn visible_windows(&self) -> Vec<&Window> {
        self.windows
            .iter()
            .filter(|w| !w.minimized && !w.is_popup && w.workspace as f32 == self.ws_target)
            .collect()
    }

    /// The window that currently has keyboard/touch focus, if any.
    pub fn focused_window(&self) -> Option<&Window> {
        let f = self.focus?;
        self.windows.iter().find(|w| w.id == f)
    }

    pub fn switcher_windows(&self) -> Vec<&Window> {
        self.windows
            .iter()
            .filter(|w| !w.minimized && !w.is_popup)
            .collect()
    }

    fn raise(&mut self, id: u32) {
        if let Some(pos) = self.windows.iter().position(|w| w.id == id) {
            let w = self.windows.remove(pos);
            self.windows.push(w);
            self.focus = Some(id);
            self.send_focus_change();
            self.dirty = true;
        }
    }

    fn switch_workspace(&mut self, target: f32) {
        self.ws_target = target.clamp(0.0, (N_WORKSPACES - 1) as f32);
        self.dirty = true;
    }

    fn next_app(&mut self) {
        let mut wins = self.visible_windows();
        if wins.is_empty() {
            return;
        }
        let cur = self
            .focus
            .and_then(|f| wins.iter().position(|w| w.id == f))
            .unwrap_or(0);
        let next = (cur + 1) % wins.len();
        self.raise(wins[next].id);
    }

    fn activate_switcher(&mut self) {
        let picked = self
            .switcher_windows()
            .get(self.switcher_index)
            .map(|w| (w.workspace, w.id));
        if let Some((ws, wid)) = picked {
            self.ws_target = ws as f32;
            self.raise(wid);
        }
    }

    /// Register a new toplevel for a surface (called from the
    /// xdg_surface.get_toplevel dispatch). Returns the window id.
    pub fn new_toplevel(&mut self, surface: WlSurface) -> Option<u32> {
        if self.windows.iter().any(|w| w.surface.id() == surface.id()) {
            return None;
        }
        let id = self.next_id();
        // Fill the work area by default: GNOME/CSD apps are complete
        // windows with their own header bar, and on a 5.7" screen an
        // inset 1000x700 window reads as unwanted padding. The taskbar
        // swipe-snap + titlebar double-tap still work to un-maximize.
        let (wx, wy, ww, wh) = self.work_area();
        self.windows.push(Window {
            id,
            toplevel: None,
            xdg_surface: None,
            surface,
            title: String::new(),
            app_id: String::new(),
            x: wx,
            y: wy,
            w: ww,
            h: wh,
            buffer: None,
            workspace: self.ws_target as u32,
            minimized: false,
            maximized: true,
            snap: Snap::Max,
            app: None,
            is_popup: false,
            parent: None,
            // CSD by default. GTK4 does NOT use xdg-decoration at all (it
            // only speaks the KDE org_kde_kwin_server_decoration_manager),
            // and with no KDE global GTK uses client-side decorations and
            // draws its own header bar. Defaulting to CSD here means gemshell
            // never draws the SSD titlebar on top of a client's own chrome
            // (the "2 close buttons" report). A client that explicitly asks
            // for server-side via xdg-decoration (`set_mode(ServerSide)`)
            // gets `csd = false` and the compositor titlebar instead.
            csd: true,
            decoration: None,
        });
        self.dirty = true;
        Some(id)
    }

    /// The toplevel resource is in (data_init created it in the dispatch).
    pub fn adopt_toplevel(&mut self, win: u32, toplevel: XdgToplevel, xdg_surface: XdgSurface) {
        self.focus = Some(win);
        if let Some(w) = self.windows.iter_mut().find(|w| w.id == win) {
            w.toplevel = Some(toplevel);
            w.xdg_surface = Some(xdg_surface);
        }
        self.send_focus_change();
        self.request_configure(win);
        self.dirty = true;
    }

    /// A client created `zxdg_toplevel_decoration_v1` for `win`.
    ///
    /// Reply **client-side** by default: GTK4 does not use this protocol at
    /// all (it decides CSD from the KDE decoration manager, which we do not
    /// advertise, so it always self-decorates), and Qt/wlroots clients that
    /// want server-side will follow up with `set_mode(ServerSide)`. Replying
    /// the accepted default keeps gemshell from drawing a second titlebar
    /// over a client's own header bar (2026-09-11).
    pub fn set_decoration(&mut self, win: u32, dec: ZxdgToplevelDecorationV1) {
        if let Some(w) = self.windows.iter_mut().find(|w| w.id == win) {
            w.decoration = Some(dec.clone());
            w.csd = true;
        }
        dec.configure(DecorationMode::ClientSide);
        self.dirty = true;
    }

    pub fn set_csd(&mut self, win: u32, csd: bool) {
        if let Some(w) = self.windows.iter_mut().find(|w| w.id == win) {
            if w.csd != csd {
                w.csd = csd;
                self.dirty = true;
            }
        }
    }

    pub fn clear_decoration(&mut self, win: u32) {
        if let Some(w) = self.windows.iter_mut().find(|w| w.id == win) {
            w.decoration = None;
            // Leave `csd` alone: destroying the decoration object says
            // nothing about whether the client still self-decorates.
            self.dirty = true;
        }
    }

    /// egui points-per-pixel including the UI scale: the panel is laid
    /// out in `lw/PPP` points and rendered at `PPP * ui_scale` device px
    /// per point, so its glyph atlas stays crisp at 150 / 200%.
    pub fn egui_ppp(&self) -> f32 {
        shell::PPP * self.ui_scale
    }

    /// Change the UI scale (1.0 / 1.5 / 2.0), from Settings > Display.
    /// Re-lays out the chrome/windows and re-configures clients so they
    /// can reflow; persists to `$HOME/.config/gemshell/scale`.
    pub fn set_ui_scale(&mut self, scale: f32) {
        let scale = scale.clamp(1.0, 2.0);
        if (scale - self.ui_scale).abs() < 0.001 {
            return;
        }
        self.ui_scale = scale;
        self.lw = W as f32 / scale;
        self.lh = H as f32 / scale;
        self.renderer.ui_scale = scale;
        // Re-fit maximized/snapped windows to the new work area. A client
        // renders at the output scale (wl_output.scale below), so its
        // logical size must follow `lw`/`lh`.
        let (lw, lh) = (self.lw, self.lh);
        let (wx, wy, ww, wh) = self.work_area();
        for w in self.windows.iter_mut() {
            if w.maximized || w.snap == Snap::Max {
                w.x = wx;
                w.y = wy;
                w.w = ww;
                w.h = wh;
            } else if w.snap != Snap::None {
                let (x, y, sw, sh) = snap_rect(w.snap, None, lw, lh);
                w.x = x;
                w.y = y;
                w.w = sw;
                w.h = sh;
            } else {
                w.x = w.x.clamp(0.0, (lw - w.w).max(0.0));
                w.y = w.y.clamp(STATUS_H, (lh - TASKBAR_H - 40.0).max(STATUS_H));
            }
        }
        // Tell clients the new output scale so they render at 1:1 with the
        // physical panel (crisp) instead of being upscaled (2026-09-11).
        self.broadcast_output_scale();
        // ...and reflow every toplevel.
        let ids: Vec<u32> = self
            .windows
            .iter()
            .filter(|w| !w.is_popup)
            .map(|w| w.id)
            .collect();
        for id in ids {
            self.request_configure(id);
        }
        self.save_ui_scale(scale);
        self.dirty = true;
        log::info!("ui scale -> {scale}");
    }

    /// The usable area (below the status bar, above the taskbar), logical.
    pub fn work_area(&self) -> (f32, f32, f32, f32) {
        (
            0.0,
            STATUS_H,
            self.lw,
            (self.lh - STATUS_H - TASKBAR_H).max(1.0),
        )
    }

    /// `xdg_toplevel.set_parent`: a toplevel with a parent is a transient
    /// dialog. New windows are maximized by default (no desktop padding),
    /// but a dialog should be a centered floating box instead.
    pub fn set_parent(&mut self, win: u32, has_parent: bool) {
        if !has_parent {
            return;
        }
        let (lw, lh) = (self.lw, self.lh);
        let (_, _, ww, wh) = self.work_area();
        let mut changed = false;
        if let Some(w) = self.windows.iter_mut().find(|w| w.id == win) {
            // Only before the first buffer: once mapped, honour the size
            // the client chose.
            if w.buffer.is_none() && w.maximized {
                let nw = (ww * 0.72).clamp(360.0, ww);
                let nh = (wh * 0.72).clamp(280.0, wh);
                w.w = nw;
                w.h = nh;
                w.x = ((lw - nw) / 2.0).max(0.0);
                w.y = (STATUS_H + (lh - STATUS_H - TASKBAR_H - nh) / 2.0).max(STATUS_H);
                w.maximized = false;
                w.snap = Snap::None;
                changed = true;
            }
        }
        if changed {
            self.request_configure(win);
            self.dirty = true;
        }
    }

    /// Re-send `wl_output.scale` (and a `done`) to every bound output, so
    /// clients render at ceil(ui_scale)x and the compositor never has to
    /// upscale an app's buffer (scale applies to APP content, not just
    /// gemshell's own chrome). `wl_output.scale` is an integer: 1.0 -> 1,
    /// 1.5 -> 2 (supersampled), 2.0 -> 2 (1:1).
    fn broadcast_output_scale(&self) {
        let s = self.ui_scale.ceil().max(1.0) as i32;
        for o in &self.outputs {
            let _ = o.scale(s);
            let _ = o.done();
        }
    }

    fn save_ui_scale(&self, scale: f32) {
        if let Some(home) = std::env::var_os("HOME") {
            let dir = std::path::PathBuf::from(home).join(".config/gemshell");
            let _ = std::fs::create_dir_all(&dir);
            if let Err(e) = std::fs::write(dir.join("scale"), format!("{scale}")) {
                log::warn!("save ui scale: {e}");
            }
        }
    }

    fn load_ui_scale() -> f32 {
        std::env::var_os("HOME")
            .map(std::path::PathBuf::from)
            .and_then(|h| std::fs::read_to_string(h.join(".config/gemshell/scale")).ok())
            .and_then(|s| s.trim().parse::<f32>().ok())
            .map(|s| s.clamp(1.0, 2.0))
            .unwrap_or(1.0)
    }

    /// `xdg_toplevel.move` from a client (a CSD header-bar drag). We keep
    /// the touch forwarded to the client and additionally move the window
    /// with the same finger, so the client's drag gesture still completes
    /// (a bare `Gesture::Move` would starve it of motion/up events).
    pub fn begin_client_move(&mut self, win: u32) {
        // Prefer the finger that is currently forwarded to this window.
        let forwarded = match &self.gesture {
            Gesture::Forward { win: w, finger } if *w == win => Some(*finger),
            _ => None,
        };
        let finger = forwarded.or_else(|| self.fingers.keys().next().copied());
        let Some(finger) = finger else { return };
        let (fx, fy) = self
            .fingers
            .get(&finger)
            .map(|f| (f.x, f.y))
            .unwrap_or((0.0, 0.0));
        let (wx, wy) = self
            .windows
            .iter()
            .find(|w| w.id == win)
            .map(|w| (w.x, w.y))
            .unwrap_or((0.0, 0.0));
        self.raise(win);
        self.client_move = Some((win, finger, fx - wx, fy - wy));
        // Un-maximize so the window can actually be dragged.
        let needs_unsnap = self
            .windows
            .iter()
            .find(|w| w.id == win)
            .map(|w| w.maximized || w.snap != Snap::None)
            .unwrap_or(false);
        if needs_unsnap {
            self.unmaximize(win);
        }
        self.snap_preview = Snap::None;
        self.snap_win = Some(win);
        self.dirty = true;
    }

    pub fn new_popup(
        &mut self,
        surface: WlSurface,
        parent: Option<u32>,
        w: f32,
        h: f32,
        x: f32,
        y: f32,
    ) -> Option<u32> {
        if self
            .windows
            .iter()
            .any(|w2| w2.surface.id() == surface.id())
        {
            return None;
        }
        let id = self.next_id();
        self.windows.push(Window {
            id,
            toplevel: None,
            xdg_surface: None,
            surface,
            title: String::new(),
            app_id: String::new(),
            x: x.clamp(0.0, self.lw - 10.0),
            y: y.clamp(STATUS_H, self.lh - TASKBAR_H - 10.0),
            w: w.clamp(40.0, self.lw as f32),
            h: h.clamp(40.0, self.lh as f32),
            buffer: None,
            workspace: self.ws_target as u32,
            minimized: false,
            maximized: false,
            snap: Snap::None,
            app: None,
            is_popup: true,
            parent,
            csd: false,
            decoration: None,
        });
        self.dirty = true;
        Some(id)
    }

    pub fn adopt_popup(&mut self, win: u32, popup: XdgPopup, xdg_surface: XdgSurface) {
        if let Some(w) = self.windows.iter_mut().find(|w| w.id == win) {
            w.toplevel = None;
            w.xdg_surface = Some(xdg_surface);
            let _ = popup; // popups carry no toplevel; kept alive via the popup resource in XdgPopupData
        }
    }

    /// The anchor rect for popup placement (parent window rect, or a
    /// default in the middle).
    pub fn popup_anchor(&self, parent: Option<u32>) -> (f32, f32, f32, f32) {
        parent
            .and_then(|p| self.windows.iter().find(|w| w.id == p))
            .map(|w| (w.x, w.y, w.w, w.h))
            .unwrap_or((self.lw / 2.0, self.lh / 2.0, 200.0, 200.0))
    }

    pub fn set_title(&mut self, win: u32, title: String) {
        if let Some(w) = self.windows.iter_mut().find(|w| w.id == win) {
            w.title = title;
            if w.app_id.is_empty() {
                w.app_id = w.title.clone();
            }
            w.app = self
                .apps
                .iter()
                .position(|a| a.name == w.title || a.name == w.app_id);
            self.dirty = true;
        }
    }

    pub fn set_app_id(&mut self, win: u32, app_id: String) {
        if let Some(w) = self.windows.iter_mut().find(|w| w.id == win) {
            w.app_id = app_id.clone();
            w.app = self
                .apps
                .iter()
                .position(|a| a.name == app_id || a.name == w.title);
            self.dirty = true;
        }
    }

    pub fn close_window(&mut self, win: u32) {
        let idx = match self.windows.iter().position(|w| w.id == win) {
            Some(i) => i,
            None => return,
        };
        let w = self.windows.remove(idx);
        drop(w.toplevel);
        if self.focus == Some(win) {
            self.focus = self
                .windows
                .iter()
                .rev()
                .find(|w| !w.minimized && !w.is_popup)
                .map(|w| w.id);
            self.send_focus_change();
        }
        self.renderer.drop_window_texture(win);
        self.dirty = true;
    }

    /// Ask a client to close its window (`xdg_toplevel.close`). The window
    /// is actually removed when the client destroys the toplevel, which
    /// dispatches `XdgToplevelRequest::Destroy` → [`close_window`].
    /// Dropping the server resource instead (the old behaviour) forgot the
    /// window while the app kept running — the "Close does nothing"
    /// report (2026-09-12).
    pub fn request_close(&mut self, win: u32) {
        let toplevel = self
            .windows
            .iter()
            .find(|w| w.id == win)
            .and_then(|w| w.toplevel.as_ref());
        match toplevel {
            Some(t) => {
                // `close` is the generated xdg_toplevel.close() event send.
                let _ = t.close();
                self.dirty = true;
            }
            // No xdg_toplevel (a popup): just drop it.
            None => self.close_window(win),
        }
    }

    pub fn minimize(&mut self, win: u32) {
        if let Some(w) = self.windows.iter_mut().find(|w| w.id == win) {
            w.minimized = true;
            if self.focus == Some(win) {
                self.focus = self
                    .windows
                    .iter()
                    .rev()
                    .find(|w| !w.minimized && !w.is_popup)
                    .map(|w| w.id);
                self.send_focus_change();
            }
            self.dirty = true;
        }
    }

    pub fn set_maximized(&mut self, win: u32, maximized: bool) {
        if let Some(w) = self.windows.iter_mut().find(|w| w.id == win) {
            if maximized {
                w.maximized = true;
                w.snap = Snap::Max;
                w.x = 0.0;
                w.y = STATUS_H;
                w.w = self.lw;
                w.h = self.lh - STATUS_H - TASKBAR_H;
            } else {
                self.unmaximize(win);
                return;
            }
        }
        self.request_configure(win);
        self.dirty = true;
    }

    fn unmaximize(&mut self, win: u32) {
        let (lw, lh) = (self.lw, self.lh);
        let (_, _, ww, wh) = self.work_area();
        if let Some(w) = self.windows.iter_mut().find(|w| w.id == win) {
            // Restore to a floating size instead of keeping the work-area
            // size: dragging a maximized window must visibly shrink it.
            if w.maximized || w.snap != Snap::None {
                let nw = (ww * 0.78).clamp(360.0, ww);
                let nh = (wh * 0.82).clamp(280.0, wh);
                w.x = w.x.clamp(0.0, (lw - nw).max(0.0));
                w.y = w.y.clamp(STATUS_H, (lh - TASKBAR_H - nh).max(STATUS_H));
                w.w = nw;
                w.h = nh;
            }
            w.maximized = false;
            w.snap = Snap::None;
        }
        self.request_configure(win);
        self.dirty = true;
    }

    fn toggle_maximize_focused(&mut self) {
        let Some(f) = self.focus else { return };
        let Some(m) = self
            .windows
            .iter()
            .find(|w| w.id == f)
            .map(|w| !w.maximized)
        else {
            return;
        };
        self.set_maximized(f, m);
    }

    fn snap_focused(&mut self, snap: Snap) {
        if let Some(f) = self.focus {
            self.apply_snap(f, snap);
        }
    }

    fn apply_snap(&mut self, win: u32, snap: Snap) {
        let Some(w) = self.windows.iter_mut().find(|w| w.id == win) else {
            return;
        };
        let sw = self.lw;
        let st = STATUS_H;
        let sb = self.lh - TASKBAR_H;
        w.maximized = false;
        match snap {
            Snap::Left => {
                w.snap = Snap::Left;
                w.x = 0.0;
                w.y = st;
                w.w = sw / 2.0;
                w.h = sb - st;
            }
            Snap::Right => {
                w.snap = Snap::Right;
                w.x = sw / 2.0;
                w.y = st;
                w.w = sw / 2.0;
                w.h = sb - st;
            }
            Snap::Top => {
                w.snap = Snap::Top;
                w.x = 0.0;
                w.y = st;
                w.w = sw;
                w.h = (sb - st) / 2.0;
            }
            Snap::Bottom => {
                w.snap = Snap::Bottom;
                w.x = 0.0;
                w.y = (st + sb) / 2.0;
                w.w = sw;
                w.h = (sb - st) / 2.0;
            }
            Snap::Max => {
                w.snap = Snap::Max;
                w.maximized = true;
                w.x = 0.0;
                w.y = st;
                w.w = sw;
                w.h = sb - st;
            }
            Snap::None => {
                w.snap = Snap::None;
            }
        }
        self.request_configure(win);
        self.dirty = true;
    }

    fn request_configure(&mut self, win: u32) {
        // Bump the serial before borrowing the window (the xdg_surface
        // configure below must carry a fresh one).
        self.configure_serial = self.configure_serial.wrapping_add(1).max(1);
        let serial = self.configure_serial;
        let Some(w) = self.windows.iter().find(|w| w.id == win) else {
            return;
        };
        let Some(t) = w.toplevel.as_ref() else {
            return;
        };
        // NOTE: no ToplevelState::Active — the xdg-shell State enum at
        // this protocol version has no `active` member; focus is
        // communicated via wl_keyboard.enter instead.
        let states = match w.snap {
            Snap::Left => vec![ToplevelState::TiledLeft],
            Snap::Right => vec![ToplevelState::TiledRight],
            Snap::Top => vec![ToplevelState::TiledTop],
            Snap::Bottom => vec![ToplevelState::TiledBottom],
            Snap::Max => vec![ToplevelState::Maximized],
            Snap::None => Vec::new(),
        };
        let (cw, ch) = if w.maximized || w.snap != Snap::None {
            let chrome = w.chrome_h();
            (w.w as i32, (w.h - chrome) as i32)
        } else {
            (0, 0)
        };
        let states_bytes: Vec<u8> = states.into_iter().map(|s| s as u8).collect();
        // NOTE: generated order is (width, height, states)
        let _ = t.configure(cw, ch, states_bytes);
        // Every toplevel configure must be followed by an
        // xdg_surface.configure carrying a serial; the client acks that
        // serial and only then attaches its first buffer. The original
        // send-only-toplevel-configure meant NO standard client could
        // ever map (found writing gemsettings, 2026-09-11).
        if let Some(xs) = w.xdg_surface.as_ref() {
            let _ = xs.configure(serial);
        }
    }

    // -----------------------------------------------------------------
    // keyboard focus (wl_keyboard fan-out)

    fn send_focus_change(&mut self) {
        let focus = self.focus;
        // snapshot (client, owns-focus-window) — the loop below
        // immutably borrows self.windows per client
        let owned: Vec<(Client, Option<u32>)> = self
            .tracked_clients
            .iter()
            .cloned()
            .map(|client| {
                let owns = focus.and_then(|f| {
                    self.windows
                        .iter()
                        .find(|w| w.id == f)
                        .and_then(|w| w.toplevel.as_ref())
                        .and_then(|t| t.client())
                        .filter(|c| c.id() == client.id())
                        .map(|_| f)
                });
                (client, owns)
            })
            .collect();
        for (client, new_focus) in owned {
            let Some(cs) = client.get_data::<ClientState>() else {
                continue;
            };
            let mut inner = match cs.inner.lock().ok() {
                Some(i) => i,
                None => continue,
            };
            let Some(kbd) = inner.keyboard.as_ref() else {
                continue;
            };
            let old = inner.kbd_focus;
            if old == new_focus {
                continue;
            }
            let old_surf = old
                .and_then(|id| self.windows.iter().find(|w| w.id == id))
                .map(|w| w.surface.clone());
            let new_surf = new_focus
                .and_then(|id| self.windows.iter().find(|w| w.id == id))
                .map(|w| w.surface.clone());
            if let Some(ow) = old_surf {
                let _ = kbd.leave(self.serial, &ow);
            }
            if let Some(nw) = new_surf {
                let keys = self.pressed_keys_bytes();
                let _ = kbd.enter(self.serial, &nw, keys);
                let (d, l, k) = self.input.mods_masks();
                let _ = kbd.modifiers(self.serial, d, l, k, 0);
            }
            inner.kbd_focus = new_focus;
        }
    }

    /// Called from the wl_seat.get_keyboard dispatch: send the keymap
    /// (and repeat info) to the new keyboard, and enter if it has focus.
    pub fn keyboard_bound(&mut self, client: &Client, kbd: &WlKeyboard) {
        let Some(km) = &self.keymap_str else {
            return;
        };
        if let Some((fd, size)) = keymap_fd(km) {
            let _ = kbd.keymap(KeymapFormat::XkbV1, fd.as_fd(), size);
            let _ = kbd.repeat_info(25, 500);
        }
        // if this client already owns the focus, enter now
        let focus_win = self.focus.and_then(|f| {
            self.windows
                .iter()
                .find(|w| w.id == f)
                .map(|w| (w.surface.clone(), w.id))
        });
        let has_focus = focus_win
            .as_ref()
            .and_then(|(s, _)| {
                self.windows
                    .iter()
                    .find(|w| w.surface.id() == s.id())
                    .and_then(|w| w.toplevel.as_ref())
                    .and_then(|t| t.client())
                    .map(|c| c.id() == client.id())
            })
            .unwrap_or(false);
        if has_focus {
            if let Some((surf, _)) = focus_win {
                let keys = self.pressed_keys_bytes();
                let _ = kbd.enter(self.serial, &surf, keys);
                let (d, l, k) = self.input.mods_masks();
                let _ = kbd.modifiers(self.serial, d, l, k, 0);
            }
            if let Some(cs) = client.get_data::<ClientState>() {
                if let Ok(mut inner) = cs.inner.lock() {
                    inner.kbd_focus = self.focus;
                }
            }
        }
    }

    // -----------------------------------------------------------------
    // wm-base

    pub fn wm_base_pong(&mut self, _serial: u32) {}

    // -----------------------------------------------------------------
    // surface commits

    pub fn surface_commit(&mut self, surface: &WlSurface) {
        let Some(data) = surface.data::<SurfaceData>() else {
            return;
        };
        let (buf, win) = {
            let inner = data.inner.lock().unwrap();
            (inner.pending_buffer.clone(), inner.window)
        };
        let Some(buf) = buf else {
            return;
        };
        let Some(win) = win else {
            return;
        };
        let map: &[u8] = &buf.map[..];
        let tex = self
            .renderer
            .window_texture(win, buf.w, buf.h, buf.stride, buf.format, map);
        if let Some(w) = self.windows.iter_mut().find(|w| w.id == win) {
            w.buffer = Some(buf.clone());
            if !w.maximized && w.snap == Snap::None && !w.is_popup {
                let bw = (buf.w as f32).min(self.lw - 16.0);
                let bh = (buf.h as f32).min(self.lh - STATUS_H - TASKBAR_H - TITLEBAR_H - 16.0);
                if (bw - w.w).abs() > 1.0 || (bh - w.h).abs() > 1.0 {
                    w.w = bw;
                    w.h = bh;
                    w.x = w.x.clamp(0.0, self.lw - bw);
                    w.y = w.y.clamp(STATUS_H, self.lh - TASKBAR_H - bh);
                }
            }
            let _ = tex;
        }
        self.dirty = true;
    }

    pub fn surface_detached(&mut self, surface: &WlSurface) {
        if let Some(w) = self
            .windows
            .iter_mut()
            .find(|w| w.surface.id() == surface.id())
        {
            w.buffer = None;
            self.renderer.drop_window_texture(w.id);
        }
        self.dirty = true;
    }

    pub fn xdg_surface_detached(&mut self, surface: WlSurface) {
        if let Some(w) = self
            .windows
            .iter_mut()
            .find(|w| w.surface.id() == surface.id())
        {
            w.buffer = None;
            self.renderer.drop_window_texture(w.id);
        }
        self.dirty = true;
    }

    pub fn window_texture_id(&self, win: u32) -> Option<u32> {
        self.renderer.window_texture_id(win)
    }

    fn send_frame_callbacks(&mut self) {
        for w in &self.windows {
            let Some(data) = w.surface.data::<SurfaceData>() else {
                continue;
            };
            let cbs = std::mem::take(&mut data.inner.lock().unwrap().frame_cbs);
            for cb in cbs {
                let _ = cb.done(self.present_time_ms as u32);
            }
        }
    }

    // -----------------------------------------------------------------
    // rendering

    fn render_frame(&mut self) {
        // The renderer maps logical units (lw×lh) across the physical
        // viewport; keep it in sync with the Settings scale.
        self.renderer.ui_scale = self.ui_scale;
        // Run the egui settings panel FIRST: it can close on this frame
        // (clearing its primitives), and while it is up it is a
        // full-screen opaque card, so the scene underneath is invisible.
        let egui_frame = self.run_egui();

        let mut ops: Vec<render::Op> = Vec::with_capacity(256);
        // Always paint the wallpaper: the panel is opaque, but this keeps
        // a defined frame if it ever has a transparent edge.
        ui::draw_background(&mut ops, self.lw, self.lh);

        // The panel covers the scene, and the scene is the expensive part
        // on the A53s — skip the hidden windows/status/taskbar while the
        // modal is up so a panel tap renders one overlay, not
        // scene+overlay (settings input latency, 2026-09-12).
        if !self.settings_open {
            // windows per visible workspace (with the slide offset)
            let p = self.ws_pos;
            for ws in 0..N_WORKSPACES {
                let off = (ws as f32 - p) * self.lw;
                if off < -(self.lw) || off > self.lw {
                    continue;
                }
                for win in self
                    .windows
                    .iter()
                    .filter(|w| w.workspace == ws && !w.minimized)
                {
                    ui::draw_window(&mut ops, self, win, off);
                }
            }

            if self.snap_preview != Snap::None {
                ui::draw_snap_preview(&mut ops, self);
            }
            ui::draw_status_bar(&mut ops, self);
            ui::draw_taskbar(&mut ops, self);
            if self.launcher_open {
                ui::draw_launcher(&mut ops, self);
            }
            if self.switcher_open {
                ui::draw_switcher(&mut ops, self);
            }
            ui::draw_workspace_dots(&mut ops, self);
            if self.touch_trail {
                ui::draw_touch_trail(&mut ops, self);
            }
        }

        self.renderer.begin_frame();
        self.renderer.replay(&self.font, &ops);
        if let Some(frame) = &egui_frame {
            self.renderer
                .draw_egui(&frame.primitives, &frame.textures, self.egui_ppp());
        }
        if let Err(e) = self.renderer.present() {
            log::error!("present: {e}");
        }
        // One-shot screenshot for on-glass verification:
        // GEMSHELL_SCREENSHOT=/path writes a frame as a PNG, after
        // GEMSHELL_SCREENSHOT_DELAY_MS (default 0) so a client launched
        // just after the compositor is on screen too.
        if !SHOT_DONE.load(std::sync::atomic::Ordering::Relaxed) {
            if let Ok(path) = std::env::var("GEMSHELL_SCREENSHOT") {
                let delay: u64 = std::env::var("GEMSHELL_SCREENSHOT_DELAY_MS")
                    .ok()
                    .and_then(|s| s.parse().ok())
                    .unwrap_or(0);
                if util::now_ms().saturating_sub(self.start_ms) >= delay {
                    SHOT_DONE.store(true, std::sync::atomic::Ordering::Relaxed);
                    match self.renderer.screenshot(&path) {
                        Ok(()) => log::info!("screenshot written: {path}"),
                        Err(e) => log::error!("screenshot: {e}"),
                    }
                }
            }
        }
        self.send_frame_callbacks();
    }

    fn next_id(&mut self) -> u32 {
        self.windows.iter().map(|w| w.id).max().unwrap_or(0) + 1
    }

    fn dismiss_popups_outside(&mut self, x: f32, y: f32) {
        let mut close = Vec::new();
        for w in self.windows.iter().filter(|w| w.is_popup) {
            if !(x >= w.x && x <= w.x + w.w && y >= w.y && y <= w.y + w.h) {
                close.push(w.id);
            }
        }
        for id in close {
            self.close_window(id);
        }
    }
}

// ---- helpers ----

/// The snap rect for a zone (drawn by ui, applied to windows by the
/// compositor) — one source of truth.
pub fn snap_rect(snap: Snap, _win: Option<u32>, lw: f32, lh: f32) -> (f32, f32, f32, f32) {
    let sw = lw;
    let st = STATUS_H;
    let sb = lh - TASKBAR_H;
    match snap {
        Snap::Left => (0.0, st, sw / 2.0, sb - st),
        Snap::Right => (sw / 2.0, st, sw / 2.0, sb - st),
        Snap::Top => (0.0, st, sw, (sb - st) / 2.0),
        Snap::Bottom => (0.0, (st + sb) / 2.0, sw, (sb - st) / 2.0),
        _ => (0.0, st, sw, sb - st),
    }
}

/// xkb modifier-state mask -> egui modifiers. Slot numbering from the
/// xkb modmap: 1=Shift 2=Lock 3=Control 4=Mod1(Alt) 7=Mod4(Super);
/// Mod5 is the Gemini Fn key (level 3 / AltGr-like).
fn egui_mods(mods: u32) -> egui::Modifiers {
    egui::Modifiers {
        alt: mods & (1 << 4) != 0,
        ctrl: mods & (1 << 3) != 0,
        shift: mods & (1 << 1) != 0,
        mac_cmd: false,
        command: mods & (1 << 7) != 0,
    }
}

/// X11 keysym -> egui logical key, for the editing/navigation keys egui
/// cares about (printable characters travel as `Event::Text`).
fn egui_key_from_keysym(keysym: u32) -> Option<egui::Key> {
    use egui::Key;
    Some(match keysym {
        0xff08 => Key::Backspace,
        0xff09 => Key::Tab,
        0xff0d | 0xff8d => Key::Enter,
        0xff1b => Key::Escape,
        0xffff => Key::Delete,
        0xff50 => Key::Home,
        0xff57 => Key::End,
        0xff55 => Key::PageUp,
        0xff56 => Key::PageDown,
        0xff51 => Key::ArrowLeft,
        0xff52 => Key::ArrowUp,
        0xff53 => Key::ArrowRight,
        0xff54 => Key::ArrowDown,
        _ => return None,
    })
}

/// X11 keysym -> the text it produces, if printable. The "gemini" layout
/// resolves Fn level-3 glyphs to their Unicode keysyms, so this is all
/// the keyboard plumbing the password field needs.
fn keysym_text(keysym: u32) -> Option<String> {
    let c = match keysym {
        0x20..=0x7e | 0xa0..=0xff => char::from_u32(keysym)?,
        0x0100..=0xefff | 0x10000..=0x10ffff => char::from_u32(keysym)?,
        k if k >= 0x0100_0000 => char::from_u32(k - 0x0100_0000)?,
        _ => return None,
    };
    Some(c.to_string())
}

// `scale_pt`/`pollfd` moved to the Linux backend (`platform::linux`),
// the only place that polls fds.

// find_font/walk now live in crate::common::util (shared with
// gemsettings).

fn spawn_cmd<P: AsRef<std::ffi::OsStr>, A: AsRef<std::ffi::OsStr>, I: IntoIterator<Item = A>>(
    prog: P,
    args: I,
) {
    let prog = prog.as_ref().to_os_string();
    let args: Vec<std::ffi::OsString> = args
        .into_iter()
        .map(|a| a.as_ref().to_os_string())
        .collect();
    let name = prog.to_string_lossy().into_owned();
    std::thread::Builder::new()
        .name("spawn".into())
        .spawn(move || {
            // Log the outcome: a bare Exec name that is not on PATH or a
            // client that dies at startup used to fail silently, so
            // "apps don't launch" had no receipt (2026-09-11). The child
            // inherits our stdout/stderr, so its own errors land in the
            // gemini-gemshell journal too.
            match std::process::Command::new(&prog).args(&args).spawn() {
                Ok(child) => log::info!("spawned {name} (pid {})", child.id()),
                Err(e) => log::error!("spawn {name}: {e}"),
            }
        })
        .ok();
}
