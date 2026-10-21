#!/bin/sh
# wake-test.sh - one everyday-style suspend while sleep.target is masked (2026-10-21).
# What: the same steps systemd-suspend + gemini-suspend-prep.service would run:
#       gemini-suspend-prep pre (DPMS off, connsys off, A72s off, DCS 0x28) -> echo mem -> prep post.
#       It sleeps until the lid is opened or Esc/On / the silver key is pressed (mt6797-suspend
#       max_s=0). Afterwards it logs why it woke, the LCD bias read right before sleep, and the
#       geminipda-drm panel off/on lines.
# Why:  wake-path tests for panel-pm (docs/display.md "Panel off/on") without unmasking suspend.
# Needs: test mt6797-suspend with bias_check loaded (max_s=0 wake_keys=1 wake_lid=1 pcm_wdt=30) and
#        geminipda_drm panel_suspend=1. USB unplugged (prep refuses on external power).
# Run:  sudo -n -b busybox sh /home/andrew/panel/wake-test.sh LABEL     log: /home/andrew/panel/wake-test.log
L=${1:-test}; O=/home/andrew/panel/wake-test.log
M=/sys/module/mt6797_suspend/parameters; P=/sys/module/geminipda_drm/parameters
log() { echo "$(date +%T) [$L] $*" >> $O; sync; }
[ "$(cat $P/panel_suspend)" = Y ] || { log "panel_suspend not set - abort"; exit 1; }
[ "$(cat $M/bias_check 2>/dev/null)" = Y ] || { log "test module (bias_check) not loaded - abort"; exit 1; }
out=$(/usr/local/sbin/gemini-suspend-prep pre 2>&1); rc=$?
log "prep pre rc=$rc: $out"
[ $rc -eq 0 ] || exit 1
echo "WAKE-TEST $L: suspend now" > /dev/kmsg
t0=$(date +%s); echo mem > /sys/power/state; rc=$?; t1=$(date +%s)
log "back rc=$rc after $((t1 - t0)) s; last_wake='$(cat $M/last_wake)' bias_at_sleep='$(cat $M/bias_at_sleep)'" \
    "panel=$(cat $P/panel) err=$(cat $P/stat_panel_err) on_ms=$(cat $P/stat_panel_on_ms)"
out=$(/usr/local/sbin/gemini-suspend-prep post 2>&1)
log "prep post: $out"
log "kernel: $(dmesg | sed -n "/WAKE-TEST $L: suspend now/,\$p" | grep -E 'geminipda-drm: panel|LCD bias|refusing' | cut -c15- | tr '\n' '|')"
