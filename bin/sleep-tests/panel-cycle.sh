#!/bin/sh
# panel-cycle.sh - stock SSD2092 panel off/on through geminipda-drm panel-pm (2026-10-21).
# What: drives /sys/module/geminipda_drm/parameters/panel ("off" = stock lcm_suspend, "on" = stock
#       lcm_resume with the full init table) and prints what matters after each step: driver state,
#       last error, init time, bias enables (GPIO60 ENP / GPIO251 ENN) and the LCM reset (0x14000150).
# Why:  awake tests before panel-pm is allowed into suspend (docs/display.md "Panel off/on").
#       ONLY with the user watching the glass, ready to force a power-off; stop on any flicker/lines.
# usage (root): panel-cycle.sh state
#               panel-cycle.sh off | on
#               panel-cycle.sh cycle [SECONDS_OFF] [COUNT]     (default 10 s, 1 cycle)
P=/sys/module/geminipda_drm/parameters
log() { echo "$(date +%T) $*"; }
[ -e $P/panel ] || { echo "no $P/panel (old module?)"; exit 1; }
gpios() {
	d=/sys/kernel/debug/gpio
	if [ -r $d ]; then grep -E "gpio-(60|251) " $d | tr -s ' ' | tr '\n' ';'
	else printf 'no debugfs gpio;'; fi
}
show() {
	log "panel=$(cat $P/panel) err=$(cat $P/stat_panel_err) err_step=$(cat $P/stat_panel_err_cmd)" \
	    "on_ms=$(cat $P/stat_panel_on_ms) cycles=$(cat $P/stat_panel_cycles)" \
	    "rst=$(busybox devmem 0x14000150 32) $(gpios)"
}
do_write() {	# $1 = off|on
	log "write $1"
	echo "$1" > $P/panel || { log "write $1 FAILED"; show; return 1; }
	show
}
case "$1" in
state) show ;;
off|on) do_write "$1" ;;
cycle)	T=${2:-10}; N=${3:-1}; i=1
	show
	while [ $i -le $N ]; do
		log "--- cycle $i/$N"
		do_write off || exit 1
		sleep $T
		do_write on || exit 1
		[ "$(cat $P/panel)" = on ] || { log "panel not on - stop"; exit 1; }
		[ $i -lt $N ] && sleep 5
		i=$((i + 1))
	done ;;
*) sed -n '2,10p' "$0"; exit 1 ;;
esac
