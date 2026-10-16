#!/bin/sh
# C1 (CONN off) / B (CONN on) / C2 (CONN off), stage 6 pdn path, run-B config of 2026-10-14
O=/home/andrew/connsys/spm-abc.log
: > $O
for r in C1:yes B:no C2:yes; do
	n=${r%%:*}; c=${r##*:}
	echo "=== $n connoff=$c $(date +%T) charger=$(cat /sys/class/power_supply/bq25890-charger-0/online)" >> $O
	sh /home/andrew/gauge-ab3.sh 25 55 off 0x1 0x5fc102ff yes 0x10aa007e "clk_init=1 settle=1 pdn=1 acinactm=0" $c >> $O 2>&1
	cp /home/andrew/spm-live.txt /home/andrew/connsys/spm-live-$n.txt
	sync
	sleep 20
done
echo "=== all done $(date +%T)" >> $O; sync
