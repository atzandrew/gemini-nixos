#!/bin/sh
# E22: protected CONN-off SPM sleep, 25 cycles, A1 GPU domains on / B all 6 GPU domains off (gpu.sh) / A2 on.
for r in A1:0 B:1 A2:0; do
	n=${r%%:*}; GPU=${r##*:} sh /home/andrew/connsys/e22-run.sh 25 0x5fc102ff /home/andrew/connsys/e22-$n.log
	grep -q "all done" /home/andrew/connsys/e22-$n.log || break
	sleep 20
done
