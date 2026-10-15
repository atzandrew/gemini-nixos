# SPM sleep on the Gemini (MT6797)

Status 2026-10-14: **step 3e closed.** The SPM (System Power Manager) runs the
vendor suspend firmware from our test module `mt6797-spm-load`, and the CPU
cluster now powers down and is woken by the SPM the way stock suspend does it.
DRAM self-refresh and 26 MHz off are **not** reached; they are blocked on
connsys (Wi-Fi/BT) never sleeping. Detailed run log: `~/Build/notes/spm-3e.md`
on Dragon.

## What works
- Module `devices/planet-geminipda/kernel/modules/mt6797-spm-load`, `stage=6`:
  loads the suspend firmware, arms the SPM, sleeps cpu0 alone (cpu1-9 offline)
  for `cycles` x `sleep_ms`, wakes on the PCM timer (and CONN2AP while Wi-Fi is on).
- `pdn=1` (3e3): stock `spm_suspend` CPU_PDN branch. SW_FLAG without
  DIS_CPU_PDN, AP_STANDBY_CON low bits 0x11, `cpu_pm_enter()` +
  `psci_cpu_suspend_enter(0x01010000)` (cluster power-down, as vendor
  `mt_cpu_dormant_psci`) + `cpu_pm_exit()`, arch timer CNTP_CTL/CVAL restored by
  hand (no broadcast timer). Needs the kernel export of `psci_cpu_suspend_enter`
  (delta `drivers/firmware/psci/psci.c`). No DT idle state: DT cpuidle states
  that power cores off hung at boot (cpuidle1b/1c images, 2026-10-13).
- Verified: single runs R16-R19, 25 cycles with Wi-Fi on, 25 cycles with Wi-Fi
  off, all clean (PSCI returns 0, SW_DEBUG bit 12 CPU_PDN set every cycle).
- Recommended settings: `stage=6 pdn=1 io_en=0x81 acinactm=0
  src_mask=0x10aa007e clk_init=1 settle=1 flags=0x5fc102ff`.

## Measured (battery gauge, USB unplugged, screen blanked, Wi-Fi off)
| sleep | awake before | sleep | awake after |
|---|---|---|---|
| WFI + r0 clock off (no pdn), 2026-10-14 16:04 | 393.4 mA | 375.9 mA | 392.5 mA |
| CPU cluster power-down (pdn), 2026-10-14 19:59 | 409.2 mA | 391.1 mA | 403.2 mA |

Both save ~15-17 mA; CPU power-down adds nothing measurable. The ~375 mA floor
is static load (panel, GPU rail, connsys rails, Vcore, PLLs).

## What does not work yet, and why
- SW_DEBUG (0x10006604) shows the firmware path APSRC, VRF18, INFRA, CPU_PDN
  for every flag set tried, including stock suspend's flags. It never runs the
  26M steps (bits 0/1) or DDREN (DRAM self-refresh, bits 8/9).
- Flags ruled out (single changes, and stock flags together): DIS_DDRPHY_PDN,
  DIS_BUS_CLOCK_OFF, DIS_INFRA_PDN, DIS_MD_INFRA_PDN, DONT_TOUCH_* bits 22-28/30,
  DIS_SRCCLKEN_LOW, SRC2_MASK, r0 output (io_en 0x81).
- Remaining stock difference: connsys never sleeps. PCM_REG13 (0x10006134)
  bit 13 CONN_STATE + bit 14 CONN_SRCCLKENA stay high even with the Wi-Fi radio
  off. Our port force-disables the STP PSM (see `bluetooth-a2dp.md`) and its
  deep-idle backend `mt_combo_plt_enter_deep_idle` is a stub, so the 26 MHz
  request never drops. **Next step: connsys sleep**; then re-run the settings
  above and look for SW_DEBUG bits 0/1 and 8/9.

## Hazards
- SW_FLAG bit 9 (DIS_VPROC_VSRAM_DVS) cleared → PMIC self-reset. Bits 3 and 9
  stay set, and the pwrap DVFS table stays HWCID, until real VPROC/VCORE DVS.
- One PMIC self-reset on the no-pdn path with `clk_init=1 settle=1`, Wi-Fi off,
  25 cycles (n=1, cause unconfirmed). Prefer the pdn path.
- The toprgu WDT is not a backstop for SPM hangs; leave WDT registers alone.
- Known module nit: the restore writes SPM_CLK_SETTLE's readback instead of 0.

## Tools
- `bin/spm-run.sh "<insmod args>"`: one stage-6 run with the screen blanked.
- `bin/gauge-ab2.sh CYCLES AWAKE WIFI WAKE FLAGS BLANK SRC_MASK "EXTRA"`:
  awake/sleep/awake gauge A/B (USB must be unplugged).
- `bin/r13-wifi.sh`: read-only PCM_REG13 sampling with Wi-Fi on/off/on.
- `bin/conn-check.sh`: connsys power-off check via `/sys/kernel/debug/wcn/pwr`
  (returned rc=1 and changed nothing; Wi-Fi did not come back, and a warm
  reboot hung at the logo; use a full power-off afterwards).
- Blank the screen with the lid open: `XDG_RUNTIME_DIR=/run/user/1001
  WAYLAND_DISPLAY=wayland-0 kscreen-doctor --dpms off`; check OVL0 L0
  (0x1400b02c) reads 0.
