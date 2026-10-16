#!/bin/sh
# E12: CONN-off SPM A/B/A: 0x5fc102ff / 0x00030238 (stock 0x00030038 + bit 9) / 0x5fc102ff
O=/home/andrew/connsys/e12.log
: > $O
for r in A1:0x5fc102ff B:0x00030238 A2:0x5fc102ff; do
	n=${r%%:*}; f=${r##*:}
	echo "=== $n flags=$f $(date +%T) charger=$(cat /sys/class/power_supply/bq25890-charger-0/online)" >> $O
	sh /home/andrew/gauge-ab3.sh 25 55 off 0x1 $f yes 0x10aa007e "clk_init=1 settle=1 pdn=1 acinactm=0" yes >> $O 2>&1
	cp /home/andrew/spm-live.txt /home/andrew/connsys/spm-live-E12-$n.txt
	echo "$n dbg: $(grep -o "dbg [0-9a-f]*" /home/andrew/spm-live.txt | sort | uniq -c | tr "\n" " ")" >> $O
	sync; sleep 20
done
echo "resync=$(dmesg | grep -c RESYNC2) ackmiss=$(dmesg | grep -c "Ack Miss")" >> $O
echo "=== all done $(date +%T)" >> $O; sync
