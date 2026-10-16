#!/bin/sh
s() { for i in $(busybox seq $1); do echo "$2 $(busybox devmem 0x10006134 32)"; busybox usleep 100000; done; }
P=/sys/module/mtk_wcn/parameters
sleep 3
echo "hb=$(cat $P/btif_wak_hb_ms) allow=$(cat $P/wmt_psm_allow)"
s 100 A
echo "0 1e" > /proc/driver/wmt_dbg; echo "psm on rc=$?"
s 400 B
echo "0 0" > /proc/driver/wmt_dbg; echo "psm off rc=$?"
s 100 C
echo done
