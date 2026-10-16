#!/bin/sh
s() { for i in $(busybox seq $1); do echo "$2 $(busybox devmem 0x10006134 32)"; busybox usleep 100000; done; }
P=/sys/module/mtk_wcn/parameters
sleep 3
echo "hb=$(cat $P/btif_wak_hb_ms) allow=$(cat $P/wmt_psm_allow)"
s 100 A
echo "7 0 0" > /proc/driver/wmt_dbg; echo "bt func off rc=$?"
s 150 A2
echo "0 1e" > /proc/driver/wmt_dbg; echo "psm on rc=$?"
s 300 B0
/usr/sbin/iw dev wlan0 set power_save on; echo "ps on rc=$?"
s 400 B1
/usr/sbin/iw dev wlan0 set power_save off; echo "ps off rc=$?"
nmcli radio wifi off; echo "radio off rc=$?"
s 300 B2
nmcli radio wifi on; echo "radio on rc=$?"
echo "0 0" > /proc/driver/wmt_dbg; echo "psm off rc=$?"
s 100 C
echo done
