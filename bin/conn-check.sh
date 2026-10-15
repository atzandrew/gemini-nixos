#!/bin/sh
# 3e: connsys power-off check (read r13, one SPM cycle, power connsys back on). Run as root, detached.
L=/home/andrew/conn-check.txt; : > $L
log() { echo "$(date +%T) $*" >> $L; sync $L; }
r13() { log "r13 $1: $(for i in $(seq 20); do busybox devmem 0x10006134 32; sleep 0.1; done | sort | uniq -c | tr -s " \n" " ")"; }
rails() { log "rails: $(grep -sE "vcn|consys" /sys/kernel/debug/regulator/regulator_summary | awk "{print \$1\"=\"\$2}" | tr "\n" " ")"; }
mount | grep -q debugfs || mount -t debugfs none /sys/kernel/debug
r13 before; rails
nmcli radio wifi off; sleep 3
echo off > /sys/kernel/debug/wcn/pwr; log "wcn pwr off rc=$?"; sleep 3
r13 conn-off; rails
sh /home/andrew/spm-run.sh "flags=0x5fc102ff sleep_ms=2000 cycles=1 wake_unmask=0x1 src_mask=0x10aa007e clk_init=1 settle=1"
log "spm: $(grep -E "cyc 1/1|not running" /home/andrew/spm-live.txt)"
r13 after-spm
echo on > /sys/kernel/debug/wcn/pwr; log "wcn pwr on rc=$?"; sleep 3
nmcli radio wifi on
for i in $(seq 30); do ip -br addr show wlan0 2>/dev/null | grep -q "192.168" && break; sleep 2; done
log "wlan0: $(ip -br addr show wlan0 2>&1)"
r13 conn-back; rails
log done
