#!/bin/sh
# Hang hunt: RUNS protected s2.sh suspends of MAX s in a row, with the TOPRGU WDT kept
# running in the sleep loop (wdt_pause=0) and SoC-only reset (wdt_int_rst=1), so a hang
# becomes a WDT reset (~1 min) instead of a power-hold. mt6797-suspend writes a breadcrumb
# to NONRST_REG2 (0x10007024) at every step; after a reset read it with
#   sudo busybox devmem 0x10007024 32      (or insmod ~/mt6797-suspend.ko prints it).
# Run: sudo -n -b busybox sh /home/andrew/connsys/hunt.sh NAME RUNS MAX ["EXTRA"]
N=$1; RUNS=$2; MAX=$3; EX="wdt_pause=0 wdt_int_rst=1 $4"
D=/home/andrew/connsys; O=$D/hunt-$N.log
: > $O
echo "$(date +%T) start: runs=$RUNS max=$MAX extra='$EX' crumb=$(busybox devmem 0x10007024 32)" >> $O; sync
i=1
while [ $i -le $RUNS ]; do
	sh $D/s2.sh $MAX 2000 200 "$EX" $D/hunt-$N-$i.log
	echo "$(date +%T) run $i: $(grep -o 'back rc=.*' $D/hunt-$N-$i.log) crumb=$(busybox devmem 0x10007024 32)" >> $O; sync
	grep -q "back rc=0" $D/hunt-$N-$i.log || { echo "stopped after run $i" >> $O; break; }
	i=$((i+1))
	sleep 20
done
echo "=== done" >> $O; sync
