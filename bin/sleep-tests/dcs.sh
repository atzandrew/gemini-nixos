#!/bin/sh
# Send MIPI DCS short commands to the SSD2092 panel while DSI0 stays in video mode,
# exactly like vendor ddp_dsi.c DSI_set_cmdq_V2 (VDO branch) + DSI_EnableVM_CMD:
# VM_CMD_CON = CM_DATA_1<<24 | cmd<<16 | 0x05 (DCS short, no param)<<8 | VM_CMD_EN|TS_VFP_EN,
# then toggle DSI_START.VM_CMD_START (bit 16) 0 -> 1; the packet goes out in the vertical front porch.
# Poll DSI_INTSTA.VM_CMD_DONE (bit 5, raw flag; INTEN untouched), then clear it (RMW, bit 5 = 0).
# usage (root): dcs.sh off-on [SECONDS]   0x28 display off, wait, 0x29 display on
#               dcs.sh sleep-wake [SECONDS] 0x28, 50 ms, 0x10 sleep in, 120 ms ... 0x11 sleep out, 120 ms, 0x29
B=0x1401c000
rd() { busybox devmem $(($B + $1)) 32; }
wr() { busybox devmem $(($B + $1)) 32 $2; }
log() { echo "$(date +%T.%N | cut -c1-12) $*"; }
send() {	# $1 = DCS command (hex)
	st=$(rd 0x0c); [ $(( st & 0x20 )) -ne 0 ] && wr 0x0c $(( st & ~0x20 ))
	wr 0x130 $(( ($1 << 16) | (0x05 << 8) | 0x21 ))
	s=$(rd 0x00); wr 0x00 $(( s & ~0x10000 )); wr 0x00 $(( s | 0x10000 ))
	i=0; while [ $(( $(rd 0x0c) & 0x20 )) -eq 0 ]; do i=$((i+1)); [ $i -gt 200 ] && { log "cmd $1: NO VM_CMD_DONE (INTSTA $(rd 0x0c))"; return 1; }; done
	st=$(rd 0x0c); wr 0x0c $(( st & ~0x20 ))
	log "cmd $1 done after $i polls; VM_CMD_CON $(rd 0x130) START $(rd 0x00) INTSTA $(rd 0x0c)"
}
ms() { busybox usleep $(( $1 * 1000 )); }
# guards: DSI0 running, sync-pulse video mode, VM_CMD_EN + TS_VFP_EN as LK left them
[ "$(rd 0x14)" = 0x00000001 ] || { log "MODE_CTRL $(rd 0x14) != 1 - abort"; exit 1; }
[ $(( $(rd 0x00) & 1 )) -eq 1 ] || { log "DSI_START $(rd 0x00) not running - abort"; exit 1; }
[ $(( $(rd 0x130) & 0xff )) -eq $((0x21)) ] || { log "VM_CMD_CON $(rd 0x130) low byte != 0x21 - abort"; exit 1; }
T=${2:-5}
log "start: START $(rd 0x00) MODE $(rd 0x14) VM_CMD_CON $(rd 0x130) INTSTA $(rd 0x0c)"
case "$1" in
off-on)	send 0x28; log "display off, holding ${T} s"; sleep $T; send 0x29 ;;
sleep-wake) send 0x28; ms 50; send 0x10; ms 120; log "sleep-in, holding ${T} s"; sleep $T
	send 0x11; ms 120; send 0x29 ;;
*) echo "usage: $0 off-on|sleep-wake [SECONDS]"; exit 1 ;;
esac
log "end: VM_CMD_CON $(rd 0x130) INTSTA $(rd 0x0c)"
