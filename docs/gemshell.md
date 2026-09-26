# gemshell — the native Gemini PDA Wayland compositor + desktop shell

Last updated: 2026-09-26

A custom, Rust-based, Wayland-native desktop for the Gemini PDA: the
`gemshell` compositor (KMS + Mesa OpenGL, a handful of shell services)
plus an **in-process egui settings panel** (Wi-Fi / Bluetooth / Audio).
Registered as a choosable session — `gemcli session set gemshell` —
alongside gnome / console
(docs/desktop-selection.md).

Status: **on glass (2026-09-11)** — the compositor boots, imports the LK
framebuffer (GPU-direct present), renders the shell UI, and runs clients;
the 2026-09-12 rework replaced the separate `gemsettings` wl_shm client
with a GPU-tessellated egui overlay and moved all system-data access
behind the `gemdata::DataProvider` trait (see below). Nested x86_64 dev
loop: `bash bin/gemshell-nested.sh` (host EGL/GBM + wl_shm, seconds per
iteration; it opens the settings panel by default). On macOS the same
script opens a native preview window instead (no Wayland on macOS — see
"Platform backends" below). Host check:
`bash bin/gemshell-host-check.sh`. **Owed: a human look at the glass to
confirm the orientation/rotation direction + touch calibration** (the
2026-09-12 present() fix corrected a left-right mirror; see Orientation).
Full receipt: session-log 2026-09-11 "gemshell ON GLASS" + 2026-09-12.

### 2026-09-11 glass-report fix batch

A user report from the glass (on the pre-egui build, plus several
issues still present at HEAD) drove one batch of fixes; all build- and
host-verified, deploy pending:

- **Text (the real root cause).** `common/font.rs` advanced the pen by
  `h_advance_unscaled(id) * px` — raw font units (≈600) × 26 ≈ **16 000
  px per glyph**. `Op::Text` therefore showed only its first character
  and `Op::TextCentered` placed the pen thousands of px off-screen, so
  the launcher title/labels, the Close button and the whole status bar
  were blank. Fix: the SCALED advance via
  `ab_glyph::ScaleFont::h_advance` (`face.as_scaled(px)`). The
  2026-09-12 raster-origin fix was necessary but not sufficient.
- **SVG app icons.** `common/icons.rs` now decodes SVG app icons with
  `resvg` (text support off) lazily, per used icon; SVGs are indexed by
  name at startup and rendered on first request. Adwaita/GNOME/COSMIC
  ship app icons as SVG only, so before this every non-PNG app (most of
  the 168 `.desktop` entries) fell back to a blank letter tile.
- **Launcher** — scroll clamp sign/limit were wrong (see Input model),
  the `*= 0.8` decay snapped the grid back to the top, taps inside the
  LauncherScroll gesture were ignored, and there was no way to close
  the drawer. Fixed + added a Close button (tap outside also closes).
- **Idle CPU** — the main loop re-rendered every 16 ms even when
  nothing changed (≈60 % of a core). It now renders only when `dirty`:
  0 timeout when a frame is due, the remainder of the ~60 Hz interval
  while a frame is in flight, and a 1 s idle tick so the status/clock
  channel (which cannot wake `poll`) is still drained.
- **Brightness + output selection** — new Display tab (brightness
  slider); the Audio tab now lists the `gemini_speakers` filter sink as
  "Built-in Speakers" next to "Headphones / Jack" (see Settings panel).

### 2026-09-11 second glass-report batch

Three more glass reports; build + host verified, deployed to the unit:

- **"Apps don't launch at all".** GTK4's
  `gdk/wayland/gdkdisplay-wayland.c` refuses the whole display unless
  the compositor exposes **`wl_data_device_manager`** ("The Wayland
  compositor does not provide one or more of the required interfaces, not
  using Wayland display"), and the app then dies with "Failed to open
  display". Added the global plus minimal dispatch for
  `wl_data_device_manager` / `wl_data_source` / `wl_data_device`
  (clipboard is a no-op for now). Receipt: gnome-calculator stays
  running on the device (`MESA-EGL: failed to get driver name` warnings
  are from libmutter preferring the render node; the app still runs).
- **Settings froze for seconds and handled taps from 30 s+ ago.** The
  panel synchronously ran a ~1–2 s nmcli/bluetoothctl/wpctl snapshot on
  the compositor thread on open and every 3 s, and every slider step
  queued another. New `compositor::data::CachedData` wraps the provider:
  reads come from an in-memory snapshot (instant), mutations run on a
  worker, refreshes are coalesced (`refresh_pending`), and mutations are
  coalesced **by kind, last-wins** and run *before* the snapshot, so a
  fast mutation (devmem backlight) is never stuck behind `nmcli`.
  Brightness/volume/mute do not request a snapshot at all. Poll interval
  3 s → 8 s. The panel stays responsive during a `wpctl` call because
  the compositor never blocks on it.
- **Two sets of window chrome (two close buttons).** GTK4 does **not**
  speak `zxdg_decoration_manager_v1`; it decides CSD from the **KDE**
  `org_kde_kwin_server_decoration_manager` (`gdk_wayland_display_prefers_ssd()`
  → `gtk_window_should_use_csd()`), which gemshell does not advertise, so
  GTK always self-decorates — while gemshell drew the SSD titlebar too.
  Fix: windows default to **CSD** (`Window.csd`), so gemshell does not
  draw a titlebar over a client's own header bar. `draw_window`,
  `content_rect`/`local_coords` and the titlebar hit-test all branch on
  `csd`. `zxdg_decoration_manager_v1` **is** still advertised for
  Qt/wlroots clients and answered client-side; a client that explicitly
  asks for server-side (`set_mode(ServerSide)`) gets `csd = false` and the
  compositor titlebar. `xdg_toplevel.move` (a CSD header-bar drag) moves
  the window with the finger that is down.

### 2026-09-12 third glass-report batch

Three further glass reports. Builds green (x86_64 nested + aarch64
`gemshell-0.1.0`, `0yi8y633…`/device build `b44hsvc9…`); on-glass deploy
owed.

- **"Apps launch maximized and can't be shrunk or closed."** New
toplevels open maximized (2026-09-11) and a CSD app's own header-bar
buttons were the only chrome, so a maximized window had no reachable
restore/close affordance. Two fixes:
  - **Status-bar window controls** for the focused window, left of the
    clock: **minimize** · **restore/maximize** · **close**
    (`ui::draw_window_controls`, hit-tested in `handle_tap` via
    `ui::window_control_zones()` at cx 112/172/232, same `ZONE_HALF`
    radius as the status zones; `Compositor::focused_window()`).
  - **`Close` now actually closes.** The old `close_window()` dropped
    the server-side `xdg_toplevel` resource and never sent
    `xdg_toplevel.close`, so the compositor forgot the window while the
    app kept running. New `request_close()` sends `t.close()`; the frame
    is removed by the client's `XdgToplevel::Destroy` dispatch. Nested
    receipt: a minimal GTK4 app logged `CLOSE_REQUEST_RECEIVED` +
    `APP_SHUTDOWN`; forwarded touch reaching widgets was proved
    separately (a trivial GTK4 button fired `clicked` from a `wl_touch`).
    `Delete` (XF86_Tools) and the SSD titlebar ✕ now route through it.
- **Settings still slow (~2 s to close after a button press).** The
  compositor already renders only on `dirty`, but while the panel was up
  it still re-rendered the **whole scene** (window textures, status bar,
  taskbar) beneath the modal — the expensive part on the A53s.
  `render_frame` now runs the egui panel **first** and, while
  `settings_open`, skips the hidden scene (keeps the wallpaper). The
  settings card is a full-screen **opaque** `CentralPanel` (verified: all
  four corners are the panel BG `18,21,26`). `Close` clears the panel
  primitives on the same frame, so the modal is gone in that present.
  The loop re-arms `dirty` only while `settings_open &&
  shell.wants_repaint()` (egui animations/hover) instead of a fixed 60 Hz
  clock. (On-glass latency measurement still owed — the frame-time claim
  is a code-path argument, not a device measurement.)
- **`c`/`v`/`b`/`n` change volume/brightness without Fn.** Fn is
  `KEY_RIGHTALT` (level-3, Mod5); the media syms on C/V/B/N
  (`XF86AudioLowerVolume`/`Raise`/`MonBrightnessDown`/`Up`) are level 3.
  `Input::process_key` now reconciles a stuck Mod5 against the input
  core's real key state: on any non-RALT key, if xkb still holds Mod5
  but `EVIOCGKEY(96)` reports RALT up, it forces the RALT key-up before
  resolving the keysym. Root cause is not proven (the poll loop can drop
  an RALT-up while a frame renders); this is a self-healing safety net.
  Only on a real evdev fd — nested mode (`kbd_fd = -1`) skips it.

### 2026-09-26 — Wayland-app touch FIXED (buffer-scale transform) + known offset

**Symptom (on glass):** apps open and render, but **ignore every touch**
(the compositor's own chrome/launcher work). Root cause was NOT the
protocol: `wl_seat.capabilities` advertises Touch, the client calls
`get_touch`, and `wl_touch.down/motion/up` reach it. It was the
**coordinate space**.

`Compositor::local_coords` omitted `wl_surface.set_buffer_scale`.
Wayland input coordinates are **surface-local** = buffer pixels ÷ buffer
scale. With the persisted UI scale 1.5 (`wl_output.scale = 2`), clients
allocate 2× buffers and call `set_buffer_scale(2)` — gnome-calculator:
configure 1440×588, buffer **2880×1232** → surface-local **1440×616** —
while gemshell delivered e.g. `(884, 1024)`: 2× too large, outside the
surface, so GTK dropped it. At UI scale 1.0 (output scale 1, factor 1)
the bug is invisible, which is why the 2026-09-12 pass missed it.

**Fix:** track `set_buffer_scale` in `SurfaceInner` (it was in the
ignored request arm), carry it on `Window`, and apply it in
`local_coords` (`k = bw/buf.w × buffer_scale`). The non-maximized window
auto-size now also divides buffer dims by the scale. Fixed + verified on
glass at 150% (`agipxng1i25ygry2am7x20pppjxd16gb-gemshell-0.1.0`).

**Known imperfection (future session):** taps respond but land
**slightly offset** from the button. Likely the ignored
`xdg_surface.set_window_geometry`: GTK's CSD buffer carries a shadow
margin (~14 surface-local px) that is not excluded when the buffer rect
maps to the window, so the content origin is off by that inset. Fix ≈
honour `set_window_geometry` (and/or the CSD inset) in the
buffer→surface transform.

**Still open (app hosting):** no session D-Bus is started for clients
(GTK/portal apps log `Cannot autolaunch D-Bus without X11 $DISPLAY`;
`org.freedesktop.portal.Desktop` unavailable), and clients log
`MESA-EGL: failed to get driver name for fd -1` /
`failed to create dri2 screen`.

### 2026-09-26 (e) — wl_shm buffer leak FIXED (`wl_buffer.release`) — the freeze root cause

**Symptom (on glass):** gemshell was "fine for a few minutes then
suddenly extremely slow", then **locked up**; ping still answered
(kernel alive) but sshd could not even send a banner (userspace dead).
GNOME was stable under the same loads.

**Root cause: the compositor never sent `wl_buffer.release`.** The
`Dispatch<WlBuffer, BufferData, Compositor>` impl was empty and
`SurfaceInner` did not keep the buffer resource. A compositor MUST
release a wl_buffer when it is done reading it, so a wl_shm client may
recycle it. Without the release, GTK4 can never reuse a buffer and
**allocates a fresh shm pool every frame**. Receipt on glass:
`gnome-calculator` grew **384 KB → 920 MB RSS in ~27 s** (`Shmem:`
37 MB → 843 MB). A few apps exhaust the 3.7 GB RAM; with no swap the
kernel enters reclaim thrash (visible as the panfrost shrinker's
`Purging N bytes`, `panfrost_gem_shrinker_scan`) and userspace starves
— the hard-wedge signature. GNOME is unaffected because wlroots/GTK
release correctly there.

**Fix:** `SurfaceInner.pending_buffer_wl` now stores the attached
`wl_buffer`; `surface_commit` calls `release()` immediately after
`window_texture()` has copied the pixels into a GL texture (nothing
reads the buffer afterwards), including on the no-window/early-return
paths.

**Verified on glass:** the same calculator now plateaus at **~107 MB
RSS**, `Shmem:` ~75 MB flat (`wasa7dasvxm15kihi2xv8m2nznjglk8x-gemshell-0.1.0`);
user-confirmed under sustained use (7 clients: `Shmem:` ~200 MB,
gemshell ~175 MB, `MemAvailable` ~3.0 GB — flat). Run transiently
(`gemshell-dev.service`); a reboot needs the fix deployed (toplevel
build + `bin/deploy.sh`, or a reflash).

### Variable UI scale

Settings > Display offers **100% / 150% / 200%**. Design:

- The compositor lays out in **logical units** `lw = W/ui_scale`,
  `lh = H/ui_scale` (`Compositor::{lw,lh,ui_scale}`); `Renderer::ui_scale`
  maps logical → the full physical viewport in the vertex shader
  (`uRes = width/ui_scale`), so the existing pixel-space ops just work.
- UI text is rasterized into the glyph atlas at `26 × SUP` (`SUP = 2`) and
  every metric is divided by SUP, so text stays crisp at 150/200% (and is
  downscaled by linear filtering at 100%).
- egui (`pixels_per_point = PPP × ui_scale`) is rasterized at the scaled
  resolution too; the panel's screen rect stays `lw/PPP` points.
- Touch is read in physical scene coordinates and converted to logical at
  ingest (`x / ui_scale`) so hit-tests line up at every scale.
- The scale persists to `$HOME/.config/gemshell/scale` and is re-applied
  at boot; changing it re-configures toplevels so clients reflow.
- **It applies to apps, not just gemshell chrome.** `wl_output.scale` is
  set to `ceil(ui_scale)` — an integer, so 100%→1, 150%→2 (the client
  renders a 2× buffer that the compositor maps onto 1.5× physical pixels:
  supersampled, crisp) and 200%→2 (1:1). Changing the scale broadcasts a
  new `wl_output.scale` + `done` to every bound output and re-configures
  every toplevel, so apps reflow at the new scale.

### No desktop padding; apps fill the work area

New toplevels open **maximized to the work area** (below the status bar,
above the taskbar) instead of an inset 1000×700 box — GNOME/CSD apps are
complete windows with their own header bar, and on a 5.7" screen the inset
read as unwanted padding. Transient dialogs (a toplevel that sets
`xdg_toplevel.set_parent`) open as a centered floating box instead.

`buffer_rect` used to letterbox a client buffer inside the content rect,
leaving a visible strip of window background around every app: GTK CSD
buffers reserve a transparent shadow margin, so a maximized window's
buffer is slightly smaller than the configure we sent (e.g. 2062×939 vs
2160×948). It now **stretches to fill** when the aspect mismatch is under
~13% (imperceptible, and the app reaches the screen edges) and only
letterboxes for larger mismatches (video, portrait dialogs).

### Dragging by the client's own title bar

With CSD the app's header bar is the drag handle. `xdg_toplevel.move` from
a client now records `client_move = (win, finger, dx, dy)` and the
compositor moves the window with that finger **while still forwarding**
the touch events to the client, so the client's own drag gesture still
completes. Un-maximizing via a drag restores a floating size (78% × 82% of
the work area) so the movement is visible.

On-device receipt (gnome-calculator): SSD titlebar pixels 0, window-bg
pixels 0, and the app's background spans the full 2160 px width.

## Orientation (landscape product / portrait fb)

The Gemini is a landscape clamshell but the LK framebuffer is a
**portrait 1080x2160** buffer. gemwl renders its landscape scene with
`WL_OUTPUT_TRANSFORM_90`; `geminipda-drm` advertises panel-orientation
`LEFT_UP` (=90) for the same reason (driver header, 2026-09-10). gemshell
follows: the logical scene is **2160x1080** (`compositor::W/H`), and
`present()` counter-rotates 90 into the fb in the compute copy
(`COPY_CS`, `fbSize`/`srcSize`/`rotation` uniforms). Touch evdev is
portrait like the fb; `Input::touch_to_scene` applies the matching
inverse rotation.

- `GEMSHELL_ROTATE` (default **270**) — display counter-rotation.
- `GEMSHELL_TOUCH_ROTATE` (default = `GEMSHELL_ROTATE`).

Because display and touch use the same transform, an incorrect direction
would look wrong but still hit the right controls; confirm visually.

**2026-09-12 mirror fix.** The on-glass report was "left-right mirrored,
top/bottom correct". Cause: the compute copy sampled the scene FBO with
`texelFetch`, which indexes the texture's BOTTOM-UP storage directly, so
the intended 90° rotation was actually a transpose (a rotation composed
with a horizontal reflection — exactly a left-right mirror). `COPY_CS`
now flips the scene Y (`t = ivec2(s.x, srcSize.y - 1 - s.y)`) before the
fetch; the mapping dst(p.x,p.y) = scene(p.y, fbW-1-p.x) is now
orientation-preserving. It also matches `touch_to_scene` rotation 90
(`(ny*scene_w, (1-nx)*scene_h)`), so touch and display finally agree.
The rotation DIRECTION (90 vs 270) is still the calibration knob:
`GEMSHELL_ROTATE`/`GEMSHELL_TOUCH_ROTATE`.

**2026-09-12 on-glass result:** `90` rendered the scene **180° out**, so
both defaults moved to **270** (the 180° counterpart, and the exact
inverse of the touch-270 mapping) — confirmed correct.

### Touch (Protocol-B) — the dead-touch root cause

The NT36772 driver reports Protocol B as
`ABS_MT_SLOT` → `input_mt_report_slot_state` (`ABS_MT_TRACKING_ID`) →
`ABS_MT_POSITION_X/Y` (`novatek-nt36xxx.c nvt36xxx_report`). Two bugs
kept touch dead; both are fixed (2026-09-12):

1. **`ABS_MT_SLOT` was `57`** — 57 is `ABS_MT_TRACKING_ID`; SLOT is
   `0x2f = 47`. The match lists SLOT first, so every tracking-id event
   was consumed as a slot change and a finger-down was never
   registered. This was *the* cause (touch was dead even before any
   transform issue).
2. **Emit at SYN_REPORT, not at TRACKING_ID.** Because TRACKING_ID
   precedes `POSITION_X/Y`, the old code emitted the down with the
   previous contact's (or `0,0`) coordinates. `read_touch` now records
   per-slot down/up + changed flags and emits `TouchDown`/`Motion`/`Up`
   at `SYN_REPORT`, when positions are final.

Also `read_events`' evdev fds MUST be `O_NONBLOCK` — the drain loop's
follow-up `read()` otherwise blocks and freezes the whole compositor on
the first event.

### Touch test mode (`GEMSHELL_TOUCH_TRAIL`)

Set `GEMSHELL_TOUCH_TRAIL=1` (the system service sets it by default for
the bring-up) and every finger draws a coloured trail on the scene
(`ui::draw_touch_trail`), one colour per finger id, with pen-up breaks;
normal gestures are bypassed while the mode is on, and a **3-finger
touch clears** the canvas. This is how touch is verified on the glass —
remove the env from `services/gemshell.nix` to restore normal gestures.
For scripted strokes use the repo injector's new multi-point mode:
`tapxy X Y [X2 Y2 …]` (built from `bin/touch-inject.c`).

## Platform backends — device, x86_64 nested, macOS preview

The compositor core is platform neutral; the OS-specific half lives behind
traits in `pkgs/gemshell/src/platform/`. Two backend implementations ship,
selected in exactly ONE place (`platform::create_backend`) — everything
else is dispatched through the traits at runtime:

| Backend | Where | Display | Present | Input | Event loop |
|---|---|---|---|---|---|
| Linux (device) | `platform/linux.rs` | EGL/GBM (Mesa, panfrost render node) | compute blit into the LK framebuffer (`/dev/gemfb`) | raw evdev + xkbcommon | `poll()` over display/socket/GBM/evdev fds |
| Linux (nested) | `platform/linux.rs` + `compositor/nested.rs` | EGL/GBM (host render node) | scene FBO readback -> `wl_shm` on the host compositor | host `wl_seat` forwarded into our seat | same `poll()` loop |
| macOS | `platform/macos.rs` | native Cocoa window + desktop OpenGL 3.3 core (glutin/CGL) | `glBlitFramebuffer` scene FBO -> window | Cocoa mouse/keyboard mapped to gestures | winit event loop |

The seams: the renderer takes a `Glsl` dialect (GLES 3.00 vs desktop 3.30
core), input is an `InputSource`, presentation a `Presenter`, and the loop
a `Backend`. The Linux backend keeps the EGL/LK-fb/evdev code the device
always used; it moved out of the compositor, it did not change.

### Nested mode on x86_64 (development)

`GEMSHELL_NESTED=1` (via `bin/gemshell-nested.sh`) runs gemshell as a
**Wayland client** under the host compositor: headless EGL/GBM on
`/dev/dri/renderD128`, the scene FBO read back and presented into an
`xdg_toplevel` via `wl_shm` (nearest-neighbour downscale,
`GEMSHELL_NESTED_SCALE`, default 0.5), and the host's
pointer/keyboard/touch forwarded into gemshell's own seat. Children
connect to the `wayland-gemshell` socket. This is the fast UI loop — no
device flash. The script sets `GEMSHELL_OPEN_SETTINGS=1` so the egui
panel is visible immediately (any other client via `GEMSHELL_AUTOSTART`).
Nested mode selects `gemdata-dummy` as its `DataProvider`, so the UI is
exercised against in-memory Wi-Fi/BT/audio state without touching the
workstation's network. Build: `nix build .#packages.x86_64-linux.gemshell`.

### macOS preview window (2026-09-11)

On macOS `bash bin/gemshell-nested.sh` builds with the host Rust toolchain
(`cargo build` in `pkgs/gemshell`) and runs the same binary with the macOS
backend — a native window, no Wayland anywhere. It is a **UI test harness**
for the shell chrome, the egui settings panel and input; there are no
Wayland clients to host, and `gemdata-dummy` supplies the Wi-Fi/BT/audio
state. Window size from `GEMSHELL_NESTED_SCALE` (0.5); the panel opens
with `GEMSHELL_OPEN_SETTINGS=1`; `GEMSHELL_SCREENSHOT`/`_DELAY_MS` write
the scene FBO to a PNG. `GEMSHELL_PROFILE=release` builds optimised
(default is debug — much faster to compile).

Why it is not "nested Wayland": macOS has **no libwayland, no compositor
socket and no Wayland session**, so the constraint is the platform, not
the code. The renderer's desktop-GL shader set (`*_CORE` in render.rs),
the core-profile VAO and the `blit_to_default_fb` present exist for this
path; the Wayland Rust crates build via their pure-Rust backend (the
`system`/libwayland feature is Linux-only in `Cargo.toml`).

Keyboard mapping in the preview is deliberately small (there is no
xkbcommon on macOS): Escape/arrows/Home/End/Page/Delete, and **F1-F8
stand in for the Gemini's Fn layer** (launcher, app switcher, brightness,
volume, sleep) so those shortcuts can be exercised from a Mac.

**egui host-side validation (2026-09-12):** the nested binary also works
headless-ish for verification — run it with `GEMSHELL_SCREENSHOT=/tmp/
x.png GEMSHELL_SCREENSHOT_DELAY_MS=2500 GEMSHELL_OPEN_SETTINGS=1` under a
host Wayland session and it writes the composited scene FBO to a PNG
(useful for checking text regressions without the device).

Build gotchas found the hard way (2026-09-11, aarch64 nix build —
the host `cargo check` never links, so all of these died only on the
real build):

- **libglvnd's `libEGL.so.1` does NOT export the platform entry
  points** (`eglGetPlatformDisplayEXT`, `eglCreatePlatformSurface`) —
  verified by readelf on the store lib. They are resolved through
  `eglGetProcAddress` (the EGL 1.5 way), which libglvnd's dispatch
  answers from the mesa-geminipda ICD (src/compositor/render.rs).
- **The store's xkbcommon 1.13.1 (aarch64) exports only the V_0.5.0-era
  state API**: `xkb_state_mods_for_key`, `xkb_state_key_get_mods`,
  `xkb_state_mods_get_mask`, `xkb_state_mod_get_*` and
  `xkb_state_group_get_index` are all absent from its dynamic symbol
  table (each died with an undefined reference in turn). The wl
  modmap masks are therefore built per slot with
  `xkb_state_mod_index_is_active` (src/compositor/input.rs — the FFI
  block carries the note).
- **`c_char` on aarch64 is `u8`**, `i8` on x86_64 — the hand-rolled
  FFI must say `c_char` (or cast at the call), never `i8`.
- **Link set declared in `pkgs/gemshell/build.rs`**
  (wayland-server/client, xkbcommon, gbm, EGL, GLESv2, png, z) —
  deterministic, and the reason the build doesn't depend on each
  crate's own link emission.

Why this exists: GNOME works but is sluggish on the T880/A72 (GNOME 50
+ gdm + full app suite). gemshell targets the same KMS/GL stack GNOME
proved (geminipda-drm card0 + panfrost via Mesa kmsro, 2026-09-10) but
with a minimal, single-process compositor that draws its own shell UI —
no shell app suite, no daemon zoo.

Status: **build-level** (2026-09-11). On-glass bring-up checklist at the
bottom.

## Components

| Piece | Where | Notes |
|---|---|---|
| `gemshell` (compositor + shell) | `pkgs/gemshell/` (bin `gemshell`) | single process: wayland server + GL renderer + input + chrome + egui settings panel |
| `platform` (backends) | `pkgs/gemshell/src/platform/{mod,linux,macos}.rs` | the OS-specific half behind traits: `InputSource`, `Presenter`, `Backend`; the one `#[cfg]` fork (`create_backend`) picks Linux or macOS |
| `shell` (egui UI) | `pkgs/gemshell/src/shell.rs` | the settings panel: layout, fonts, widgets; emits GPU triangle meshes |
| `gemdata` | `pkgs/gemshell/crates/gemdata/` | the `DataProvider` trait + data types (Wi-Fi/BT/audio/battery/status) |
| `gemdata-device` | `pkgs/gemshell/crates/gemdata-device/` | real impl + all gemcli device functions (nmcli/bluetoothctl/wpctl/sysfs/i2c/devmem); unit-tested parsers |
| `gemdata-dummy` | `pkgs/gemshell/crates/gemdata-dummy/` | in-memory fake for nested mode |
| `gemcli` | `pkgs/gemshell/crates/gemcli/` (bin `gemcli`) | thin clap frontend over `gemdata-device` (no logic of its own) |
| derivation | `pkgs/gemshell.nix`, `pkgs/gemcli.nix` | rustPlatform, native aarch64, release profile; both build the same workspace |
| session module | `services/gemshell.nix` | `services.gemshellDesktop.enable` (co-installed, marker-selected) |
| session wiring | `services/desktop-select.nix`, `services/scripts/gemini-desktop-apply`, `pkgs/gemshell/crates/gemdata-device/src/session.rs` | mode `gemshell` |

## Architecture (compositor)

Single thread, one event loop (the Linux backend's `poll()` over the
wayland display fd, the gbm device fd and the two evdev nodes; the macOS
backend's winit loop). No async runtime, no threads in the hot path (one
background thread polls status: battery/wifi/bt/volume). Frame pacing is
dirty-driven: 0 while a frame is due, the remainder of the 16 ms/60 Hz
interval while one is in flight, and a 1 s idle tick so the status channel
is still drained (there is no eventfd for it). It does NOT re-render when
nothing changed (2026-09-11).

- **Display path** — `/dev/dri/card0` (geminipda-drm, the LK
  framebuffer as DRM/KMS; single fixed mode 1080×2160 portrait, XRGB8888
  shadow plane). gbm device on card0 → gbm surface (XRGB8888) →
  `eglCreatePlatformSurface(EGL_PLATFORM_GBM_MESA)`. eglSwapBuffers
  page-flips through gbm; the flip event on the gbm device fd paces the
  next frame (34 ms timer fallback if the driver never signals).
  Mesa kmsro pairs card0 with renderD128 (panfrost) — exactly the GNOME
  path. GL = GLES 3.x, one textured-quad program, one 2048² glyph atlas,
  icon textures.
- **Wayland** — `wayland-server` 0.31 (system/libwayland backend) +
  `wayland-protocols` 0.32 (server). Globals: `wl_compositor`,
  `wl_shm` (hand-implemented pool/buffer state), `wl_seat`
  (keyboard+pointer+touch), `wl_output`, `xdg_wm_base`
  (xdg-shell v1, incl. minimal xdg-popup for menus), no
  linux-dmabuf in v1 (clients ship wl_shm buffers — the gemdemo-
  verified path; dmabuf/EGLImage import is the follow-up for
  GPU-direct apps).
- **Clients** — any standard wl_shm xdg-shell client: GTK apps (via
  GTK's wayland backend), Qt (ozone-wayland), gemdemo,
  labwc? (labwc is a compositor — no). Chrome/Firefox work when
  launched inside the session (their GL goes through the same
  /run/opengl-driver ICD; launched with LD_LIBRARY_PATH carrying the
  glvnd client libs so dlopen-based loaders find them).
- **Input** — raw evdev (protocol-B multitouch from the NT36772;
  EV_KEY from the AW9523 gpio-matrix keyboard), xkbcommon keymap from
  layout `gemini` (XKB_CONFIG_EXTRA_PATH, same as every other desktop
  here). The compositor consumes gestures itself and forwards
  one-finger interactions as wl_touch / synthetic wl_pointer to
  clients.

### Windows, workspaces, snapping

- One window = one xdg_toplevel. Compositor-drawn titlebar (36 px):
  title, maximize button, close button. Window size follows the client
  buffer; on maximize/snap the compositor requests a new size via
  xdg_toplevel configure and letterboxes the client buffer if the
  client does not follow.
- Workspaces: 3 horizontal pages (1 = center, 2 = left, 3 = right of
  the gesture direction). A 2-finger horizontal swipe drags the
  page strip (both adjacent pages visible while swiping) and switches
  on release.
- Snapping: drag a window by its titlebar — edges light up a translucent
  preview (left/right half, top/bottom half, center-top = maximize);
  release applies it. Keyboard: Fn+←/→/↑-drag etc. (table below).
- Focus: raise-on-tap, no focus-follows. The focused window gets a
  highlighted titlebar; Escape/3-finger-down drops overlays.

### Shell UI (compositor-drawn)

- **Status bar** (top, 44 px): time (left); right side tap targets:
  gear (opens the settings panel), speaker, bluetooth, wifi, battery
  (% + charge bolt). Tapping gear/speaker/wifi/bt opens the in-process
  egui settings panel on the matching section.
- **Settings panel** (egui, full-screen modal): Wi-Fi / Bluetooth /
  Audio tabs. See "Settings panel (egui)" below. There is no separate
  settings client any more (the old `gemsettings` binary was removed
  2026-09-12).
- **Taskbar** (bottom, 84 px): launcher button (grid) + one tile per
  running app (icon or letter tile, title under it, running dot).
  Tap = focus + bring its workspace to front; tap the focused one again
  = hide (minimize); the focused app shows a highlight bar.
- **Launcher**: full-screen app grid (icons + names, 1-finger scroll,
  clamped to the real content height so every row is reachable), built
  from the XDG desktop dirs (`/usr/share/applications`,
  `/home/cjdell/.local/share/applications`, `/root/.local/share/…`).
  Tap launches (Exec= parsing, `%u %U %f` stripped, `Terminal=`/
  `NoDisplay=` respected); a Close button (top-right) or a tap outside
  a tile closes the drawer. App icons are PNG or SVG
  (`common/icons.rs`).
- **App switcher** (Fn+Tab / 2-finger up-swipe): center row of running
  apps, Tab cycles, release activates; 2-finger up-swipe = next app.

### Input model (multitouch, no mouse)

The touch panel is the only pointer (10-pt multitouch, raw portrait —
identity mapping to the 1080×2160 scene, confirmed 2026-09-10).
Gestures are decided by the compositor BEFORE forwarding:

| Fingers | Gesture | Action |
|---|---|---|
| 1 | tap status-bar/taskbar/titlebar/launcher | UI action |
| 1 | tap window content | forward to app (click) |
| 1 | drag from titlebar | move window (+ snap previews) |
| 1 | drag window content | forward to app (scroll) |
| 1 | double-tap titlebar | maximize/restore |
| 1 | drag in launcher | scroll |
| 2 | horizontal swipe | swipe workspaces (live slide) |
| 2 | up-swipe | next app (switcher) |
| 3 | up-swipe | open launcher |
| 3 | down-swipe | close overlay / home |

When a 2nd/3rd finger lands during a forwarded touch, the compositor
sends `wl_touch.up` for the forwarded finger(s) first (clients must
cancel their gesture), then the compositor gesture takes over.

### Keyboard shortcuts (layout `gemini`, Fn = level-3 mod)

| Action | Keysym | Physical |
|---|---|---|
| App switcher | Mod5+Tab (Tab level 0) | Fn+Tab |
| Previous workspace | Mod5+Prior | Fn+↑ |
| Next workspace | Mod5+Next | Fn+↓ |
| Snap left / right | Mod5+Home / Mod5+End | Fn+← / Fn+→ |
| Maximize / restore | Mod5+grave | Fn+M |
| Close focused window | Mod5+Delete | Fn+Backspace |
| Launcher | XF86TaskPane | Fn+A |
| Volume − / + / mute | XF86AudioLower/RaiseVolume, XF86AudioMute | Fn+C / Fn+V / Fn+T |
| Brightness − / + | XF86MonBrightnessDown/Up | Fn+B / Fn+N |
| Close overlay | Escape (level 0) | Esc |

Media XF86 keysyms are matched regardless of modifier mask (the Fn
layer carries Mod5 through the ISO_Level3_Shift modifier).

## Data access (`gemdata`)

All system-data reads/writes go through ONE trait, `gemdata::
DataProvider` (`pkgs/gemshell/crates/gemdata/src/lib.rs`): `status()`,
`wifi()` / `set_wifi_enabled` / `scan_wifi` / `connect_wifi` /
`disconnect_wifi`, `bluetooth()` / `set_bluetooth_enabled` /
`connect_bluetooth` / `scan_bluetooth`, `audio()` / `set_default_sink` /
`set_volume` / `set_muted`, `set_brightness`. The trait and its data
types (`WifiState`, `BtState`, `AudioState`, `ShellStatus`, …) are UI-,
GL- and wayland-free.

Two implementations:

- **`gemdata-device`** — the real one. Each subsystem is a thin
  command/sysfs wrapper around a *pure parser* (`parse_nmcli_wifi_list`,
  `parse_wpctl_status_sinks`, `parse_bluetoothctl_show`, …), so the
  text formats are unit-tested with plain `cargo test` off-device. It
  also carries ALL the gemcli device functions (a72, backlight,
  battery, boot, devmem, gpio, gpu, guard, i2c, power, profile,
  session, sleep, speaker, status, wdt) — moved out of the gemcli
  crate 2026-09-12 so `gemcli` and gemshell share ONE implementation.
- **`gemdata-dummy`** — in-memory fake (selected in
  `GEMSHELL_NESTED` mode) whose mutations actually change the fake state,
  so toggles/connect/password flows round-trip on the workstation.

`gemcli` is now a thin clap frontend over `gemdata-device` (parse →
call; no logic). The gemshell status poller
(`compositor/status.rs`) reads `DataProvider::status()` every 2 s; the
settings panel reads the full snapshots on open / after a mutation /
every 3 s. The actual interfaces:

- **Battery** — sysfs `/sys/class/power_supply/bq25890-battery-*`
  (capacity is voltage-derived, no fuel gauge); `bq25890-charger-*`
  `status`/`online` for the charge bolt.
- **Wi-Fi** — `nmcli` (NetworkManager): `radio wifi`, `device wifi
  list`, `device wifi connect`, `connection down`.
- **Bluetooth** — `bluetoothctl show/devices/connect/scan`.
- **Volume / sinks** — `wpctl get-volume` / `wpctl status` /
  `set-default` / `set-volume` / `set-mute` (PipeWire at /run/gemwl-audio).
- **Brightness** — `/sys/class/backlight/*/brightness` (world-writable
  via the plumbing udev rule).

## Settings panel (egui)

An **in-process, GPU-tessellated** overlay drawn by the compositor
(`src/shell.rs` + `compositor/render.rs::draw_egui`). No separate client,
no CPU rasterization:

- egui does font loading/shaping, layout, scrolling, focus and text
  editing. It emits premultiplied triangle meshes + a font-atlas texture
  delta; `draw_egui` uploads the atlas and draws the meshes with a GLES
  index shader (`EGUI_VERT`/`EGUI_FRAG`), clip rects → GL scissor. The
  UI runs at `PPP = 2.0` (1080×540 points for the 2160×1080 scene).
- Screens: **Wi-Fi** (radio toggle, network cards with signal/security,
  Connect / password dialog, Disconnect), **Bluetooth** (power, known
  devices, Connect, scan), **Audio** (volume slider, mute, output/sink
  radio list → `set_default_sink`), **Display** (interface scale
  100% / 150% / 200% + brightness slider → `set_brightness`, Fn-key hint).
  The Audio list includes the
  L/R-correcting `gemini_speakers` filter node as **Built-in Speakers**
  next to the hardware **Headphones / Jack** sink; `wpctl status` only
  shows the filter under its node name, so the provider resolves
  `node.description` + the filter default through `wpctl inspect` and
  selects by numeric id. The amp itself follows the default sink in
  `gemini-speakerd`, so picking a sink is enough.
- Fonts: `GEMSHELL_FONT` (DejaVu on device) is registered first with
  egui's bundled fonts as fallback, so text never disappears.
- Open with the status-bar gear / wifi / bt / speaker zones;
  Escape or Close dismisses. `GEMSHELL_OPEN_SETTINGS=1` opens it at
  startup (dev).

**2026-09-12 text/UI fix.** The previous hand-rolled glyph atlas had a
double-origin bug in `common/font.rs`: `OutlinedGlyph::draw` already
yields raster-local pixel coordinates, but the code subtracted
`bounds.min` again, pushing every glyph above the baseline out of its
raster — the all-blank text. The atlas build now uses the coordinates
directly. (The chrome text in `ui.rs` uses the same fixed atlas.)

**2026-09-11 advance fix (the other half of "no text").** The same
module computed the pen advance as `h_advance_unscaled(id) * px` — raw
font units (~600) × px size = ~16 000 px per glyph. Every `Op::Text`
rendered only its first character and every `Op::TextCentered` string
was centred ~10 000 px off-screen. The atlas now stores
`face.as_scaled(px).h_advance(id)` and `Font::text_width` is correct, so
chrome/launcher text lays out properly. This is verified by a nested
screenshot (title/Close/labels all present).

## Session wiring

`gemcli session set gemshell [--apply]` writes the marker
(`/var/lib/gemini/desktop`); on the next boot
`gemini-desktop-apply` (Before=display-manager.service) creates the
`/run/gemini-console` sentinel (GDM skipped) WITHOUT starting a tty1
getty (no chvt — the compositor owns the panel; a console on tty1
would fight the shadow-plane blit for the same scanout memory).
`gemini-gemshell.service` (User=cjdell) runs the compositor after the
GPU power-on + panfrost-load (same ordering as the GNOME module) and
force-disables the legacy gemwl compositor (it would double-drive the
panel via /dev/gemfb).

cjdell groups gain `input` (/dev/input evdev) and `bluetooth`
(bluetoothctl) — `video`/`audio`/`networkmanager` already there.

## Footprint (target, honest numbers owed on glass)

The GL floor is Mesa/panfrost: expect the compositor RSS in the
~40–60 MB range (comparable to any GL app; weston ≈ 50 MB), NOT
literally "a handful of MB" — that budget is unreachable once the GPU
driver is loaded, same as for GNOME's mutter. The point vs GNOME: one
~50 MB compositor replaces gdm + mutter + gsd-* + gnome-shell's app
suite (hundreds of MB), and nothing else runs by default. Measure
`smem`/`/proc/*/status` on glass and record here.

## Build

- x86_64 fast loop: `bash bin/gemshell-host-check.sh` (cargo check/test
  of the whole workspace in a nix shell: cargo/rustc/pkg-config +
  wayland/libxkbcommon/libglvnd). `--test` runs the data-crate unit
  tests (the gemshell bin can't link on the host).
- aarch64 (real): `sudo nix build --store local .#packages.aarch64-linux.gemshell`
  (or the full toplevel; long builds under `bin/run-job.sh`). `gemcli`
  builds from the same workspace via `pkgs/gemcli.nix`
  (`buildAndTestSubdir = crates/gemcli`).
- Cargo.lock is committed (crate set pinned, incl. egui 0.29).
- The nix `src` excludes the host `target/` tree (`cleanSourceWith`).

## On-glass bring-up checklist (owed)

1. Boot with `gemcli session set gemshell --reboot`; confirm the panel
   shows the gemshell scene (NOT the uninitialised flicker — rule 5:
   if it flickers, stop → TWRP → fix the image).
2. Status bar: time advances; battery % ≈ UPower's; wifi icon tracks
   the NM connection; tapping gear/wifi/bt/sound opens the settings panel.
3. Settings panel (egui): connect a known Wi-Fi (SSID+password over the
   hardware keyboard), toggle BT, pick a sink, move the volume slider.
4. Touch: tap-launch from the launcher; drag a window by its titlebar
   to an edge (snap preview + snap); double-tap titlebar maximize;
   2-finger swipe switches workspaces (slide); 2-finger up = next app;
   3-finger up/down launcher/home; one-finger scroll inside an app.
   With a maximized app focused, the **status-bar ✕ / restore /
   minimize** controls (left of the clock) must close, un-maximize and
   minimize it; the app's own CSD ✕ must really quit it.
5. Keyboard: Fn+Tab switcher, Fn+←/→ snap, Fn+↑/↓ workspaces,
   Fn+M maximize, Fn+⌫ close, Fn+B/N brightness (panel dims),
   Fn+C/V/T volume. **Regression:** plain `c`/`v`/`b`/`n` (no Fn) must
   insert letters, never change volume/brightness.
6. Run gemdemo under gemshell (GL client path) at 60 fps.
7. Record: RSS (compositor), fps, boot time, anything that needed a fix
   here + in the session log.
