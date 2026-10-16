#!/bin/sh
# E16 = E14 redo with the panel protected (DCS display-off): CONN-off SPM A1/B/A2, 25 cycles each,
# flags 0x5fc102ff / 0x5fc102f7 (bit 3 DIS_VCORE_DVS cleared; pwrap table = HWCID no-op) / 0x5fc102ff. Look for dbg bits 8/9.
for r in A1:0x5fc102ff B:0x5fc102f7 A2:0x5fc102ff; do
	n=${r%%:*}; f=${r##*:}
	sh /home/andrew/connsys/e16-run.sh 25 $f /home/andrew/connsys/e16-$n.log
	grep -q "all done" /home/andrew/connsys/e16-$n.log || break
	sleep 20
done
