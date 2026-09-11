//! Boot-target selection via the `para` partition (32-byte MISC
//! command at offset 0) — device-side ports of
//! services/scripts/gemini-boot-recovery + gemini-boot-debian, plus the
//! NixOS default (para clear).
//!
//! LK boots RECOVERY whenever the command is exactly "boot-recovery"
//! (docs/boot-chain.md §8c). The dual-boot initramfs in `boot` (p22)
//! picks the OS from the same command (docs/repartition-android-space.md
//! §5): "boot-debian" → Debian p29, anything else / zeros → NixOS (p32
//! userdata, the default). LK ignores the boot-debian marker — only the
//! exact "boot-recovery" command sends LK to TWRP.
//!
//! The 32-byte layout (AGENTS.md cheat-sheet): "boot-recovery\0" + 18
//! zeros; "boot-debian\0" + 20 zeros; 32 zeros = default.
//!
//! [corrected 2026-09-09] gemini-boot-recovery wrote a SHORT 15-byte
//! record (no conv=sync, so dd left bytes 15..31 of the 32-byte region
//! untouched); gemcli always writes the full padded 32-byte command,
//! matching the newer gemini-boot-debian behaviour + the docs' layout.

use std::io::{Read, Seek, SeekFrom, Write};
use std::path::PathBuf;

use crate::error::{cmsg, Res};

pub const PARA_CANDIDATES: [&str; 3] = [
    "/dev/block/platform/mtk-msdc.0/11230000.msdc0/by-name/para",
    "/dev/block/by-name/para",
    "/dev/mmcblk0p2",
];

pub fn find_para() -> Option<PathBuf> {
    for p in PARA_CANDIDATES {
        let pb = PathBuf::from(p);
        if pb.exists() {
            return Some(pb);
        }
    }
    None
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ParaState {
    Twrp,   // "boot-recovery" — LK boots TWRP on next power-on
    Debian, // "boot-debian" — initrd boots Debian p29
    Nixos,  // zeros — initrd boots NixOS p32 (default)
    Other(String),
}

impl ParaState {
    pub fn describe(&self) -> &str {
        match self {
            ParaState::Twrp => "boot-recovery (TWRP on next power-on)",
            ParaState::Debian => "boot-debian (Debian p29 on next power-on)",
            ParaState::Nixos => "clear (NixOS p32 — the default)",
            ParaState::Other(_) => "unknown marker",
        }
    }
}

/// Read the current 32-byte para command.
pub fn read_cmd() -> Res<(PathBuf, [u8; 32])> {
    let Some(p) = find_para() else {
        return Err(cmsg("para partition not found"));
    };
    let mut f = std::fs::File::open(&p).map_err(|e| cmsg(format!("{}: {e}", p.display())))?;
    let mut buf = [0u8; 32];
    f.read_exact(&mut buf)
        .map_err(|e| cmsg(format!("{}: read: {e}", p.display())))?;
    Ok((p, buf))
}

pub fn decode(buf: &[u8; 32]) -> ParaState {
    if buf.starts_with(b"boot-recovery") {
        ParaState::Twrp
    } else if buf.starts_with(b"boot-debian") {
        ParaState::Debian
    } else if buf.iter().all(|&b| b == 0) {
        ParaState::Nixos
    } else {
        let n = buf.iter().position(|&b| b == 0).unwrap_or(32).min(16);
        ParaState::Other(
            buf[..n]
                .iter()
                .map(|b| {
                    if b.is_ascii_graphic() || *b == b' ' {
                        (*b as char).to_string()
                    } else {
                        format!("\\x{b:02x}")
                    }
                })
                .collect(),
        )
    }
}

/// Write a full 32-byte command (padded with zeros) + fsync, like
/// `printf ... | dd bs=32 count=1 conv=sync,fsync`.
pub fn write_cmd(p: &PathBuf, cmd: &[u8]) -> Res<()> {
    if cmd.len() > 32 {
        return Err(cmsg("command longer than 32 bytes"));
    }
    let mut buf = [0u8; 32];
    buf[..cmd.len()].copy_from_slice(cmd);
    let mut f = std::fs::OpenOptions::new()
        .write(true)
        .open(p)
        .map_err(|e| cmsg(format!("{}: {e}", p.display())))?;
    f.seek(SeekFrom::Start(0))
        .map_err(|e| cmsg(format!("{}: seek: {e}", p.display())))?;
    f.write_all(&buf)
        .map_err(|e| cmsg(format!("{}: write: {e}", p.display())))?;
    f.sync_all()
        .map_err(|e| cmsg(format!("{}: fsync: {e}", p.display())))?;
    Ok(())
}

/// `boot status` — the current para marker + path.
pub fn status_text() -> Res<String> {
    let (p, buf) = read_cmd()?;
    let st = decode(&buf);
    let mut s = String::new();
    s.push_str(&format!(
        "para: {} ({})\n",
        p.display(),
        if find_para().is_some() {
            "present"
        } else {
            "missing"
        }
    ));
    s.push_str(&format!("command: {}\n", st.describe()));
    match st {
        ParaState::Other(txt) => s.push_str(&format!("raw: {txt}\n")),
        _ => {}
    }
    Ok(s)
}

/// Reboot the unit. Plain reboot powers the PDA OFF (TOPRGU) — the
/// STICKY para command decides what the next power-on boots (that is
/// the intended semantics of the boot-recovery/boot-debian CLIs, which
/// the user then completes with a power-on). Tries the graceful
/// systemctl path first, falls back to the reboot(2) syscall.
pub fn reboot_system() -> Res<()> {
    crate::util::sync_all();
    let r = std::process::Command::new("/run/current-system/sw/bin/systemctl")
        .arg("reboot")
        .status();
    if r.is_ok() {
        // Never reached on success (we are being shut down).
        std::thread::sleep(std::time::Duration::from_secs(30));
    }
    // SAFETY: RB_AUTOBOOT — matches the `reboot` command the scripts exec.
    // (Linux-only syscall; the crate also compiles on the macOS dev host,
    // where the boot verbs are unreachable — return an error instead.)
    #[cfg(target_os = "linux")]
    let rc = unsafe { libc::reboot(libc::LINUX_REBOOT_CMD_RESTART) };
    #[cfg(not(target_os = "linux"))]
    let rc = -1;
    if rc != 0 {
        return Err(cmsg(format!(
            "reboot syscall failed: {}",
            std::io::Error::last_os_error()
        )));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    #[test]
    fn decode_markers() {
        let z = [0u8; 32];
        assert_eq!(super::decode(&z), super::ParaState::Nixos);
        let mut b = z;
        b[..14].copy_from_slice(b"boot-recovery\0");
        assert_eq!(super::decode(&b), super::ParaState::Twrp);
        let mut c = z;
        c[..12].copy_from_slice(b"boot-debian\0");
        assert_eq!(super::decode(&c), super::ParaState::Debian);
        let mut d = z;
        d[..4].copy_from_slice(b"JUNK");
        assert!(matches!(super::decode(&d), super::ParaState::Other(_)));
    }
}
