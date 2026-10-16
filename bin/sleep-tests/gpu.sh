#!/bin/sh
# gpu.sh off|on|st — all 6 MT6797 GPU domains via stock clk-mt6797-pg.c sequence (see e21.sh). Prints status; exit 1 on timeout.
S=0x10006000
rd() { busybox devmem $(($S + $1)) 32; }
wr() { busybox devmem $(($S + $1)) 32 $(( $2 )); }
sb() { wr $1 $(( $(rd $1) | $2 )); }
cb() { wr $1 $(( $(rd $1) & ~$2 )); }
wt() { i=0; while :; do v=$(( $(rd $1) & $2 )); if [ $3 = 1 ]; then [ $v -eq $(( $2 )) ] && return 0; else [ $v -eq 0 ] && return 0; fi
	i=$((i+1)); [ $i -gt 500 ] && { echo "TIMEOUT $1 $2 $3 v=$v"; exit 1; }; done; }
wst() { wt 0x180 $1 $2 && wt 0x184 $1 $2; }
core_off() { c=$((0x340 + 4*$1)); sb $c 0x100; wt 0x33c $((1<<(20+$1))) 1; sb $c 0x10; sb $c 0x2; cb $c 0x1; cb $c 0x4; cb $c 0x8; wst $((1<<(11-$1))) 0; }
core_on()  { c=$((0x340 + 4*$1)); sb $c 0x4; sb $c 0x8; wst $((1<<(11-$1))) 1; sb $c 0x1; cb $c 0x2; cb $c 0x10; cb $c 0x100; wt 0x33c $((1<<(20+$1))) 0; }
case $1 in
off)	[ "$(cat /sys/bus/platform/devices/13040000.mali/power/runtime_status)" = suspended ] || { echo "mali active"; exit 1; }
	wr 0 $(( (0xb16<<16) | 1 )); for n in 3 2 1 0; do core_off $n; done
	sb 0x33c 0x3; wt 0x33c 0x30000 1; sb 0x338 0x2; sb 0x338 0x10; cb 0x338 0x1; cb 0x338 0x4; cb 0x338 0x8; wst 0x1000 0
	sb 0x334 0x2; sb 0x334 0x10; cb 0x334 0x1; cb 0x334 0x4; cb 0x334 0x8; wst 0x2000 0 ;;
on)	wr 0 $(( (0xb16<<16) | 1 ))
	sb 0x334 0x4; sb 0x334 0x8; wst 0x2000 1; cb 0x334 0x10; cb 0x334 0x2; sb 0x334 0x1
	sb 0x338 0x4; sb 0x338 0x8; wst 0x1000 1; cb 0x338 0x10; cb 0x338 0x2; sb 0x338 0x1; cb 0x33c 0x3; wt 0x33c 0x30000 0
	for n in 0 1 2 3; do core_on $n; done ;;
esac
echo "gpu $1: PWR_STATUS=$(rd 0x180) SRAM=$(rd 0x33c)"
