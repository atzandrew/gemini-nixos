#!/usr/bin/env bash
# emmc-check.sh — report the eMMC bus mode/clock, measure uncached read
# speed, and do ROUNDS write + read-back integrity checks. Run ON THE GEMINI.
#
# Why: 2026-10-04 the eMMC ran legacy HS52 (48 MHz, 42 MB/s). HS200 at
# 192 MHz passed tuning and light I/O but failed a 1 GiB write/read-back
# (AUTO_CMD23 CRC -84, then I/O errors). A marginal tuning window corrupts
# data quietly, so a new bus mode is only trusted after several clean rounds
# AND no new mmc/blk/ext4 errors in the kernel log.
#
# Usage (from Hydra):
#   scp bin/emmc-check.sh atzero@192.168.0.139:/tmp/
#   ssh -t atzero@192.168.0.139 bash /tmp/emmc-check.sh [MB] [ROUNDS]
#   defaults: MB=1024, ROUNDS=3
#
# Needs MB of free RAM in /dev/shm and MB free on /. Writes ~/emmc-check.bin
# and deletes it. Exit: 0 all OK, 2 data mismatch, 3 new kernel errors.
#
# 2026-10-04: + ROUNDS, + MT6797 driver lines (mtk_sd mt6797 quirks:
# pads, registers, tuning windows), + error count since the script started.
# 2026-10-04 (b): + retune count (each retune after boot = a transfer error),
# + mmcblk error lines, + the full kernel log saved to /tmp/emmc-check-dmesg.txt
# at the end (sudo here has a terminal; a later `ssh host sudo dmesg` without
# -t has none and silently captures nothing). Fetch it BEFORE powering off:
#   scp atzero@192.168.0.139:/tmp/emmc-check-dmesg.txt logs/
set -eu
MB=${1:-1024}
ROUNDS=${2:-3}
F="$HOME/emmc-check.bin"
ERRPAT='mmc0.*(error|timeout|crc|-84|-110)|mmcblk0.*(error|recovery|timed out)|msdc.*(error|crc)|blk_update_request|I/O error|EXT4-fs (error|warning)|Buffer I/O'
sudo -v
trap 'rm -f "$F" /dev/shm/emmc-src.bin' EXIT

# grep -v mt6797: the driver's own info lines contain "crc" (crcsts_sel,
# "resp/crc latch") and matched msdc.*crc -> false "errors" (2026-10-04).
# ("mt6797 bus error" lines DO count: real non-tuning transfer errors.)
INFO='mt6797 (quirks|timing|tune|tuned|tuning|pads|init)'
errcount() { sudo dmesg | grep -iE "$ERRPAT" | grep -vEc "$INFO" || :; }
tunecount() { sudo dmesg | grep -c 'mt6797 tuned' || :; }
savelog() { sudo dmesg >/tmp/emmc-check-dmesg.txt 2>&1 || :; echo "== full kernel log saved: /tmp/emmc-check-dmesg.txt (fetch before power-off)"; }
ERR0=$(errcount)
TUNE0=$(tunecount)

echo "== bus"
sudo grep -E 'clock|timing spec|bus width|signal voltage' /sys/kernel/debug/mmc0/ios
sudo sh -c 'cat /sys/kernel/debug/mmc0/mmc0:*/ext_csd' | cut -c393-394 | sed 's/^/card DEVICE_TYPE: 0x/'
echo "== mtk_sd mt6797 (probe, pads, tuning)"
sudo dmesg | grep -E 'mt6797 (quirks|pads|init|tune|tuned|tuning)' | tail -20 || echo "(none: not the mt6797 driver build)"
echo "== mt6797 bus errors since boot (outside tuning): $(sudo dmesg | grep -c 'mt6797 bus error' || :)"
sudo dmesg | grep 'mt6797 bus error' | tail -5 || :
echo "== kernel log (mmc)"
sudo dmesg | grep -iE 'mmc0|msdc|tun' | grep -v 'mt6797' | tail -15 || :
echo "== kernel errors already in the log before this run: $ERR0"

echo "== uncached sequential read, 500 MiB"
sudo dd if=/dev/mmcblk0 of=/dev/null bs=1M count=500 iflag=direct 2>&1 | tail -1

BAD=0
for r in $(seq 1 "$ROUNDS"); do
  echo "== round $r/$ROUNDS: write ${MB} MiB random (direct, fsync), read back uncached, compare"
  head -c $((MB*1024*1024)) /dev/urandom >/dev/shm/emmc-src.bin 2>/dev/null || {
    echo "not enough RAM in /dev/shm for ${MB} MiB; retry with a smaller size"; exit 1; }
  SRC=$(sha256sum /dev/shm/emmc-src.bin | cut -d' ' -f1)
  dd if=/dev/shm/emmc-src.bin of="$F" bs=1M oflag=direct conv=fsync 2>&1 | tail -1
  rm -f /dev/shm/emmc-src.bin
  sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
  DST=$(dd if="$F" bs=1M iflag=direct 2>/dev/null | sha256sum | cut -d' ' -f1)
  rm -f "$F"
  if [ "$SRC" = "$DST" ]; then echo "INTEGRITY: OK ($SRC)"; else
    echo "INTEGRITY: MISMATCH  src=$SRC  readback=$DST"; BAD=1; break; fi
done

ERR1=$(errcount)
TUNE1=$(tunecount)
echo "== retunes during this run: $((TUNE1-TUNE0)) (since boot: $TUNE1; >0 during the run = transfer errors)"
echo "== kernel errors (mmc/blk/ext4) during this run: $((ERR1-ERR0))"
sudo dmesg | grep -iE "$ERRPAT" | grep -vE "$INFO" | tail -10 || :
savelog
if [ "$BAD" = 1 ]; then echo "VERDICT: FAIL (data mismatch) -> do NOT trust this bus mode"; exit 2; fi
if [ "$ERR1" -gt "$ERR0" ] || [ "$TUNE1" -gt "$TUNE0" ]; then echo "VERDICT: FAIL (new kernel errors or retunes) -> do NOT trust this bus mode"; exit 3; fi
echo "VERDICT: PASS ($ROUNDS x ${MB} MiB, no new kernel errors)"
