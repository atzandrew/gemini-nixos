#!/bin/sh
# E28: genpd-mfg kernel, protected CONN-off SPM sleep 25 cyc: A1 plain / B gemini-sleep-gpio off (amps+USB1) / A2 plain
for r in A1:0 B:1 A2:0; do
	n=${r%%:*}; HOOK=${r##*:} sh /home/andrew/connsys/e28-run.sh 25 0x5fc102ff /home/andrew/connsys/e28-$n.log
	grep -q "all done" /home/andrew/connsys/e28-$n.log || break
	sleep 20
done
