#!/usr/bin/env bash
# flash-nixos.sh — flash THIS repo's Mobile NixOS artifacts onto the Gemini
# PDA. The device has NO fastboot; every write happens from the patched
# no-swipe TWRP (root adbd). This script is the NixOS-port equivalent of
# the GeminiPDA project's flash-nohelp.sh pipeline, adapted to converge to
# TWRP from WHATEVER state the device is in:
#
#   running Linux (g_ether ssh — no adbd):  para=boot-recovery over ssh,
#     then WDT EXRST self-boot (MODE=0x2200005D restore + 0x10007004=0x48) → LK boots
#     TWRP.  (Works from the current GeminiPDA Debian rootfs AND from the
#     future NixOS rootfs — both ship busybox.)
#   Android: adb reboot recovery hop (bin/boot-switch.sh twrp)
#   POC / preloader / offline: physical interaction prompts, same as
#     bin/boot-switch.sh.
#
# Usage (repo root; adb-only steps re-exec inside the devshell):
#   bash bin/flash-nixos.sh preflight
#       READ-ONLY readiness check for a flash from THIS host: artifacts
#       (+ sha256 cross-check against the build's ledger), the LK cmdline
#       requirement in boot.img, the toolchain, the SSH key, sudo, and the
#       device state if one is on USB. Touches nothing on the device.
#       Exit non-zero when a REQUIRED prerequisite is missing.
#   bash bin/flash-nixos.sh status
#       Device state + which local artifacts exist.
#   bash bin/flash-nixos.sh boot [boot.img]
#       Converge to TWRP → back up current `boot` → flash the image into
#       `boot`. STAYS in TWRP (para untouched): the unverified image is
#       never booted unattended. Next step = `boot-nixos` when ready.
#   bash bin/flash-nixos.sh rootfs [rootfs.img] [--yes]
#       Converge to TWRP → stream the image into the big `linux`
#       partition (p27, by-name; ~58 GiB). DESTROYS the current NixOS
#       rootfs; prompts unless --yes (docs/repartition-android-space.md §12).
#   bash bin/flash-nixos.sh all [--yes]
#       boot + rootfs, skipping the interactive prompts.
#   bash bin/flash-nixos.sh boot-nixos
#       Clear para + reboot from TWRP → NORMAL boots the `boot` partition
#       (the flashed boot.img; para zeros = NixOS `linux` default).
#       Rollback of `boot` from TWRP: bin/boot-switch.sh restore.
#   bash bin/flash-nixos.sh grow-rootfs
#       Converge to TWRP → OFFLINE-grow the `linux` rootfs filesystem to the
#       full partition size (e2fsck -fy + resize2fs with a pushed static
#       e2fsprogs). This is the recovery path for make_ext4fs-geometry
#       images whose fs the kernel can only online-grow to 2x (R13 —
#       images built since 2026-09-07 use mke2fs and grow on first boot
#       via systemd-growfs-root). NOT destructive (grows in place), but
#       it IS a TWRP cycle: reboots the device. Follow with `boot-nixos`
#       to boot the grown rootfs.
#
# [2026-09-17] macOS is a supported host (this is the whole point of the
# RNDIS → CDC-ECM gadget change): the Linux-rootfs hop over the USB NIC,
# the adb half and the artifact paths all work from a Mac — see
# docs/usb-network.md + docs/macos-build.md. USB-state detection
# (poc/preloader/brom) is best-effort on macOS (no usbutils; the shared
# helpers in bin/lib/host.sh use system_profiler), while the states this
# pipeline actually needs (linux / twrp / offline) are exact everywhere.
# NOT available on macOS: preloader/BROM recovery (no USB passthrough into
# Apple's container, docs/disaster-recovery/) — that stays a Linux host job.
#
# Default images: the Linux build model publishes result/boot.img +
# result/system.img (`bash bin/build.sh`), the macOS model collects the
# same two into ~/.cache/gemini-macos/out (bin/macos/build.sh). Override
# with GEMINI_BOOT_IMG / GEMINI_ROOTFS_IMG.
#
# LONG OPERATION: the rootfs push+dd can take 5-20 min over USB. Run it
# under the detached job runner so a session never stalls:
#   bash bin/run-job.sh start flash-rootfs -- \
#     bash bin/flash-nixos.sh rootfs --yes
#   bash bin/run-job.sh wait flash-rootfs
#
# SAFETY MODEL (why the default leaves TWRP sticky):
#   A failed boot image on this device can strand the unit (a hung kernel
#   has no software path back; recovery then = mtkclient preloader mode,
#   see the DR playbook docs/disaster-recovery/). So: flash while
#   para=boot-recovery (every power-on = TWRP), verify your images, and
#   only then `boot-nixos` (para-clear + reboot). Keep the boot backups in
#   stock-dump/ — restore is one adb command. Since the 2026-09-10
#   repartition TWRP + NixOS are the only systems (Android and the
#   Debian `linux` rootfs were reclaimed — docs/repartition-android-space.md §12).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Shared host helpers: platform detection, the USB NIC + its ping, the
# USB-id probe (lsusb ⇄ system_profiler) and the devshell re-exec.
. "$ROOT/bin/lib/host.sh"

# adb lives in the flake devshell (bare host PATH has no adb — AGENTS.md
# rule 7); on macOS the same devshell supplies the GNU coreutils whose
# `timeout` this script's adb wrappers use. Re-exec once inside it; the
# GEMINI_DEVSH_REEXEC guard inside the helper prevents a loop.
if gemini_need_devshell; then
  gemini_devshell_reexec "$ROOT/bin/flash-nixos.sh" "$@" || exit 1
fi

DEV="$GEMINI_DEV_IP"
# The admin key is committed in this repo (keys/gemini_ed25519, see
# keys/README.md). Resolved NON-fatally: `preflight` must still run (and
# report it) when the key is missing, and a flash from TWRP needs no ssh at
# all — the ssh paths themselves (bin/device-ssh.sh) check it.
KEY=$(gemini_ssh_key 2>/dev/null || printf '%s\n' "$GEMINI_SSH_KEY")
# Artifacts: Linux publishes result/ in the repo; the macOS build model
# collects into ~/.cache/gemini-macos/out. Prefer result/, fall back to
# the mac cache on darwin (never silently — `status` shows the path used).
if [ -f "$ROOT/result/boot.img" ] || ! gemini_is_macos; then
  ART_DIR="$ROOT/result"
else
  ART_DIR="${GEMINI_MACOS_CACHE:-$HOME/.cache/gemini-macos}/out"
fi
BOOT_IMG_DEFAULT="${GEMINI_BOOT_IMG:-$ART_DIR/boot.img}"
ROOTFS_IMG_DEFAULT="${GEMINI_ROOTFS_IMG:-$ART_DIR/system.img}"
# TWRP by-name partition directory (verified path on this unit)
P=/dev/block/platform/mtk-msdc.0/11230000.msdc0/by-name
# The single NixOS rootfs partition (2026-09-10 repartition): the old
# Android system/cache/userdata + Debian linux + boot2/boot3 collapsed
# into one ~58 GiB `linux` (p27). See bin/repartition-nixos.sh.
TARGET_PART=linux
BACKUP_DIR="$ROOT/stock-dump"
# Static (musl) aarch64 e2fsprogs for offline rootfs growth from TWRP
# (grow-rootfs verb). Rebuild + pin if GC'd:
#   nix build nixpkgs#legacyPackages.x86_64-linux.pkgsCross.aarch64-multiplatform.pkgsStatic.e2fsprogs
#   bash bin/gc-pin.sh e2fsprogs-static-aarch64 <out>
E2FS_STATIC=/nix/store/k0wplgv6nwhcp710y5z7zh37c6rvk87j-e2fsprogs-static-aarch64-unknown-linux-musl-1.47.4-bin
YES=0

adb_q()  { timeout 30 adb "$@"; }
adb_sh() { timeout 300 adb shell "$@"; }
# long ops: the 1.5 GiB rootfs push + on-device dd need minutes, not 30 s.
adb_push() { timeout 900 adb "$@"; }
devssh() { bash "$ROOT/bin/device-ssh.sh" "$@"; }

# Arm the LK watchdog for an EXRST self-boot (Linux → TWRP/Debian hop).
# The A72 bring-up (services/scripts/cl2-up.sh, run by gemini-a72-up at
# every boot) leaves WDT MODE disarmed (0x10007000 = 0), so the LENGTH
# arm write alone silently no-ops — the documented "reboot trap",
# docs/phase-2-on-glass.md §2b. Restore LK's mode value (key | 0x5D)
# first, then arm. [added 2026-09-10]
wdt_exrst() {
  devssh "busybox devmem 0x10007000 32 0x2200005D; busybox devmem 0x10007004 32 0x48" 2>/dev/null || true
}

say() { printf '>> %s\n' "$*"; }
die() { echo "!! $*" >&2; exit 1; }

usage() { awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"; }

# ---- state --------------------------------------------------------------
# adb-ish states: twrp|android|unauthorized|adb-offline|poc|preloader|brom|offline
# PLUS: linux (ssh reachable over g_ether, no adb)
state() {
  local line
  line=$(adb devices 2>/dev/null | tail -n +2 | grep -v '^$' || true)
  if [ -n "$line" ]; then
    if echo "$line" | grep -q 'recovery'; then echo twrp; return; fi
    if echo "$line" | grep -qE '\bdevice\b'; then echo android; return; fi
    if echo "$line" | grep -q 'unauthorized'; then echo unauthorized; return; fi
    echo adb-offline; return
  fi
  # no adb device — is a Linux rootfs up over the USB NIC instead?
  if gemini_ping; then
    if timeout 8 ssh -i "$KEY" -o BatchMode=yes -o IdentitiesOnly=yes \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=5 root@"$DEV" true >/dev/null 2>&1; then
      echo linux; return
    fi
    echo linux-nossh; return
  fi
  if gemini_usb_present 0e8d:2008; then echo poc; return; fi
  if gemini_usb_present 0e8d:2000; then echo preloader; return; fi
  if gemini_usb_present 0e8d:0003; then echo brom; return; fi
  echo offline
}

wait_for() { # want [iterations x5s]
  local want="$1" n="${2:-36}" i s
  for ((i=1; i<=n; i++)); do
    s=$(state)
    [ "$s" = "$want" ] && { echo "  $(date +%H:%M:%S) state: $want"; return 0; }
    sleep 5
  done
  die "timed out waiting for '$want' (last: $s)"
}

# ---- converge to TWRP from any state ------------------------------------
converge_twrp() {
  local s
  s=$(state)
  case "$s" in
    twrp) say "already in TWRP"; return 0 ;;
    linux|linux-nossh)
      say "Linux up over g_ether (no adb) — para-write + WDT EXRST self-boot to TWRP"
      # para = p2 of the LARGEST mmcblk (eMMC numbering differs across
      # kernel builds — a hardcoded /dev/mmcblk1p2 once silently created a
      # regular file instead of writing the eMMC; detect by size always).
      devssh 'best=""; bs=0; for D in $(lsblk -dn -o NAME | grep -E "^mmcblk[0-9]+$"); do S=$(blockdev --getsize64 /dev/$D 2>/dev/null || echo 0); if [ "$S" -gt "$bs" ]; then bs=$S; best=$D; fi; done; [ -b /dev/${best}p2 ] || { echo "no para partition (largest mmcblk=$best)"; exit 1; }; { printf "boot-recovery\0"; head -c 18 /dev/zero; } > /tmp/bootcmd.bin; dd if=/tmp/bootcmd.bin of=/dev/${best}p2 bs=32 count=1 conv=fsync 2>/dev/null && dd if=/dev/${best}p2 bs=32 count=1 2>/dev/null | grep -qa "boot-recovery" && echo "PARA-WRITTEN+VERIFIED ($best)" || { echo "!! para write/verify FAILED"; exit 1; }' \
        || die "para write over ssh failed"
      say "arming WDT for EXRST self-boot (MODE=0x2200005D restore + 0x10007004=0x48)"
      wdt_exrst
      say "device resetting — waiting for TWRP (adb recovery)..."
      # TWRP's adbd is the authority (platform-neutral: no lsusb here any
      # more — it was the last thing keeping this Linux-only).
      local i s
      for ((i=1; i<=36; i++)); do
        sleep 5
        s=$(state)
        if [ "$s" = twrp ]; then
          say "TWRP up after ~$((i*5))s (adbd settling)"
          sleep 5
          return 0
        fi
      done
      die "TWRP did not appear within 180s — may need a physical power-on"
      ;;
    android)
      say "in Android — hopping to TWRP (adb reboot recovery + sticky para)"
      bash "$ROOT/bin/boot-switch.sh" twrp
      return 0
      ;;
    poc)
      say "Power-Off-Charging (0e8d:2008) — press the POWER KEY on the device"
      say "(sticky para lands it in TWRP). Waiting up to 3 min..."
      local i s
      for ((i=1; i<=36; i++)); do
        sleep 5; s=$(state)
        [ "$s" = twrp ] && { wait_for twrp 6; return 0; }
        [ "$s" = android ] && { bash "$ROOT/bin/boot-switch.sh" twrp; return 0; }
      done
      die "still in POC — press the power key or replug USB"
      ;;
    preloader|brom)
      die "device in $s download mode (no adb). Power off / press power to abort, then re-run."
      ;;
    offline)
      die "no device on USB. Connect the cable and power on (para sticky = TWRP), then re-run."
      ;;
    unauthorized|adb-offline)
      die "adb state '$s' — accept the RSA prompt / replug USB, then re-run."
      ;;
  esac
}

# ---- artifact checks ------------------------------------------------------
need_img() { # path what
  [ -f "$1" ] || die "$2 not found: $1 — build it: bash bin/build.sh start bootimg (see docs/building.md)"
}

# ---- TWRP-side helpers ----------------------------------------------------
twrp_dd_part() { # devnode src-dest-label  (image already pushed to /tmp on device)
  adb_sh "dd if=$1 of=$P/$2 bs=1M conv=fsync"
}

# ---- commands --------------------------------------------------------------
cmd_preflight() { # read-only: is a flash actually possible from this host?
  local fail=0 warn=0 checked=0 f hash path s
  say "flash preflight — host: $(gemini_platform), artifacts: $ART_DIR"

  # 1. The artifact set. A `boot` flash needs boot.img; only a full
  #    reflash (rootfs verb) needs system.img.
  for f in "$BOOT_IMG_DEFAULT" "$ROOTFS_IMG_DEFAULT"; do
    if [ -f "$f" ]; then
      printf '  ok    artifact %s (%s, sha256 %s…)\n' "$f" \
        "$(du -h "$f" | cut -f1)" "$(sha256sum "$f" | cut -c1-16)"
    elif [ "$f" = "$ROOTFS_IMG_DEFAULT" ]; then
      printf '  warn  artifact %s MISSING — only a ROOTFS reflash needs it (bash bin/build.sh start rootfs)\n' "$f"
      warn=1
    else
      printf '  FAIL  artifact %s MISSING — a boot flash needs it\n' "$f"
      fail=1
    fi
  done

  # 2. Integrity: cross-check the artifacts against the build's ledger
  #    (<target>.sha256 next to the artifacts — the mac build writes it,
  #    and it records the IN-VM path `/out/<name>`, mapped here to this
  #    host's artifact dir, which is what makes the copy we would flash
  #    the same bytes the build produced — rule 0).
  for f in "$ART_DIR"/*.sha256; do
    [ -f "$f" ] || continue
    while read -r hash path; do
      case "$path" in /out/*) path="$ART_DIR/${path#/out/}" ;; esac
      [ -f "$path" ] || continue
      checked=$((checked + 1))
      if [ "$(sha256sum "$path" | cut -d' ' -f1)" != "$hash" ]; then
        printf '  FAIL  sha256 mismatch vs %s: %s\n' "$(basename "$f")" "$path"
        fail=1
      fi
    done < "$f"
  done
  [ "$checked" -gt 0 ] &&
    printf '  ok    integrity %s artifact file(s) match the build ledger\n' "$checked"

  # 3. The one boot.img field that bricks the boot when absent (LK's
  #    platform_parse_bootopt) + the rule-5 fbcon build marker. Both live
  #    in the 512-byte cmdline field of the header page.
  if [ -f "$BOOT_IMG_DEFAULT" ]; then
    if head -c 4096 "$BOOT_IMG_DEFAULT" | grep -q 'bootopt=64S3,32N2,64N2'; then
      printf '  ok    cmdline  bootopt=64S3,32N2,64N2 present (LK requirement)\n'
    else
      printf '  FAIL  cmdline  bootopt=64S3,32N2,64N2 MISSING — the boot will hang on the LK logo\n'
      fail=1
    fi
    if head -c 4096 "$BOOT_IMG_DEFAULT" | grep -q 'fbcon=rotate:3'; then
      printf '  ok    cmdline  fbcon=rotate:3 present (the rule-5 fbcon/exclude-display build)\n'
    else
      printf '  warn  cmdline  fbcon=rotate:3 not found — is this the fbcon/exclude-display build?\n'
      warn=1
    fi
  fi

  # 4. Toolchain: adb + GNU coreutils. On both platforms these come from
  #    the flake devshell, which this script already re-execs into — so
  #    reaching here means they exist; check anyway (it is the point).
  if gemini_ensure_tools adb timeout sha256sum; then
    printf '  ok    toolchain adb + GNU coreutils (timeout/stat/sha256sum) on PATH\n'
  else
    printf '  FAIL  toolchain adb/timeout/sha256sum missing on PATH\n'
    fail=1
  fi

  # 5. SSH key: only needed to converge a RUNNING LINUX device to TWRP
  #    (para write + WDT EXRST over ssh). From TWRP the flash is adb-only.
  #    The key is committed in the repo — keys/gemini_ed25519 — so this
  #    should only be missing on a hand-trimmed checkout.
  if [ -f "$KEY" ]; then
    printf '  ok    ssh key  %s\n' "$KEY"
  else
    printf '  warn  ssh key  %s MISSING — needed only if the device is at the NixOS desktop\n' "$KEY"
    printf '        (committed at keys/gemini_ed25519; or point GEMINI_SSH_KEY at a key)\n'
    warn=1
  fi

  # 5b. The pinned HOST key: keys/known_hosts must carry the public half of
  #     the key the device will present as its sshd host key (both come from
  #     keys/gemini_ed25519 via config/gemini.nix). If they drift, a plain
  #     ssh against the repo known_hosts is what breaks — catch it here.
  if [ -f "$GEMINI_KNOWN_HOSTS" ] && [ -f "$KEY.pub" ]; then
    if awk '!/^#/ { print $3 }' "$GEMINI_KNOWN_HOSTS" \
         | grep -Fxq "$(awk '{ print $2 }' "$KEY.pub")"; then
      printf '  ok    hostkey  keys/known_hosts carries the pinned device host key\n'
    else
      printf '  FAIL  hostkey  keys/known_hosts does NOT match %s.pub\n' "$KEY"
      fail=1
    fi
  fi

  # 6. sudo: needed to (re-)apply the host address on the USB NIC when the
  #    device has to be converged from Linux (or rebooted).
  if [ "$(id -u)" = 0 ] || sudo -n true 2>/dev/null; then
    printf '  ok    sudo     available without a password prompt\n'
  else
    printf '  warn  sudo     needs a password — run `sudo -v` (macOS caches per-tty)\n'
    warn=1
  fi

  # 7. Device (read-only: iface lookup + ping + adb devices).
  s=$(state)
  printf '  info  device   %s\n' "$s"
  case "$s" in
    twrp)    say "        -> a flash can run NOW (adb only; TWRP is up)" ;;
    linux)   say "        -> a flash will converge to TWRP over the USB NIC ssh hop (needs 5 + 6)" ;;
    android) say "        -> a flash will hop to TWRP via adb reboot recovery" ;;
    offline) say "        -> no device on USB: connect it and power on (para sticky = TWRP)" ;;
    *)       say "        -> see bin/boot-switch.sh for this state" ;;
  esac

  echo
  if [ "$fail" = 1 ]; then
    die "preflight: a REQUIRED prerequisite is missing (the FAIL lines above)"
  fi
  if [ "$warn" = 1 ]; then
    say "preflight: flash POSSIBLE, with the warnings above"
  else
    say "preflight: all checks pass — flash possible"
  fi
}

cmd_status() {
  echo "device state : $(state)"
  for f in "$BOOT_IMG_DEFAULT" "$ROOTFS_IMG_DEFAULT"; do
    if [ -f "$f" ]; then
      printf 'artifact      : %s  (%s, %s)\n' "$f" "$(du -h "$f" | cut -f1)" \
        "$(stat -c%y "$f" | cut -d. -f1)"
    else
      printf 'artifact      : %s  (MISSING — build with: bash bin/build.sh start bootimg)\n' "$f"
    fi
  done
  echo "hint: adb-side boot-target control = bash bin/boot-switch.sh status"
}

cmd_boot() {
  local img="${1:-$BOOT_IMG_DEFAULT}"
  need_img "$img" "boot image"
  converge_twrp
  bash "$ROOT/bin/boot-switch.sh" flash "$img" twrp
  say "boot.img flashed. Device is in TWRP (para sticky). When ready to test:"
  say "  bash bin/flash-nixos.sh boot-nixos     (or: bash bin/boot-switch.sh android)"
}

cmd_rootfs() {
  local img="${1:-$ROOTFS_IMG_DEFAULT}"
  need_img "$img" "rootfs image"
  converge_twrp
  # sanity: the target partition exists and is big (>= 20 GiB). TWRP has
  # no blockdev; resolve the by-name symlink and read the size from
  # /proc/partitions (column 3, KiB units) on the device.
  local tgt base kb
  tgt=$(adb_sh "readlink -f $P/$TARGET_PART" | tr -d '\r' || true)
  base=$(basename "$tgt")
  kb=$(adb_sh 'cat /proc/partitions' | tr -d '\r' | awk -v b="$base" '$4==b{print $3}')
  if [ -z "$tgt" ] || [ -z "$kb" ] || [ "$kb" -lt $((20 * 1024 * 1024)) ]; then
    die "by-name/$TARGET_PART missing or too small (readlink=$tgt, blocks=$kb) — refusing. Partition list: $(adb_sh 'ls '$P | tr '\n' ' ')"
  fi
  echo ">> target: $P/$TARGET_PART -> $tgt = $((kb / 1024 / 1024)) GiB"
  if [ "$YES" != 1 ]; then
    echo "!! This DESTROYS the current NixOS rootfs on $TARGET_PART."
    read -r -p "Type 'wipe rootfs' to continue: " ans
    [ "$ans" = "wipe rootfs" ] || { echo "aborted."; exit 1; }
  fi
  # Stream the image STRAIGHT to the partition: the rootfs image is now
  # ~8 GB (GNOME closure) and does NOT fit TWRP's ~1.9 GiB /tmp tmpfs.
  # Unmount first so TWRP cannot flush stale data over the image.
  adb_sh "umount /data 2>/dev/null; umount /sdcard 2>/dev/null; umount /cache 2>/dev/null; umount $P/$TARGET_PART 2>/dev/null; sync; true" >/dev/null
  say "streaming rootfs -> $P/$TARGET_PART ($(stat -c%s "$img") bytes; several minutes)..."
  timeout 3600 adb shell "dd of=$P/$TARGET_PART bs=1M conv=fsync" < "$img"
  say "rootfs flashed. Device is in TWRP (para sticky)."
}

cmd_all() {
  cmd_boot "${1:-$BOOT_IMG_DEFAULT}"
  cmd_rootfs "${2:-$ROOTFS_IMG_DEFAULT}"
  say "boot + rootfs flashed. Boot into NixOS when ready: bash bin/flash-nixos.sh boot-nixos"
}

cmd_boot_nixos() {
  case "$(state)" in
    twrp) : ;;
    *) converge_twrp ;;
  esac
  say "clearing para + rebooting → NORMAL boots the \`boot\` partition"
  say "  (para zeros = NixOS on the linux partition)"
  adb_sh "dd if=/dev/zero of=$P/para bs=32 count=1 conv=fsync" >/dev/null
  adb_q reboot >/dev/null 2>&1 || true
  say "reboot sent. First NixOS boot: watch the serial console (ttyS0,921600) or fbcon."
  say "If it comes back, expect g_ether at $DEV (ssh). If it hangs: no software path —"
  say "recovery = mtkclient preloader mode OR re-power-on (para cleared now = normal boot)."
}

cmd_grow_rootfs() {
  converge_twrp
  [ -d "$E2FS_STATIC/bin" ] || die "static e2fsprogs not present: $E2FS_STATIC (rebuild + gc-pin, see header)"
  local tgt base kb
  tgt=$(adb_sh "readlink -f $P/$TARGET_PART" | tr -d '\r' || true)
  base=$(basename "$tgt")
  kb=$(adb_sh 'cat /proc/partitions' | tr -d '\r' | awk -v b="$base" '$4==b{print $3}')
  if [ -z "$tgt" ] || [ -z "$kb" ] || [ "$kb" -lt $((20 * 1024 * 1024)) ]; then
    die "by-name/$TARGET_PART missing or too small (readlink=$tgt, blocks=$kb) — refusing"
  fi
  say "target: $P/$TARGET_PART -> $tgt = $((kb / 1024 / 1024)) GiB"
  # TWRP may auto-mount the partition; offline growth needs it unmounted.
  adb_sh "umount /data 2>/dev/null; umount $tgt 2>/dev/null; umount $P/$TARGET_PART 2>/dev/null; true" >/dev/null
  say "pushing static e2fsprogs (e2fsck + resize2fs)..."
  adb_push push "$E2FS_STATIC/bin/e2fsck" /tmp/e2fsck >/dev/null
  adb_push push "$E2FS_STATIC/sbin/resize2fs" /tmp/resize2fs >/dev/null
  adb_sh "chmod +x /tmp/e2fsck /tmp/resize2fs" >/dev/null
  say "e2fsck -fy (journal replay + health check — offline, no online-resize limits)"
  # e2fsck exits 1 when it MODIFIED the fs (journal replay / repairs) —
  # that is success here; don't let set -euo pipefail kill the run.
  adb_sh "/tmp/e2fsck -fy $P/$TARGET_PART" 2>&1 | tail -3 || true
  say "resize2fs -> full partition size"
  adb_sh "/tmp/resize2fs $P/$TARGET_PART" 2>&1 | tail -3
  say "verify: fs state + free space"
  adb_sh "/tmp/e2fsck -fn $P/$TARGET_PART" 2>&1 | tail -3 || true
  say "grow done. Device is in TWRP. Boot the grown rootfs when ready:"
  say "  bash bin/flash-nixos.sh boot-nixos"
}

# (Debian/dual-boot para marker helpers removed 2026-09-10: after the
# repartition TWRP + NixOS are the only systems — see bin/repartition-nixos.sh.)

# ---- main --------------------------------------------------------------------
args=()
for a in "$@"; do
  case "$a" in
    --yes) YES=1 ;;
    *) args+=("$a") ;;
  esac
done
set -- "${args[@]}"

case "${1:-}" in
  preflight)   cmd_preflight ;;
  status)      cmd_status ;;
  boot)        cmd_boot "${2:-$BOOT_IMG_DEFAULT}" ;;
  rootfs)      cmd_rootfs "${2:-$ROOTFS_IMG_DEFAULT}" ;;
  all)         cmd_all "${2:-$BOOT_IMG_DEFAULT}" "${3:-$ROOTFS_IMG_DEFAULT}" ;;
  boot-nixos)  cmd_boot_nixos ;;
  grow-rootfs) cmd_grow_rootfs ;;
  -h|--help|help|"") usage ;;
  *) echo "!! unknown command: ${1:-}" >&2; usage; exit 1 ;;
esac
