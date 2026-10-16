#!/bin/sh
# E26: awake group test, SPM-run state (blank, Wi-Fi/BT off, CONN off, cpus 1-9 offline), NO SPM.
# Group = GPIO 94 usb1-drvvbus, 72 sw7226-en, 70 fusb301a-sw-en (DO 0x10005120 bits 30/8/6),
#         243/244 speaker amps (DO 0x10005170 bits 19/20). A = as found, B = group low. A/B/A/B/A, 60 s each.
O=/home/andrew/connsys/e27.log; : > $O
P=/sys/class/power_supply/mt6351-battery
log() { echo "$(date +%T) $*" >> $O; sync; }
D() { busybox devmem "$@"; }
G1=0x10005120; M1=$(( (1<<30)|(1<<8)|(1<<6) ))
G2=0x10005170; M2=$(( (1<<19)|(1<<20) ))
s1=$(D $G1 32); s2=$(D $G2 32)
grp_off() { D $G1 32 $(( $(D $G1 32) & ~M1 )); D $G2 32 $(( $(D $G2 32) & ~M2 )); }
amps_off() { D $G2 32 $(( $(D $G2 32) & ~M2 )); }
usb_off() { D $G1 32 $(( $(D $G1 32) & ~M1 )); }
grp_on()  { D $G1 32 $(( ($(D $G1 32) & ~M1) | (s1 & M1) )); D $G2 32 $(( ($(D $G2 32) & ~M2) | (s2 & M2) )); }
m() { c0=$(cat $P/charge_counter); t0=$(date +%s.%N); sleep 60; c1=$(cat $P/charge_counter); t1=$(date +%s.%N)
	echo "$1 $c0 $c1 $t0 $t1" | awk "{printf \"%-10s %6.1f mA\", \$1, (\$2-\$3)*3.6/(\$5-\$4)}" >> $O
	echo "  G1=$(D $G1 32) G2=$(D $G2 32)" >> $O; sync; }
dpms() { runuser -u andrew -- env XDG_RUNTIME_DIR=/run/user/1001 WAYLAND_DISPLAY=wayland-0 kscreen-doctor --dpms $1 >/dev/null 2>&1; }
restore() {
	grp_on; dpms on
	echo "7 3 1" > /proc/driver/wmt_dbg; echo 0 > /sys/module/mtk_wcn/parameters/conn_pwr_off
	nmcli radio wifi on; for c in 1 2 3 4 5 6 7; do echo 1 > /sys/devices/system/cpu/cpu$c/online 2>/dev/null; done
	sleep 10; bluetoothctl power on >/dev/null 2>&1; log "restored; online=$(cat /sys/devices/system/cpu/online) G1=$(D $G1 32) G2=$(D $G2 32)"
}
[ "$(cat /sys/class/power_supply/bq25890-charger-0/online)" = 0 ] || { log "plugged - abort"; exit 1; }
log "start s1=$s1 s2=$s2"
dpms off; sleep 3
nmcli radio wifi off; bluetoothctl power off >/dev/null 2>&1; sleep 2
echo 1 > /sys/module/mtk_wcn/parameters/conn_pwr_off; echo "7 0 0" > /proc/driver/wmt_dbg; echo "7 3 0" > /proc/driver/wmt_dbg
sleep 2; echo 1 > /sys/devices/virtual/workqueue/cpumask
for c in 9 8 7 6 5 4 3 2 1; do echo 0 > /sys/devices/system/cpu/cpu$c/online 2>/dev/null; done
dpms off; sleep 10
m A1; amps_off; sleep 2; m B-amps; grp_on; sleep 2; m A2; usb_off; sleep 2; m C-usb; grp_on; sleep 2; m A3
restore
log "=== all done"
