#!/bin/sh
# E17: awake A/B/A gauge, no SPM: DPMS blank only / + DCS 0x28 display-off / DPMS blank only, 60 s each (x2 rounds)
O=/home/andrew/connsys/e17.log; : > $O
P=/sys/class/power_supply/mt6351-battery
B=0x1401c000
rd() { busybox devmem $(($B + $1)) 32; }
wr() { busybox devmem $(($B + $1)) 32 $2; }
dcs() { st=$(rd 0x0c); wr 0x0c $(( st & ~0x20 )); wr 0x130 $(( ($1 << 16) | (0x05 << 8) | 0x21 ))
	s=$(rd 0x00); wr 0x00 $(( s & ~0x10000 )); wr 0x00 $(( s | 0x10000 ))
	i=0; while [ $(( $(rd 0x0c) & 0x20 )) -eq 0 ]; do i=$((i+1)); [ $i -gt 200 ] && return 1; done
	st=$(rd 0x0c); wr 0x0c $(( st & ~0x20 )); }
dpms() { runuser -u andrew -- env XDG_RUNTIME_DIR=/run/user/1001 WAYLAND_DISPLAY=wayland-0 kscreen-doctor --dpms $1 >/dev/null 2>&1; }
m() { c0=$(cat $P/charge_counter); t0=$(date +%s.%N); sleep 60; c1=$(cat $P/charge_counter); t1=$(date +%s.%N)
	echo "$1 $c0 $c1 $t0 $t1" | awk "{printf \"%-14s %6.1f mA\n\", \$1, (\$2-\$3)*3.6/(\$5-\$4)}" >> $O; sync; }
[ "$(cat /sys/class/power_supply/bq25890-charger-0/online)" = 0 ] || { echo "plugged - abort" >> $O; exit 1; }
dpms off; sleep 10
[ "$(busybox devmem 0x1400b02c 32)" = 0x00000000 ] || { echo "not blanked - abort" >> $O; dpms on; exit 1; }
for r in 1 2; do
	m "blank"; dcs 0x28; m "blank+dispoff"; dcs 0x29
done
m "blank"
dpms on; echo "=== all done $(date +%T)" >> $O
