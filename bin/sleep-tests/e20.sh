#!/bin/sh
# E20: Vcore HW sleep voltage A/B/A (Vcore in HW mode: 0x44a VSLEEP_SEL=0, 0x600=2, 0x612=0x101, VOSEL_ON 0x30).
# VOSEL_SLEEP (0x60c): A1 0x30 (0.9 V) / B 0x20 (0.8 V) / A2 0x30. Protected SPM (DCS 0x28/0x29), CONN off, 25 cycles.
for r in A1:0x30 B:0x20 A2:0x30; do
	n=${r%%:*}; v=${r##*:}
	sh /home/andrew/connsys/pmw.sh 0x60c $v
	sh /home/andrew/connsys/e20-run.sh 25 0x5fc102ff /home/andrew/connsys/e20-$n.log
	echo "vcore after $n: $(sh /home/andrew/connsys/vcore.sh)" >> /home/andrew/connsys/e20-$n.log
	grep -q "all done" /home/andrew/connsys/e20-$n.log || break
	sleep 20
done
sh /home/andrew/connsys/pmw.sh 0x60c 0x30
