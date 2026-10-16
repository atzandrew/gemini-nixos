#!/bin/sh
s() { for i in $(busybox seq $1); do echo "$2 $(busybox devmem 0x10006134 32)"; busybox usleep 100000; done; }
sleep 3
s 50 A
nmcli radio wifi off; echo "radio off rc=$?"
s 300 B
nmcli radio wifi on; echo "radio on rc=$?"
s 50 C
echo done
