#!/bin/sh
# E15: does DCS display-off (0x28) protect the panel through SPM 26M-off?
# CONN off, baseline flags 0x5fc102ff, only CYC (default 3) x 2 s SPM cycles. Panel: DPMS off + DCS 0x28 before,
# L0 re-checked right before insmod; after: DCS 0x29, DPMS on, CONN/Wi-Fi/BT back, cpus back online.
CYC=${1:-3}
FLAGS=${2:-0x5fc102ff}
O=${3:-/home/andrew/connsys/e15.log}
: > $O
log() { echo "$(date +%T) $*" >> $O; sync; }
B=0x1401c000
rd() { busybox devmem $(($B + $1)) 32; }
wr() { busybox devmem $(($B + $1)) 32 $2; }
dcs() {	# vendor DSI_set_cmdq_V2 VDO branch, DCS short no-param (see ~/panel/dcs.sh)
	st=$(rd 0x0c); wr 0x0c $(( st & ~0x20 )); wr 0x130 $(( ($1 << 16) | (0x05 << 8) | 0x21 ))
	s=$(rd 0x00); wr 0x00 $(( s & ~0x10000 )); wr 0x00 $(( s | 0x10000 ))
	i=0; while [ $(( $(rd 0x0c) & 0x20 )) -eq 0 ]; do i=$((i+1)); [ $i -gt 200 ] && { log "dcs $1 NO DONE"; return 1; }; done
	st=$(rd 0x0c); wr 0x0c $(( st & ~0x20 )); log "dcs $1 done ($i polls)"
}
dpms() { runuser -u andrew -- env XDG_RUNTIME_DIR=/run/user/1001 WAYLAND_DISPLAY=wayland-0 kscreen-doctor --dpms $1 >/dev/null 2>&1; }
l0() { busybox devmem 0x1400b02c 32; }
restore() {
	dcs 0x29; dpms on
	if [ "$CONNOFF" = 1 ]; then log "pre-on r13=$(busybox devmem 0x10006134 32)"; echo "7 3 1" > /proc/driver/wmt_dbg
		echo 0 > /sys/module/mtk_wcn/parameters/conn_pwr_off; fi
	nmcli radio wifi on; for c in 1 2 3 4 5 6 7; do echo 1 > /sys/devices/system/cpu/cpu$c/online 2>/dev/null; done
	sleep 10; bluetoothctl power on >/dev/null 2>&1; log "restored; online=$(cat /sys/devices/system/cpu/online)"
}
[ "$(rd 0x14)" = 0x00000001 ] && [ $(( $(rd 0x130) & 0xff )) -eq $((0x21)) ] || { log "DSI not in expected state - abort"; exit 1; }
for i in $(seq 300); do [ "$(cat /sys/class/power_supply/bq25890-charger-0/online)" = 0 ] && break; sleep 1; done
[ "$(cat /sys/class/power_supply/bq25890-charger-0/online)" = 0 ] || { log "still plugged - abort"; exit 1; }
log "unplugged; cycles=$CYC flags=$FLAGS"; sleep 5
dpms off; sleep 3
nmcli radio wifi off; bluetoothctl power off >/dev/null 2>&1; sleep 2
echo 1 > /sys/module/mtk_wcn/parameters/conn_pwr_off; echo "7 0 0" > /proc/driver/wmt_dbg; echo "7 3 0" > /proc/driver/wmt_dbg; CONNOFF=1
sleep 2; log "connoff r13=$(busybox devmem 0x10006134 32) PWR_CON=$(busybox devmem 0x1000632c 32)"
echo 1 > /sys/devices/virtual/workqueue/cpumask
for c in 9 8 7 6 5 4 3 2 1; do echo 0 > /sys/devices/system/cpu/cpu$c/online 2>/dev/null; done
dpms off; sleep 2
dcs 0x28 || { restore; exit 1; }
sleep 1
[ "$(l0)" = 0x00000000 ] || { log "L0 on right before insmod - abort"; restore; exit 1; }
log "insmod (L0 $(l0))"
P=/sys/class/power_supply/mt6351-battery; c0=$(cat $P/charge_counter); t0=$(date +%s.%N)
insmod /home/andrew/mt6797-spm-load.ko stage=6 io_en=0x81 acinactm=0 sleep_ms=2000 cycles=$CYC wake_unmask=0x1 flags=$FLAGS src_mask=0x10aa007e clk_init=1 settle=1 pdn=1
c1=$(cat $P/charge_counter); t1=$(date +%s.%N)
rmmod mt6797-spm-load
log "SPM sleep $(echo $c0 $c1 $t0 $t1 | awk '{printf "%.1f mA over %.1f s", ($1-$2)*3.6/($4-$3), $4-$3}')"
log "spm: $(dmesg | grep "mt6797-spm-load: cyc" | tail -$((CYC*2)) | grep -o "dbg [0-9a-f]*" | sort | uniq -c | tr "\n" " ")"
restore
log "=== all done"
