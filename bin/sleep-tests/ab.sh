#!/bin/sh
# A/B/A suspend power test: three protected s2.sh suspends of MAX s each (A, B, A), 20 s awake between.
# A and B differ in GAP (awake ms between 2 s SPM cycles) and/or EXTRA module args.
# Run: sudo -n -b busybox sh /home/andrew/connsys/ab.sh NAME MAX GAP_A "EXTRA_A" GAP_B "EXTRA_B"
# e.g. ab.sh gap 180 200 "" 50 ""      ab.sh touch 180 200 "" 200 "touch_rst=1"
N=$1; MAX=$2; GA=$3; EA=$4; GB=$5; EB=$6
D=/home/andrew/connsys; O=$D/ab-$N.log
: > $O
for r in A1 B A2; do
	case $r in B) g=$GB; e=$EB ;; *) g=$GA; e=$EA ;; esac
	sh $D/s2.sh $MAX 2000 $g "$e" $D/ab-$N-$r.log
	echo "$r gap=$g extra='$e': $(grep -o 'back rc=.*' $D/ab-$N-$r.log)" >> $O; sync
	grep -q "back rc=0" $D/ab-$N-$r.log || { echo "stopped after $r" >> $O; break; }
	sleep 20
done
echo "=== done" >> $O
