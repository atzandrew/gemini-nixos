#!/bin/sh
# dsi-stream.sh - stop / restart DSI0 video streaming while the panel is off (2026-10-22).
# What: "stop" = stock ddp_dsi_stop for DSI0 (vendor 3.18 ddp_dsi.c): MODE_CTRL -> 0 (command
#       mode), wait INTSTA bit 31 (BUSY) clear, PHY_LCCON bit 0 (LC_HS_TX_EN) -> 0 (clock lane LP).
#       "start" = stock ddp_dsi_start + DSI_Start: MODE_CTRL -> 1 (sync-pulse video), LC_HS_TX_EN
#       -> 1, START bit 0 0->1 (bit 16 VM_CMD kept). RDMA/OVL/mutex are NOT touched (stock also
#       stops/resets them; if the picture comes back shifted that is the missing piece).
#       Both refuse unless geminipda-drm panel-pm says "off" and ENP/ENN read 0: DSI is never
#       stopped or started with the panel powered. The panel itself is switched with the
#       "panel" parameter (panel-cycle.sh), which needs DSI streaming (VM_CMD), so: off -> stop ...
#       start -> on.
# Why:  sleep power step 1 (notes/next-session-sleep-power-prompt.md): does stopping the video
#       stream in suspend save power? USER PRESENT (AGENTS.md rule 5): one OK per DSI write step.
# usage (root): dsi-stream.sh state | stop | start
#               dsi-stream.sh cycle [SECONDS]   awake test: panel off, stop, wait, start, panel on
B=0x1401c000; P=/sys/module/geminipda_drm/parameters
log() { echo "$(date +%T) $*"; }
rd() { busybox devmem $(($B + $1)) 32; }
wr() { busybox devmem $(($B + $1)) 32 $2; }
gp() { grep -E "\|$1 " /sys/kernel/debug/gpio | grep -oE ' (hi|lo) ' | tr -d ' '; }
show() {
	log "panel=$(cat $P/panel) enp=$(gp enp) enn=$(gp enn) rst=$(busybox devmem 0x14000150 32)" \
	    "DSI START=$(rd 0x00) INTSTA=$(rd 0x0c) MODE=$(rd 0x14) LCCON=$(rd 0x104) VM_CMD_CON=$(rd 0x130)"
}
safe() {	# panel off and unbiased?
	[ "$(cat $P/panel)" = off ] && [ "$(gp enp)" = lo ] && [ "$(gp enn)" = lo ] && return 0
	log "REFUSED: panel not off / bias not low"; show; return 1
}
stop() {
	safe || return 1
	[ "$(rd 0x14)" = 0x00000001 ] || { log "MODE not 1 (already stopped?)"; show; return 1; }
	wr 0x14 0
	i=0; while [ $(( $(rd 0x0c) & 0x80000000 )) -ne 0 ]; do
		i=$((i+1)); [ $i -gt 500 ] && { log "BUSY did not clear"; show; return 1; }; done
	l=$(rd 0x104); wr 0x104 $(( l & ~1 ))
	log "stopped (busy polls $i)"; show
}
start() {
	safe || return 1
	wr 0x14 1
	l=$(rd 0x104); wr 0x104 $(( l | 1 ))
	s=$(rd 0x00); wr 0x00 $(( s & ~1 )); wr 0x00 $(( s | 1 ))
	log "started"; show
}
case "$1" in
state) show ;;
stop) stop ;;
start) start ;;
cycle)	T=${2:-10}
	show
	echo off > $P/panel; log "panel off"; show
	stop || exit 1
	sleep $T
	start || exit 1
	echo on > $P/panel; log "panel on: $(cat $P/panel) err=$(cat $P/stat_panel_err) on_ms=$(cat $P/stat_panel_on_ms)"; show ;;
*) echo "usage: $0 state|stop|start|cycle [secs]"; exit 1 ;;
esac
