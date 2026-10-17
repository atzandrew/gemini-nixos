#!/bin/sh
# S2: real system suspend through the SPM (mt6797-suspend mode=1). Protected like e28-run:
# USB unplugged, DPMS off, Wi-Fi/BT/CONN off, cpu8/9 offline, DCS 0x28 (panel display-off)
# before; DCS 0x29 + everything back after. .enter sleeps POLL ms per SPM cycle and leaves on
# Esc/On or silver key, lid opening, or MAX s. Run: sudo -n -b busybox sh /home/andrew/connsys/s2.sh [MAX] [POLL] [GAP]
MAX=${1:-60}; POLL=${2:-2000}; GAP=${3:-200}
D=/home/andrew/connsys; O=$D/s2.log
: > $O
log() { echo "$(date +%T) $*" >> $O; sync; }
B=0x1401c000
rd() { busybox devmem $(($B + $1)) 32; }
wr() { busybox devmem $(($B + $1)) 32 $2; }
dcs() {	# DCS short no-param via DSI VM_CMD (see ~/panel/dcs.sh); never 0x10
	st=$(rd 0x0c); wr 0x0c $(( st & ~0x20 )); wr 0x130 $(( ($1 << 16) | (0x05 << 8) | 0x21 ))
	s=$(rd 0x00); wr 0x00 $(( s & ~0x10000 )); wr 0x00 $(( s | 0x10000 ))
	i=0; while [ $(( $(rd 0x0c) & 0x20 )) -eq 0 ]; do i=$((i+1)); [ $i -gt 200 ] && { log "dcs $1 NO DONE"; return 1; }; done
	st=$(rd 0x0c); wr 0x0c $(( st & ~0x20 )); log "dcs $1 done ($i polls)"
}
dpms() { runuser -u andrew -- env XDG_RUNTIME_DIR=/run/user/1001 WAYLAND_DISPLAY=wayland-0 kscreen-doctor --dpms $1 >/dev/null 2>&1; }
l0() { busybox devmem 0x1400b02c 32; }
restore() {
	dcs 0x29; dpms on
	if [ "$CONNOFF" = 1 ]; then echo "7 3 1" > /proc/driver/wmt_dbg
		echo 0 > /sys/module/mtk_wcn/parameters/conn_pwr_off; fi
	nmcli radio wifi on; sleep 10; bluetoothctl power on >/dev/null 2>&1
	log "restored; online=$(cat /sys/devices/system/cpu/online)"
}
[ "$(rd 0x14)" = 0x00000001 ] && [ $(( $(rd 0x130) & 0xff )) -eq $((0x21)) ] || { log "DSI not in expected state - abort"; exit 1; }
lsmod | grep -q mt6797_suspend && rmmod mt6797-suspend
insmod /home/andrew/mt6797-suspend.ko mode=1 max_s=$MAX poll_ms=$POLL gap_ms=$GAP || { log "insmod failed"; exit 1; }
echo deep > /sys/power/mem_sleep
log "module ok; mem_sleep=$(cat /sys/power/mem_sleep); waiting for unplug"
for i in $(seq 300); do [ "$(cat /sys/class/power_supply/bq25890-charger-0/online)" = 0 ] && break; sleep 1; done
[ "$(cat /sys/class/power_supply/bq25890-charger-0/online)" = 0 ] || { log "still plugged - abort"; rmmod mt6797-suspend; exit 1; }
log "unplugged; max=$MAX poll=$POLL gap=$GAP"; sleep 5
dpms off; sleep 3
nmcli radio wifi off; bluetoothctl power off >/dev/null 2>&1; sleep 2
echo 1 > /sys/module/mtk_wcn/parameters/conn_pwr_off; echo "7 0 0" > /proc/driver/wmt_dbg; echo "7 3 0" > /proc/driver/wmt_dbg; CONNOFF=1
sleep 2; log "connoff r13=$(busybox devmem 0x10006134 32)"
for c in 9 8; do echo 0 > /sys/devices/system/cpu/cpu$c/online 2>/dev/null; done
dpms off; sleep 2
dcs 0x28 || { restore; exit 1; }
sleep 1
[ "$(l0)" = 0x00000000 ] || { log "L0 on right before suspend - abort"; restore; exit 1; }
P=/sys/class/power_supply/mt6351-battery; c0=$(cat $P/charge_counter); t0=$(date +%s.%N)
log "suspend (L0 $(l0))"
echo "S2: suspend now" > /dev/kmsg; sync
echo mem > /sys/power/state; rc=$?
c1=$(cat $P/charge_counter); t1=$(date +%s.%N)
log "back rc=$rc; $(echo $c0 $c1 $t0 $t1 | awk '{printf "%.1f mA over %.1f s (incl. ~15 s entry/exit)", ($1-$2)*3.6/($4-$3), $4-$3}')"
restore
dmesg | sed -n '/S2: suspend now/,$p' > $D/s2-dmesg.log
log "$(grep 'mt6797-suspend:' $D/s2-dmesg.log | cut -c15- | tr '\n' '|')"
log "suspend_stats success=$(cat /sys/power/suspend_stats/success) fail=$(cat /sys/power/suspend_stats/fail) last_failed_dev=$(cat /sys/power/suspend_stats/last_failed_dev)"
rmmod mt6797-suspend; log "=== done"
