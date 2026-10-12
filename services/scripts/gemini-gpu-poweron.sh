#!/bin/bash
# gpu-poweron.sh — power up the Mali-T880 GPU (MT6797 MFG MTCMOS domains)
#
# Runs the EXACT vendor power-on sequences from the BSP kernel
# (gemini-linux-kernel-3.18/drivers/clk/mediatek/clk-mt6797-pg.c,
#  spm_mtcmos_ctrl_mfg_async / _mfg / _mfg_core0-3) against the live
# SPM registers, then opens the mfg_bg3d clock gate.
#
# Registers (SPM base 0x10006000, relative offsets per clk-mt6797-pg.c,
# which the project has established is authoritative over mt_spm.h):
#   MFG_ASYNC  0x334  status BIT(13)
#   MFG        0x338  status BIT(12)
#   MFG_SRAM   0x33C  MFG SRAM_PDN bits[1:0], ACK bits[17:16];
#                     CORE0-3 SRAM ACK bits[23:20]
#   MFG_CORE0  0x340  status BIT(11), SRAM_PDN bit 8
#   MFG_CORE1  0x344  status BIT(10), SRAM_PDN bit 8
#   MFG_CORE2  0x348  status BIT( 9), SRAM_PDN bit 8
#   MFG_CORE3  0x34C  status BIT( 8), SRAM_PDN bit 8
#   PWR_STATUS 0x180 / PWR_STATUS_2ND 0x184
#   POWERON_CONFIG_EN 0x000 (write 1 = SPM register control on)
#
# PWR bit definitions (match mainline mtk-scpsys.c and vendor
# mt_spm_mtcmos_internal.h):
#   PWR_RST_B=1<<0  PWR_ISO=1<<1  PWR_ON=1<<2  PWR_ON_2ND=1<<3  PWR_CLK_DIS=1<<4
#
# Also enables the mfgcfg BG3D clock gate (0x13000000 bit 0; CLR reg +8,
# per BSP clk-mt6797.c GATE(MFG_BG3D, ..., mfg_cg_regs, 0, 0)).
#
# [corrected 2026-08-31] THE GPU SRAM LDO (DVDD_SRAM_GPU) IS PART OF THE
# POWER-ON — with it off the GPU faults GPU_SHAREABILITY_FAULT on the first
# job (root-caused + A/B-verified this day, see session-log). The vendor
# kbase power-on (mali-r12p1 .../platform/mt6797/mtk_config_platform.c
# mtk_pm_callback_power_on + base/power/mt6797/mt_gpufreq.c
# mt_gpufreq_ext_ic_init) writes the infracfg_ao GPU-LDO bank
# 0x10001fbc (VGPU_SRAM enable, comment in mt_gpufreq.c: "enable: //
# VGPU_SRAM") + 0xfc0/0xfc4/0xfc8/0xfd8/0xfe4 (SRAM voltage config), the
# MFG timing register 0x1300001c (async_value), and the MFG PMU enable
# 0x130003e0-0x3f0. Boot state leaves the LDO off (0xfbc=0) and the
# config bank at 0x09090909/0xffffffff — the GPU then renders nothing but
# faults. These writes are the same values the stock Android driver makes
# on every GPU power-on, so they are proven safe on this hardware.
#
# Idempotent: safe to re-run (vendor sequence is read-modify-write).
# Exit code 0 = all domains ON + clock gate open.

SPM=0x10006000
SRAM=0x1000633c
MFGCFG_CLR=0x13000008

# --- helpers ----------------------------------------------------------
rd() { busybox devmem "$1" 32; }
wr() { busybox devmem "$1" 32 "$2"; }

# read-modify-write helpers (no atomicity needed — no contention here)
setb() { local v=$(( $(rd "$1") | $2 )); wr "$1" "$v"; }
clrb() { local v=$(( $(rd "$1") & ~$2 )); wr "$1" "$v"; }

# wait until bit $1 is set in BOTH status regs
wait_sta() {
    local mask=$1 tries=$2
    for ((i = 0; i < tries; i++)); do
        local a=$(rd 0x10006180) b=$(rd 0x10006184)
        if (( (a & mask) == mask && (b & mask) == mask )); then return 0; fi
        sleep 0.02
    done
    return 1
}

# wait until mask $1 of reg $2 is CLEARED
wait_clr() {
    local mask=$1 reg=$2 tries=$3
    for ((i = 0; i < tries; i++)); do
        local v=$(rd "$reg")
        if (( (v & mask) == 0 )); then return 0; fi
        sleep 0.02
    done
    return 1
}

fail() { echo "GPU-POWERON: FAILED at $1"; exit 1; }
ok()   { echo "GPU-POWERON: $1"; }

# --- power on one domain (vendor sequence) -----------------------------
# $1 = ctl reg, $2 = status bit, $3 = sram ack bit (in SRAM reg) or 0
power_on() {
    local ctl=$1 sta=$2 ack=$3

    setb "$ctl" $((0x4 | 0x8))                    # PWR_ON | PWR_ON_2ND
    wait_sta "$sta" 250 || fail "status BIT($sta) for $ctl"

    clrb "$ctl" 0x10                              # PWR_CLK_DIS
    clrb "$ctl" 0x2                               # PWR_ISO
    setb "$ctl" 0x1                               # PWR_RST_B

    if (( ack != 0 )); then
        clrb "$ctl" 0x100                         # SRAM_PDN (bit 8)
        wait_clr "$ack" "$SRAM" 250 || fail "sram ack BIT($ack) for $ctl"
    fi
}

# --- MFG (SRAM_PDN lives in the SRAM reg, bits[1:0]/ACK[17:16]) -------
power_on_mfg() {
    local ctl=0x10006338

    setb "$ctl" $((0x4 | 0x8))
    wait_sta $((1 << 12)) 250 || fail "status BIT(12) MFG"

    clrb "$ctl" 0x10
    clrb "$ctl" 0x2
    setb "$ctl" 0x1

    clrb 0x1000633c 0x3                           # MFG SRAM_PDN bits[1:0]
    wait_clr $((0x3 << 16)) 0x1000633c 250 || fail "sram ack MFG"
}

# --- main --------------------------------------------------------------
# VGPU rail first: the Richtek RT5735 (i2c7 hw @0x11010000, addr 0x1c) powers
# the GPU (vendor: VGPU_SET_BY_EXTIC). At stock boot BOTH VSEL registers
# have the EN bit (0x80) clear — VGPU is OFF — and the MFG MTCMOS domains
# cannot power on without it (proven 2026-08-31: status bits never assert
# until EN is set). VSEL0 = 0x33 (918 mV), VSEL1 = 0x50 (1.1 V) are the
# boot states the preloader leaves. The rail resets to EN=0 on every power
# cycle, so it must be on before the MTCMOS steps.
#
# [2026-10-12] Kernels with the fan53555 RT5735 driver own the rail (DT
# &vgpu, regulator-always-on): the kernel enables it at probe (EN in VSEL0
# only) and this script only checks it. Older boot images have no driver:
# then we enable both VSEL registers from userspace as before, so this
# script works with either boot image.
# [corrected 2026-09-04] Adapter NUMBERS shift when i2c nodes are enabled
# (enabling i2c6 moved the RT5735 from i2c-2 to i2c-3) — resolve the bus by
# controller base 0x11010000 instead of a hardcoded index.
vgpu_reg() {
    local r
    for r in /sys/class/regulator/regulator.*; do
        [ "$(cat "$r/name" 2>/dev/null)" = vgpu ] && { echo "$r"; return 0; }
    done
    return 1
}
rt5735_bus() {
    local d h want=11010000
    for d in /sys/class/i2c-adapter/i2c-*/of_node/reg; do
        [ -f "$d" ] || continue
        h=$(od -An -N8 -tx1 "$d" 2>/dev/null | tr -d ' \n')
        [ "${h:8:8}" = "$want" ] && { echo "$d" | sed -n 's#.*i2c-\([0-9]*\)/.*#\1#p'; return 0; }
    done
    return 1
}
VGPU_ENABLED=0
if [ -d /sys/bus/i2c/drivers/fan53555-regulator ]; then
    # Kernel driver present: wait (up to 5 s) for the vgpu regulator.
    for i in $(seq 50); do VGPU=$(vgpu_reg) && break; sleep 0.1; done
    if [ -n "$VGPU" ] && [ "$(cat "$VGPU/state" 2>/dev/null)" = enabled ]; then
        VGPU_ENABLED=1
        ok "VGPU rail kernel-owned ($VGPU: $(cat "$VGPU/microvolts") uV, enabled)"
    else
        echo "GPU-POWERON: WARNING: fan53555 driver present but vgpu regulator ${VGPU:+not enabled ($VGPU)}${VGPU:-not found}"
    fi
else
    VGPU_BUS=$(rt5735_bus)
    if [ -n "$VGPU_BUS" ] && command -v i2cset >/dev/null 2>&1; then
        i2cset -y "$VGPU_BUS" 0x1c 0x11 0xb3 2>/dev/null && \
        i2cset -y "$VGPU_BUS" 0x1c 0x10 0xd0 2>/dev/null && VGPU_ENABLED=1
    fi
    if (( VGPU_ENABLED )); then
        ok "VGPU rail enabled from userspace (RT5735 VSEL0=0x$(rd 0 >/dev/null; i2cget -y "$VGPU_BUS" 0x1c 0x11 2>/dev/null))"
    else
        echo "GPU-POWERON: WARNING: i2cset not available or VGPU enable failed (bus=$VGPU_BUS)"
    fi
fi
sleep 0.2

wr 0x10006000 1                                   # POWERON_CONFIG_EN = 1
ok "POWERON_CONFIG_EN set"

# --- VGPU SRAM LDO + voltage config (vendor mtk_platform_init +
#     mt_gpufreq_ext_ic_init; infracfg_ao base 0x10001000) ---------------
# The GPU core rail (DVDD_GPU via RT5735) was enabled above; these bits
# power the SRAM bit-cell supply (DVDD_SRAM_GPU, spec pin G16 1.62-1.98 V)
# and set its voltage taps. Without them the GPU faults
# GPU_SHAREABILITY_FAULT on the first job (proven 2026-08-31).
wr 0x10001fc0 0x0f0f0f0f
wr 0x10001fc4 0x0f0f0f0f
wr 0x10001fc8 0x0f
wr 0x10001fd8 0x88888888   # RG_GPULDO_RSV_H_0-8
wr 0x10001fe4 0x00000008   # (readback keeps hardwired 0xB000 bits)
wr 0x10001fbc 0x1ff        # VGPU_SRAM LDO enable (0xff at ext_ic_init,
                           #  0x1ff at power_on)
ok "VGPU SRAM LDO enabled + voltage config (0x10001fbc=$(rd 0x10001fbc))"

# --- MFG timing + PMU enable (vendor mtk_pm_callback_power_on) ----------
# 0x1300001c |= async_value (0x5 for mfg < 780 MHz — we run 500.5 MHz).
# 0x130003e0-0x3f0 = PMU enable (hardware implements bits [9:0] only).
setb 0x1300001c 0x5
wr 0x130003e0 0xffffffff
wr 0x130003e4 0xffffffff
wr 0x130003e8 0xffffffff
wr 0x130003ec 0xffffffff
wr 0x130003f0 0xffffffff
ok "MFG timing + PMU enabled (0x1300001c=$(rd 0x1300001c) 0x130003e0=$(rd 0x130003e0))"

# --- MFG bus protection release (vendor spm_topaxi_protect(MFG_PROT_MASK,
#     0) in power_on; MFG_PROT_MASK = bit 21 per clk-mt6797-pg.h). Boot
#     leaves it clear on this unit, but release explicitly for robustness. --
clrb 0x10001220 0x200000
ok "MFG topaxi protection released (0x10001220=$(rd 0x10001220))"

echo "GPU-POWERON: status before: 0x180=$(rd 0x10006180) 0x184=$(rd 0x10006184)"

power_on 0x10006334 $((1 << 13)) 0 && ok "MFG_ASYNC ON"     # async
power_on_mfg                                              && ok "MFG ON"
power_on 0x10006340 $((1 << 11)) $((1 << 20)) && ok "MFG_CORE0 ON"
power_on 0x10006344 $((1 << 10)) $((1 << 21)) && ok "MFG_CORE1 ON"
power_on 0x10006348 $((1 <<  9)) $((1 << 22)) && ok "MFG_CORE2 ON"
power_on 0x1000634c $((1 <<  8)) $((1 << 23)) && ok "MFG_CORE3 ON"

wr "$MFGCFG_CLR" 1                                 # open BG3D clock gate
ok "mfg_bg3d gate opened (0x13000000=$(rd 0x13000000))"

echo "GPU-POWERON: status after:  0x180=$(rd 0x10006180) 0x184=$(rd 0x10006184)"
for r in 0x10006334 0x10006338 0x1000633c 0x10006340 0x10006344 0x10006348 0x1000634c; do
    echo "GPU-POWERON: $r = $(rd $r)"
done

# final check: all six status bits in both regs
st=0x2B00    # bits 8..13 = CORE3..ASYNC
a=$(rd 0x10006180); b=$(rd 0x10006184)
if (( (a & st) == st && (b & st) == st )); then
    ok "ALL MFG DOMAINS POWERED"
    exit 0
fi
fail "status bits missing: 0x180=$a 0x184=$b"
