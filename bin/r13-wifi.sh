#!/bin/sh
# read-only: sample PCM_REG13 with Wi-Fi on, off, on again
L=/home/andrew/r13-wifi.txt; : > $L
s() { echo "$1: $(for i in $(seq 30); do busybox devmem 0x10006134 32; sleep 0.1; done | sort | uniq -c | tr -s " \n" " ")" >> $L; }
s wifi-on
nmcli radio wifi off; sleep 4; s wifi-off
nmcli radio wifi on; sleep 2; s wifi-back
echo done >> $L; sync
