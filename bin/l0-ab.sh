#!/bin/sh
# OVL0 L0 off/on A/B (idle power of DRAM scanout). Run as root, screen on, on battery.
# SRC_CON 0x1400b02c: expect 0x1 (L0 on, L3 off by geminipda-drm ovl_l3_off).
P=/sys/class/power_supply/mt6351-battery
R=0x1400b02c
S=${1:-60}
v=$(busybox devmem $R 32)
[ "$v" = "0x00000001" ] || { echo "SRC_CON is $v, expected 0x00000001 — not touching it"; exit 1; }
m() {
	c0=$(cat $P/charge_counter); t0=$(date +%s.%N)
	sleep $S
	c1=$(cat $P/charge_counter); t1=$(date +%s.%N)
	echo "$1 $c0 $c1 $t0 $t1" | awk '{printf "%-7s %7.1f mA over %.1f s\n", $1, ($2-$3)*3.6/($5-$4), $5-$4}'
}
cat $P/status
m L0-on
busybox devmem $R 32 0x0; m L0-off
busybox devmem $R 32 0x1; m L0-on
busybox devmem $R 32 0x0; m L0-off
busybox devmem $R 32 0x1
echo "SRC_CON now $(busybox devmem $R 32)"
