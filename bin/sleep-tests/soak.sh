#!/bin/sh
# soak.sh - unattended sleep series that survives the SPM hang (2026-10-21).
# What: everyday-style suspends back to back (gemini-suspend-prep pre -> echo mem
#       -> prep post, as wake-test.sh), each one SLEEP_S long (mt6797-suspend
#       max_s), until END or low battery. A hang ends in a PCM watchdog reset;
#       the Gemini reboots, greetd logs andrew into Plasma (autologin block, only
#       while armed), gemini-soak.service starts this script again, which logs
#       the hang + breadcrumb and carries on. Result: hangs per hour asleep.
# Why:  docs/spm-sleep.md "Current status", plan Stage 2 (notes/plan-sleep-2026-10-20.md).
# Safety: the kernel powers the panel off in every suspend (geminipda_drm
#       panel_suspend) and bias_check refuses sleep with the LCD bias on; this
#       script checks both are active before every sleep and stops otherwise.
# Files (Gemini): /home/andrew/soak/{soak.sh,armed,soak.log,state,stop}
#       /etc/systemd/system/gemini-soak.service (bin/sleep-tests/gemini-soak.service)
# Run (root: sudo -n busybox sh /home/andrew/soak/soak.sh ...):
#   arm HOURS [SLEEP_S] [LABEL] [MIN_BAT]   arm, add autologin, enable + start the service
#   disarm                                  remove autologin, disable the service, max_s=0
#   run                                     (the service) the series
#   Stop gracefully: touch /home/andrew/soak/stop (checked between sleeps).
set -u
D=/home/andrew/soak; O=$D/soak.log; A=$D/armed; ST=$D/state
G=/etc/greetd/config.toml; U=gemini-soak.service
M=/sys/module/mt6797_suspend/parameters; P=/sys/module/geminipda_drm/parameters
BAT=/sys/class/power_supply/mt6351-battery; CHG=/sys/class/power_supply/bq25890-charger-0/online
PREP=/usr/local/sbin/gemini-suspend-prep
log() { echo "$(date '+%F %T') $*" >> $O; sync; }

autologin_off() { sed -i '/^# gemini-soak BEGIN/,/^# gemini-soak END/d' $G; }
autologin_on() {
	autologin_off
	cat >> $G <<-'EOF'
	# gemini-soak BEGIN (bin/sleep-tests/soak.sh arm; disarm deletes this block)
	[initial_session]
	command = "startplasma-wayland"
	user = "andrew"
	# gemini-soak END
	EOF
}

disarm() {	# $1 = reason
	rm -f $A $ST
	autologin_off
	systemctl disable $U >/dev/null 2>&1
	[ -d $M ] && echo 0 > $M/max_s
	log "=== disarmed: $1 (autologin off, service disabled, max_s=0)"
}

case "${1:-}" in
arm)
	[ -n "${2:-}" ] || { echo "usage: $0 arm HOURS [SLEEP_S] [LABEL] [MIN_BAT]" >&2; exit 2; }
	mkdir -p $D; rm -f $D/stop
	end=$(( $(date +%s) + ${2} * 3600 ))
	printf 'END=%s\nSLEEP_S=%s\nLABEL=%s\nMIN_BAT=%s\n' $end "${3:-600}" "${4:-soak}" "${5:-30}" > $A
	autologin_on
	systemctl daemon-reload; systemctl enable $U >/dev/null 2>&1
	log "=== armed: $(tr '\n' ' ' < $A)(until $(date -d @$end '+%F %T'))"
	systemctl start --no-block $U
	exit 0 ;;
disarm) disarm "by hand"; systemctl stop --no-block $U; exit 0 ;;
run) ;;
*) echo "usage: $0 arm HOURS [SLEEP_S] [LABEL] [MIN_BAT] | disarm | run" >&2; exit 2 ;;
esac

[ -e $A ] || exit 0
. $A

# A boot while "asleep" in the state file = the last sleep never came back.
crumb=$(dmesg | grep -o 'last breadcrumb.*' | head -1)
if [ -e $ST ]; then
	read t0 n < $ST
	log "HANG [$LABEL]: sleep #$n started $(date -d @$t0 +%T), boot at $(date -d "-$(cut -d. -f1 /proc/uptime) sec" +%T); $crumb"
	rm -f $ST
else
	log "boot/start [$LABEL]; ${crumb:-no breadcrumb}"
fi

# Wait for the Plasma session (autologin) and let it settle (Wi-Fi up for SSH).
i=0; until [ -S /run/user/1001/wayland-0 ] || [ $i -ge 300 ]; do sleep 2; i=$((i+2)); done
[ -S /run/user/1001/wayland-0 ] || log "no Plasma session after 300 s - trying anyway"
sleep 60

n=0
while :; do
	[ -e $D/stop ] && { rm -f $D/stop; disarm "stop file"; exit 0; }
	[ $(date +%s) -lt $END ] || { disarm "END reached"; exit 0; }
	cap=$(cat $BAT/capacity)
	[ $cap -ge $MIN_BAT ] || { disarm "battery $cap% < $MIN_BAT%"; systemctl poweroff; exit 0; }
	if [ "$(cat $CHG)" != 0 ]; then log "on external power - waiting"; sleep 60; continue; fi
	[ "$(cat $P/panel_suspend)" = Y ] && [ "$(cat $M/bias_check)" = Y ] ||
		{ disarm "panel_suspend/bias_check not active"; exit 1; }

	n=$((n + 1))
	echo $SLEEP_S > $M/max_s
	out=$($PREP pre 2>&1); rc=$?
	if [ $rc -ne 0 ]; then log "#$n prep refused: $out"; sleep 30; continue; fi
	c0=$(cat $BAT/charge_counter); t0=$(date +%s)
	echo "$t0 $n" > $ST; log "#$n sleep ${SLEEP_S} s (bat $cap%)"
	echo "SOAK $LABEL #$n: suspend now" > /dev/kmsg
	echo mem > /sys/power/state; rc=$?
	t1=$(date +%s); c1=$(cat $BAT/charge_counter); rm -f $ST
	ma=$(awk -v a=$c0 -v b=$c1 -v d=$((t1 - t0)) 'BEGIN { if (d > 0) printf "%.0f", (a - b) * 3.6 / d }')
	log "#$n back rc=$rc after $((t1 - t0)) s, ${ma} mA, wake='$(cat $M/last_wake)' bias='$(cat $M/bias_at_sleep)'"
	$PREP post >/dev/null 2>&1
	echo 0 > $M/max_s
	sleep 30
done
