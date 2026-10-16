#!/bin/sh
# E25: genpd-mfg kernel, protected CONN-off SPM sleep 25 cyc: A1 auto (GPU domains genpd-off) / B mali control=on (domains on) / A2 auto
M=/sys/bus/platform/devices/13040000.mali/power/control
for r in A1:auto B:on A2:auto; do
	n=${r%%:*}; echo ${r##*:} > $M; sleep 2
	echo "$(date +%T) control=$(cat $M) sta=$(busybox devmem 0x10006180 32)" > /home/andrew/connsys/e25-$n.pre
	sh /home/andrew/connsys/e20-run.sh 25 0x5fc102ff /home/andrew/connsys/e25-$n.log
	echo auto > $M
	grep -q "all done" /home/andrew/connsys/e25-$n.log || break
	sleep 20
done
