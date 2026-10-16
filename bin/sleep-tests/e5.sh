#!/bin/sh
s() { for i in $(busybox seq $1); do echo "$2 $(busybox devmem 0x10006134 32)"; busybox usleep 100000; done; }
sleep 3
s 50 A
nmcli radio wifi off; echo "radio off rc=$?"
s 50 A2
echo "7 3 0" > /proc/driver/wmt_dbg; echo "wifi func off rc=$?"
s 300 B
echo "7 3 1" > /proc/driver/wmt_dbg; echo "wifi func on rc=$?"
s 50 C1
nmcli radio wifi on; echo "radio on rc=$?"
s 100 C
echo done
