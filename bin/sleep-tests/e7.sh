#!/bin/sh
s() { for i in $(busybox seq $1); do echo "$2 $(busybox devmem 0x10006134 32) $(busybox devmem 0x1000632c 32)"; busybox usleep 100000; done; }
P=/sys/module/mtk_wcn/parameters
sleep 3
echo "conn_pwr_off=$(cat $P/conn_pwr_off)"
s 30 A
nmcli radio wifi off; echo "radio off rc=$?"
echo "7 0 0" > /proc/driver/wmt_dbg; echo "bt func off rc=$?"
s 20 A2
echo "7 3 0" > /proc/driver/wmt_dbg; echo "wifi func off rc=$?"
s 300 B
echo "7 3 1" > /proc/driver/wmt_dbg; echo "wifi func on rc=$?"
s 30 C1
nmcli radio wifi on; echo "radio on rc=$?"
s 100 C
echo done
