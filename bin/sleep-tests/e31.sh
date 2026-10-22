#!/bin/sh
# E31: awake VCN18 A/B, SPM-run state (blank, Wi-Fi/BT off, CONN domain off, cpus 1-9 offline), NO SPM.
# B = MT6351 VCN18 (MT6631 combo chip 1.8 V, SW mode, stays on in sleep) off: LDO_VCN18_CON0 0x0a52
# EN (bit 1) cleared via pmw.sh. Stock turns VCN18/28/33 off with connsys off. A/B/A/B/A, 60 s each.
# Restore writes the found value back before connsys is powered on again.
O=/home/andrew/connsys/e31.log; : > $O
P=/sys/class/power_supply/mt6351-battery
PMW=/home/andrew/connsys/pmw.sh
RG=/sys/kernel/debug/regmap/1000d000.pwrap/registers
log() { echo "$(date +%T) $*" >> $O; sync; }
pr() { busybox grep -E "^0a52:" $RG | cut -d" " -f2; }
v0=$(pr)
case $v0 in da62) ;; *) log "VCN18_CON0=$v0, expected da62 - abort"; exit 1;; esac
vcn_off() { sh $PMW 0x0a52 0xda60; }
vcn_on()  { sh $PMW 0x0a52 0x$v0; }
m() { c0=$(cat $P/charge_counter); t0=$(date +%s.%N); sleep 60; c1=$(cat $P/charge_counter); t1=$(date +%s.%N)
	echo "$1 $c0 $c1 $t0 $t1" | awk "{printf \"%-6s %6.1f mA\", \$1, (\$2-\$3)*3.6/(\$5-\$4)}" >> $O
	echo "  0a52=$(pr)" >> $O; sync; }
dpms() { runuser -u andrew -- env XDG_RUNTIME_DIR=/run/user/1001 WAYLAND_DISPLAY=wayland-0 kscreen-doctor --dpms $1 >/dev/null 2>&1; }
restore() {
	vcn_on; sleep 1; log "0a52=$(pr) before conn on"
	dpms on
	echo "7 3 1" > /proc/driver/wmt_dbg; echo 0 > /sys/module/mtk_wcn/parameters/conn_pwr_off
	nmcli radio wifi on; for c in 1 2 3 4 5 6 7; do echo 1 > /sys/devices/system/cpu/cpu$c/online 2>/dev/null; done
	echo ff > /sys/devices/virtual/workqueue/cpumask
	sleep 10; timeout 10 bluetoothctl power on >/dev/null 2>&1
	sleep 10; log "restored; online=$(cat /sys/devices/system/cpu/online) 0a52=$(pr) wifi=$(nmcli -t -f STATE g)"
}
[ "$(cat /sys/class/power_supply/bq25890-charger-0/online)" = 0 ] || { log "plugged - abort"; exit 1; }
log "start 0a52=$v0 bat=$(cat $P/capacity)%"
dpms off; sleep 3
nmcli radio wifi off; timeout 5 bluetoothctl power off >/dev/null 2>&1; sleep 2
echo 1 > /sys/module/mtk_wcn/parameters/conn_pwr_off; echo "7 0 0" > /proc/driver/wmt_dbg; echo "7 3 0" > /proc/driver/wmt_dbg
sleep 2; echo 1 > /sys/devices/virtual/workqueue/cpumask
for c in 9 8 7 6 5 4 3 2 1; do echo 0 > /sys/devices/system/cpu/cpu$c/online 2>/dev/null; done
dpms off; sleep 10
m A1; vcn_off; sleep 2; m B1; vcn_on; sleep 2; m A2; vcn_off; sleep 2; m B2; vcn_on; sleep 2; m A3
restore
log "=== all done"
