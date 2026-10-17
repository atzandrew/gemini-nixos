#!/bin/sh
# S1: system-suspend probe. mt6797-suspend mode=0 = a real `echo mem > /sys/power/state`
# (every driver's suspend/resume, CPUs 1-7 offline/online by the kernel) whose platform
# .enter only busy-waits MS ms: no SPM, mm/audio/conn domains pinned on, panel untouched.
# A72s (cpu8/9) are taken offline first. Run: sudo -n -b busybox sh /home/andrew/connsys/s1.sh [MS]
MS=${1:-5000}
D=/home/andrew/connsys; O=$D/s1.log
: > $O
log() { echo "$(date +%T) $*" >> $O; sync; }
log "start; online=$(cat /sys/devices/system/cpu/online) ms=$MS"
for c in 9 8; do echo 0 > /sys/devices/system/cpu/cpu$c/online 2>/dev/null; done
lsmod | grep -q mt6797_suspend || insmod /home/andrew/mt6797-suspend.ko mode=0 probe_ms=$MS || { log "insmod failed"; exit 1; }
echo deep > /sys/power/mem_sleep
log "mem_sleep=$(cat /sys/power/mem_sleep) online=$(cat /sys/devices/system/cpu/online)"
sleep 3
echo "S1: suspend now" > /dev/kmsg; sync
echo mem > /sys/power/state; rc=$?
log "back rc=$rc online=$(cat /sys/devices/system/cpu/online)"
dmesg | sed -n '/S1: suspend now/,$p' > $D/s1-dmesg.log
log "suspend_stats success=$(cat /sys/power/suspend_stats/success) fail=$(cat /sys/power/suspend_stats/fail) last_failed_dev=$(cat /sys/power/suspend_stats/last_failed_dev)"
sleep 5; rmmod mt6797-suspend; log "=== done"
