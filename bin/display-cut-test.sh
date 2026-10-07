#!/bin/sh
# display-cut-test.sh — measure what the "blanked" display pipeline costs, no build.
#
# A: desktop stopped, backlight off (= today's blanked state minus Plasma)
# B: scanout stopped (OVL0 + RDMA0 engines off -> no more 60 Hz frame reads from DRAM)
# C: DSI link stopped as well
# The screen stays dead afterwards: REBOOT when it's done.
#
# Run on battery, detached:
#   sudo sh -c 'nohup sh /home/atzero/display-cut-test.sh > /home/atzero/display-cut.txt 2>&1 &'
# Result: cat ~/display-cut.txt   (takes ~4 min)

PS=/sys/class/power_supply/mt6351-battery
SECS=${SECS:-60}
OVL0=0x1400b000; RDMA0=0x1400f000; DSI0=0x1401c000

rd() { busybox devmem "$1" 32; }
wr() { busybox devmem "$1" 32 "$2"; }
avg() { s=0; n=0; while [ $n -lt "$1" ]; do v=$(cat $PS/current_now); s=$((s + v)); n=$((n + 1)); sleep 1; done; echo $((s / n / 1000)); }
regs() { echo "  OVL0_EN=$(rd $((OVL0 + 0x0c))) RDMA0_GLOBAL_CON=$(rd $((RDMA0 + 0x10))) DSI_START=$(rd $((DSI0 + 0x00))) DSI_MODE_CTRL=$(rd $((DSI0 + 0x14)))"; }

[ -r $PS/current_now ] || { echo "no gauge at $PS"; exit 1; }
[ "$(cat $PS/status)" = "Discharging" ] || echo "WARNING: status=$(cat $PS/status) (unplug USB for a valid number)"
echo "before:"; regs

echo "stopping greetd, backlight off"
systemctl stop greetd
for b in /sys/class/backlight/*; do echo 4 > "$b/bl_power" 2>/dev/null; echo 0 > "$b/brightness" 2>/dev/null; done
sleep 20

A=$(avg "$SECS"); echo "A desktop stopped, backlight off : $A mA"

wr $((OVL0 + 0x0c)) 0                                     # OVL_EN = 0
wr $((RDMA0 + 0x10)) $(( $(rd $((RDMA0 + 0x10))) & ~1 ))  # RDMA ENGINE_EN = 0
echo "scanout stopped:"; regs
sleep 10
B=$(avg "$SECS"); echo "B + OVL0/RDMA0 off              : $B mA"

wr $((DSI0 + 0x14)) 0                                     # DSI_MODE_CTRL = command mode
wr $((DSI0 + 0x00)) 0                                     # DSI_START = 0
echo "DSI stopped:"; regs
sleep 10
C=$(avg "$SECS"); echo "C + DSI stopped                 : $C mA"

echo "scanout (DRAM reads) cost ≈ $((A - B)) mA, DSI link ≈ $((B - C)) mA (more negative = more drain)"
echo "done — reboot now (sudo systemctl reboot)"
