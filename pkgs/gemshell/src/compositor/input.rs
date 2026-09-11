//! Input — raw evdev (keyboard + multitouch) + xkbcommon.
//!
//! The kernel reports the touch panel in raw portrait coordinates
//! (identity with the 1080x2160 scene — the DT inverted-x+y transform
//! was verified on glass 2026-09-10m), so we normalize against the
//! ABS ranges and map 1:1. The keyboard is the AW9523 gpio-matrix
//! (Fn = KEY_RIGHTALT, level-3 — xkb layout "gemini" carries the Fn
//! layer; config/xkb/symbols/gemini).
//!
//! This module is purely evdev -> [`Event`] + xkb key state. Gesture
//! policy (which finger does what, snapping, workspaces) lives in the
//! compositor (mod.rs) — it needs window/UI state.

use crate::common::util;
use crate::platform::{InputEvent, InputSource};
use std::ffi::CString;
use std::os::fd::RawFd;

// ---- xkbcommon FFI (the xkbcommon crate exposes the same lib; the
// ---- compositor links libxkbcommon directly, so a local shim keeps
// ---- the settings binary (which needs no xkb) free of the dep) ----
mod xkb {
    pub type xkb_context_t = std::os::raw::c_void;
    pub type xkb_keymap_t = std::os::raw::c_void;
    pub type xkb_state_t = std::os::raw::c_void;

    #[repr(C)]
    pub struct xkb_rule_names {
        pub rules: *const std::os::raw::c_char,
        pub model: *const std::os::raw::c_char,
        pub layout: *const std::os::raw::c_char,
        pub variant: *const std::os::raw::c_char,
        pub options: *const std::os::raw::c_char,
    }

    extern "C" {
        pub fn xkb_context_new(flags: u32) -> *mut xkb_context_t;
        pub fn xkb_context_unref(ctx: *mut xkb_context_t);
        pub fn xkb_keymap_new_from_names(
            ctx: *mut xkb_context_t,
            rmlvo: *const xkb_rule_names,
            format: u32,
        ) -> *mut xkb_keymap_t;
        pub fn xkb_keymap_unref(km: *mut xkb_keymap_t);
        pub fn xkb_keymap_get_as_string(
            km: *mut xkb_keymap_t,
            format: u32,
        ) -> *const std::os::raw::c_char;
        pub fn xkb_keymap_mod_get_index(
            km: *mut xkb_keymap_t,
            name: *const std::os::raw::c_char,
        ) -> u8;
        pub fn xkb_state_new(km: *mut xkb_keymap_t) -> *mut xkb_state_t;
        pub fn xkb_state_unref(st: *mut xkb_state_t);
        pub fn xkb_state_update_key(st: *mut xkb_state_t, key: u32, dir: u32) -> u32;
        pub fn xkb_state_key_get_one_sym(st: *mut xkb_state_t, key: u32) -> u32;
        // NOTE: this store's xkbcommon 1.13.1 (aarch64, nixpkgs 26.11 pin)
        // exports ONLY the V_0.5.0-era state API: the newer
        // xkb_state_mods_get_mask / xkb_state_mod_get_* / xkb_state_
        // group_get_index family is NOT in its dynamic symbol table
        // (verified 2026-09-11 by readelf on the store .so — the
        // aarch64 build died with undefined references to each of them
        // in turn). Build the wl modmap masks per slot with
        // xkb_state_mod_index_is_active instead.
        pub fn xkb_state_mod_index_is_active(
            st: *mut xkb_state_t,
            mod_index: u8,
            state: u32,
        ) -> u32;
    }
    pub const XKB_KEY_UP: u32 = 0;
    pub const XKB_KEY_DOWN: u32 = 1;
    pub const XKB_STATE_MODS_DEPRESSED: u32 = 1;
    pub const XKB_STATE_MODS_LATCHED: u32 = 2;
    pub const XKB_STATE_MODS_LOCKED: u32 = 4;
    pub const XKB_KEYMAP_FORMAT_TEXT_V1: u32 = 1;
}

// evdev constants
const EV_SYN: u16 = 0;
const EV_KEY: u16 = 1;
const EV_ABS: u16 = 3;
const SYN_REPORT: u16 = 0;
const ABS_MT_POSITION_X: u16 = 53; // 0x35
const ABS_MT_POSITION_Y: u16 = 54; // 0x36
const ABS_MT_TRACKING_ID: u16 = 57; // 0x39
                                    // ABS_MT_SLOT = 0x2f = 47 (linux/input-event-codes.h). This was
                                    // mistakenly 57 (= ABS_MT_TRACKING_ID) — and because the SLOT arm is
                                    // matched FIRST, every TRACKING_ID event was swallowed as a slot change,
                                    // so a finger-down was never registered and touch was completely dead
                                    // (root cause on glass, 2026-09-12). 57 is TRACKING_ID, 47 is SLOT.
const ABS_MT_SLOT: u16 = 47; // 0x2f

const MAX_SLOTS: usize = 32;

#[derive(Clone, Copy, Debug, Default)]
struct AbsRange {
    min: f32,
    max: f32,
}

#[repr(C)]
#[derive(Default)]
struct RawEvent {
    tv_sec: i64,
    tv_usec: i64,
    etype: u16,
    code: u16,
    value: i32,
}

#[repr(C)]
#[derive(Default)]
struct AbsInfo {
    value: i32,
    min: i32,
    max: i32,
    fuzz: i32,
    flat: i32,
    resolution: i32,
}

fn eviocgabs(code: u16) -> u64 {
    // EVIOCGABS(abs) = _IOR('E', 0x40 + abs, struct input_absinfo) —
    // linux/input.h. _IOC(_IOC_READ=2, 'E'=0x45, 0x40+abs, size=24).
    // The first cut had the direction (0x40<<28 = _IOC_WRITE), type
    // (0x15 instead of 'E') and nr (abs instead of 0x40+abs) all wrong,
    // so the ioctl silently failed and abs_range fell back to the
    // hard-coded 0..1079 / 0..2159 defaults (which happened to match the
    // NT36772, masking the bug). Fixed 2026-09-11.
    (2u64 << 30) | (24u64 << 16) | (0x45u64 << 8) | (0x40 + code as u64)
}

/// `EVIOCGKEY(96)`: the input core's current key bitmap.
/// `KEY_CNT = KEY_MAX + 1 = 0x300` bits → 96 bytes (the kernel caps at
/// `BITS_TO_LONGS(KEY_CNT) * sizeof(long)`, which is 96 either arch).
const EVIOCGKEY_LEN: usize = 0x300 / 8;
const EVIOCGKEY: libc::c_ulong =
    (2u64 << 30) | ((EVIOCGKEY_LEN as u64) << 16) | (0x45u64 << 8) | 0x18u64;

/// Fn on the Gemini keymatrix is `KEY_RIGHTALT` (AltGr / level 3).
const KEY_RIGHTALT: u32 = 100;
/// Wayland modmask bit for Mod5 (the slot `ISO_Level3_Shift` lands in on
/// the gemini layout; matches `compositor::MOD5`).
const MOD5: u32 = 1 << 8;

/// An input event the compositor acts on.
///
/// The neutral definition (shared with the macOS backend) lives in
/// [`crate::platform`]; this module re-exports it so the evdev code reads
/// naturally.
pub use crate::platform::InputEvent as Event;

pub struct Input {
    pub kbd_fd: std::os::raw::c_int,
    pub touch_fd: std::os::raw::c_int,
    pub kbd_name: String,
    pub touch_name: String,
    ctx: *mut xkb::xkb_context_t,
    keymap: *mut xkb::xkb_keymap_t,
    state: *mut xkb::xkb_state_t,
    keymap_str: Option<String>,
    pub pressed: Vec<u32>,
    /// wl modmap slot -> xkb mod bit
    slot_bits: [u32; 13],
    /// wl modmap slot -> xkb mod index (255 = absent)
    slot_idx: [u8; 13],
    slot_x: [f32; MAX_SLOTS],
    slot_y: [f32; MAX_SLOTS],
    slot_down: [bool; MAX_SLOTS],
    // Per-frame MT state: which slots changed this frame and whether the
    // change was a contact down/up. The NT36772 driver reports
    // ABS_MT_TRACKING_ID *before* ABS_MT_POSITION_X/Y
    // (novatek-nt36xxx.c nvt36xxx_report: input_mt_slot ->
    // input_mt_report_slot_state -> touchscreen_report_pos), so events
    // MUST be emitted at SYN_REPORT using the now-updated positions —
    // emitting at TRACKING_ID time used the previous contact's (or
    // 0,0) coordinates, which made every tap land at the origin
    // ("no touch input at all", on glass 2026-09-12).
    slot_changed: [bool; MAX_SLOTS],
    slot_new_down: [bool; MAX_SLOTS],
    slot_new_up: [bool; MAX_SLOTS],
    touch_rotate: i32,
    cur_slot: usize,
    x_range: AbsRange,
    y_range: AbsRange,
}

/// Scan /dev/input for the keyboard + touch nodes (by device name).
/// Returns (kbd path, touch path, kbd name, touch name).
/// Find the keyboard + touch evdev nodes.
///
/// Scans /sys/class/input/inputN (name + dev major:minor) and
/// classifies by CAPABILITY (EVIOCGBIT), not name string: a device with
/// EV_ABS + ABS_MT_SLOT is the touch panel (NT36772 protocol-B); an
/// EV_KEY-only device is a keyboard (prefer the AW9523 by name, else
/// the first EV_KEY-only node — mt6351-keys is the power key cluster).
/// The earlier /dev/input/eventN/device/name probe was wrong: eventN is
/// a plain char device, so that path never exists (first on-glass run
/// 2026-09-11: "no keyboard/touch evdev node").
pub fn find_nodes() -> (Option<String>, Option<String>, String, String) {
    let mut kbd: Option<(String, String)> = None; // (path, name)
    let mut kbd_fallback: Option<(String, String)> = None;
    let mut touch: Option<(String, String)> = None;
    let Ok(rd) = std::fs::read_dir("/sys/class/input") else {
        return (None, None, String::new(), String::new());
    };
    for e in rd.flatten() {
        let base = e.path();
        let name = util::read_to_string(&base.join("name")).map(|s| s.trim().to_string());
        let Some(name) = name else { continue };
        // The evdev node NAME: inputN/eventM/ → /dev/input/eventM.
        // NOTE (verified on glass 2026-09-11): the node name is the
        // device INDEX, while the minor in eventM/dev is 64+index —
        // deriving the name from the minor (event{minor}) pointed at a
        // non-existent node ("event65" for index 1). Use the subdir
        // name; inputN/dev is also not exported by this kernel.
        let event = (|| -> Option<String> {
            let Ok(evs) = std::fs::read_dir(&base) else {
                return None;
            };
            for ev in evs.flatten() {
                if let Some(n) = ev.file_name().to_str() {
                    if n.starts_with("event") {
                        return Some(n.to_string());
                    }
                }
            }
            None
        })();
        let Some(event) = event else { continue };
        let path = format!("/dev/input/{event}");
        // Capabilities from sysfs (the capabilities/ dir; no ioctl
        // needed): ev = type bits, abs/key = the per-type code bitmaps.
        // NOTE: the files are space-separated hex WORDS on this kernel
        // (the key bitmap = "169000000000 35ff57ff3ffcffc"; ev/abs are
        // single words) — OR the words together (they are disjoint
        // 32-bit slices, so order does not matter).
        let hex = |f: &std::path::Path| -> u64 {
            util::read_to_string(f)
                .map(|s| {
                    s.split_whitespace()
                        .fold(0u64, |acc, t| acc | u64::from_str_radix(t, 16).unwrap_or(0))
                })
                .unwrap_or(0)
        };
        let ev = hex(&base.join("capabilities/ev"));
        let abs = hex(&base.join("capabilities/abs"));
        let key = hex(&base.join("capabilities/key"));
        let has_abs = ev & (1 << 0x03) != 0;
        let has_key = ev & (1 << 0x01) != 0;
        let is_touch = has_abs && abs & (1 << 0x2f) != 0; // ABS_MT_SLOT
        let lower = name.to_lowercase();
        if is_touch {
            touch = Some((path.clone(), name.clone()));
        } else if has_key && key != 0 {
            // EV_KEY device: prefer the AW9523 main keyboard by name.
            let pref = lower.contains("aw9523") || lower == "keyboard";
            if pref && kbd.is_none() {
                kbd = Some((path.clone(), name.clone()));
            } else if kbd_fallback.is_none() {
                kbd_fallback = Some((path.clone(), name.clone()));
            }
        }
    }
    let kbd = kbd.or(kbd_fallback);
    match (kbd, touch) {
        (Some((kp, kn)), Some((tp, tn))) => (Some(kp), Some(tp), kn, tn),
        (k, t) => (
            k.as_ref().map(|x| x.0.clone()),
            t.as_ref().map(|x| x.0.clone()),
            k.map(|x| x.1).unwrap_or_default(),
            t.map(|x| x.1).unwrap_or_default(),
        ),
    }
}

/// Open an evdev node read-only.
/// (EVIOCGBIT(ev,len) = _IOC(_IOC_READ, 'E', 0x20+ev, len); we classify
/// devices from the sysfs capabilities/ files instead, see find_nodes.)
///
/// **O_NONBLOCK is load-bearing (fixed 2026-09-12):** `read_keyboard` /
/// `read_touch` drain their node with `loop { read; if short { break } }`.
/// Without O_NONBLOCK the read after the queue drains BLOCKS, and the
/// whole compositor freezes on the first input event (observed on glass:
/// the main thread sat in `evdev_read` on fd 3 → "touch unresponsive").
fn open_ro(path: &str) -> Result<std::os::raw::c_int, String> {
    use std::os::unix::ffi::OsStrExt;
    let fd = unsafe {
        libc::open(
            path.as_ptr() as *const std::os::raw::c_char,
            libc::O_RDONLY | libc::O_CLOEXEC | libc::O_NONBLOCK,
        )
    };
    if fd < 0 {
        return Err(format!("open {path}: {}", std::io::Error::last_os_error()));
    }
    Ok(fd)
}

fn abs_range(fd: std::os::raw::c_int, cx: u16, cy: u16) -> Option<(AbsRange, AbsRange)> {
    let mut ax = AbsInfo::default();
    let mut ay = AbsInfo::default();
    let rcx = unsafe { libc::ioctl(fd, eviocgabs(cx) as _, &mut ax as *mut AbsInfo) };
    let rcy = unsafe { libc::ioctl(fd, eviocgabs(cy) as _, &mut ay as *mut AbsInfo) };
    if rcx < 0 || rcy < 0 {
        return None;
    }
    Some((
        AbsRange {
            min: ax.min as f32,
            max: ax.max as f32,
        },
        AbsRange {
            min: ay.min as f32,
            max: ay.max as f32,
        },
    ))
}

impl Input {
    pub fn open(
        kbd_path: &str,
        touch_path: &str,
        kbd_name: &str,
        touch_name: &str,
    ) -> Result<Self, String> {
        let kbd_fd = open_ro(kbd_path)?;
        let touch_fd = open_ro(touch_path)?;
        log::info!("keyboard: {kbd_name} ({kbd_path})");
        log::info!("touch: {touch_name} ({touch_path})");

        // xkb keymap: layout from XKB_DEFAULT_LAYOUT (default "gemini");
        // XKB_CONFIG_EXTRA_PATH carries the gemini symbols dir.
        let layout = std::env::var("XKB_DEFAULT_LAYOUT").unwrap_or_else(|_| "gemini".into());
        log::info!("keymap layout: {layout}");
        let ctx = unsafe { xkb::xkb_context_new(0) };
        if ctx.is_null() {
            return Err("xkb_context_new failed".into());
        }
        let layout_c = CString::new(layout.clone()).map_err(|_| "nul".to_string())?;
        let rules_opt = std::env::var("XKB_DEFAULT_RULES")
            .ok()
            .and_then(|s| CString::new(s).ok());
        let options_opt = std::env::var("XKB_DEFAULT_OPTIONS")
            .ok()
            .and_then(|s| CString::new(s).ok());
        let rmlvo = xkb::xkb_rule_names {
            rules: rules_opt
                .as_ref()
                .map(|c| c.as_ptr())
                .unwrap_or(std::ptr::null()),
            model: std::ptr::null(),
            layout: layout_c.as_ptr(),
            variant: std::ptr::null(),
            options: options_opt
                .as_ref()
                .map(|c| c.as_ptr())
                .unwrap_or(std::ptr::null()),
        };
        // 3rd arg is COMPILE FLAGS (0 = NO_FLAGS), not the format enum
        // (XKB_KEYMAP_FORMAT_TEXT_V1 == 1 → "unrecognized keymap
        // compilation flags: 0x1" from the lib, verified 2026-09-11).
        let keymap = unsafe { xkb::xkb_keymap_new_from_names(ctx, &rmlvo, 0) };
        if keymap.is_null() {
            return Err(format!(
                "xkb_keymap_new_from_names failed (layout={layout})"
            ));
        }
        let keymap_str = unsafe {
            let s = xkb::xkb_keymap_get_as_string(keymap, xkb::XKB_KEYMAP_FORMAT_TEXT_V1);
            if s.is_null() {
                String::new()
            } else {
                std::ffi::CStr::from_ptr(s).to_string_lossy().into_owned()
            }
        };
        log::info!("keymap compiled ({layout}): {} bytes", keymap_str.len());
        let state = unsafe { xkb::xkb_state_new(keymap) };
        if state.is_null() {
            return Err("xkb_state_new failed".into());
        }
        let mut slot_bits = [0u32; 13];
        let mut slot_idx = [32u8; 13]; // slot -> xkb mod index (255 = absent)
        for (slot, name) in [
            "", "Shift", "Lock", "Control", "Mod1", "Mod2", "Mod3", "Mod4", "Mod5",
        ]
        .iter()
        .enumerate()
        {
            if slot == 0 {
                continue;
            }
            let idx = unsafe {
                xkb::xkb_keymap_mod_get_index(keymap, CString::new(*name).unwrap().as_ptr())
            };
            if idx < 32 {
                slot_bits[slot] |= 1 << idx;
                slot_idx[slot] = idx;
            }
        }
        let (xr, yr) = abs_range(touch_fd, ABS_MT_POSITION_X, ABS_MT_POSITION_Y).unwrap_or((
            AbsRange {
                min: 0.0,
                max: 1079.0,
            },
            AbsRange {
                min: 0.0,
                max: 2159.0,
            },
        ));
        log::info!(
            "touch ranges x [{:.0}..={:.0}] y [{:.0}..={:.0}]",
            xr.min,
            xr.max,
            yr.min,
            yr.max
        );

        Ok(Input {
            kbd_fd,
            touch_fd,
            kbd_name: kbd_name.to_string(),
            touch_name: touch_name.to_string(),
            ctx,
            keymap,
            state,
            keymap_str: Some(keymap_str),
            pressed: Vec::new(),
            slot_bits,
            slot_idx,
            slot_x: [0.0; MAX_SLOTS],
            slot_y: [0.0; MAX_SLOTS],
            slot_down: [false; MAX_SLOTS],
            slot_changed: [false; MAX_SLOTS],
            slot_new_down: [false; MAX_SLOTS],
            slot_new_up: [false; MAX_SLOTS],
            touch_rotate: touch_rotate_env(),
            cur_slot: 0,
            x_range: xr,
            y_range: yr,
        })
    }

    /// Nested/host mode: no evdev nodes. The keymap is still built
    /// (layout from `XKB_DEFAULT_LAYOUT`, default "gemini", falling back
    /// to "us" if the gemini symbols dir is not on the host); keys are
    /// fed in via [`Input::process_key`].
    pub fn new_virtual() -> Result<Self, String> {
        let (ctx, keymap, keymap_str, slot_bits, slot_idx) = Self::build_xkb()?;
        let state = unsafe { xkb::xkb_state_new(keymap) };
        if state.is_null() {
            return Err("xkb_state_new failed".into());
        }
        Ok(Input {
            kbd_fd: -1,
            touch_fd: -1,
            kbd_name: "nested".into(),
            touch_name: "nested".into(),
            ctx,
            keymap,
            state,
            keymap_str,
            pressed: Vec::new(),
            slot_bits,
            slot_idx,
            slot_x: [0.0; MAX_SLOTS],
            slot_y: [0.0; MAX_SLOTS],
            slot_down: [false; MAX_SLOTS],
            slot_changed: [false; MAX_SLOTS],
            slot_new_down: [false; MAX_SLOTS],
            slot_new_up: [false; MAX_SLOTS],
            touch_rotate: touch_rotate_env(),
            cur_slot: 0,
            x_range: AbsRange {
                min: 0.0,
                max: 1079.0,
            },
            y_range: AbsRange {
                min: 0.0,
                max: 2159.0,
            },
        })
    }

    /// Build the xkb context/keymap/slot tables for the configured
    /// layout. Shared shape with `open` (kept as a helper for the
    /// virtual path; `open` retains its inline copy for now).
    #[allow(clippy::type_complexity)]
    fn build_xkb() -> Result<
        (
            *mut xkb::xkb_context_t,
            *mut xkb::xkb_keymap_t,
            Option<String>,
            [u32; 13],
            [u8; 13],
        ),
        String,
    > {
        let ctx = unsafe { xkb::xkb_context_new(0) };
        if ctx.is_null() {
            return Err("xkb_context_new failed".into());
        }
        let mut layout = std::env::var("XKB_DEFAULT_LAYOUT").unwrap_or_else(|_| "gemini".into());
        let mut keymap = make_keymap(ctx, &layout);
        if keymap.is_null() {
            log::warn!("xkb layout '{layout}' unavailable; falling back to 'us'");
            layout = "us".into();
            keymap = make_keymap(ctx, &layout);
        }
        if keymap.is_null() {
            return Err(format!(
                "xkb_keymap_new_from_names failed (layout={layout})"
            ));
        }
        let keymap_str = unsafe {
            let s = xkb::xkb_keymap_get_as_string(keymap, xkb::XKB_KEYMAP_FORMAT_TEXT_V1);
            if s.is_null() {
                String::new()
            } else {
                std::ffi::CStr::from_ptr(s).to_string_lossy().into_owned()
            }
        };
        log::info!("keymap compiled ({layout}): {} bytes", keymap_str.len());
        let mut slot_bits = [0u32; 13];
        let mut slot_idx = [32u8; 13];
        for (slot, name) in [
            "", "Shift", "Lock", "Control", "Mod1", "Mod2", "Mod3", "Mod4", "Mod5",
        ]
        .iter()
        .enumerate()
        {
            if slot == 0 {
                continue;
            }
            let idx = unsafe {
                xkb::xkb_keymap_mod_get_index(keymap, CString::new(*name).unwrap().as_ptr())
            };
            if idx < 32 {
                slot_bits[slot] |= 1 << idx;
                slot_idx[slot] = idx;
            }
        }
        Ok((ctx, keymap, Some(keymap_str), slot_bits, slot_idx))
    }

    /// Nested input: feed one raw evdev scancode (the host's
    /// `wl_keyboard.key` value) and return (keysym, wl mods mask).
    /// xkbcommon keycodes are scancode + 8.
    pub fn process_key(&mut self, code: u32, pressed: bool) -> (u32, u32) {
        let xkb_code = code + 8;
        let dir = if pressed {
            xkb::XKB_KEY_DOWN
        } else {
            xkb::XKB_KEY_UP
        };
        if pressed {
            if !self.pressed.contains(&code) {
                self.pressed.push(code);
            }
        } else {
            self.pressed.retain(|&k| k != code);
        }
        unsafe { xkb::xkb_state_update_key(self.state, xkb_code, dir) };
        // Recover a stuck AltGr/Fn. The Fn key is KEY_RIGHTALT; if xkb
        // still holds Mod5 down but the input core says RALT is up (a
        // key-up the compositor never consumed — the poll loop can drop
        // events while a frame renders), force it up so the next c/v/b/n
        // is a letter, not the Fn media code. On-glass report 2026-09-12:
        // "CVBN keys change volume/brightness without holding Fn".
        // Only on a real evdev fd: nested mode forwards the host's
        // scancodes, so our own bitmap says nothing about them.
        if self.kbd_fd >= 0
            && code != KEY_RIGHTALT
            && self.current_mods() & MOD5 != 0
            && !self.physical_key_down(KEY_RIGHTALT)
        {
            self.pressed.retain(|&k| k != KEY_RIGHTALT);
            unsafe { xkb::xkb_state_update_key(self.state, KEY_RIGHTALT + 8, xkb::XKB_KEY_UP) };
        }
        let keysym = unsafe { xkb::xkb_state_key_get_one_sym(self.state, xkb_code) };
        (keysym, self.current_mods())
    }

    /// Is `code` physically held right now, per the input core
    /// (`EVIOCGKEY`)? False on a nested/non-evdev fd.
    fn physical_key_down(&self, code: u32) -> bool {
        if self.kbd_fd < 0 || (code as usize) >= EVIOCGKEY_LEN * 8 {
            return false;
        }
        let mut buf = [0u8; EVIOCGKEY_LEN];
        let r = unsafe { libc::ioctl(self.kbd_fd, EVIOCGKEY, buf.as_mut_ptr()) };
        if r < 0 {
            return false;
        }
        let byte = (code / 8) as usize;
        let bit = (code % 8) as u8;
        (buf[byte] >> bit) & 1 == 1
    }

    /// Non-blocking drain of both evdev nodes.
    pub fn read_events(&mut self, scene_w: f32, scene_h: f32) -> Vec<Event> {
        let mut out = Vec::new();
        self.read_keyboard(&mut out);
        self.read_touch(&mut out, scene_w, scene_h);
        out
    }

    fn read_keyboard(&mut self, out: &mut Vec<Event>) {
        let mut ev = RawEvent::default();
        loop {
            let n = unsafe {
                libc::read(
                    self.kbd_fd,
                    &mut ev as *mut _ as *mut _,
                    std::mem::size_of::<RawEvent>(),
                )
            };
            if n != std::mem::size_of::<RawEvent>() as isize {
                break;
            }
            if ev.etype != EV_KEY {
                continue;
            }
            let code = ev.code as u32;
            // xkbcommon keycodes are the evdev scancode + 8; feeding the
            // bare scancode looked up the wrong key (latent bug, found
            // while adding nested input, 2026-09-11).
            let xkb_code = code + 8;
            let dir = if ev.value == 0 {
                xkb::XKB_KEY_UP
            } else {
                xkb::XKB_KEY_DOWN
            };
            unsafe { xkb::xkb_state_update_key(self.state, xkb_code, dir) };
            if ev.value == 0 {
                self.pressed.retain(|&k| k != code);
            } else if ev.value == 1 {
                self.pressed.push(code);
            }
            let keysym = unsafe { xkb::xkb_state_key_get_one_sym(self.state, xkb_code) };
            if keysym == 0 {
                continue;
            }
            // The depressed-mods mask, per slot (see the FFI note: the
            // state mask getters are absent from this store's xkbcommon;
            // the state was updated just above).
            let (mods, _, _) = self.mods_masks();
            let pressed_keysyms: Vec<u32> = self
                .pressed
                .iter()
                .map(|&k| unsafe { xkb::xkb_state_key_get_one_sym(self.state, k + 8) })
                .collect();
            let keymap = if out.iter().all(|e| !matches!(e, Event::Key { .. })) {
                self.keymap_str.clone()
            } else {
                None
            };
            out.push(Event::Key {
                code: code as u32,
                keysym,
                pressed: ev.value != 0,
                mods,
                pressed_keysyms,
                keymap,
            });
        }
    }

    fn read_touch(&mut self, out: &mut Vec<Event>, scene_w: f32, scene_h: f32) {
        let mut ev = RawEvent::default();
        loop {
            let n = unsafe {
                libc::read(
                    self.touch_fd,
                    &mut ev as *mut _ as *mut _,
                    std::mem::size_of::<RawEvent>(),
                )
            };
            if n != std::mem::size_of::<RawEvent>() as isize {
                break;
            }
            match (ev.etype, ev.code) {
                (EV_ABS, ABS_MT_SLOT) => self.cur_slot = ev.value as usize % MAX_SLOTS,
                (EV_ABS, ABS_MT_POSITION_X) => {
                    // Store NORMALISED 0..1; the scene mapping (which
                    // includes the portrait-panel/landscape-scene
                    // rotation) is applied at SYN_REPORT below.
                    self.slot_x[self.cur_slot] = self.norm(ev.value as f32, self.x_range);
                    self.slot_changed[self.cur_slot] = true;
                }
                (EV_ABS, ABS_MT_POSITION_Y) => {
                    self.slot_y[self.cur_slot] = self.norm(ev.value as f32, self.y_range);
                    self.slot_changed[self.cur_slot] = true;
                }
                (EV_ABS, ABS_MT_TRACKING_ID) => {
                    // Record the down/up transition now, but do NOT emit
                    // yet: the driver sends TRACKING_ID before the
                    // POSITION axes, so the coordinates are only final at
                    // SYN_REPORT (see the struct-field note).
                    let s = self.cur_slot;
                    self.slot_changed[s] = true;
                    if ev.value >= 0 && !self.slot_down[s] {
                        self.slot_down[s] = true;
                        self.slot_new_down[s] = true;
                    } else if ev.value < 0 && self.slot_down[s] {
                        self.slot_down[s] = false;
                        self.slot_new_up[s] = true;
                    }
                }
                (EV_SYN, SYN_REPORT) => {
                    for s in 0..MAX_SLOTS {
                        if !(self.slot_changed[s] || self.slot_new_down[s] || self.slot_new_up[s]) {
                            continue;
                        }
                        let (x, y) = self.touch_to_scene(s, scene_w, scene_h);
                        if self.slot_new_down[s] {
                            self.slot_new_down[s] = false;
                            out.push(Event::TouchDown { id: s as u32, x, y });
                        } else if self.slot_new_up[s] {
                            self.slot_new_up[s] = false;
                            out.push(Event::TouchUp { id: s as u32 });
                        } else if self.slot_down[s] {
                            out.push(Event::TouchMotion { id: s as u32, x, y });
                        }
                        self.slot_changed[s] = false;
                    }
                }
                _ => {}
            }
        }
    }

    /// Map a slot's normalised panel coordinates to logical scene
    /// coordinates, applying the panel counter-rotation. The LK fb (and
    /// therefore the NT36772 evdev range) is PORTRAIT 1080x2160 while the
    /// logical scene is landscape; `rotate` mirrors the display present
    /// rotation (`GEMSHELL_TOUCH_ROTATE`, default 270). See
    /// docs/gemshell.md "Orientation".
    fn touch_to_scene(&self, slot: usize, scene_w: f32, scene_h: f32) -> (f32, f32) {
        let nx = self.slot_x[slot].clamp(0.0, 1.0);
        let ny = self.slot_y[slot].clamp(0.0, 1.0);
        match self.touch_rotate {
            90 => (ny * scene_w, (1.0 - nx) * scene_h),
            180 => ((1.0 - nx) * scene_w, (1.0 - ny) * scene_h),
            270 => ((1.0 - ny) * scene_w, nx * scene_h),
            _ => (nx * scene_w, ny * scene_h),
        }
    }

    fn norm(&self, v: f32, r: AbsRange) -> f32 {
        if r.max <= r.min {
            0.0
        } else {
            (v - r.min) / (r.max - r.min)
        }
    }

    pub fn keymap_string(&self) -> Option<String> {
        self.keymap_str.clone()
    }

    /// The currently depressed mods, wl-style (bit i = modmap slot i).
    pub fn current_mods(&self) -> u32 {
        let mut mods = 0u32;
        for slot in 1..13 {
            if self.slot_idx[slot] < 32
                && unsafe {
                    xkb::xkb_state_mod_index_is_active(
                        self.state,
                        self.slot_idx[slot],
                        xkb::XKB_STATE_MODS_DEPRESSED,
                    )
                } != 0
            {
                mods |= 1 << slot;
            }
        }
        mods
    }

    /// The full wl_keyboard modifiers-event masks (depressed, latched,
    /// locked) in modmap slot space — per-slot via
    /// xkb_state_mod_index_is_active (see the FFI note for why the mask
    /// getters are off the table on this store's xkbcommon).
    pub fn mods_masks(&self) -> (u32, u32, u32) {
        let mut out = [0u32; 3];
        for slot in 1..13 {
            if self.slot_idx[slot] >= 32 {
                continue;
            }
            for (i, mode) in [
                xkb::XKB_STATE_MODS_DEPRESSED,
                xkb::XKB_STATE_MODS_LATCHED,
                xkb::XKB_STATE_MODS_LOCKED,
            ]
            .iter()
            .enumerate()
            {
                if unsafe {
                    xkb::xkb_state_mod_index_is_active(self.state, self.slot_idx[slot], *mode)
                } != 0
                {
                    out[i] |= 1 << slot;
                }
            }
        }
        (out[0], out[1], out[2])
    }
}

/// The evdev input source as the platform sees it — a thin delegation to
/// the inherent methods above.
impl InputSource for Input {
    fn poll(&mut self, scene_w: f32, scene_h: f32) -> Vec<InputEvent> {
        self.read_events(scene_w, scene_h)
    }

    fn process_key(&mut self, code: u32, pressed: bool) -> (u32, u32) {
        Input::process_key(self, code, pressed)
    }

    fn keymap_string(&self) -> Option<String> {
        Input::keymap_string(self)
    }

    fn mods_masks(&self) -> (u32, u32, u32) {
        Input::mods_masks(self)
    }

    fn poll_fds(&self) -> (RawFd, RawFd) {
        (self.kbd_fd as RawFd, self.touch_fd as RawFd)
    }
}

/// Touch counter-rotation: `GEMSHELL_TOUCH_ROTATE` (default: the same
/// value as `GEMSHELL_ROTATE`, i.e. 90 — the panel is portrait-mounted).
fn touch_rotate_env() -> i32 {
    if let Ok(v) = std::env::var("GEMSHELL_TOUCH_ROTATE") {
        if let Ok(n) = v.parse() {
            return n;
        }
    }
    std::env::var("GEMSHELL_ROTATE")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(270)
}

/// Build an xkb keymap from RMLVO (`XKB_DEFAULT_RULES`/`_OPTIONS` env +
/// the given layout). xkbcommon keys are (evdev scancode + 8).
fn make_keymap(ctx: *mut xkb::xkb_context_t, layout: &str) -> *mut xkb::xkb_keymap_t {
    let Ok(layout_c) = CString::new(layout) else {
        return std::ptr::null_mut();
    };
    let rules_opt = std::env::var("XKB_DEFAULT_RULES")
        .ok()
        .and_then(|s| CString::new(s).ok());
    let options_opt = std::env::var("XKB_DEFAULT_OPTIONS")
        .ok()
        .and_then(|s| CString::new(s).ok());
    let rmlvo = xkb::xkb_rule_names {
        rules: rules_opt
            .as_ref()
            .map(|c| c.as_ptr())
            .unwrap_or(std::ptr::null()),
        model: std::ptr::null(),
        layout: layout_c.as_ptr(),
        variant: std::ptr::null(),
        options: options_opt
            .as_ref()
            .map(|c| c.as_ptr())
            .unwrap_or(std::ptr::null()),
    };
    unsafe { xkb::xkb_keymap_new_from_names(ctx, &rmlvo, 0) }
}

impl Drop for Input {
    fn drop(&mut self) {
        unsafe {
            xkb::xkb_state_unref(self.state);
            xkb::xkb_keymap_unref(self.keymap);
            xkb::xkb_context_unref(self.ctx);
        }
    }
}
