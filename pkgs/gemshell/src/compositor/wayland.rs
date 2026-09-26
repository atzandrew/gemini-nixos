//! Wayland protocol state — the globals, per-client objects and buffer
//! bookkeeping. Every request handler funnels into a method on
//! [`super::Compositor`] (window/input policy lives there).
//!
//! User data uses interior mutability (`Arc<Mutex<...>>`) — the
//! wayland-server 0.31 Resource API only exposes `&U` (smithay's
//! pattern).
//!
//! Globals: wl_compositor, wl_shm (XRGB8888 + ARGB8888), wl_seat
//! (keyboard+pointer+touch), wl_output, xdg_wm_base (xdg-shell v1).
//! No data-device (clipboard) in v1 — the settings app is built
//! without clipboard support for exactly this reason.

use std::os::unix::io::{FromRawFd, OwnedFd};
use std::sync::{Arc, Mutex};

use wayland_server::protocol::wl_buffer::WlBuffer;
use wayland_server::protocol::wl_callback::WlCallback;
use wayland_server::protocol::wl_compositor::WlCompositor;
// The data-device manager is REQUIRED by GTK4 even for apps that never
// touch the clipboard: gdkdisplay-wayland.c refuses the whole display if
// the compositor does not expose `wl_data_device_manager`
// ("The Wayland compositor does not provide one or more of the required
// interfaces, not using Wayland display"). Without it every GTK app
// launched from the launcher exited immediately (2026-09-11).
use wayland_protocols::xdg::shell::server::xdg_popup::XdgPopup;
use wayland_protocols::xdg::shell::server::xdg_positioner::{Anchor, XdgPositioner};
use wayland_protocols::xdg::shell::server::xdg_surface::XdgSurface;
use wayland_protocols::xdg::shell::server::xdg_toplevel::XdgToplevel;
use wayland_protocols::xdg::shell::server::xdg_wm_base::XdgWmBase;
use wayland_server::protocol::wl_data_device::WlDataDevice;
use wayland_server::protocol::wl_data_device_manager::WlDataDeviceManager;
use wayland_server::protocol::wl_data_source::WlDataSource;
use wayland_server::protocol::wl_keyboard::WlKeyboard;
use wayland_server::protocol::wl_output::WlOutput;
use wayland_server::protocol::wl_pointer::WlPointer;
use wayland_server::protocol::wl_region::WlRegion;
use wayland_server::protocol::wl_seat::{Capability, WlSeat};
use wayland_server::protocol::wl_shm::WlShm;
use wayland_server::protocol::wl_shm_pool::WlShmPool;
use wayland_server::protocol::wl_surface::WlSurface;
use wayland_server::protocol::wl_touch::WlTouch;
use wayland_server::{
    backend::ClientData, Client, DataInit, Dispatch, DisplayHandle, GlobalDispatch, Resource,
};
// xdg-decoration: without this GTK4 assumes CSD and draws its own title
// bar while we draw the compositor titlebar too ("2 close buttons per
// window", reported on glass 2026-09-11). We expose the global and
// negotiate: clients that speak it get CSD (GNOME/libadwaita apps draw a
// header bar regardless), clients that don't keep our SSD titlebar.
use wayland_protocols::xdg::decoration::zv1::server::zxdg_decoration_manager_v1::{
    Request as ZxdgDecorationManagerV1Request, ZxdgDecorationManagerV1,
};
use wayland_protocols::xdg::decoration::zv1::server::zxdg_toplevel_decoration_v1::{
    Mode as DecorationMode, Request as ZxdgToplevelDecorationV1Request, ZxdgToplevelDecorationV1,
};

use wayland_protocols::xdg::shell::server::xdg_popup::Request as XdgPopupRequest;
use wayland_protocols::xdg::shell::server::xdg_positioner::Request as XdgPositionerRequest;
use wayland_protocols::xdg::shell::server::xdg_surface::Request as XdgSurfaceRequest;
use wayland_protocols::xdg::shell::server::xdg_toplevel::Request as XdgToplevelRequest;
use wayland_protocols::xdg::shell::server::xdg_wm_base::Request as XdgWmBaseRequest;
use wayland_server::protocol::wl_buffer::Request as WlBufferRequest;
use wayland_server::protocol::wl_callback::Request as WlCallbackRequest;
use wayland_server::protocol::wl_compositor::Request as WlCompositorRequest;
use wayland_server::protocol::wl_data_device::Request as WlDataDeviceRequest;
use wayland_server::protocol::wl_data_device_manager::Request as WlDataDeviceManagerRequest;
use wayland_server::protocol::wl_data_source::Request as WlDataSourceRequest;
use wayland_server::protocol::wl_keyboard::Request as WlKeyboardRequest;
use wayland_server::protocol::wl_output::Request as WlOutputRequest;
use wayland_server::protocol::wl_pointer::Request as WlPointerRequest;
use wayland_server::protocol::wl_region::Request as WlRegionRequest;
use wayland_server::protocol::wl_seat::Request as WlSeatRequest;
use wayland_server::protocol::wl_shm::Event as WlShmEvent;
use wayland_server::protocol::wl_shm::Request as WlShmRequest;
use wayland_server::protocol::wl_shm_pool::Request as WlShmPoolRequest;
use wayland_server::protocol::wl_surface::Request as WlSurfaceRequest;
use wayland_server::protocol::wl_touch::Request as WlTouchRequest;

use super::Compositor;

// ---------------------------------------------------------------------------
// user data

/// wl_surface state (mutable parts behind the mutex).
#[derive(Default)]
pub struct SurfaceData {
    pub inner: Mutex<SurfaceInner>,
}

#[derive(Default)]
pub struct SurfaceInner {
    /// the window this surface belongs to (set when the toplevel/popup
    /// is created)
    pub window: Option<u32>,
    pub pending_buffer: Option<Arc<BufferData>>,
    /// The `wl_buffer` resource attached for the next commit, kept so the
    /// compositor can send `wl_buffer.release` once its pixels have been
    /// copied into a GL texture. A compositor MUST release a buffer when
    /// done with it; without this, wl_shm clients (GTK4) can never recycle
    /// a buffer and allocate a brand-new shm pool every frame — a single
    /// `gnome-calculator` grew to 920 MB RSS in ~27 s on glass, exhausting
    /// RAM (the "Purging …" panfrost reclaim storm + freeze, 2026-09-26).
    pub pending_buffer_wl: Option<WlBuffer>,
    /// true if the client attached/damaged since the last commit
    pub damaged: bool,
    /// frame callbacks waiting for the next present
    pub frame_cbs: Vec<WlCallback>,
    /// true once committed with a buffer (mapped)
    pub mapped: bool,
    /// `wl_surface.set_buffer_scale` (default 1). Input events
    /// (wl_touch/wl_pointer) are in SURFACE-LOCAL coordinates — the
    /// buffer size divided by this scale — not buffer pixels. Ignoring
    /// it delivered 2x-too-large touch coords at ui_scale 1.5 (the
    /// "apps don't respond to touch" report, 2026-09-26).
    pub buffer_scale: i32,
}

/// A wl_shm buffer (a view into its pool). Immutable after creation.
#[derive(Clone, Debug)]
pub struct BufferData {
    pub map: Arc<memmap2::Mmap>,
    pub offset: usize,
    pub w: u32,
    pub h: u32,
    pub stride: u32,
    /// wl_shm format fourcc
    pub format: u32,
}

impl BufferData {
    /// Row `y` as a byte slice (BGRX / BGRA byte order on little-endian).
    pub fn row(&self, y: u32) -> &[u8] {
        let start = self.offset + (y * self.stride) as usize;
        &self.map[start..start + self.w as usize * 4]
    }
}

/// A wl_shm_pool (an mmap of the client's fd).
pub struct PoolData {
    pub map: Arc<memmap2::Mmap>,
    /// keep the fd alive for the pool's lifetime
    pub fd: OwnedFd,
    pub size: usize,
}

pub struct XdgSurfaceData {
    pub surface: WlSurface,
    /// the toplevel/popup window id, once created (the compositor also
    /// tracks this; the field is set via interior mutability so the
    /// Destroy handler can enforce the role rule)
    pub window: Mutex<Option<u32>>,
}

impl XdgSurfaceData {
    pub fn new(surface: WlSurface) -> Self {
        XdgSurfaceData {
            surface,
            window: Mutex::new(None),
        }
    }
}

#[derive(Clone)]
pub struct XdgToplevelData {
    pub window: u32,
}

#[derive(Clone)]
pub struct XdgPopupData {
    pub window: u32,
    /// parent toplevel window id
    pub parent: Option<u32>,
}

#[derive(Default)]
pub struct PositionerData {
    pub inner: Mutex<PositionerInner>,
}

#[derive(Clone, Copy)]
pub struct PositionerInner {
    pub w: i32,
    pub h: i32,
    pub offset_x: i32,
    pub offset_y: i32,
    pub anchor: Anchor,
}

impl Default for PositionerInner {
    fn default() -> Self {
        PositionerInner {
            w: 0,
            h: 0,
            offset_x: 0,
            offset_y: 0,
            anchor: Anchor::None,
        }
    }
}

#[derive(Default, Clone)]
pub struct OutputData {}
#[derive(Default, Clone)]
pub struct CompositorData {}
#[derive(Default, Clone)]
pub struct ShmData {}
#[derive(Default, Clone)]
pub struct RegionData {}
#[derive(Default, Clone)]
pub struct WmBaseData {}

#[derive(Default)]
pub struct DecorationManagerData {}

#[derive(Clone)]
pub struct ToplevelDecorationData {
    pub window: u32,
}
/// Per-client state (interior mutability: the Resource API only gives
/// &U).
#[derive(Debug, Default)]
pub struct ClientState {
    pub inner: Mutex<ClientInner>,
}

#[derive(Debug, Default)]
pub struct ClientInner {
    pub seat: Option<WlSeat>,
    pub keyboard: Option<WlKeyboard>,
    pub pointer: Option<WlPointer>,
    pub touch: Option<WlTouch>,
    /// the window this client currently has keyboard focus on
    pub kbd_focus: Option<u32>,
}

impl ClientData for ClientState {}

// ---------------------------------------------------------------------------
// globals + requests
//
// wayland-server 0.31 API: `Dispatch`/`GlobalDispatch` impls live on the
// State type (Compositor) — `fn request(state: &mut State, ...)` with the
// resource data as a plain `&U` (no interior mutability in the handlers;
// per-client state lives in `ClientState` via `Client::get_data`).
// `data_init.init(res, u)` requires `Compositor: Dispatch<I, U>` for the
// pair, and `Display::create_global` requires `Compositor:
// GlobalDispatch<I, U>`.

impl GlobalDispatch<WlCompositor, CompositorData, Compositor> for Compositor {
    fn bind(
        _state: &mut Compositor,
        _handle: &DisplayHandle,
        _client: &Client,
        resource: wayland_server::New<WlCompositor>,
        _global_data: &CompositorData,
        data_init: &mut DataInit<'_, Compositor>,
    ) {
        let _ = data_init.init(resource, CompositorData {});
    }
}

impl GlobalDispatch<WlShm, ShmData, Compositor> for Compositor {
    fn bind(
        _state: &mut Compositor,
        _handle: &DisplayHandle,
        _client: &Client,
        resource: wayland_server::New<WlShm>,
        _global_data: &ShmData,
        data_init: &mut DataInit<'_, Compositor>,
    ) {
        let res = data_init.init(resource, ShmData {});
        use wayland_server::protocol::wl_shm::{Format, WlShm};
        let _ = res.send_event(WlShmEvent::Format {
            format: wayland_server::WEnum::Value(Format::Xrgb8888),
        });
        let _ = res.send_event(WlShmEvent::Format {
            format: wayland_server::WEnum::Value(Format::Argb8888),
        });
    }
}

impl GlobalDispatch<WlSeat, (), Compositor> for Compositor {
    fn bind(
        _state: &mut Compositor,
        _handle: &DisplayHandle,
        _client: &Client,
        resource: wayland_server::New<WlSeat>,
        _global_data: &(),
        data_init: &mut DataInit<'_, Compositor>,
    ) {
        let res = data_init.init(resource, ());
        let _ = res.capabilities(Capability::Keyboard | Capability::Pointer | Capability::Touch);
    }
}

impl GlobalDispatch<WlOutput, OutputData, Compositor> for Compositor {
    fn bind(
        state: &mut Compositor,
        _handle: &DisplayHandle,
        _client: &Client,
        resource: wayland_server::New<WlOutput>,
        _global_data: &OutputData,
        data_init: &mut DataInit<'_, Compositor>,
    ) {
        let res = data_init.init(resource, OutputData {});
        use wayland_server::protocol::wl_output::{Mode, Subpixel, Transform};
        let w = super::W as i32;
        let h = super::H as i32;
        // `geometry` takes millimetres (a 5.7" 1080x2160 panel ≈ 61x122 mm),
        // not pixels; the pixel mode is advertised below. The logical size
        // clients should lay out in is mode / scale.
        let _ = res.geometry(
            0,
            0,
            61,
            122,
            Subpixel::Unknown,
            "gemini".into(),
            "geminipda".into(),
            Transform::Normal,
        );
        // Output scale = ceil(UI scale), so APP content is rendered at >=1x
        // the physical panel resolution and is never upscaled by gemshell
        // (2026-09-11). `wl_output.scale` is an integer, so at 150% we
        // advertise 2: the client renders a 2x buffer that the compositor
        // maps onto 1.5x physical pixels (supersampled, crisp); at 100% it
        // is 1 (1:1) and at 200% it is 2 (1:1).
        let _ = res.scale(state.ui_scale.ceil().max(1.0) as i32);
        let _ = res.mode(Mode::Current | Mode::Preferred, w, h, 60_000);
        let _ = res.done();
        state.outputs.push(res);
    }
}

impl GlobalDispatch<XdgWmBase, WmBaseData, Compositor> for Compositor {
    fn bind(
        _state: &mut Compositor,
        _handle: &DisplayHandle,
        _client: &Client,
        resource: wayland_server::New<XdgWmBase>,
        _global_data: &WmBaseData,
        data_init: &mut DataInit<'_, Compositor>,
    ) {
        let _ = data_init.init(resource, WmBaseData {});
    }
}

/// Global user data for the (minimal, clipboard-less) data-device
/// manager. Distinct from `()` so `create_global`'s interface type stays
/// inferable (wl_seat also uses `()`).
#[derive(Default)]
pub struct DataDeviceManagerData {}

impl GlobalDispatch<WlDataDeviceManager, DataDeviceManagerData, Compositor> for Compositor {
    fn bind(
        _state: &mut Compositor,
        _handle: &DisplayHandle,
        _client: &Client,
        resource: wayland_server::New<WlDataDeviceManager>,
        _global_data: &DataDeviceManagerData,
        data_init: &mut DataInit<'_, Compositor>,
    ) {
        let _ = data_init.init(resource, ());
    }
}

impl GlobalDispatch<ZxdgDecorationManagerV1, DecorationManagerData, Compositor> for Compositor {
    fn bind(
        _state: &mut Compositor,
        _handle: &DisplayHandle,
        _client: &Client,
        resource: wayland_server::New<ZxdgDecorationManagerV1>,
        _global_data: &DecorationManagerData,
        data_init: &mut DataInit<'_, Compositor>,
    ) {
        let _ = data_init.init(resource, ());
    }
}

// ---------------------------------------------------------------------------
// requests

impl Dispatch<WlCompositor, CompositorData, Compositor> for Compositor {
    fn request(
        _state: &mut Compositor,
        _client: &Client,
        _resource: &WlCompositor,
        request: <WlCompositor as Resource>::Request,
        _data: &CompositorData,
        _dhandle: &DisplayHandle,
        data_init: &mut DataInit<'_, Compositor>,
    ) {
        match request {
            WlCompositorRequest::CreateSurface { id: surface } => {
                let _ = data_init.init(surface, SurfaceData::default());
            }
            WlCompositorRequest::CreateRegion { id: region } => {
                let _ = data_init.init(region, RegionData {});
            }
            _ => {}
        }
    }
}

impl Dispatch<WlShm, ShmData, Compositor> for Compositor {
    fn request(
        _state: &mut Compositor,
        _client: &Client,
        resource: &WlShm,
        request: <WlShm as Resource>::Request,
        _data: &ShmData,
        _dhandle: &DisplayHandle,
        data_init: &mut DataInit<'_, Compositor>,
    ) {
        match request {
            WlShmRequest::CreatePool { fd, size, id } => {
                let len = size as usize;
                if len == 0 {
                    resource.post_error(0u32, "zero-size pool");
                    return;
                }
                match unsafe { memmap2::MmapOptions::new().len(len).map(&fd) } {
                    Ok(map) => {
                        let pool = PoolData {
                            map: Arc::new(map),
                            fd,
                            size: len,
                        };
                        let _ = data_init.init(id, pool);
                    }
                    Err(e) => {
                        resource.post_error(0u32, format!("mmap failed: {e}"));
                    }
                }
            }
            _ => {}
        }
    }
}

impl Dispatch<WlShmPool, PoolData, Compositor> for Compositor {
    fn request(
        _state: &mut Compositor,
        _client: &Client,
        resource: &WlShmPool,
        request: <WlShmPool as Resource>::Request,
        data: &PoolData,
        _dhandle: &DisplayHandle,
        data_init: &mut DataInit<'_, Compositor>,
    ) {
        match request {
            WlShmPoolRequest::CreateBuffer {
                offset,
                width,
                height,
                stride,
                format,
                id: buffer,
            } => {
                let (w, h, stride) = (width as u32, height as u32, stride as u32);
                if w == 0 || h == 0 || stride < w * 4 {
                    resource.post_error(1u32, "invalid buffer geometry");
                    return;
                }
                let need = offset.max(0) as usize + h as usize * stride as usize;
                if need > data.map.len() {
                    resource.post_error(1u32, "buffer out of pool bounds");
                    return;
                }
                let bd = BufferData {
                    map: data.map.clone(),
                    offset: offset.max(0) as usize,
                    w,
                    h,
                    stride,
                    format: match format {
                        wayland_server::WEnum::Value(f) => f as u32,
                        wayland_server::WEnum::Unknown(u) => u,
                    },
                };
                let _ = data_init.init(buffer, bd);
            }
            WlShmPoolRequest::Resize { size } => {
                // v1: buffers stay bounded by the original map (GTK
                // allocates pools generously; if a client ever outgrows
                // it, it creates a bigger pool)
                let _ = size;
            }
            WlShmPoolRequest::Destroy => {}
            _ => {}
        }
    }
}

impl Dispatch<WlBuffer, BufferData, Compositor> for Compositor {
    fn request(
        _state: &mut Compositor,
        _client: &Client,
        _resource: &WlBuffer,
        _request: <WlBuffer as Resource>::Request,
        _data: &BufferData,
        _dhandle: &DisplayHandle,
        _data_init: &mut DataInit<'_, Compositor>,
    ) {
    }
}

impl Dispatch<WlRegion, RegionData, Compositor> for Compositor {
    fn request(
        _state: &mut Compositor,
        _client: &Client,
        _resource: &WlRegion,
        _request: <WlRegion as Resource>::Request,
        _data: &RegionData,
        _dhandle: &DisplayHandle,
        _data_init: &mut DataInit<'_, Compositor>,
    ) {
        // kept alive; geometry unused (v1: opaque windows)
    }
}

impl Dispatch<WlCallback, (), Compositor> for Compositor {
    fn request(
        _state: &mut Compositor,
        _client: &Client,
        _resource: &WlCallback,
        _request: <WlCallback as Resource>::Request,
        _data: &(),
        _dhandle: &DisplayHandle,
        _data_init: &mut DataInit<'_, Compositor>,
    ) {
    }
}

impl Dispatch<WlSurface, SurfaceData, Compositor> for Compositor {
    fn request(
        state: &mut Compositor,
        _client: &Client,
        resource: &WlSurface,
        request: <WlSurface as Resource>::Request,
        data: &SurfaceData,
        _dhandle: &DisplayHandle,
        data_init: &mut DataInit<'_, Compositor>,
    ) {
        match request {
            WlSurfaceRequest::Destroy => {
                state.surface_detached(resource);
            }
            WlSurfaceRequest::Attach { buffer, x: _, y: _ } => {
                let mut inner = data.inner.lock().unwrap();
                // Keep the resource too, so `surface_commit` can hand it
                // back to the client (`wl_buffer.release`) once the pixels
                // are consumed (2026-09-26).
                inner.pending_buffer_wl = buffer.clone();
                inner.pending_buffer =
                    buffer.and_then(|b| b.data::<BufferData>().cloned().map(std::sync::Arc::new));
                inner.damaged = true;
            }
            WlSurfaceRequest::Damage { .. } | WlSurfaceRequest::DamageBuffer { .. } => {
                data.inner.lock().unwrap().damaged = true;
            }
            WlSurfaceRequest::Frame { callback } => {
                let cb = data_init.init(callback, ());
                data.inner.lock().unwrap().frame_cbs.push(cb);
            }
            WlSurfaceRequest::Commit => {
                state.surface_commit(resource);
            }
            WlSurfaceRequest::SetOpaqueRegion { .. }
            | WlSurfaceRequest::SetInputRegion { .. }
            | WlSurfaceRequest::SetBufferTransform { .. } => {}
            WlSurfaceRequest::SetBufferScale { scale } => {
                // Not double-buffered: applies immediately, and input
                // coords are surface-local (buffer px / scale).
                data.inner.lock().unwrap().buffer_scale = scale.max(1);
            }
            _ => {}
        }
    }
}

impl Dispatch<XdgSurface, XdgSurfaceData, Compositor> for Compositor {
    fn request(
        state: &mut Compositor,
        _client: &Client,
        resource: &XdgSurface,
        request: <XdgSurface as Resource>::Request,
        data: &XdgSurfaceData,
        _dhandle: &DisplayHandle,
        data_init: &mut DataInit<'_, Compositor>,
    ) {
        match request {
            XdgSurfaceRequest::Destroy => {
                if data.window.lock().unwrap().is_some() {
                    // protocol: the toplevel/popup must die first
                    resource.post_error(
                        1u32,
                        "xdg_surface destroyed while a toplevel/popup is alive",
                    );
                    return;
                }
                state.xdg_surface_detached(data.surface.clone());
            }
            XdgSurfaceRequest::GetToplevel { id: id_toplevel } => {
                let surface = data.surface.clone();
                let Some(win) = state.new_toplevel(surface.clone()) else {
                    resource.post_error(3u32, "surface already has a toplevel role");
                    return;
                };
                *data.window.lock().unwrap() = Some(win);
                if let Some(sd) = surface.data::<SurfaceData>() {
                    sd.inner.lock().unwrap().window = Some(win);
                }
                let t = data_init.init(id_toplevel, XdgToplevelData { window: win });
                state.adopt_toplevel(win, t, resource.clone());
            }
            XdgSurfaceRequest::GetPopup {
                parent,
                positioner,
                id: id_popup,
            } => {
                let surface = data.surface.clone();
                let ppos = *positioner
                    .data::<PositionerData>()
                    .unwrap()
                    .inner
                    .lock()
                    .unwrap();
                let parent_win = parent
                    .and_then(|p| {
                        p.data::<XdgSurfaceData>()
                            .map(|d| *d.window.lock().unwrap())
                    })
                    .flatten();
                // place near the parent window's edge (the positioner's
                // anchor is a refinement we approximate)
                let (px, py, pw, ph) = state.popup_anchor(parent_win);
                let (w, h) = (ppos.w as f32, ppos.h as f32);
                let x = match ppos.anchor {
                    Anchor::BottomLeft => px + pw - w - 4.0,
                    Anchor::TopLeft => px + 4.0,
                    _ => px + pw - w - 4.0,
                };
                let y = match ppos.anchor {
                    Anchor::BottomLeft => py + ph + 4.0,
                    Anchor::TopLeft => py + 4.0,
                    _ => py + ph + 4.0,
                };
                let Some(win) = state.new_popup(surface.clone(), parent_win, w, h, x, y) else {
                    resource.post_error(3u32, "surface already has a role");
                    return;
                };
                *data.window.lock().unwrap() = Some(win);
                if let Some(sd) = surface.data::<SurfaceData>() {
                    sd.inner.lock().unwrap().window = Some(win);
                }
                let p = data_init.init(
                    id_popup,
                    XdgPopupData {
                        window: win,
                        parent: parent_win,
                    },
                );
                state.adopt_popup(win, p, resource.clone());
            }
            XdgSurfaceRequest::SetWindowGeometry { .. }
            | XdgSurfaceRequest::AckConfigure { .. } => {}
            _ => {}
        }
    }
}

impl Dispatch<XdgToplevel, XdgToplevelData, Compositor> for Compositor {
    fn request(
        state: &mut Compositor,
        _client: &Client,
        _resource: &XdgToplevel,
        request: <XdgToplevel as Resource>::Request,
        data: &XdgToplevelData,
        _dhandle: &DisplayHandle,
        _data_init: &mut DataInit<'_, Compositor>,
    ) {
        let win = data.window;
        match request {
            XdgToplevelRequest::Destroy => state.close_window(win),
            XdgToplevelRequest::SetTitle { title } => state.set_title(win, title),
            XdgToplevelRequest::SetAppId { app_id } => state.set_app_id(win, app_id),
            _ => {}
            XdgToplevelRequest::SetMaximized => state.set_maximized(win, true),
            XdgToplevelRequest::UnsetMaximized => state.set_maximized(win, false),
            XdgToplevelRequest::SetMinimized => state.minimize(win),
            XdgToplevelRequest::SetFullscreen { .. } => {
                log::debug!("set_fullscreen (ignored in v1)");
            }
            XdgToplevelRequest::Move { seat: _, serial: _ } => state.begin_client_move(win),
            XdgToplevelRequest::SetParent { parent } => state.set_parent(win, parent.is_some()),
            XdgToplevelRequest::SetMaxSize { .. }
            | XdgToplevelRequest::SetMinSize { .. }
            | XdgToplevelRequest::Resize { .. }
            | XdgToplevelRequest::ShowWindowMenu { .. } => {}
        }
    }
}

impl Dispatch<XdgPopup, XdgPopupData, Compositor> for Compositor {
    fn request(
        state: &mut Compositor,
        _client: &Client,
        _resource: &XdgPopup,
        request: <XdgPopup as Resource>::Request,
        data: &XdgPopupData,
        _dhandle: &DisplayHandle,
        _data_init: &mut DataInit<'_, Compositor>,
    ) {
        match request {
            XdgPopupRequest::Destroy => state.close_window(data.window),
            XdgPopupRequest::Grab { .. } => {
                // the compositor dismisses popups on outside input
            }
            _ => {}
        }
    }
}

impl Dispatch<XdgPositioner, PositionerData, Compositor> for Compositor {
    fn request(
        _state: &mut Compositor,
        _client: &Client,
        _resource: &XdgPositioner,
        request: <XdgPositioner as Resource>::Request,
        data: &PositionerData,
        _dhandle: &DisplayHandle,
        _data_init: &mut DataInit<'_, Compositor>,
    ) {
        let mut inner = data.inner.lock().unwrap();
        match request {
            XdgPositionerRequest::SetSize { width, height } => {
                inner.w = width;
                inner.h = height;
            }
            XdgPositionerRequest::SetAnchor { anchor } => {
                inner.anchor = match anchor {
                    wayland_server::WEnum::Value(a) => a,
                    wayland_server::WEnum::Unknown(_) => Anchor::None,
                };
            }
            XdgPositionerRequest::SetOffset { x, y } => {
                inner.offset_x = x;
                inner.offset_y = y;
            }
            _ => {}
        }
    }
}

impl Dispatch<XdgWmBase, WmBaseData, Compositor> for Compositor {
    fn request(
        state: &mut Compositor,
        _client: &Client,
        _resource: &XdgWmBase,
        request: <XdgWmBase as Resource>::Request,
        _data: &WmBaseData,
        _dhandle: &DisplayHandle,
        data_init: &mut DataInit<'_, Compositor>,
    ) {
        match request {
            XdgWmBaseRequest::CreatePositioner { id: positioner } => {
                let _ = data_init.init(positioner, PositionerData::default());
            }
            XdgWmBaseRequest::GetXdgSurface { surface, id } => {
                let _ = data_init.init(id, XdgSurfaceData::new(surface.clone()));
            }
            XdgWmBaseRequest::Pong { serial } => state.wm_base_pong(serial),
            _ => {}
        }
    }
}

impl Dispatch<WlSeat, (), Compositor> for Compositor {
    fn request(
        state: &mut Compositor,
        client: &Client,
        _resource: &WlSeat,
        request: <WlSeat as Resource>::Request,
        _data: &(),
        _dhandle: &DisplayHandle,
        data_init: &mut DataInit<'_, Compositor>,
    ) {
        let Some(cs) = client.get_data::<ClientState>() else {
            return;
        };
        match request {
            WlSeatRequest::GetPointer { id: pointer } => {
                let p = data_init.init(pointer, ());
                if let Ok(mut inner) = cs.inner.lock() {
                    inner.pointer = Some(p);
                }
            }
            WlSeatRequest::GetKeyboard { id: keyboard } => {
                let k = data_init.init(keyboard, ());
                if let Ok(mut inner) = cs.inner.lock() {
                    inner.keyboard = Some(k.clone());
                }
                state.keyboard_bound(client, &k);
            }
            WlSeatRequest::GetTouch { id: touch } => {
                let t = data_init.init(touch, ());
                if let Ok(mut inner) = cs.inner.lock() {
                    inner.touch = Some(t);
                }
            }
            _ => {}
        }
    }
}

impl Dispatch<WlPointer, (), Compositor> for Compositor {
    fn request(
        _state: &mut Compositor,
        _client: &Client,
        _resource: &WlPointer,
        _request: <WlPointer as Resource>::Request,
        _data: &(),
        _dhandle: &DisplayHandle,
        _data_init: &mut DataInit<'_, Compositor>,
    ) {
        // touch-only: no cursor, requests accepted + ignored
    }
}

impl Dispatch<WlKeyboard, (), Compositor> for Compositor {
    fn request(
        _state: &mut Compositor,
        _client: &Client,
        _resource: &WlKeyboard,
        _request: <WlKeyboard as Resource>::Request,
        _data: &(),
        _dhandle: &DisplayHandle,
        _data_init: &mut DataInit<'_, Compositor>,
    ) {
        // only request: release — ignore
    }
}

impl Dispatch<WlTouch, (), Compositor> for Compositor {
    fn request(
        _state: &mut Compositor,
        _client: &Client,
        _resource: &WlTouch,
        _request: <WlTouch as Resource>::Request,
        _data: &(),
        _dhandle: &DisplayHandle,
        _data_init: &mut DataInit<'_, Compositor>,
    ) {
        // wl_touch has no requests
    }
}

impl Dispatch<WlOutput, OutputData, Compositor> for Compositor {
    fn request(
        _state: &mut Compositor,
        _client: &Client,
        _resource: &WlOutput,
        _request: <WlOutput as Resource>::Request,
        _data: &OutputData,
        _dhandle: &DisplayHandle,
        _data_init: &mut DataInit<'_, Compositor>,
    ) {
    }
}

// ---------------------------------------------------------------------------
// data-device manager (minimal; see the import comment)
//
// GTK4 only needs the GLOBAL to exist to open the Wayland display. We
// never send a `data_offer`/`selection`, so copy-paste is a no-op — but
// apps launch and render. Requests are accepted and ignored (the objects
// must exist so the client can create/destroy them without protocol
// errors). Drag-and-drop and the clipboard are a follow-up; the settings
// panel does not need them.

impl Dispatch<WlDataDeviceManager, (), Compositor> for Compositor {
    fn request(
        _state: &mut Compositor,
        _client: &Client,
        _resource: &WlDataDeviceManager,
        request: <WlDataDeviceManager as Resource>::Request,
        _data: &(),
        _dhandle: &DisplayHandle,
        data_init: &mut DataInit<'_, Compositor>,
    ) {
        match request {
            WlDataDeviceManagerRequest::CreateDataSource { id } => {
                let _ = data_init.init(id, ());
            }
            WlDataDeviceManagerRequest::GetDataDevice { id, seat: _ } => {
                let _ = data_init.init(id, ());
            }
            WlDataDeviceManagerRequest::Release => {}
            _ => {}
        }
    }
}

impl Dispatch<WlDataSource, (), Compositor> for Compositor {
    fn request(
        _state: &mut Compositor,
        _client: &Client,
        _resource: &WlDataSource,
        request: <WlDataSource as Resource>::Request,
        _data: &(),
        _dhandle: &DisplayHandle,
        _data_init: &mut DataInit<'_, Compositor>,
    ) {
        match request {
            WlDataSourceRequest::Offer { mime_type: _ } => {}
            WlDataSourceRequest::Destroy => {}
            WlDataSourceRequest::SetActions { dnd_actions: _ } => {}
            _ => {}
        }
    }
}

impl Dispatch<WlDataDevice, (), Compositor> for Compositor {
    fn request(
        _state: &mut Compositor,
        _client: &Client,
        _resource: &WlDataDevice,
        request: <WlDataDevice as Resource>::Request,
        _data: &(),
        _dhandle: &DisplayHandle,
        _data_init: &mut DataInit<'_, Compositor>,
    ) {
        match request {
            WlDataDeviceRequest::StartDrag { .. } => {}
            WlDataDeviceRequest::SetSelection { .. } => {}
            WlDataDeviceRequest::Release => {}
            _ => {}
        }
    }
}

impl Dispatch<ZxdgDecorationManagerV1, (), Compositor> for Compositor {
    fn request(
        state: &mut Compositor,
        _client: &Client,
        _resource: &ZxdgDecorationManagerV1,
        request: <ZxdgDecorationManagerV1 as Resource>::Request,
        _data: &(),
        _dhandle: &DisplayHandle,
        data_init: &mut DataInit<'_, Compositor>,
    ) {
        match request {
            ZxdgDecorationManagerV1Request::Destroy => {}
            ZxdgDecorationManagerV1Request::GetToplevelDecoration { id, toplevel } => {
                // The toplevel's own user data tells us which Window this is.
                let win = toplevel.data::<XdgToplevelData>().map(|d| d.window);
                if let Some(win) = win {
                    let dec = data_init.init(id, ToplevelDecorationData { window: win });
                    state.set_decoration(win, dec);
                }
            }
            _ => {}
        }
    }
}

impl Dispatch<ZxdgToplevelDecorationV1, ToplevelDecorationData, Compositor> for Compositor {
    fn request(
        state: &mut Compositor,
        _client: &Client,
        resource: &ZxdgToplevelDecorationV1,
        request: <ZxdgToplevelDecorationV1 as Resource>::Request,
        data: &ToplevelDecorationData,
        _dhandle: &DisplayHandle,
        _data_init: &mut DataInit<'_, Compositor>,
    ) {
        match request {
            ZxdgToplevelDecorationV1Request::SetMode { mode } => {
                // The request carries a `WEnum`; unknown values fall back to
                // CSD (the safe default: the client will self-decorate).
                let csd = match mode {
                    wayland_server::WEnum::Value(DecorationMode::ServerSide) => false,
                    _ => true,
                };
                state.set_csd(data.window, csd);
                resource.configure(if csd {
                    DecorationMode::ClientSide
                } else {
                    DecorationMode::ServerSide
                });
            }
            ZxdgToplevelDecorationV1Request::UnsetMode => {
                // "compositor decides": default to server-side (our titlebar).
                state.set_csd(data.window, false);
                resource.configure(DecorationMode::ServerSide);
            }
            ZxdgToplevelDecorationV1Request::Destroy => state.clear_decoration(data.window),
            _ => {}
        }
    }
}

// ---------------------------------------------------------------------------
// helpers for mod.rs

/// Build a keymap fd for the wl_keyboard.keymap event.
///
/// The scratch file comes from the platform shm helper (memfd_create on
/// Linux, shm_open elsewhere) so the compositor core stays portable.
pub fn keymap_fd(keymap_str: &str) -> Option<(OwnedFd, u32)> {
    use std::io::{Seek, SeekFrom, Write};
    let data = keymap_str.as_bytes();
    let mut file = crate::platform::shm::anonymous_file("gemshell-keymap", data.len()).ok()?;
    file.write_all(data).ok()?;
    // Rewind so a reader/mmap sees the keymap from offset 0.
    file.seek(SeekFrom::Start(0)).ok()?;
    Some((OwnedFd::from(file), data.len() as u32))
}
