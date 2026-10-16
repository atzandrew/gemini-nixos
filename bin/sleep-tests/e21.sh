#!/bin/sh
# E21: awake A/B/A gauge, DPMS blank. B = all 6 GPU domains off (stock clk-mt6797-pg.c sequence), 60 s each
O=/home/andrew/connsys/e21.log; : > $O
P=/sys/class/power_supply/mt6351-battery
S=0x10006000
rd() { busybox devmem $(($S + $1)) 32; }
wr() { busybox devmem $(($S + $1)) 32 $(( $2 )); }
sb() { wr $1 $(( $(rd $1) | $2 )); }
clr() { wr $1 $(( $(rd $1) & ~$2 )); }
# wait OFFSET MASK want(0|1)
wt() { i=0; while :; do v=$(( $(rd $1) & $2 )); if [ $3 = 1 ]; then [ $v -eq $(( $2 )) ] && return 0; else [ $v -eq 0 ] && return 0; fi
	i=$((i+1)); [ $i -gt 500 ] && { echo "TIMEOUT $1 $2 $3 v=$v" >> $O; return 1; }; done; }
wst() { wt 0x180 $1 $2 && wt 0x184 $1 $2; }
key() { wr 0x000 $(( (0xb16<<16) | 1 )); }
# PWR_CON bits: 0 RST_B 1 ISO 2 ON 3 ON_2ND 4 CLK_DIS
core_off() { c=$((0x340 + 4*$1)); sb $c 0x100; wt 0x33c $((1<<(20+$1))) 1; sb $c 0x10; sb $c 0x2; clr $c 0x1; clr $c 0x4; clr $c 0x8; wst $((1<<(11-$1))) 0; }
core_on()  { c=$((0x340 + 4*$1)); sb $c 0x4; sb $c 0x8; wst $((1<<(11-$1))) 1; sb $c 0x1; clr $c 0x2; clr $c 0x10; clr $c 0x100; wt 0x33c $((1<<(20+$1))) 0; }
mfg_off() { sb 0x33c 0x3; wt 0x33c 0x30000 1; sb 0x338 0x2; sb 0x338 0x10; clr 0x338 0x1; clr 0x338 0x4; clr 0x338 0x8; wst 0x1000 0; }
mfg_on()  { sb 0x338 0x4; sb 0x338 0x8; wst 0x1000 1; clr 0x338 0x10; clr 0x338 0x2; sb 0x338 0x1; clr 0x33c 0x3; wt 0x33c 0x30000 0; }
as_off() { sb 0x334 0x2; sb 0x334 0x10; clr 0x334 0x1; clr 0x334 0x4; clr 0x334 0x8; wst 0x2000 0; }
as_on()  { sb 0x334 0x4; sb 0x334 0x8; wst 0x2000 1; clr 0x334 0x10; clr 0x334 0x2; sb 0x334 0x1; }
gpu_off() { key; for n in 3 2 1 0; do core_off $n; done; mfg_off; as_off; }
gpu_on()  { key; as_on; mfg_on; for n in 0 1 2 3; do core_on $n; done; }
snap() { echo "$1 PWR_STATUS=$(rd 0x180) 2ND=$(rd 0x184) ASYNC=$(rd 0x334) MFG=$(rd 0x338) SRAM=$(rd 0x33c) C0..3=$(rd 0x340) $(rd 0x344) $(rd 0x348) $(rd 0x34c) rpm=$(cat /sys/bus/platform/devices/13040000.mali/power/runtime_status)" >> $O; sync; }
dpms() { runuser -u andrew -- env XDG_RUNTIME_DIR=/run/user/1001 WAYLAND_DISPLAY=wayland-0 kscreen-doctor --dpms $1 >/dev/null 2>&1; }
m() { c0=$(cat $P/charge_counter); t0=$(date +%s.%N); sleep 60; c1=$(cat $P/charge_counter); t1=$(date +%s.%N)
	echo "$1 $c0 $c1 $t0 $t1" | awk "{printf \"%-14s %6.1f mA\n\", \$1, (\$2-\$3)*3.6/(\$5-\$4)}" >> $O; sync; }
[ "$(cat /sys/class/power_supply/bq25890-charger-0/online)" = 0 ] || { echo "plugged - abort" >> $O; exit 1; }
echo "=== E21 start $(date +%T)" >> $O
dpms off; sleep 10
[ "$(busybox devmem 0x1400b02c 32)" = 0x00000000 ] || { echo "not blanked - abort" >> $O; dpms on; exit 1; }
m A1-gpu-on
snap pre
[ "$(cat /sys/bus/platform/devices/13040000.mali/power/runtime_status)" = suspended ] || { echo "mali active - skip B" >> $O; dpms on; exit 1; }
gpu_off; snap off
m B-gpu-off
snap before-on; gpu_on; snap on
m A2-gpu-on; [ "$(cat /sys/bus/platform/devices/13040000.mali/power/runtime_status)" = suspended ] && { gpu_off; snap off2; m B2-gpu-off; gpu_on; snap on2; m A3-gpu-on; }
dpms on; echo "=== all done $(date +%T)" >> $O
