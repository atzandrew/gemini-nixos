#!/bin/sh
# gemini-thermal-check.sh — SoC thermal sensor checks, run ON the Gemini.
#
#   sh gemini-thermal-check.sh pre    decode the thermal calibration that LK
#                                     passes in /chosen/atag,devinfo (works on
#                                     any kernel; run before flashing)
#   sh gemini-thermal-check.sh post   after booting the thermal kernel: driver
#                                     messages, zone, trips, cooling devices
#   sh gemini-thermal-check.sh load [SECONDS]
#                                     log the zone temperature every 2 s while
#                                     all online CPUs spin, then 60 s of cooldown
#
# docs/thermal.md. Calibration = efuse 0x10206180/84/88 = devinfo index 31/32/33
# (vendor mtk_tc.c ADDRESS_INDEX_1/0/2), checked with the vendor range rules.

P=/proc/device-tree/chosen/atag,devinfo

zone() {
	for z in /sys/class/thermal/thermal_zone*; do
		[ "$(cat "$z/type" 2>/dev/null)" = soc-thermal ] && { echo "$z"; return 0; }
	done
	return 1
}

case "${1:-pre}" in
pre)
	[ -r "$P" ] || { echo "no $P - LK did not pass devinfo (thermal driver will refuse to probe)"; exit 1; }
	od -A n -t u4 -v "$P" | tr -s ' ' '\n' | sed '/^$/d' | awk '
	{ w[NR-1] = $1 }
	function bits(x, hi, lo) { return int(x / 2^lo) % 2^(hi - lo + 1) }
	function hex(x) { return sprintf("%04x%04x", int(x / 65536), x % 65536) }
	END {
		n = NR
		printf "atag,devinfo: %d words, header size %d, tag 0x%s, trailing count %d\n", n, w[0], hex(w[1]), w[n-1]
		if (w[0] != n || w[1] != 1090521092 || w[n-1] != n - 3) { print "MALFORMED (driver will refuse)"; exit 1 }
		a = w[2+31]; b = w[2+32]; c = w[2+33]	# efuse 0x180, 0x184, 0x188
		printf "efuse 0x180=0x%s 0x184=0x%s 0x188=0x%s\n", hex(a), hex(b), hex(c)
		en = bits(a, 0, 0); degc = bits(a, 6, 1); sign = bits(a, 7, 7); slope = bits(a, 31, 26)
		v1 = bits(a, 25, 17); v2 = bits(a, 16, 8)
		ge = bits(b, 31, 22); oe = bits(b, 21, 12); id = bits(b, 9, 9); v3 = bits(b, 8, 0)
		v4 = bits(c, 31, 23); vabb = bits(c, 22, 14)
		if (id == 0) slope = 0
		printf "cali_en %d  ADC GE %d OE %d  DEGC %d  slope %s%d (id %d)\n", en, ge, oe, degc, sign ? "-" : "+", slope, id
		printf "VTS MCU1..4 %d %d %d %d  ABB %d\n", v1, v2, v3, v4, vabb
		ok = en && ge >= 265 && ge <= 758 && oe >= 265 && oe <= 758 && degc >= 1 && degc <= 63 &&
		     v1 <= 484 && v2 <= 484 && v3 <= 484 && v4 <= 484 && vabb <= 484
		print ok ? "calibration VALID (vendor range checks pass)" : "calibration INVALID (driver will refuse to probe)"
	}'
	;;
post)
	echo "== kernel messages"
	dmesg | grep -i -E 'thermal|lk-devinfo|1100b000' | tail -20
	z=$(zone) || { echo "no soc-thermal zone"; exit 1; }
	echo "== $z"
	echo "temp $(cat "$z/temp") mC  mode $(cat "$z/mode")  policy $(cat "$z/policy")"
	for t in "$z"/trip_point_*_type; do
		i=${t%_type}; echo "$(basename "$i"): $(cat "$t") $(cat "${i}_temp") mC"
	done
	echo "== cooling devices"
	for c in /sys/class/thermal/cooling_device*; do
		echo "$(basename "$c"): $(cat "$c/type") state $(cat "$c/cur_state")/$(cat "$c/max_state")"
	done
	ls -d /sys/bus/nvmem/devices/* 2>/dev/null
	;;
load)
	z=$(zone) || { echo "no soc-thermal zone"; exit 1; }
	secs=${2:-120}
	echo "zone $z, load ${secs}s on $(nproc) CPUs, then 60 s cooldown"
	pids=
	for i in $(seq "$(nproc)"); do sha256sum /dev/zero & pids="$pids $!"; done
	t=0
	while [ $t -le $((secs + 60)) ]; do
		[ $t -eq "$secs" ] && { kill $pids 2>/dev/null; echo "-- load stopped"; }
		f0=$(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq 2>/dev/null)
		f4=$(cat /sys/devices/system/cpu/cpufreq/policy4/scaling_cur_freq 2>/dev/null)
		echo "t=${t}s  $(cat "$z/temp") mC  LL ${f0} kHz  L ${f4} kHz"
		sleep 2; t=$((t + 2))
	done
	kill $pids 2>/dev/null
	;;
*)
	sed -n '2,13p' "$0"; exit 2 ;;
esac
