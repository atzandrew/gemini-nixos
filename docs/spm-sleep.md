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
- **Open: random sleep hang.** In everyday use (module loaded at boot) about
  1 in 2–3 five-minute sleeps hangs, always at breadcrumb step "PSCI suspend"
  (inside the CPU_SUSPEND call to ATF), 0.5–4 min in. The PCM watchdog resets
  the SoC ~30 s later; unsaved work is lost. Cause unknown; see "Sleep hang
  investigation (2026-10-22)" at the end for what has been ruled out and the
  open leads.
- Do not clear bit 9 (`DIS_VPROC_VSRAM_DVS`): hard crash + PMIC power cycle
  (2026-10-14).
- Module options in use: `bias_check` and `srcclken_low` (both default on),
  `pcm_wdt=30` from `/etc/modprobe.d/gemini-suspend.conf`. The PCM watchdog
  is the safety net: a stuck sleep resets the SoC ~30 s later and the
  breadcrumb survives in NONRST_REG2 (printed and cleared at module load).
- Sleep power: ~170 mA (gauge). Measured levers are in the "Sleep power" and
  "Off-SoC split" sections. Power domains still on in sleep
  (`SPM_PWR_STATUS` 0x2b00005c): display MM/DIS (bit 3) and AUDIO (bit 24);
  ISP, VDEC, MFG, MJC, VENC are off. The display pipeline stays on because
  geminipda-drm only adopts LK's setup.
- Soak runner for unattended series: `bin/sleep-tests/soak.sh` (+
  `gemini-soak.service`), survives hang reboots. Use MIN_BAT ≥ 40: at 28 %
  the battery browned out during the next boot (2026-10-22).
- On external power `gemini-suspend-prep` refuses suspend; Plasma then retries
  every 10 s and the screen stays lit under a shut lid. Open.
- Lid EINT wake works mechanically but is **not usable** (see that section).
- Next: the hang (leads at the end of this file), then the stock steps we
  skip ([`stock-suspend-mt6797.md`](stock-suspend-mt6797.md)).

## History

Status 2026-10-14: **step 3e closed.** The SPM (System Power Manager) runs the
vendor suspend firmware from our test module `mt6797-spm-load`, and the CPU
cluster now powers down and is woken by the SPM the way stock suspend does it.
DRAM self-refresh and 26 MHz off are **not** reached; they are blocked on
connsys (Wi-Fi/BT) never sleeping.

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
returns from PSCI CPU_SUSPEND.
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

## Sleep power: DSI streaming in suspend (2026-10-22)
Question: does the display pipeline that keeps streaming in suspend (geminipda-drm only adopts LK's
pipeline; MM/DIS domain bit 3 stays on) cost much? Test: stop DSI0 video streaming for the sleep, the
stock `ddp_dsi_stop` way (MODE_CTRL 0, wait INTSTA BUSY clear, PHY_LCCON LC_HS_TX_EN 0), restart with
MODE_CTRL 1 + HS clock + START 0->1, all only while panel-pm has the panel off (bias low, reset held).
Tools: `bin/sleep-tests/dsi-stream.sh`, `s2.sh` env `DSI_STOP=1` / `MOD=`. User present (rule 5).
- Awake cycle (panel off, stop, 10 s, start, panel on 862 ms): picture correct, no shift. RDMA/OVL/mutex
  need no reset for a plain stop/restart. While stopped, Plasma commits log "vblank wait timed out"
  (the vblank IRQ is RDMA0's, which stalls with the stream); a driver version must handle that.
- 300 s SPM sleeps, installed module dd3cbe9 (pcm_wdt=30, bias_check), lid open, Wi-Fi/BT/connsys off:

| run | DSI in sleep | mA (kernel window) | screen after |
|---|---|---|---|
| A 10:08 | streaming (shipped) | 184.1 over 332 s | normal |
| B 10:16 | stopped, HS clock LP | 178.9 over 331 s | normal (panel on 874 ms) |

- Result: ~5 mA (3%), near the run-to-run spread (175-184 mA). The stream itself (DSI PHY HS + RDMA
  DRAM reads) is not where the ~175 mA goes. Still unmeasured: the MM/DIS domain + MMPLL/clocks being
  off (needs a real display driver, plan step 2); this result does not justify step 2 on its own.
  Next: the AUDIO domain (step 3) and the stock steps we skip.

## Sleep power: VA10 LP, Vcore 0.85 V, AUDIO domain (2026-10-22, 10:29-11:16)
`bin/sleep-tests/pwr-series.sh`, 300 s SPM sleeps, installed dd3cbe9 (pcm_wdt=30, bias_check), lid open,
Wi-Fi/BT/connsys off, panel off by panel_suspend. All runs clean (rc 0, woke on max_s).

| run | setting | mA |
|---|---|---|
| A | baseline | 183.0 / 181.5 / 180.3 |
| L | VA10 HW LP on SRCLKEN0 (`ldo_lp=2 ldo_lp_mode=2`) | 181.6 |
| V | Vcore HW sleep mode, VOSEL_SLEEP 0x28 = 0.85 V (E19 1b order) | 181.0 |
| U | AUDIO domain really off (PWR_STATUS 0x2a00005c in .enter) | 181.6 / 180.8 |

- None saves anything measurable. VA10 HW LP no longer hangs (the 2026-10-17 "VA10 hang" was the old random
  hang, bit 15). Open question: do the SRCLKEN-driven PMIC sleep modes engage at all? If SRCCLKEN stays high
  (power was identical with/without srcclken_low), both L and V are no-ops by construction.
- AUDIO domain facts: the AFE probe takes a runtime-PM ref and never drops it (mt6797-afe-pcm.c
  `pm_runtime_get_sync` in probe), so the domain is on all the time. Unbinding the AFE releases it, but
  **mtk-scpsys cannot switch the domain off or on: the SRAM_PDN ack (PWR_CON 0x10006314 bits 15:12) needs
  the `infra_audio_26m` clock** (vendor clk-mt6797-pg.c `PGATE(SCP_SYS_AUDIO, pg_audio, NULL, infra_audio_26m)`),
  which the AFE has already gated, and our scpsys audio domain has `clk_id CLK_NONE`. "Failed to power off
  domain audio" (a second unbind completed it because the ack had latched), and "Failed to power on domain
  audio" on rebind: audio is gone until a reboot. Also missing for a real fix: AFE component
  `.suspend/.resume = mtk_afe_suspend/resume` + a register backup list (as mt8173), and `snd_soc_pm_ops` in
  the mt6797-mt6351 machine driver. Not worth it for sleep power (0 mA); maybe for awake idle later.
- Conclusion with the DSI result: display stream ~5 mA, the rest of these ~0. The ~180 mA is not in these
  SoC-side levers. Next: the off-SoC split (awake A/B, group-then-split) and whether SRCCLKEN/26M really drops.
- SRCCLKEN check (read-only, vendor pcm_def_mt6797.h): R13 in sleep = 0x00060000 = only bit 17 WAKE_EVENT_STA +
  bit 18 EMI_IDLE. No 26M requests (MD1/MD2/MD32/CONN/LTE SRCCLKENA bits 2/5/6/14/29 = 0), no CONN_DDR_EN (4) or
  CONN_STATE (13); with dbg 0x10ff (26M step runs) the SoC looks like it reaches its 26M-off state. So the
  ~180 mA is most likely off-SoC → the off-SoC split is the next step.

## Off-SoC split (2026-10-22, E30 + stock GPIO table)
- E30 (bin/sleep-tests/e30.sh, awake, SPM-run state, 60 s each): A1 302.5 / USB+touch 279.1 / A2 288.0 /
  USB 267.8 / A3 277.9 / touch 276.4 / A4 277.5 mA (baseline drifting down after boot). USB group
  (GPIO 94 usb1-drvvbus, 72 sw7226-en, 70 fusb301a-sw-en, 107 otg-drvvbus) low = **~15 mA**; touch
  SSD2092 held in reset (GPIO68) = ~0. Restore clean.
- Stock (gemian 3.18 aeon6797_6m_n.dws) defaults all of 94/72/70/107/68 to output LOW; we hog them high.
- Stock-vs-live GPIO diff (DIR/DOUT/MODE, all 262 pins): besides the USB group, only GPIO58 (AW9523B SHDN,
  stock drives it low on screen blank), 60 (ENP, off in sleep anyway), 68 (touch reset), 256 (fingerprint
  reset: stock out-low, ours input) and EINT-mode pins differ. ⇒ no other pin-driven off-SoC loads.
- Stock touch suspend is empty (POWER_OFF and sleep-in compiled out); it relies on the LCM power-off.
- Next: connsys rails in the CONN-off state. VCN18 is SW-on (stays on in sleep); stock turns VCN18/28/33 off.
- E31 (bin/sleep-tests/e31.sh, awake, CONN domain off): VCN18 (0x0a52 da62 → da60; QI_EN cleared, rail really off)
  A1 288.0 / B1 286.0 / A2 286.5 / B2 285.9 / A3 285.8 mA ⇒ **~0–1 mA, not a lever**. Restore clean (Wi-Fi + BT back;
  the hci0 opcode errors at conn power-on are the same as after E30). VCN28 is HW mode (off in sleep with no 26M
  request); VCN33 is disabled by Linux with connsys off.

## Sleep hang investigation (2026-10-21/22)
**Status: not fixed; cause unknown.** All hangs stop at breadcrumb step "PSCI suspend" and end in a PCM
watchdog reset.

### srcclken_low (bit 15), 2026-10-21
We forced `SPM_FLAG_DIS_SRCCLKEN_LOW` (bit 15) in every suspend; stock suspend never sets it (only the idle
modes do). With it forced, sleeps hung about once per 20–25 min asleep, also when the module was reloaded
after boot. dd3cbe9 stopped forcing it (`srcclken_low`, default on). Afterwards, sleeps with the module
**reloaded after boot** by the test scripts (soak.sh/s2.sh `MOD=`) stopped hanging:

| Run | Sleeps | Asleep | Hangs (old rate predicts) | Power |
|---|---|---|---|---|
| 2026-10-21 afternoon | 12 × 600 s | 2 h | 0 (5–6) | 178–181 mA |
| 2026-10-21/22 overnight | 34 × 600 s | 5.7 h | 0 (~15) | 173–181 mA |

This was first reported as the fix. It is not: everyday use loads the module **at boot**
(`/etc/modules-load.d/gemini-suspend.conf`), which these runs never tested.

### Boot-loaded module, 2026-10-22
- Boot-loaded, srcclken_low on, 300 s sleeps through soak.sh (= gemini-suspend-prep + `echo mem`, the
  everyday path): **5 hangs in 13 sleeps** (1–3.5 min in). The old and new gemini-suspend-prep both hang;
  USB pins on or off both hang.
- Reloaded after boot with srcclken_low on: 0 hangs in ~57 sleeps. 9 of those went through s2.sh, which skips
  gemini-suspend-prep, so this comparison changed more than one thing.
- The 2026-10-21 11:25 hang was a reloaded module (old build, bit 15 forced) at a random buffer address.
- The test copy and the installed copy of the module differ only in where `srcclken_low` is stored, its
  default and its help text (binary diff).
- A firmware-buffer integrity check (74bb2c3) found the buffer intact around clean sleeps; it never ran
  around a hanging one, so corruption is not seen, but also not excluded.

### Buffer-address tests, 2026-10-22
At boot the SPM firmware buffer always lands at phys 0x41c02000, 8 KB past the connsys window
(0x41a00000–0x41bfffff). Two tests, pass/fail fixed in advance, both through soak.sh, 300 s sleeps:

| Test | Load | Buffer | Result |
|---|---|---|---|
| reload forced onto the boot page (test-only allocation option, dd3cbe9 + placement) | reloaded | 0x41c02000 | hung on sleep 3 |
| buffer kept out of 0x41a00000–0x41ffffff (dd3cbe9 + exclusion) | at boot | 0x43000000 | hung on sleep 2 |

**The buffer address is not the cause.** "A reloaded module does not hang" is also weaker than it looked:
the reload above hung on its 3rd sleep.

### What does not help find it
- The SPM's watchdog wake mode (`pcm_wdt_wake`, tried 2026-10-20): an SPM wake does not rescue the hang.
- The SoC reset wipes all SPM registers, and the ATF log (0x7FF40000) is re-initialised at boot, so there is
  no post-mortem state. The ATF WDT-dump area (0x7FF6A000) survives resets but our hangs leave nothing there.

### Open leads (none proven)
1. **SMC64 vs SMC32 CPU_SUSPEND.** Our kernel issues CPU_SUSPEND as SMC64 0xC4000001; stock and our DT use
   SMC32 0x84000001, and cjdell found ATF hangs on SMC64 CPU_ON for the A72s. It sits exactly at the hang
   step. Needs a kernel change + flash.
2. **Module loads.** 2026-10-18: hangs followed loading any extra kernel module after mt6797-suspend, even
   stock `crc-ccitt` (5 hangs in ~17 suspends after a load vs 0 in 24 without). Fits the boot case (Wi-Fi/BT
   modules load ~4 s after mt6797-suspend), but not the 2026-10-22 reload that hung with no module loaded
   after it.
3. Stock steps we skip ([`stock-suspend-mt6797.md`](stock-suspend-mt6797.md)): thermal chunks, the pwrap
   SUSPEND table (`dvs=1`, de1fa94, untested), PMIC preparation, CIRQ/IRQ masking, DRAM self-refresh.
4. connsys: gemini-suspend-prep power-cycles connsys around every sleep and logs "live client resync FAIL".
   Not examined.
