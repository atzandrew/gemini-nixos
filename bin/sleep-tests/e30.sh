#!/bin/sh
# E30: awake off-SoC split, SPM-run state (blank, Wi-Fi/BT off, CONN off, cpus 1-9 offline), NO SPM.
# U = USB group low: GPIO 94 usb1-drvvbus, 72 sw7226-en, 70 fusb301a-sw-en (DO 0x10005120 b30/8/6),
#     107 otg-drvvbus (DO 0x10005130 b11).
# T = touch SSD2092 held in reset: GPIO 68 CTP_RST low (DO 0x10005120 b4). Driver unbound for the
#     whole run (its polling would skew awake numbers), rebound at the end (= probe reset + init).
# Sequence, 60 s each: A1 / UT / A2 / U / A3 / T / A4. A = as found.
O=/home/andrew/connsys/e30.log; : > $O
P=/sys/class/power_supply/mt6351-battery
log() { echo "$(date +%T) $*" >> $O; sync; }
D() { busybox devmem "$@"; }
R1=0x10005120; R2=0x10005130
MU1=$(( (1<<30)|(1<<8)|(1<<6) )); MU2=$(( 1<<11 )); MT1=$(( 1<<4 ))
s1=$(D $R1 32); s2=$(D $R2 32)
clr() { D $1 32 $(( $(D $1 32) & ~$2 )); }
put() { D $1 32 $(( ($(D $1 32) & ~$2) | ($3 & $2) )); }
u_off() { clr $R1 $MU1; clr $R2 $MU2; }
u_on()  { put $R1 $MU1 $s1; put $R2 $MU2 $s2; }
t_off() { clr $R1 $MT1; }
t_on()  { put $R1 $MT1 $s1; }
m() { c0=$(cat $P/charge_counter); t0=$(date +%s.%N); sleep 60; c1=$(cat $P/charge_counter); t1=$(date +%s.%N)
	echo "$1 $c0 $c1 $t0 $t1" | awk "{printf \"%-6s %6.1f mA\", \$1, (\$2-\$3)*3.6/(\$5-\$4)}" >> $O
	echo "  R1=$(D $R1 32) R2=$(D $R2 32)" >> $O; sync; }
dpms() { runuser -u andrew -- env XDG_RUNTIME_DIR=/run/user/1001 WAYLAND_DISPLAY=wayland-0 kscreen-doctor --dpms $1 >/dev/null 2>&1; }
TD=/sys/bus/i2c/drivers/ssd2092
restore() {
	u_on; t_on; sleep 1
	echo 4-0053 > $TD/bind 2>/dev/null || log "touch rebind failed"
	dpms on
	echo "7 3 1" > /proc/driver/wmt_dbg; echo 0 > /sys/module/mtk_wcn/parameters/conn_pwr_off
	nmcli radio wifi on; for c in 1 2 3 4 5 6 7; do echo 1 > /sys/devices/system/cpu/cpu$c/online 2>/dev/null; done
	echo ff > /sys/devices/virtual/workqueue/cpumask
	sleep 10; bluetoothctl power on >/dev/null 2>&1
	log "restored; online=$(cat /sys/devices/system/cpu/online) R1=$(D $R1 32) R2=$(D $R2 32) touch=$([ -e $TD/4-0053 ] && echo bound || echo UNBOUND)"
}
[ "$(cat /sys/class/power_supply/bq25890-charger-0/online)" = 0 ] || { log "plugged - abort"; exit 1; }
log "start s1=$s1 s2=$s2 bat=$(cat $P/capacity)%"
dpms off; sleep 3
nmcli radio wifi off; bluetoothctl power off >/dev/null 2>&1; sleep 2
echo 1 > /sys/module/mtk_wcn/parameters/conn_pwr_off; echo "7 0 0" > /proc/driver/wmt_dbg; echo "7 3 0" > /proc/driver/wmt_dbg
sleep 2; echo 1 > /sys/devices/virtual/workqueue/cpumask
for c in 9 8 7 6 5 4 3 2 1; do echo 0 > /sys/devices/system/cpu/cpu$c/online 2>/dev/null; done
[ -e $TD/4-0053 ] && echo 4-0053 > $TD/unbind
log "regs: $(for r in /sys/class/regulator/regulator.*; do n=$(cat $r/name); case $n in vcn*) echo -n "$n=$(cat $r/state) ";; esac; done)"
dpms off; sleep 10
m A1; u_off; t_off; sleep 2; m UT
u_on; t_on; sleep 2; m A2
u_off; sleep 2; m U; u_on; sleep 2; m A3
t_off; sleep 2; m T; t_on; sleep 2; m A4
restore
log "=== all done"
