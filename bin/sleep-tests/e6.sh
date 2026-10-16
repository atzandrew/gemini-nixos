#!/bin/sh
s() { for i in $(busybox seq $1); do echo "$2 $(busybox devmem 0x10006134 32)"; busybox usleep 100000; done; }
fast() { i=0; while [ $i -lt $1 ]; do echo "F $(busybox devmem 0x10006134 32) $(busybox devmem 0x1000632c 32)"; i=$((i+1)); done; }
sleep 3
s 30 A
nmcli radio wifi off; echo "radio off rc=$?"
echo "7 3 0" > /proc/driver/wmt_dbg; echo "wifi func off rc=$?"
s 50 B
fast 1500 > /home/andrew/connsys/e6-fast.log &
FP=$!
busybox usleep 200000
echo "7 3 1" > /proc/driver/wmt_dbg; echo "wifi func on rc=$?"
wait $FP
nmcli radio wifi on; echo "radio on rc=$?"
s 50 C
echo done
