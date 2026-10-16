#!/bin/sh
# E13: CONN-off SPM A/B/A, flags 0x5fc102ff; B adds src2_mask=0x00f1001f (stock suspend SRC2_MASK)
O=/home/andrew/connsys/e13.log
: > $O
for r in A1:0xffffffff B:0x00f1001f A2:0xffffffff; do
	n=${r%%:*}; s2=${r##*:}
	echo "=== $n src2_mask=$s2 $(date +%T) charger=$(cat /sys/class/power_supply/bq25890-charger-0/online)" >> $O
	sh /home/andrew/gauge-ab3.sh 25 55 off 0x1 0x5fc102ff yes 0x10aa007e "clk_init=1 settle=1 pdn=1 acinactm=0 src2_mask=$s2" yes >> $O 2>&1
	cp /home/andrew/spm-live.txt /home/andrew/connsys/spm-live-E13-$n.txt
	echo "$n dbg: $(grep -o "dbg [0-9a-f]*" /home/andrew/spm-live.txt | sort | uniq -c | tr "\n" " ")" >> $O
	echo "$n $(grep -m1 SRC2_MASK /home/andrew/spm-live.txt | grep -o "SRC2_MASK.*")" >> $O
	sync; sleep 20
done
echo "resync=$(dmesg | grep -c RESYNC2) ackmiss=$(dmesg | grep -c "Ack Miss") sem=$(dmesg | grep -c "HW semaphore timeout")" >> $O
echo "=== all done $(date +%T)" >> $O; sync
