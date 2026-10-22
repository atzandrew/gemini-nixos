#!/bin/sh
# pwr-series.sh - one A/B power series of s2.sh sleeps, one setting per run (2026-10-22).
# What: runs s2.sh (installed mt6797-suspend, pcm_wdt=30, bias_check) once per run spec and logs one
#       summary line each to ~/connsys/series.log. Specs (in order, as arguments):
#         A        baseline, nothing extra
#         L        VA10 HW low-power on SRCLKEN0 (module ldo_lp=2 ldo_lp_mode=2; stock SUSPEND setting)
#         V<hex>   Vcore HW sleep voltage VOSEL_SLEEP=<hex> (0x28 = 0.85 V; 0x30 = 0.9 V = today's awake
#                  value), E19 step 1b order through pmw.sh: VSLEEP_SEL 0x44a 3->0, VOSEL_ON 0x60a=0x30,
#                  0x60c, VOSEL_CTRL HW 0x600=2, VSLEEP_EN 0x612=0x101. Restored after the run (SW mode,
#                  LK values). 0.8 V (0x20) hung on 2026-10-16.
#         U        AUDIO domain test: unbind the sound card (mt6797-mt6351) and the AFE (mt6797-audio; its
#                  probe holds a runtime-PM ref for good, so the domain never goes off), rebind after.
#       Stops at the first run without "=== done" or with rc != 0. A hang = PCM WDT reset (~30 s).
# Why:  sleep power after the DSI test (docs/spm-sleep.md); retests 2026-10-16/17 results that ran with
#       the random hang still present. Never touches the panel or DSI (kernel panel_suspend does).
# Run:  sudo -n -b busybox sh /home/andrew/connsys/pwr-series.sh [SLEEP_S] SPEC...   (USB unplugged)
S=${1:-300}; shift
D=/home/andrew/connsys; O=$D/series.log
MOD=/lib/modules/$(uname -r)/extra/mt6797-suspend.ko
log() { echo "$(date +%T) $*" >> $O; sync; }
vc() { sh $D/vcore.sh; }
vset() {	# $1 = VOSEL_SLEEP
	sh $D/pmw.sh 0x44a 0xf821; sh $D/pmw.sh 0x60a 0x30; sh $D/pmw.sh 0x60c $1
	sh $D/pmw.sh 0x600 0x0002; sh $D/pmw.sh 0x612 0x0101
	log "vcore set: $(vc) 044a=$(grep ^044a: /sys/kernel/debug/regmap/1000d000.pwrap/registers)"
}
SND=/sys/bus/platform/drivers; PST() { busybox devmem 0x10006180 32; }
aud() { echo "genpd audio $(cat /sys/kernel/debug/pm_genpd/audio/current_state) PWR_STATUS $(PST)"; }
uoff() { echo sound > $SND/mt6797-mt6351/unbind; echo 11220000.audio-controller > $SND/mt6797-audio/unbind; sleep 1; log "audio unbound: $(aud)"; }
uon() { echo 11220000.audio-controller > $SND/mt6797-audio/bind; echo sound > $SND/mt6797-mt6351/bind; sleep 1; log "audio rebound: $(aud) cards: $(grep -c mt6797 /proc/asound/cards)"; }
vrestore() {
	sh $D/pmw.sh 0x612 0x0001; sh $D/pmw.sh 0x600 0x0000
	sh $D/pmw.sh 0x60a 0x40; sh $D/pmw.sh 0x60c 0x40; sh $D/pmw.sh 0x44a 0xfb21
	log "vcore restored: $(vc) 044a=$(grep ^044a: /sys/kernel/debug/regmap/1000d000.pwrap/registers)"
}
log "=== series start: ${S}s, runs: $*; vcore $(vc); bat $(cat /sys/class/power_supply/mt6351-battery/capacity)%"
n=0
for r in "$@"; do
	n=$((n+1)); ex="pcm_wdt=30"; L=$D/series-$n-$r.log
	case "$r" in
	A) ;;
	L) ex="$ex ldo_lp=2 ldo_lp_mode=2" ;;
	V0x*) vset ${r#V} ;;
	U) uoff ;;
	*) log "unknown spec $r - stop"; break ;;
	esac
	log "run $n $r: start (extra '$ex')"
	MOD=$MOD sh $D/s2.sh $S 2000 200 "$ex" $L
	case "$r" in V0x*) vrestore ;; U) uon ;; esac
	log "run $n $r: $(grep -o 'back rc=.*' $L) | $(grep -o 'woke: [^;]*' $D/s2-dmesg.log | head -1) | panel $(cat /sys/module/geminipda_drm/parameters/panel)"
	grep -q "=== done" $L && grep -q "back rc=0" $L || { log "run $n $r did not finish cleanly - stop"; break; }
	[ "$(cat /sys/class/power_supply/mt6351-battery/capacity)" -lt 40 ] && { log "battery < 40 % - stop"; break; }
	sleep 20
done
log "=== series end"
