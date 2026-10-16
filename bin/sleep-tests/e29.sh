#!/bin/sh
# E29 amps only: genpd-mfg kernel, protected CONN-off SPM sleep 25 cyc: B amps off (GPIO 243/244) / A plain
for r in B:1 A:0; do
	n=${r%%:*}; HOOK=${r##*:} sh /home/andrew/connsys/e29-run.sh 25 0x5fc102ff /home/andrew/connsys/e29-$n.log
	grep -q "all done" /home/andrew/connsys/e29-$n.log || break
	sleep 20
done
