#!/bin/sh
# panel-sleep.sh - short real suspends with the panel powered off by the kernel (2026-10-21).
# What: sets geminipda-drm panel_suspend=1 (stock panel off in device suspend, full re-init in
#       resume), then runs s2.sh RUNS times (SPM sleep of MAX s, test module ~/mt6797-suspend.ko
#       with bias_check: it refuses to sleep if GPIO60/251 are still high). After each run it logs
#       the bias read right before sleep, the driver's panel off/on lines and errors. Stops at the
#       first run where the bias was not ENP 0 ENN 0 or the panel did not come back "on".
#       panel_suspend goes back to 0 at the end.
# Why:  step 3 of the panel-off plan (docs/display.md "Panel off/on"); first sleeps since burn #6.
#       Only with the user present. USB must be unplugged (s2.sh waits up to 300 s for it).
# Run:  sudo -n -b busybox sh /home/andrew/panel/panel-sleep.sh [RUNS] [MAX] ["EXTRA"]
#       defaults 10 runs, 30 s, EXTRA "pcm_wdt=30" (stuck sleep -> SoC reset after ~30 s).
#       Log: /home/andrew/panel/panel-sleep.log (+ s2-N.log per run).
RUNS=${1:-10}; MAX=${2:-30}; EX=${3:-pcm_wdt=30}
P=/sys/module/geminipda_drm/parameters; D=/home/andrew/panel; O=$D/panel-sleep.log
DM=/home/andrew/connsys/s2-dmesg.log
log() { echo "$(date +%T) $*" >> $O; sync; }
: > $O
[ "$(cat $P/panel)" = on ] || { log "panel not on ($(cat $P/panel)) - abort"; exit 1; }
echo 1 > $P/panel_suspend
log "start: runs=$RUNS max=$MAX extra='$EX' panel_suspend=$(cat $P/panel_suspend) cycles=$(cat $P/stat_panel_cycles)"
i=1
while [ $i -le $RUNS ]; do
	c0=$(cat $P/stat_panel_cycles)
	: > $DM
	sh /home/andrew/connsys/s2.sh $MAX 2000 200 "$EX" $D/s2-$i.log
	b=$(grep -o "LCD bias before sleep: ENP . ENN ." $DM | tail -1)
	pd=$(grep -c "geminipda-drm: panel off" $DM)
	po=$(grep -o "geminipda-drm: panel on.*" $DM | tail -1)
	log "run $i: [$b] off-lines=$pd [$po] state=$(cat $P/panel) err=$(cat $P/stat_panel_err)" \
	    "cycles $c0->$(cat $P/stat_panel_cycles) | $(grep -E 'back rc|last_wake' $D/s2-$i.log | tr '\n' ' ' | cut -c1-160)"
	[ "$(cat $P/panel)" = on ] || { log "panel not on after run $i - stop"; break; }
	echo "$b" | grep -q "ENP 0 ENN 0" || { log "bias not confirmed off in run $i - stop"; break; }
	i=$((i + 1)); sleep 5
done
echo 0 > $P/panel_suspend
log "=== done; panel_suspend=$(cat $P/panel_suspend)"
