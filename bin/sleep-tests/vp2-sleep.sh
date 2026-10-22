#!/bin/sh
# Suspend A/B/A for VPROC2 settings with the rail kept ON (no rail-off hang risk):
# three s2.sh suspends of MAX s; B runs with da9214-test.ko loaded with ARGS_B
# (e.g. "vproc2_mv=600 phases=1"), unloaded (= restored) after.
# Run: sudo -n -b busybox sh /home/andrew/connsys/vp2-sleep.sh NAME MAX "ARGS_B"
N=$1; MAX=$2; AB=$3
D=/home/andrew/connsys; O=$D/vp2s-$N.log; K=/home/andrew/da9214-test.ko
: > $O
for r in A1 B A2; do
	if [ $r = B ]; then insmod $K $AB || { echo "insmod failed" >> $O; break; }; fi
	sh $D/s2.sh $MAX 2000 200 "" $D/vp2s-$N-$r.log
	[ $r = B ] && rmmod da9214-test
	echo "$r args='$([ $r = B ] && echo $AB)': $(grep -o "back rc=.*" $D/vp2s-$N-$r.log)" >> $O; sync
	grep -q "back rc=0" $D/vp2s-$N-$r.log || { echo "stopped after $r" >> $O; break; }
	sleep 20
done
echo "=== done" >> $O; sync
