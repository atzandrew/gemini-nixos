#!/usr/bin/env bash
# usb-watch.sh — watch the Gemini's USB state over time (timestamps).
# Ported from the legacy GeminiPDA build/usb-watch.sh (2026-09-07,
# golden-repo pivot). DR use: boot classification + the BROM-entry
# experiment (docs/disaster-recovery/gather.md step 5).
#
# Device USB states (VID map — see docs/disaster-recovery/README.md):
#   0e8d:2000  preloader download-wait (~9 s window on every power-on)
#   0e8d:2001  patched preloader (post-reset; mtkclient can't handshake it)
#   0e8d:2008  POC charging gadget (charger kernel running, screen dark)
#   0e8d:201c  Android (adb available)
#   18d1:4ee2  TWRP (adb available)
#   0525:a4a1  USB gadget NIC, CDC-ECM — SSH 10.15.19.82 (since 2026-09-17)
#   0525:a4a2  the same gadget while it was RNDIS (pre-2026-09-17 images)
#   0525:a4a7  g_serial console
#   0e8d:0003  BROM mode
#
# Diagnosis: a repeating 2000 window every ~15-30 s = boot loop (kernel
# hang, LK WDT reset); a single 2000 then silence = battery-gated boot
# (preloader runs on USB power but refuses LK handoff — flat battery).
#
# [2026-09-17] Works on Linux AND macOS: the bus is read through
# bin/lib/host.sh (lsusb on Linux, system_profiler on macOS — there is no
# usbutils on darwin). On macOS the preloader/BROM ids are best-effort
# (system_profiler is slower and may not report unclaimed devices), while
# 0525:a4a1 (the gadget NIC) and 18d1:4ee2 (TWRP adb) — the two the flash
# cycle needs — are the ones that matter.
#
# Usage: bash bin/usb-watch.sh [seconds]   (default: forever, Ctrl-C)
set -u

cd "$(dirname "$0")/.."
. bin/lib/host.sh

# Linux needs lsusb (usbutils) — it lives in the flake devshell (rule 7).
# macOS needs nothing extra (system_profiler is part of the OS).
if gemini_is_linux && ! gemini_ensure_tools lsusb; then
  gemini_devshell_reexec "$(cd "$(dirname "$0")" && pwd)/usb-watch.sh" "$@" || exit 1
fi

SECS="${1:-0}"
i=0
while true; do
  if [ "$SECS" -gt 0 ] && [ "$i" -ge "$SECS" ]; then break; fi
  line=$(gemini_usb_ids 2>/dev/null | grep -E '^(0e8d|18d1|0525):' | tr '\n' ' ')
  ts=$(date +%H:%M:%S)
  if [ -n "$line" ]; then
    echo "[$ts] $line"
  else
    echo "[$ts] (nothing — device offline)"
  fi
  sleep 2
  i=$((i+2))
done
