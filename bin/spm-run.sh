#!/bin/sh
# One stage-6 SPM sleep run with the screen blanked (OVL0 L0 off), cores offlined,
# per-cycle lines synced to ~/spm-live.txt. Run as root (sudo -b ...), then close the lid.
#   spm-run.sh "<extra insmod args>"   e.g. "flags=0x7fc102ff sleep_ms=100 cycles=1 wake_unmask=0x21"
ARGS="$1"
L=/home/andrew/spm-live.txt
P=/sys/class/power_supply/mt6351-battery
log() { echo "$*" >> $L; sync $L; }
pkill -f "dmesg -W" 2>/dev/null
: > $L
( dmesg -W | grep --line-buffered -E "spm-load: (cyc |stage 6)" | while read -r l; do echo "$l" >> $L; sync $L; done ) &
echo 1 > /sys/devices/virtual/workqueue/cpumask
for c in 9 8 7 6 5 4 3 2 1; do echo 0 > /sys/devices/system/cpu/cpu$c/online 2>/dev/null; done
log "$(date +%T) waiting 20 s for the screen to blank (close the lid)"
sleep 20
SRC=$(busybox devmem 0x1400b02c 32)
log "$(date +%T) online=$(cat /sys/devices/system/cpu/online) SRC_CON=$SRC $(cat $P/status) args: $ARGS"
if [ "$SRC" != "0x00000000" ]; then log "screen not blanked (L0 still on) - not running"; pkill -f "dmesg -W"; exit 1; fi
c0=$(cat $P/charge_counter); t0=$(date +%s.%N)
insmod /home/andrew/mt6797-spm-load.ko stage=6 io_en=0x81 acinactm=3 $ARGS
c1=$(cat $P/charge_counter); t1=$(date +%s.%N)
rmmod mt6797-spm-load
log "$(echo "$c0 $c1 $t0 $t1" | awk '{printf "run %.1f mA over %.1f s", ($1-$2)*3.6/($4-$3), $4-$3}')"
log "$(date +%T) done"
sleep 1; pkill -f "dmesg -W"
