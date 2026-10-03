#!/bin/sh
# drm-stats.sh — sample geminipda-drm's per-commit copy statistics.
#
# What: reads /sys/module/geminipda_drm/parameters/stat_* twice, N seconds
#       apart, and prints commits/s (≈ frames reaching the panel), average
#       and max copy time (incl. cache sync), pixels in the last commit and
#       how many commits were full-frame copies.
# Why:  the kernel has no ftrace; these counters (added 2026-10-03) are the
#       direct measure of the scanout copy cost. A/B the copy path live with
#       the fast_copy / cache_sync parameters (see geminipda-drm.c header).
# Usage (on the Gemini):
#       sh drm-stats.sh [seconds]          # default 10; run while animating
#       echo 0 | sudo tee /sys/module/geminipda_drm/parameters/fast_copy
#       echo 0 | sudo tee /sys/module/geminipda_drm/parameters/cache_sync
#       echo 0 | sudo tee /sys/module/geminipda_drm/parameters/stat_copy_us_max
set -eu
P=/sys/module/geminipda_drm/parameters
S=${1:-10}
[ -r "$P/stat_commits" ] || { echo "geminipda-drm has no stat_* parameters (old module?)" >&2; exit 1; }

c0=$(cat $P/stat_commits); t0=$(cat $P/stat_copy_us_total); f0=$(cat $P/stat_full_frames)
sleep "$S"
c1=$(cat $P/stat_commits); t1=$(cat $P/stat_copy_us_total); f1=$(cat $P/stat_full_frames)

dc=$((c1 - c0)); dt=$((t1 - t0)); df=$((f1 - f0))
if [ "$dc" -gt 0 ]; then avg=$((dt / dc)); else avg=0; fi
echo "window ${S}s: commits $dc ($(awk "BEGIN{printf \"%.1f\", $dc/$S}")/s)  avg copy ${avg} us  max $(cat $P/stat_copy_us_max) us"
echo "full-frame commits $df of $dc   last commit $(cat $P/stat_px_last) px (full = 2332800)"
echo "fast_copy=$(cat $P/fast_copy) cache_sync=$(cat $P/cache_sync)"
