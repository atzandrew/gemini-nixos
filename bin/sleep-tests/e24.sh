#!/bin/sh
# E24: awake gauge, DPMS blank, new kernel (genpd mfg): 3 x 60 s, sample GPU domain state each minute
O=/home/andrew/connsys/e24.log; : > $O
P=/sys/class/power_supply/mt6351-battery
dpms() { runuser -u andrew -- env XDG_RUNTIME_DIR=/run/user/1001 WAYLAND_DISPLAY=wayland-0 kscreen-doctor --dpms $1 >/dev/null 2>&1; }
m() { c0=$(cat $P/charge_counter); t0=$(date +%s.%N); sleep 60; c1=$(cat $P/charge_counter); t1=$(date +%s.%N)
	echo "$1 $c0 $c1 $t0 $t1" | awk "{printf \"%-14s %6.1f mA\", \$1, (\$2-\$3)*3.6/(\$5-\$4)}" >> $O
	echo "  sta=$(busybox devmem 0x10006180 32) mali=$(cat /sys/bus/platform/devices/13040000.mali/power/runtime_status)" >> $O; sync; }
[ "$(cat /sys/class/power_supply/bq25890-charger-0/online)" = 0 ] || { echo "plugged - abort" >> $O; exit 1; }
dpms off; sleep 10
M=/sys/bus/platform/devices/13040000.mali/power/control
for r in 1 2; do m auto-$r; echo on > $M; sleep 2; m forced-on-$r; echo auto > $M; sleep 2; done; m auto-3
dpms on; echo "=== all done $(date +%T)" >> $O
