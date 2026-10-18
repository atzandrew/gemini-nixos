#!/bin/sh
# Awake A/B/A power test for VPROC2 settings (da9214-test.ko, notes/vproc-research.md).
# No suspend. USB unplugged, DPMS off, Wi-Fi/BT off, cpu1-7 offline (cpu8/9 already torn
# down); each phase loads da9214-test.ko with its args ("" = stock 1.00 V / 2 phases),
# settles SETTLE s and measures the gauge for MEAS s. Everything back after.
# Run: sudo -n -b busybox sh /home/andrew/connsys/vp2-awake.sh NAME MEAS "ARGS1" "ARGS2" ...
# e.g. vp2-awake.sh volt 120 "" "vproc2_mv=600" "" "phases=1" ""
N=$1; MEAS=$2; shift 2; SETTLE=20
D=/home/andrew/connsys; O=$D/vp2-$N.log; K=/home/andrew/da9214-test.ko
P=/sys/class/power_supply/mt6351-battery
: > $O
log() { echo "$(date +%T) $*" >> $O; sync; }
dpms() { timeout 10 runuser -u andrew -- env XDG_RUNTIME_DIR=/run/user/1001 WAYLAND_DISPLAY=wayland-0 kscreen-doctor --dpms $1 >/dev/null 2>&1; }
unload() { lsmod | grep -q da9214_test && rmmod da9214-test; }
restore() {
	unload
	for c in 1 2 3 4 5 6 7; do echo 1 > /sys/devices/system/cpu/cpu$c/online; done
	dpms on; nmcli radio wifi on; sleep 10; timeout 10 bluetoothctl power on >/dev/null 2>&1
	log "restored; online=$(cat /sys/devices/system/cpu/online)"
}
[ -f $K ] || { log "no $K"; exit 1; }
grep -qw 8 /sys/devices/system/cpu/online && { log "cpu8 online - abort"; exit 1; }
log "waiting for unplug"
for i in $(seq 300); do [ "$(cat /sys/class/power_supply/bq25890-charger-0/online)" = 0 ] && break; sleep 1; done
[ "$(cat /sys/class/power_supply/bq25890-charger-0/online)" = 0 ] || { log "still plugged - abort"; exit 1; }
log "unplugged; meas=${MEAS}s phases: $*"; sleep 5
dpms off; timeout 20 nmcli radio wifi off; timeout 10 bluetoothctl power off >/dev/null 2>&1
for c in 7 6 5 4 3 2 1; do echo 0 > /sys/devices/system/cpu/cpu$c/online; done
log "dpms off, radios off, online=$(cat /sys/devices/system/cpu/online)"; sleep 30
for a in "$@"; do
	unload
	if [ -n "$a" ]; then insmod $K $a || { log "insmod '$a' failed - stop"; break; }; fi
	sleep $SETTLE
	c0=$(cat $P/charge_counter); t0=$(date +%s.%N); sleep $MEAS
	c1=$(cat $P/charge_counter); t1=$(date +%s.%N)
	log "$(echo "$c0 $c1 $t0 $t1" | awk '{printf "%7.1f mA over %.0f s", ($1-$2)*3.6/($4-$3), $4-$3}') args='$a' $(dmesg | grep da9214-test | tail -1 | cut -c16-)"
done
restore
echo "=== done" >> $O; sync
