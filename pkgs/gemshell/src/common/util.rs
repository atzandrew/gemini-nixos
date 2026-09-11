//! Tiny filesystem/time helpers.

use std::fs;
use std::time::{SystemTime, UNIX_EPOCH};

/// Milliseconds since the unix epoch (monotonic enough for UI timing).
pub fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

/// Treat an environment variable as a boolean flag: set and not one of
/// the false-y spellings ("", "0", "false", "no", "off"). `is_some()`
/// alone made `GEMSHELL_OPEN_SETTINGS=0` open the panel.
pub fn env_flag(name: &str) -> bool {
    match std::env::var(name) {
        Ok(v) => !matches!(
            v.trim().to_ascii_lowercase().as_str(),
            "" | "0" | "false" | "no" | "off"
        ),
        Err(_) => false,
    }
}

/// Read a file as a string; None on any error.
pub fn read_to_string(p: &std::path::Path) -> Option<String> {
    fs::read_to_string(p).ok()
}

/// First existing path in `candidates`.
pub fn first_existing(candidates: &[&std::path::Path]) -> Option<std::path::PathBuf> {
    candidates
        .iter()
        .find(|p| p.exists())
        .map(|p| p.to_path_buf())
}

/// `which` for one tool over $PATH (the first executable match).
pub fn which(name: &str) -> Option<std::path::PathBuf> {
    let path = std::env::var_os("PATH")?;
    for dir in std::env::split_paths(&path) {
        let cand = dir.join(name);
        if cand.is_file() {
            return Some(cand);
        }
    }
    None
}

/// Depth-first search for the first file under `dir` matching `f`.
pub fn walk(dir: &str, f: impl Fn(&std::path::Path) -> bool) -> Option<std::path::PathBuf> {
    let mut stack = vec![std::path::PathBuf::from(dir)];
    while let Some(d) = stack.pop() {
        if let Ok(rd) = std::fs::read_dir(&d) {
            for e in rd.flatten() {
                let p = e.path();
                if p.is_dir() {
                    stack.push(p);
                } else if f(&p) {
                    return Some(p);
                }
            }
        }
    }
    None
}

/// Find a UI TTF: `$GEMSHELL_FONT` (the service sets the DejaVu store
/// path) then the usual profile/user font dirs. Shared by the compositor
/// and gemsettings.
pub fn find_font() -> Option<std::path::PathBuf> {
    if let Ok(p) = std::env::var("GEMSHELL_FONT") {
        let pb = std::path::PathBuf::from(&p);
        if pb.is_file() {
            return Some(pb);
        }
    }
    // macOS ships its fonts as .ttc/.otf in /System/Library/Fonts; the
    // plain-TTF files live in Supplemental. Prefer a real UI face over
    // whatever the directory walk hits first (Apple Braille, ...).
    for file in [
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/Supplemental/Verdana.ttf",
        "/System/Library/Fonts/Supplemental/Tahoma.ttf",
        "/System/Library/Fonts/Geneva.ttf",
    ] {
        let pb = std::path::PathBuf::from(file);
        if pb.is_file() {
            return Some(pb);
        }
    }
    for dir in [
        "/run/current-system/sw/share/fonts",
        "/home/cjdell/.local/share/fonts",
        "/home/cjdell/.nix-profile/share/fonts",
        "/usr/share/fonts/dejavu",
        "/usr/share/fonts/truetype",
        "/usr/share/fonts/TTF",
        "/usr/share/fonts",
        "/System/Library/Fonts/Supplemental",
        "/System/Library/Fonts",
        "/Library/Fonts",
    ] {
        if let Some(p) = walk(dir, |p| {
            p.extension().and_then(|e| e.to_str()) == Some("ttf")
        }) {
            return Some(p);
        }
    }
    None
}

/// The XDG data base directories, highest precedence first:
/// `$XDG_DATA_HOME` (default `$HOME/.local/share`) then every
/// colon-separated `$XDG_DATA_DIRS` entry (default
/// `/usr/local/share:/usr/share`).
///
/// NixOS puts the system profile at `/run/current-system/sw/share`
/// via XDG_DATA_DIRS — there is no `/usr/share/applications`, so the
/// old hard-coded scan found zero `.desktop` files (on glass
/// 2026-09-11: "0 apps from .desktop files").
pub fn xdg_data_dirs(home: &str) -> Vec<std::path::PathBuf> {
    let mut out = Vec::new();
    let data_home = std::env::var_os("XDG_DATA_HOME")
        .map(std::path::PathBuf::from)
        .filter(|p| p.is_absolute())
        .unwrap_or_else(|| std::path::PathBuf::from(home).join(".local/share"));
    out.push(data_home);
    match std::env::var_os("XDG_DATA_DIRS") {
        Some(v) if !v.is_empty() => out.extend(std::env::split_paths(&v)),
        _ => {
            out.push(std::path::PathBuf::from("/usr/local/share"));
            out.push(std::path::PathBuf::from("/usr/share"));
        }
    }
    out
}

/// Local time (hour, minute) — no chrono (the device runs UTC; the
/// settings app is expected to be used with the system clock = UTC
/// until a tz is picked; `time.timeZone` applies to glibc-aware tools,
/// but this is a raw clock read: matches what `date` shows in a
/// systemd-journald timestamp).
pub fn clock_hm() -> (u32, u32) {
    let s = now_ms() / 1000;
    let secs_of_day = (s % 86400) as u32;
    (secs_of_day / 3600, (secs_of_day % 3600) / 60)
}
