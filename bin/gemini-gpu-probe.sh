#!/bin/bash
# gemini-gpu-probe.sh — READ-ONLY survey for GPU DVFS (step 0).
# Run on the Gemini:  sudo bash ~/gemini-gpu-probe.sh | tee ~/gpu-probe.txt
#
# Writes nothing: only reads the LK efuse copy (nvmem), RT5735 registers
# over i2c (register-address write + read, no data written; -f only when the
# kernel driver owns the chip), and a few
# clock/MFG registers via `busybox devmem ADDR 32` (no value argument).
# Sources: vendor mt_gpufreq.c / rt5735.c (gemian 3.18), mainline clk-mt6797.c.

[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
rd() { busybox devmem "$1" 32; }
hr() { echo; echo "== $*"; }

# --- 1. Efuse words -> vendor GPU table -------------------------------
hr "1. efuse (LK devinfo copy)"
NV=$(ls /sys/bus/nvmem/devices/mtk-lk-devinfo*/nvmem 2>/dev/null | head -1)
if [ -z "$NV" ]; then
	echo "mtk-lk-devinfo nvmem not found"; ls /sys/bus/nvmem/devices/
else
	w() { od -An -tx4 -j $((4 * $1)) -N4 "$NV" | tr -d ' '; }
	W3=$((0x$(w 3))); W8=$((0x$(w 8))); W22=$((0x$(w 22))); W61=$((0x$(w 61)))
	printf 'word3=0x%08x word8=0x%08x word22=0x%08x word61=0x%08x\n' $W3 $W8 $W22 $W61
	bond=$(( (W8 >> 8) & 0xf )); fc0=$(( (W22 >> 24) & 0xf )); fc1=$(( W22 & 0xf ))
	ptp=$(( (W61 >> 4) & 0xf )); turbo=$(( (W3 >> 31) & 1 ))
	case $bond in 0) cid="0 (free run)";; 1) cid="1 (800M)";; 2) cid="2 (700M)";;
		3) cid="3 (600M)";; 4|12|13|14|15) cid="4 (500M)";; *) cid="0 (free run, default)";; esac
	case $fc1 in 0|3) type=0;; 1|6) type=1;; 2|7) type=2;; *) type=0;; esac
	[ $ptp -le 5 ] && type=$((type + 4)); [ $ptp -gt 6 ] && type=$((type + 8))
	[ $fc1 = 15 ] && type=11; [ $fc1 = 4 ] && type=12
	echo "speed bond=$bond -> table cid $cid"
	echo "func_code0=$fc0 func_code1=$fc1 ptp_version=$ptp -> table type $type"
	echo "turbo bit (word3[31], only used on tablet-turbo builds)=$turbo"
fi

# --- 2. RT5735 VGPU buck ---------------------------------------------
hr "2. RT5735 (i2c7 @0x11010000, addr 0x1c)"
# i2c-mt65xx adapters don't show their address in `i2cdetect -l`: resolve the
# bus from the controller's DT reg (same lookup as gemini-gpu-poweron.sh).
B=""
for d in /sys/class/i2c-adapter/i2c-*; do
	h=$(od -An -N8 -tx1 "$d/of_node/reg" 2>/dev/null | tr -d ' \n')
	[ "${h:8:8}" = 11010000 ] && { B=${d##*/i2c-}; break; }
done
if [ -z "$B" ]; then
	echo "i2c7 adapter not found:"; i2cdetect -l
else
	DRV=$(basename "$(readlink /sys/bus/i2c/devices/$B-001c/driver 2>/dev/null)" 2>/dev/null)
	# With a kernel driver bound, i2cget needs -f (reads only; no writes here).
	F=""; [ -n "$DRV" ] && F=-f
	echo "bus i2c-$B, kernel driver on 0x1c: ${DRV:-none}"
	for r in 0x03 0x04 0x10 0x11 0x12 0x13 0x14 0x16; do
		v=$(i2cget $F -y "$B" 0x1c $r 2>/dev/null) || v=ERR
		line="reg $r = $v"
		if [ "$v" != ERR ]; then case $r in
			0x03) line="$line  product ID (RT5735 = 0x10)";;
			0x10|0x11) line="$line  EN=$(( (v >> 7) & 1 ))  $(( 600000 + (v & 0x7f) * 6250 )) uV  ($([ $r = 0x11 ] && echo PROGVSEL0 || echo PROGVSEL1))";;
			0x12) line="$line  DISCHG=$(( (v >> 4) & 1 )) PGDVS=$(( (v >> 1) & 1 )) PGDCDC=$(( v & 1 ))";;
			0x13) line="$line  DVS-up field=$(( (v >> 2) & 7 ))";;
			0x14) line="$line  PWM0=$(( (v >> 7) & 1 )) PWM1=$(( (v >> 6) & 1 )) DVSMODE=$(( (v >> 5) & 1 ))";;
			0x16) line="$line  IOC=$(( (v >> 6) & 3 )) TPWTH=$(( (v >> 4) & 3 )) DVS-down=$(( (v >> 1) & 3 )) REARM=$(( v & 1 ))";;
		esac; fi
		echo "$line"
	done
	echo "vendor init: 0x12=0x00 0x13=0x19 0x14=0x01 0x16=0x63"
fi
for rg in /sys/class/regulator/regulator.*; do
	[ "$(cat $rg/name 2>/dev/null)" = vgpu ] && echo "regulator vgpu ($rg): $(cat $rg/state) $(cat $rg/microvolts) uV, constraints $(cat $rg/min_microvolts)-$(cat $rg/max_microvolts) uV"
done

# --- 3. MFGPLL + mfg_sel ----------------------------------------------
hr "3. MFGPLL (apmixed 0x1000c000) + mfg_sel (topckgen CLK_CFG_1)"
C0=$(rd 0x1000c240); C1=$(rd 0x1000c244); PW=$(rd 0x1000c24c); CFG1=$(rd 0x10000050)
echo "MFGPLL_CON0=$C0 CON1=$C1 PWR_CON0=$PW CLK_CFG_1=$CFG1"
pcw=$(( C1 & 0x1fffff )); pd=$(( (C1 >> 24) & 7 ))
echo "pcw=0x$(printf %x $pcw) posdiv field=$pd (/$((1 << pd)))"
echo "VCO = $(( 26000 * pcw / 16384 )) kHz, output = $(( 26000 * pcw / 16384 / (1 << pd) )) kHz"
sel=$(( (CFG1 >> 24) & 3 )); gate=$(( (CFG1 >> 31) & 1 ))
echo "mfg_sel parent index=$sel (0 clk26m, 1 mfgpll_ck, 2 syspll_d3, 3 univpll_d3), gate(pdn)=$gate"

# --- 4. MFG timing register -------------------------------------------
hr "4. MFG timing 0x1300001c (expect 0xa: top OPP >= 780 MHz) + PMU enable"
echo "0x1300001c=$(rd 0x1300001c)  PMU enable 0x130003e0=$(rd 0x130003e0) (expect 0x3ff)"

# --- 5. What the kernel thinks ------------------------------------------
hr "5. clk_summary (mainline's computed rates)"
CS=/sys/kernel/debug/clk/clk_summary
[ -r $CS ] || mount -t debugfs none /sys/kernel/debug 2>/dev/null
grep -E ' (mfgpll|mfgpll_ck|mfgpll_d2|mfg_sel|mfg_52m_sel|syspll_d3|univpll_d3|univpll) ' $CS 2>/dev/null || echo "clk_summary not readable"

hr "6. panfrost / devfreq"
dmesg | grep -i -E 'panfrost|mali' | tail -8
echo "devfreq devices: $(ls /sys/class/devfreq/ 2>/dev/null | tr '\n' ' ')"
echo "GPU power-on service: $(systemctl is-active gemini-gpu-poweron.service 2>/dev/null)"
echo "SoC temp: $(cat /sys/class/thermal/thermal_zone*/temp 2>/dev/null | tr '\n' ' ')"
echo; echo "done (read-only)"
