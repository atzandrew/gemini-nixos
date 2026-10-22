#!/bin/sh
# Awake A/B/A power test of the polled input drivers (2026-10-18
# "Review of upstream (cjdell) workarounds"). No suspend. USB unplugged, DPMS off, Wi-Fi/BT
# off, cpu1-7 offline (cpu8/9 left as they are). Each phase = "LABEL:shell command" run
# after resetting to the as-is state; settles SETTLE s, then measures the gauge for MEAS s and
# the interrupt rates (keyboard i2c 1101c000, touch i2c 11011000, IPIs, arch_timer).
# Run: sudo -n -b busybox sh /home/andrew/connsys/poll-awake.sh NAME MEAS "A:" "K:kbd 1000" ...
#   helpers for phases: kbd MS (matrix_keypad poll_ms), touch_off, conn_off (connsys power-off as s2.sh)
N=$1; MEAS=$2; shift 2; SETTLE=20
D=/home/andrew/connsys; O=$D/poll-$N.log
P=/sys/class/power_supply/mt6351-battery; KP=/sys/module/matrix_keypad/parameters/poll_ms
TD=/sys/bus/i2c/drivers/ssd2092
: > $O
log() { echo "$(date +%T) $*" >> $O; sync; }
dpms() { timeout 10 runuser -u andrew -- env XDG_RUNTIME_DIR=/run/user/1001 WAYLAND_DISPLAY=wayland-0 kscreen-doctor --dpms $1 >/dev/null 2>&1; }
KP0=$(cat $KP)
kbd() { echo $1 > $KP; }
touch_off() { [ -e $TD/4-0053 ] && echo 4-0053 > $TD/unbind; }
touch_on() { [ -e $TD/4-0053 ] || echo 4-0053 > $TD/bind; }
CONN=0
conn_off() { echo 1 > /sys/module/mtk_wcn/parameters/conn_pwr_off; echo "7 0 0" > /proc/driver/wmt_dbg; echo "7 3 0" > /proc/driver/wmt_dbg; CONN=1; sleep 2; }
conn_on() { [ $CONN = 1 ] || return 0; echo "7 3 1" > /proc/driver/wmt_dbg; echo 0 > /sys/module/mtk_wcn/parameters/conn_pwr_off; CONN=0; sleep 5; }
wmtrx() { p=$(pgrep -x consys_wmtrxd); [ -n "$p" ] && awk '/^voluntary_ctxt/{v=$2} /^nonvoluntary_ctxt/{n=$2} END{print v+n}' /proc/$p/status || echo 0; }
asis() { kbd $KP0; touch_on; conn_on; }
irqs() { awk '/1101c000.i2c/{k=0;for(i=2;i<=11;i++)k+=$i} /11011000.i2c/{t=0;for(i=2;i<=11;i++)t+=$i}
	/^ *IPI1:/{p=0;for(i=2;i<=11;i++)p+=$i} /arch_timer/{a=0;for(i=2;i<=11;i++)a+=$i}
	END{print k+0, t+0, p+0, a+0}' /proc/interrupts; }
restore() {
	asis
	for c in 1 2 3 4 5 6 7; do echo 1 > /sys/devices/system/cpu/cpu$c/online; done
	dpms on; nmcli radio wifi on; sleep 10; timeout 10 bluetoothctl power on >/dev/null 2>&1
	log "restored; poll_ms=$(cat $KP) touch=$([ -e $TD/4-0053 ] && echo bound || echo UNBOUND) online=$(cat /sys/devices/system/cpu/online)"
}
log "waiting for unplug (as-is: kbd poll_ms=$KP0)"
for i in $(seq 300); do [ "$(cat /sys/class/power_supply/bq25890-charger-0/online)" = 0 ] && break; sleep 1; done
[ "$(cat /sys/class/power_supply/bq25890-charger-0/online)" = 0 ] || { log "still plugged - abort"; exit 1; }
log "unplugged; meas=${MEAS}s phases: $*"; sleep 5
dpms off; timeout 20 nmcli radio wifi off; timeout 10 bluetoothctl power off >/dev/null 2>&1
for c in 7 6 5 4 3 2 1; do echo 0 > /sys/devices/system/cpu/cpu$c/online; done
log "dpms off, radios off, online=$(cat /sys/devices/system/cpu/online)"; sleep 30
for ph in "$@"; do
	l=${ph%%:*}; cmd=${ph#*:}
	asis; [ -n "$cmd" ] && eval "$cmd"
	sleep $SETTLE
	set -- $(irqs); k0=$1 t0i=$2 p0=$3 a0=$4; w0=$(wmtrx)
	c0=$(cat $P/charge_counter); t0=$(date +%s.%N); sleep $MEAS
	c1=$(cat $P/charge_counter); t1=$(date +%s.%N)
	set -- $(irqs); w1=$(wmtrx)
	log "$(echo "$c0 $c1 $t0 $t1 $k0 $1 $t0i $2 $p0 $3 $a0 $4 $w0 $w1" | awk '{d=$4-$3; printf "%7.1f mA over %.0f s | irq/s kbd %.0f touch %.0f ipi %.0f timer %.0f | wmtrxd %.0f/s", ($1-$2)*3.6/d, d, ($6-$5)/d, ($8-$7)/d, ($10-$9)/d, ($12-$11)/d, ($14-$13)/d}') [$l] '$cmd'"
done
restore
echo "=== done" >> $O; sync
