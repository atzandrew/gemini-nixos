#!/bin/sh
# Hang hunt: RUNS protected s2.sh suspends of MAX s in a row, with the TOPRGU WDT kept
# running in the sleep loop (wdt_pause=0) and SoC-only reset (wdt_int_rst=1), so a hang
# becomes a WDT reset (~1 min) instead of a power-hold. mt6797-suspend writes a breadcrumb
# to NONRST_REG2 (0x10007024) at every step; after a reset read it with
#   sudo busybox devmem 0x10007024 32      (or insmod ~/mt6797-suspend.ko prints it).
# BARGS (optional): every even run loads ~/da9214-test.ko with BARGS (e.g. "vproc2_mv=600
# phases=1") and unloads it after (= the A/B pattern of the 21:12 hang).
# BARGS "noi2c=1" = load/unload only, no DA9214 I2C (module-vs-I2C split).
# BPOST (optional): shell command run after every even run, e.g. one raw I2C read of the
# volatile DA9214 STATUS_A: "busybox i2cget -f -y 2 0x68 0x50" (I2C with no module).
# KEEP=1 (optional): load da9214-test once before run 2 and keep it loaded until the end
# (presence vs unload as the trigger).
# MOD (optional): module to load instead of ~/da9214-test.ko, e.g. a stock module with no
# hardware access (/lib/modules/$(uname -r)/kernel/lib/crc-ccitt.ko); BARGS may then be "".
# BEX (env, optional): extra mt6797-suspend args for even runs only (A/B of a module option),
# e.g. BEX="vproc2_sleep_mv=300".
# The panel stays display-off (DCS 0x28) + DPMS off for the whole series; the last run (or
# an early stop) turns it back on (s2.sh PANEL_KEEP_OFF / panel-on).
# Run: sudo -n -b busybox sh /home/andrew/connsys/hunt.sh NAME RUNS MAX ["EXTRA"] ["BARGS"] ["BPOST"] [KEEP] [MOD]
N=$1; RUNS=$2; MAX=$3; EX="wdt_pause=0 wdt_int_rst=1 $4"; BA=$5; BP=$6; KEEP=${7:-0}
MOD=${8:-/home/andrew/da9214-test.ko}; MN=$(basename $MOD .ko); B=0; { [ -n "$BA" ] || [ -n "$8" ]; } && B=1
D=/home/andrew/connsys; O=$D/hunt-$N.log
: > $O; PO=0
echo "$(date +%T) start: runs=$RUNS max=$MAX extra='$EX' bargs='$BA' bpost='$BP' bex='$BEX' keep=$KEEP mod=$MOD crumb=$(busybox devmem 0x10007024 32)" >> $O; sync
i=1
while [ $i -le $RUNS ]; do
	b=""; if [ $B = 1 ] && { [ "$KEEP" = 1 ] && [ $i -eq 2 ] || [ "$KEEP" != 1 ] && [ $((i % 2)) -eq 0 ]; }; then b="B($MN $BA) "
		insmod $MOD $BA || { echo "$MN insmod failed" >> $O; break; }; fi
	[ "$KEEP" = 1 ] && [ $i -gt 2 ] && b="K "
	e="$EX"; [ -n "$BEX" ] && [ $((i % 2)) -eq 0 ] && { e="$EX $BEX"; b="${b}X($BEX) "; }
	PO=1; [ $i -eq $RUNS ] && PO=0	# panel stays off between runs; only the last run turns it on
	PANEL_KEEP_OFF=$PO sh $D/s2.sh $MAX 2000 200 "$e" $D/hunt-$N-$i.log
	[ -n "$b" ] && [ "$KEEP" != 1 ] && rmmod $MN
	if [ -n "$BP" ] && [ $((i % 2)) -eq 0 ]; then
		b="${b}P($(sh -c "$BP" 2>&1 | tr '\n' ' ')) "; fi
	echo "$(date +%T) run $i: $b$(grep -o "back rc=.*" $D/hunt-$N-$i.log) $(grep -o "text=[0-9a-fx]*" $D/hunt-$N-$i.log) crumb=$(busybox devmem 0x10007024 32)" >> $O; sync
	grep -q "back rc=0" $D/hunt-$N-$i.log || { echo "stopped after run $i" >> $O; break; }
	i=$((i+1))
	sleep 20
done
[ "$KEEP" = 1 ] && rmmod $MN 2>/dev/null
[ "$PO" = 1 ] && { sh $D/s2.sh panel-on "" "" "" $D/hunt-$N-panel.log; echo "$(date +%T) panel on (early stop)" >> $O; }
echo "=== done" >> $O; sync
