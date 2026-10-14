#!/bin/sh
# SPM step 3d-3: A/B/A gauge run with Wi-Fi off, per-cycle log synced to disk.
# usage (as root, from the Gemini's own keyboard, lid open):
#   systemd-inhibit --what=handle-lid-switch:sleep:idle sh /home/andrew/gauge-ab2.sh CYCLES [AWAKE_S] [WIFI on|off] [WAKE_UNMASK] [FLAGS] [BLANK yes|no]
CYC=${1:-25}
AW=${2:-55}
WIFI=${3:-off}
WAKE=${4:-0x1}
FLAGS=${5:-0xffc102ff}
BLANK=${6:-no}		# "yes": wait 20 s, then require OVL0 L0 off (screen blanked) or abort
P=/sys/class/power_supply/mt6351-battery
L=/home/andrew/spm-live.txt
log() { echo "$*" >> $L; sync $L; echo "$*"; }
pkill -f "dmesg -W" 2>/dev/null
: > $L
( dmesg -W | grep --line-buffered -E "spm-load: (cyc |stage 6)" | while read -r l; do echo "$l" >> $L; sync $L; done ) &
LOGPID=$!
# (earlier runs: a leftover dmesg -W doubled every line; killed at the end now)
m() {
	c0=$(cat $P/charge_counter); t0=$(date +%s.%N)
	"$@" >/dev/null 2>&1
	c1=$(cat $P/charge_counter); t1=$(date +%s.%N)
	log "$(echo "$1 $c0 $c1 $t0 $t1" | awk '{printf "%-7s %7.1f mA over %.1f s (%d uAh)", $1, ($2-$3)*3.6/($5-$4), $5-$4, $2-$3}')"
}
echo 1 > /sys/devices/virtual/workqueue/cpumask
for c in 9 8 7 6 5 4 3 2 1; do echo 0 > /sys/devices/system/cpu/cpu$c/online 2>/dev/null; done
if [ "$BLANK" = yes ]; then
	log "$(date +%T) waiting 20 s for the screen to blank (close the lid)"
	sleep 20
	[ "$(busybox devmem 0x1400b02c 32)" = "0x00000000" ] || { log "screen not blanked (L0 on) - abort"; pkill -f "dmesg -W"; exit 1; }
fi
[ "$WIFI" = off ] && nmcli radio wifi off
sleep 5
log "$(date +%T) $(cat $P/status) online=$(cat /sys/devices/system/cpu/online) cycles=$CYC awake=${AW}s wifi=$WIFI wake=$WAKE flags=$FLAGS blank=$BLANK SRC_CON=$(busybox devmem 0x1400b02c 32)"
[ "$AW" -gt 0 ] && m sleep $AW
m insmod /home/andrew/mt6797-spm-load.ko stage=6 io_en=0x81 acinactm=3 sleep_ms=2000 cycles=$CYC wake_unmask=$WAKE flags=$FLAGS
rmmod mt6797-spm-load
[ "$AW" -gt 0 ] && m sleep $AW
log "$(date +%T) done"
[ "$WIFI" = off ] && nmcli radio wifi on
sleep 1
kill $LOGPID 2>/dev/null; pkill -f "dmesg -W"
