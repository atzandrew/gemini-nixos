# SPM sleep on the Gemini (MT6797)

## Current status (2026-10-22)
- **Suspend is on** for everyday use (lid, power key, lock screen).
  The panel is really powered off in every suspend (geminipda-drm
  `panel_suspend=1`, stock vendor off/on sequence; see
  [`display.md`](display.md) "Panel off/on"). `mt6797-suspend` `bias_check`
  (default on) refuses to sleep while the LCD bias (GPIO60 ENP / GPIO251 ENN)
  is still high.
- **Rule: never sleep with the panel bias on.** DCS 0x28 (display-off) alone
  is not enough: 6 screen burns (image retention), the last after ~2 h of
  12–30 min test sleeps on 2026-10-20. Stock always turns the bias off in
  suspend (`stock-suspend-mt6797.md`).
- **The random sleep hang is fixed** (dd3cbe9, `srcclken_low` default Y).
  Cause: we forced `SPM_FLAG_DIS_SRCCLKEN_LOW` (bit 15) in every suspend;
  stock suspend never sets it (only the idle modes do). With it forced, about
  one hang per 20–25 min asleep, random in time, always at breadcrumb step
  "PSCI suspend", ending in a PCM watchdog reset. Without it:

  | Run | Sleeps | Asleep | Hangs (old rate predicts) | Power |
  |---|---|---|---|---|
  | 2026-10-21 afternoon | 12 × 600 s | 2 h | 0 (5–6) | 178–181 mA |
  | 2026-10-21/22 overnight | 34 × 600 s | 5.7 h | 0 (~15) | 173–181 mA |

  Power is the same either way, so SRCCLKEN (26 MHz request) is probably
  still held high by something else (likely connsys). `srcclken_low=0`
  brings the old behaviour back, for tests only.
- What the hang hunt ruled out (2026-10-21): sleep length (hangs at 11 s and
  at 21 min), number of SPM cycles (100 ms ticks: 2885 cycles in 15 min,
  clean), the pwrap DVFS command table (the firmware sends no pwrap commands
  in suspend). Flag lineup vs stock and the full log:
  `notes/next-session-hang-prompt.md` (Dragon). Do not clear bit 9
  (`DIS_VPROC_VSRAM_DVS`): hard crash + PMIC power cycle (2026-10-14).
- Installed module: dd3cbe9 (`bias_check`, `srcclken_low` default Y), with
  `pcm_wdt=30` from `/etc/modprobe.d/gemini-suspend.conf`. The PCM watchdog
  stays on as a safety net: a stuck sleep resets the SoC ~30 s later and the
  breadcrumb survives in NONRST_REG2 (printed and cleared at module load).
- Power domains still on in sleep (`SPM_PWR_STATUS` 0x2b00005c): display
  MM/DIS (bit 3) and AUDIO (bit 24); ISP, VDEC, MFG, MJC, VENC are off. The
  display pipeline stays on because geminipda-drm only adopts LK's setup.
- Soak runner for unattended series: `bin/sleep-tests/soak.sh` (+
  `gemini-soak.service`), survives hang reboots. Use MIN_BAT ≥ 40: at 28 %
  the battery browned out during the next boot (2026-10-22).
- On external power `gemini-suspend-prep` refuses suspend; Plasma then retries
  every 10 s and the screen stays lit under a shut lid. Open.
- Lid EINT wake works mechanically but is **not usable** (see that section).
- Next: lower sleep power (~175 mA): stop DSI streaming / display pipeline in
  suspend, the audio domain, then the stock steps we skip
  ([`stock-suspend-mt6797.md`](stock-suspend-mt6797.md)).

## History

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

## Intermittent hang in SPM sleep (2026-10-17/18)
System suspend (`mt6797-suspend`, 2 s PCM-timer cycles) occasionally never
returns from PSCI CPU_SUSPEND. Run log: `~/Build/notes/vproc-research.md`
("Hang hunt", "h9") on Dragon.
- Rate: about one per 30-40 runs of 30 s, i.e. roughly 1 in 500 SPM cycles,
  at a random cycle (seen at cycles 0, 4, 5, 13). Everyday suspend wakes
  1800 times an hour, so a suspend of an hour or more is likely to hit it.
- Where: breadcrumbs (`mt6797-suspend crumbs`, TOPRGU NONRST_REG2 0x10007024)
  always stop at "PSCI suspend". The only SPM wake source is the PCM timer
  (`wake_unmask=0x1`; keys and lid are polled by the CPU), so a missed timer
  wake and a CPU that dies on resume look the same, and a key press cannot
  wake a stuck sleep.
- Ruled out as the cause: VPROC2 voltage/rail (the original suspect), DA9214
  I2C reads, the battery gauge scale (checked against bq25896 ICHGR, within
  2 %), the mt6797-suspend load address, connsys state. An extra kernel module
  loaded between suspends may raise the rate (5/27 runs vs 1/28 plain,
  p ~ 0.09): unresolved.
- PSCI: the DT always pinned SMC32 `cpu_suspend = <0x84000001>` like stock,
  but the 0.2 path ignored it (CPU_SUSPEND went out as SMC64). Fixed to honour
  the DT id (`drivers/firmware/psci/psci.c`); no clear change in the hang rate
  (h9: ~1 in 42 runs), kept because it matches stock.
- The old "random WDT resets with the WDT fed each cycle" (9affe74) need 31 s
  without the CPU running, so they were most likely this hang too.
- Testing: `bin/sleep-tests/hunt.sh` with `wdt_pause=0 wdt_int_rst=1` turns a
  hang into a SoC-only WDT reset in ~1 min with the breadcrumb kept. The
  battery is worn (2600 of 4220 mAh): a reset's boot current (~2.4 A) can sag
  it to 3.0 V and brown out below ~50 %; charge first.
- After a WDT reset the bootloader once stalled polling SPM registers (ATF
  WDT dump at 0x7FF6A000, PC in LK) and needed a second reset.
- Way forward (as written 2026-10-18): fewer wake cycles (EINT wake for
  keys/lid, longer or no PCM timer) and observability at the hang (SPM/ATF
  state), not more A/B runs.
- **Correction 2026-10-20:** the 1-in-500 rate comes only from short runs
  (30–180 s) with the WDT running. With the WDT paused (everyday setting),
  sleeps of ~10 min or more hang almost every time, whether the cycles are
  2 s or 60 s, so the long-sleep hang is not "per cycle" and fewer cycles
  did not help. The 30 min everyday baseline planned on 10-18 was never run.
  The two may be separate problems.

## VPROC2 low voltage in sleep (2026-10-18)
- The DA9214 BUCKB rail (VPROC2, A72 cluster) stays on at 1.00 V after the
  cluster is torn down. In SPM sleep that costs ~40 mA; awake it costs nothing
  measurable.
- A/B (30 s suspends, 250 mA baseline): 1 phase alone 0 mA, auto 1-phase mode
  0 mA, 0.6 V + 1 phase −33 mA, 0.3 V + 1 phase −40 mA. So it is the voltage.
- `mt6797-suspend vproc2_sleep_mv=300` (default since 393fedb + this commit):
  .prepare sets VBUCKB_A to 0.30 V (chip minimum) through the da9211 regmap,
  only with cpu8/9 offline and the cluster isolated; .finish restores it.
  Measured 250 → 208 mA (7 runs, no hangs). Same saving as switching the rail
  off (`vproc2_off`, −43 mA) without the rail-off path.
- Stock never touches the DA9214 in suspend (no VSEL pin, GPI off); the unused
  0.60 V "B" setting of VPROC1 is not a lever.
- Panel: two screen burns (2026-10-17/18) during test series that woke the
  panel to the static lock screen between every suspend. `hunt.sh` now keeps
  it display-off for the whole series (cf32b26).

## Lid EINT wake (2026-10-20)
- The hall sensor (GPIO66) has an EINT function: mode 1 = EINT5 (vendor
  DWS: HALL = EINT5, level, active low; pinctrl-mtk-mt6797.h: GPIO61-68 =
  EINT0-7, GPIO85-93 = EINT8-16, so the keyboard INT GPIO87 = EINT10).
  Mainline leaves GPIO66 in mode 0, and nothing programs the EINT block at
  0x1000b000 (GIC SPI 170; same register layout as mainline mtk-eint): at
  boot every EINT is masked and none is routed to the AP (D0EN = 0).
- GPIO DIN still reads the lid with the pin in mode 1.
- The SPM EINT wake is R12 bit 6 (`R12_EINT_EVENT_B`, pcm_def_mt6797.h);
  stock suspend unmasks it.
- `mt6797-suspend lid_eint=1` (default): when the lid is shut at entry,
  GPIO66 goes to mode 1, EINT5 is set level / active high (= lid open),
  routed and unmasked, and R12 bit 6 is added to the SPM wake mask. The
  PCM timer becomes a 60 s safety net (`lid_poll_ms`). An EINT wake with
  the lid still shut counts as a glitch and the loop sleeps on. All of it
  is restored after the loop. Lid open at entry, or the WDT left running
  (`wdt_pause=0`, 31 s), keeps the 2 s `poll_ms` cycles.
- Test: 9.5 min asleep in 10 cycles (was ~285), lid open woke it at once
  (r12 0x40), 0 glitches.
- Why it was built: the random hang (above) looked per-cycle, so ~30x fewer
  wake-ups should have meant ~30x fewer hangs.
- **Result 2026-10-20: not usable.** Six 1–3 min sleeps woke fine. Of five
  sleeps of ~10 min or more, four hung (60 s cycles: 10:49, 11:49, 12:47;
  EINT armed with 2 s cycles: 13:02). The plain 2 s module hangs on long
  sleeps too (10-19 17:18, 10-20 07:54), so EINT is not the cause of the
  hang, and the fewer-cycles idea did not help. The 1 % gauge read ~0.5–1.2 A
  over EINT sleeps (vs ~208 mA from old 30 s runs; not cross-checked). One
  60 s cycle ended "pmic read failed" (12:40:52). Breadcrumbs cannot catch a
  hang in this mode: the 60 s cycles need the TOPRGU WDT paused, and the
  power-hold wipes NONRST_REG2.
- 607419c was installed as the everyday module on 2026-10-20 (11:34) during
  testing. Planned first step next: back to the last pre-EINT module
  (fddf570 = dc1fd99 + display check/last_wake 3bc40af), which is what the
  2026-10-19 Debian image shipped.
- Keys are only checked once per cycle while armed (lid shut = keyboard
  unreachable; opening the lid wakes it).
