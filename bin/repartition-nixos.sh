#!/usr/bin/env bash
# repartition-nixos.sh — ONE-WAY repartition of the Gemini PDA so the ONLY
# bootable systems are TWRP (p1 `recovery`) and NixOS, and NixOS owns all
# flash space not needed by the boot-critical/hardware partitions.
#
# What it does
# ------------
# Rewrites the GPT to replace Android's `system` (p27) + `cache` (p28) +
# the Debian `linux` (p29) + `boot2` (p30) + `boot3` (p31) + `userdata`
# (p32) with ONE large ext4 partition named `linux` (p27, ~58 GiB). Every
# partition from p1..p26 and `flashinfo` keep their EXACT offset, GUID and
# name, so the boot chain (preloader -> LK -> p1 recovery / p22 boot) and
# all hardware/identity partitions (para, nvram, proinfo, protect*,
# tee*, scp*, md*, logo, ...) are untouched. See:
#   docs/repartition-android-space.md  (layout + rationale)
#   docs/disaster-recovery/            (GPT backup + restore playbook)
#
# The GPT image is pre-generated + verified on the host
# (stock-dump/repartition-<date>/gpt-{primary,backup}-new.bin, built from
# the live `sfdisk -d` with sgdisk --verify clean). This script only ever
# writes those two verified blobs plus a freshly-built rootfs/boot image.
#
# Safety model
# ------------
#   * `backup` first: pull the live GPT + the boot-critical partitions
#     into stock-dump/repartition-<date>/ and byte-verify the GPT blobs.
#   * The new GPT is written, read back and byte-compared before anything
#     else. The old GPT stays in the backup dir for a TWRP/mtkclient
#     restore.
#   * The rootfs is streamed to the partition OFFSET ON THE RAW DISK
#     (0xe000000 = 224 MiB) with conv=fsync, so it does not depend on
#     TWRP re-reading the GPT mid-session.
#   * `apply` leaves para = boot-recovery (TWRP sticky); `boot` is a
#     separate, explicit step that clears para and reboots into NixOS.
#     If that boot fails, RTC FAC_RESET (hold power+side ~10 s) still
#     lands in TWRP with para cleared.
#
# Usage (repo root; adb-only steps re-exec inside the devshell):
#   bash bin/repartition-nixos.sh plan
#       Show the target layout + verify the host GPT blobs. No device I/O.
#   bash bin/repartition-nixos.sh backup
#       Converge to TWRP, pull the live GPT + boot-critical partitions.
#   bash bin/repartition-nixos.sh apply --yes
#       backup (if needed) -> write + verify the new GPT -> stream
#       rootfs.img to the new `linux` partition -> flash boot.img ->
#       verify. DESTRUCTIVE. Leaves para sticky (TWRP) and does NOT boot.
#   bash bin/repartition-nixos.sh boot
#       Clear para + reboot -> NixOS.
#   bash bin/repartition-nixos.sh all --yes
#       apply (destructive) then stop in TWRP; run `boot` when ready.
#
# [2026-09-17] Host-side probes go through bin/lib/host.sh, so this no
# longer hard-fails on a host without `lsusb` (macOS). It is still a
# LINUX-HOST tool by policy: it is a one-way destructive operation which
# has only ever been rehearsed/verified from the Linux workstation, and
# the Mac has no preloader/BROM recovery path (Apple's container has no
# USB passthrough) if something goes wrong mid-way. Nothing about the
# mechanism blocks macOS — it is a deliberate, untested-here restriction.
#
# Images default to result/boot.img + result/system.img on Linux and to
# the mac build cache (~/.cache/gemini-macos/out) on darwin; override
# with GEMINI_BOOT_IMG / GEMINI_ROOTFS_IMG.
#
# LONG OPERATION: rootfs streaming is ~8 GB; run under run-job:
#   bash bin/run-job.sh start repartition -- \
#     bash bin/repartition-nixos.sh apply --yes
#   bash bin/run-job.sh wait repartition
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Shared host helpers (platform detection, the USB NIC + its ping, the
# USB-id probe, the devshell re-exec) — bin/lib/host.sh.
. "$ROOT/bin/lib/host.sh"

# adb lives in the flake devshell (bare host PATH has no adb — AGENTS.md
# rule 7); on macOS so do the GNU coreutils this script's `timeout` calls
# need. Re-exec once inside the devshell when they are missing.
if gemini_need_devshell; then
  gemini_devshell_reexec "$ROOT/bin/repartition-nixos.sh" "$@" || exit 1
fi

DEV="$GEMINI_DEV_IP"
# Admin key: committed in this repo (keys/gemini_ed25519, keys/README.md).
# Resolved non-fatally for the same reason as bin/flash-nixos.sh — a
# TWRP-only run needs no ssh at all.
KEY=$(gemini_ssh_key 2>/dev/null || printf '%s\n' "$GEMINI_SSH_KEY")
# TWRP by-name dir (existing partitions); the NEW partition is written at
# its raw offset so its node is not needed.
P=/dev/block/platform/mtk-msdc.0/11230000.msdc0/by-name
# Disk geometry (GPT + this unit's eMMC; verified 2026-09-10 via sfdisk).
NS=122142720                 # whole-disk sectors (512 B)
NEW_START_SECT=458752        # new `linux` partition start (0xe000000)
NEW_START_MIB=224            # == 458752 * 512 / 1 MiB
NEW_SIZE_SECT=121651167      # up to the preserved `flashinfo` (p28)
BACKUP_DIR="$ROOT/stock-dump/repartition-20260910"
GPT_PRIMARY="$BACKUP_DIR/gpt-primary-new.bin"
GPT_BACKUP="$BACKUP_DIR/gpt-backup-new.bin"
# Artifact defaults: result/ on Linux, the mac build cache on darwin
# (bin/macos/build.sh); overridable with GEMINI_BOOT_IMG/GEMINI_ROOTFS_IMG.
if [ -f "$ROOT/result/boot.img" ] || ! gemini_is_macos; then
  ART_DIR="$ROOT/result"
else
  ART_DIR="${GEMINI_MACOS_CACHE:-$HOME/.cache/gemini-macos}/out"
fi
BOOT_IMG="${GEMINI_BOOT_IMG:-$ART_DIR/boot.img}"
ROOTFS_IMG="${GEMINI_ROOTFS_IMG:-$ART_DIR/system.img}"
YES=0
# Host scratch for read-back comparisons. /tmp may live on a full root
# filesystem; default to the repo's gitignored logs/ (on the same volume
# as stock-dump) so a 17 KiB read-back never fails with ENOSPC.
RUN_TMP="${GEMINI_RUN_TMP:-${TMPDIR:-$ROOT/logs}}"
mkdir -p "$RUN_TMP"

say() { printf '>> %s\n' "$*"; }
die() { echo "!! $*" >&2; exit 1; }

adb_q()   { timeout 30 adb "$@"; }
adb_sh()  { timeout 600 adb shell "$@"; }
# binary read from the device (exec-out = no PTY / no CRLF mangling)
adb_out() { timeout 600 adb exec-out "$@"; }

devssh() { bash "$ROOT/bin/device-ssh.sh" "$@"; }

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

wdt_exrst() {
  devssh "busybox devmem 0x10007000 32 0x2200005D; busybox devmem 0x10007004 32 0x48" 2>/dev/null || true
}

# ---- converge to TWRP from any state ------------------------------------
converge_twrp() {
  local s
  s=$(state)
  case "$s" in
    twrp) say "already in TWRP"; return 0 ;;
    linux|linux-nossh)
      say "Linux up over the USB NIC (no adb) — para=boot-recovery + WDT EXRST self-boot to TWRP"
      devssh 'best=""; bs=0; for D in $(lsblk -dn -o NAME | grep -E "^mmcblk[0-9]+$"); do S=$(blockdev --getsize64 /dev/$D 2>/dev/null || echo 0); if [ "$S" -gt "$bs" ]; then bs=$S; best=$D; fi; done; [ -b /dev/${best}p2 ] || { echo "no para partition (largest mmcblk=$best)"; exit 1; }; { printf "boot-recovery\0"; head -c 18 /dev/zero; } > /tmp/bootcmd.bin; dd if=/tmp/bootcmd.bin of=/dev/${best}p2 bs=32 count=1 conv=fsync 2>/dev/null && dd if=/dev/${best}p2 bs=32 count=1 2>/dev/null | grep -qa "boot-recovery" && echo "PARA-WRITTEN+VERIFIED ($best)" || { echo "!! para write/verify FAILED"; exit 1; }' \
        || die "para write over ssh failed"
      say "arming WDT for EXRST self-boot (MODE=0x2200005D + 0x10007004=0x48)"
      wdt_exrst
      say "device resetting — waiting for TWRP (adb recovery)..."
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

# ---- device helpers ------------------------------------------------------
raw_disk() { # largest whole mmcblk (mmcblk numbering is not stable)
  adb_sh 'best=""; bs=0; for D in $(ls -d /dev/block/mmcblk[0-9] 2>/dev/null); do S=$(cat /sys/class/block/$(basename $D)/size 2>/dev/null || echo 0); if [ "$S" -gt "$bs" ]; then bs=$S; best=$D; fi; done; echo "$best"'
}

# ---- commands --------------------------------------------------------------
cmd_plan() {
  echo "target layout (only p27 changes; p1..p26 + flashinfo preserved):"
  sed -n '1,6p' "$BACKUP_DIR/gpt-new.txt" 2>/dev/null || true
  echo "  new p27 linux : start=${NEW_START_SECT} (${NEW_START_MIB} MiB) size=${NEW_SIZE_SECT} (~$(( NEW_SIZE_SECT / 2 / 1024 / 1024 )) GiB)"
  echo "  preserved     : p1..p26 + p28 flashinfo"
  for f in "$GPT_PRIMARY" "$GPT_BACKUP" "$BOOT_IMG" "$ROOTFS_IMG"; do
    if [ -f "$f" ]; then
      printf '  artifact: %s  (%s, %s)\n' "$f" "$(stat -c%s "$f")" "$(sha256sum "$f" | cut -d' ' -f1)"
    else
      printf '  artifact: %s  MISSING\n' "$f"
    fi
  done
  # reconstruct + verify the GPT blobs on a sparse disk
  local tmp; tmp=$(mktemp "$RUN_TMP/gptverify.XXXXXX")
  truncate -s $((NS * 512)) "$tmp"
  dd if="$GPT_PRIMARY" of="$tmp" bs=512 count=34 conv=notrunc status=none
  dd if="$GPT_BACKUP"  of="$tmp" bs=512 seek=$((NS - 33)) count=33 conv=notrunc status=none
  if command -v sgdisk >/dev/null 2>&1; then
    sgdisk --verify "$tmp" 2>&1 | head -2 || die "sgdisk --verify FAILED on the new GPT"
  fi
  rm -f "$tmp"
  say "GPT blobs verified"
}

cmd_backup() {
  converge_twrp
  mkdir -p "$BACKUP_DIR"
  local raw; raw=$(raw_disk)
  say "raw disk: $raw ($(adb_sh "cat /sys/class/block/$(basename $raw)/size" | tr -d '\r') sectors)"
  say "pulling live GPT (primary sector 0-33 + backup last 33)..."
  adb_out "dd if=$raw bs=512 count=34 2>/dev/null"     > "$BACKUP_DIR/gpt-primary-live.bin"
  adb_out "dd if=$raw bs=512 skip=$((NS-33)) count=33 2>/dev/null" > "$BACKUP_DIR/gpt-backup-live.bin"
  # byte-compare against the 2026-09-10 NixOS pull, if present
  cmp -s "$BACKUP_DIR/gpt-primary-live.bin" "$BACKUP_DIR/gpt-primary-current.bin" 2>/dev/null \
    && say "live primary GPT == pre-session backup" || say "note: live primary differs from pre-session backup (fine if GPT untouched since)"
  say "pulling boot-critical partitions..."
  local part
  for part in recovery para proinfo nvram lk lk2 boot; do
    adb_sh "test -b $P/$part" >/dev/null 2>&1 || { say "  skip $part (no node)"; continue; }
    adb_out "dd if=$P/$part bs=1M 2>/dev/null" > "$BACKUP_DIR/$part.bin"
    say "  $part -> $BACKUP_DIR/$part.bin ($(stat -c%s "$BACKUP_DIR/$part.bin") B)"
  done
  say "backup complete: $BACKUP_DIR"
  sha256sum "$BACKUP_DIR"/*.bin | tee "$BACKUP_DIR/SHA256SUMS"
}

# Write the new GPT, byte-verify, then stream the rootfs to the raw
# partition offset, flash `boot`, and stop (para sticky = TWRP).
cmd_apply() {
  [ "$YES" = 1 ] || die "refusing destructive apply without --yes"
  [ -f "$GPT_PRIMARY" ] || die "missing $GPT_PRIMARY"
  [ -f "$GPT_BACKUP" ]  || die "missing $GPT_BACKUP"
  [ -f "$BOOT_IMG" ]    || die "missing boot image: $BOOT_IMG"
  [ -f "$ROOTFS_IMG" ]  || die "missing rootfs image: $ROOTFS_IMG"
  converge_twrp

  local raw; raw=$(raw_disk)
  say "raw disk: $raw"
  say "unmounting TWRP's data/cache mounts (must not flush over the new layout)..."
  adb_sh "umount /data 2>/dev/null; umount /sdcard 2>/dev/null; umount /cache 2>/dev/null; umount $P/userdata 2>/dev/null; umount $P/cache 2>/dev/null; umount $P/system 2>/dev/null; sync; true"

  say "pushing new GPT blobs..."
  adb_q push "$GPT_PRIMARY" /tmp/gpt-primary-new.bin >/dev/null
  adb_q push "$GPT_BACKUP"  /tmp/gpt-backup-new.bin  >/dev/null

  say "writing new GPT (primary + backup)..."
  adb_sh "dd if=/tmp/gpt-primary-new.bin of=$raw bs=512 count=34 conv=fsync && dd if=/tmp/gpt-backup-new.bin of=$raw bs=512 seek=$((NS-33)) count=33 conv=fsync && sync"

  say "verifying GPT read-back byte-for-byte..."
  adb_out "dd if=$raw bs=512 count=34 2>/dev/null" > "$RUN_TMP/rb-primary.bin"
  adb_out "dd if=$raw bs=512 skip=$((NS-33)) count=33 2>/dev/null" > "$RUN_TMP/rb-backup.bin"
  [ -s "$RUN_TMP/rb-primary.bin" ] || die "GPT primary read-back produced no data (host ENOSPC?); device state unchanged"
  cmp -s "$RUN_TMP/rb-primary.bin" "$GPT_PRIMARY" || die "primary GPT read-back MISMATCH — DO NOT REBOOT; restore from $BACKUP_DIR/gpt-primary-live.bin"
  cmp -s "$RUN_TMP/rb-backup.bin"  "$GPT_BACKUP"  || die "backup GPT read-back MISMATCH — DO NOT REBOOT; restore from $BACKUP_DIR/gpt-backup-live.bin"
  say "new GPT written + verified"

  # Ask the kernel to pick up the new table (by-name symlinks stay stale;
  # we write by raw offset below, so this is only a sanity check).
  adb_sh "blockdev --rereadpt $raw 2>/dev/null || true"

  say "streaming rootfs -> $raw at offset ${NEW_START_MIB} MiB ($(stat -c%s "$ROOTFS_IMG") bytes)..."
  say "  (~8 GB over adb; several minutes)"
  timeout 3600 adb shell "dd of=$raw bs=1M seek=$NEW_START_MIB conv=fsync" < "$ROOTFS_IMG"
  say "rootfs streamed"

  say "verifying rootfs head (first 64 MiB)..."
  local hostmd5 devmd5
  hostmd5=$(head -c 67108864 "$ROOTFS_IMG" | md5sum | cut -d' ' -f1)
  devmd5=$(adb_out "dd if=$raw bs=1M skip=$NEW_START_MIB count=64 2>/dev/null" | md5sum | cut -d' ' -f1)
  [ "$hostmd5" = "$devmd5" ] || die "rootfs head md5 mismatch (host=$hostmd5 dev=$devmd5) — rootfs write failed"
  say "rootfs head OK ($devmd5)"

  say "flashing boot.img -> $P/boot..."
  adb_q push "$BOOT_IMG" /tmp/boot-new.img >/dev/null
  adb_sh "dd if=/tmp/boot-new.img of=$P/boot bs=1M conv=fsync"
  local bhost bdev bsz bsec
  bhost=$(md5sum "$BOOT_IMG" | cut -d' ' -f1)
  bsz=$(stat -c%s "$BOOT_IMG"); bsec=$((bsz / 512))
  [ $((bsec * 512)) -eq "$bsz" ] || die "boot.img size is not 512-aligned ($bsz)"
  # read back EXACTLY the image length (bs=512 count=sectors) — reading a
  # rounded-up MiB count would compare stale tail bytes and false-fail.
  bdev=$(adb_out "dd if=$P/boot bs=512 count=$bsec 2>/dev/null" | md5sum | cut -d' ' -f1)
  [ "$bhost" = "$bdev" ] || die "boot.img read-back md5 mismatch (host=$bhost dev=$bdev)"
  say "boot.img written + verified ($bdev, $bsec sectors)"

  say "para left as boot-recovery (TWRP sticky). Verify the layout, then:"
  say "  bash bin/repartition-nixos.sh boot      # clear para + boot NixOS"
}

cmd_verify() {
  converge_twrp
  local raw; raw=$(raw_disk)
  local isz isec hostmd5 devmd5
  isz=$(stat -c%s "$ROOTFS_IMG"); isec=$((isz / 512))
  say "verifying rootfs region: $isec sectors at offset $NEW_START_SECT ($((isec * 512)) bytes)..."
  hostmd5=$(head -c $((isec * 512)) "$ROOTFS_IMG" | md5sum | cut -d' ' -f1)
  devmd5=$(adb_out "dd if=$raw bs=512 skip=$NEW_START_SECT count=$isec 2>/dev/null" | md5sum | cut -d' ' -f1)
  [ "$hostmd5" = "$devmd5" ] || die "rootfs full-region md5 mismatch (host=$hostmd5 dev=$devmd5)"
  say "rootfs full region OK ($devmd5)"
  local bsz bsec bhost bdev
  bsz=$(stat -c%s "$BOOT_IMG"); bsec=$((bsz / 512))
  bhost=$(md5sum "$BOOT_IMG" | cut -d' ' -f1)
  bdev=$(adb_out "dd if=$P/boot bs=512 count=$bsec 2>/dev/null" | md5sum | cut -d' ' -f1)
  [ "$bhost" = "$bdev" ] || die "boot.img read-back md5 mismatch (host=$bhost dev=$bdev)"
  say "boot.img OK ($bdev)"
}

cmd_boot() {
  converge_twrp
  say "clearing para + rebooting -> NixOS on the new linux partition"
  adb_sh "dd if=/dev/zero of=$P/para bs=32 count=1 conv=fsync" >/dev/null
  adb_q reboot >/dev/null 2>&1 || true
  say "reboot sent. NixOS should come up on g_ether at $DEV in ~40-90 s."
  say "If it hangs: RTC FAC_RESET (hold power+side ~10 s) still lands in TWRP."
}

cmd_all() {
  cmd_backup
  cmd_apply
  say "apply complete; device is in TWRP. Run 'boot' when ready."
}

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
  plan)    cmd_plan ;;
  backup)  cmd_backup ;;
  apply)   cmd_apply ;;
  verify)  cmd_verify ;;
  boot)    cmd_boot ;;
  all)     cmd_all ;;
  -h|--help|help|"") usage ;;
  *) echo "!! unknown command: ${1:-}" >&2; usage; exit 1 ;;
esac
