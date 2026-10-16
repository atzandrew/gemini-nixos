#!/bin/sh
# E10: CONN off/on, restore MCU_CFG_ACR bits 24/25 (diagnostic), then SPM run B (CONN on)
D=/home/andrew/connsys; P=/sys/module/mtk_wcn/parameters; O=$D/e10.log
log() { echo "$(date +%T) $*" >> $O; sync; }
: > $O
log "start charger=$(cat /sys/class/power_supply/bq25890-charger-0/online) ACR=$(busybox devmem 0x18070110 32)"
bluetoothctl power off >/dev/null 2>&1; sleep 2
echo 1 > $P/conn_pwr_off; nmcli radio wifi off
echo "7 0 0" > /proc/driver/wmt_dbg; sleep 1; echo "7 3 0" > /proc/driver/wmt_dbg; sleep 5
log "off PWR_CON=$(busybox devmem 0x1000632c 32)"
echo "7 3 1" > /proc/driver/wmt_dbg; echo 0 > $P/conn_pwr_off
nmcli radio wifi on; bluetoothctl power on >/dev/null 2>&1; sleep 15
a=$(busybox devmem 0x18070110 32); log "after re-power ACR=$a"
busybox devmem 0x18070110 32 $(printf 0x%08x $((a | 0x03000000)))
log "ACR written -> $(busybox devmem 0x18070110 32)"
sleep 5
sh /home/andrew/gauge-ab3.sh 25 55 off 0x1 0x5fc102ff yes 0x10aa007e "clk_init=1 settle=1 pdn=1 acinactm=0" no >> $O 2>&1
cp /home/andrew/spm-live.txt $D/spm-live-E10.txt
log "ACR at end=$(busybox devmem 0x18070110 32) resync=$(dmesg | grep -c RESYNC2) ackmiss=$(dmesg | grep -c "Ack Miss")"
log "all done"
