# Power states of the Gemini PDA — reboot, poweroff, and the limbo state

**Last updated:** 2026-09-10 (research + implementation session — driver
built, flashed and verified on glass; see "Implementation status" below)

Task: `systemctl reboot` leaves the device in a black-screen limbo (PMIC on,
power key dead, only a 10 s power+side-button hold recovers it) and
`systemctl poweroff` is impossible. We need a real reboot (without the
userspace WDT hack) and a real powerdown (battery safety: an off device
must not keep draining below the safe voltage when USB is disconnected).

## TL;DR

- The limbo is **arm64 mainline behaviour, not a bug in our config**:
  Linux 6.6 arm64 `machine_restart()` with no registered restart handler
  prints `Reboot failed -- System halted` and halts the boot CPU in
  `while(1)`. The PMIC stays fully powered (≈1.6 W floor, per
  `docs/power-sleep.md`), LK never re-runs (so the panel is uninitialised —
  black screen, no backlight), and the power key is routed to the dead AP —
  hence dead. Only the PMIC's hardware long-press reset (10 s power+side) or
  the WDT EXRST recover it.
- Poweroff fails earlier: `poweroff(2)` is **refused** (`-EINVAL`) because
  `kernel_can_power_off()` is false — no poweroff handler exists.
- Both mechanisms exist and are **fully source-verified** for this exact
  SoC/PMIC, and both are reachable from our kernel:
  - **Reboot** = the TOPRGU/WDT **SWRST external reset** — the exact
    sequence LK itself uses for every reboot (`mtk_wdt_reset(1)`), and the
    one our field-verified `gemini-wdt-reboot` triggers. PMIC power-cycles
    the SoC, LK re-runs (re-initialises the panel — rule-5 safe), and the
    "bypass power key" flag makes it self-boot.
  - **Poweroff** = writing **MT6351 `RTC_BBPU` (PWRBB)** over the pwrap
    regmap — the exact write LK (`rtc_bbpu_power_down`) and the vendor
    Android kernel (`mt_power_off`) use for real shutdown. The PMIC cuts
    the main rails; RTC + charger + key-scan survive; the power key cold-boots
    the device again.
- Design: one small in-repo kernel delta driver
  (`drivers/power/reset/mt6797-power.c`) registering a restart handler and a
  platform poweroff. §5. On-glass test plan in §7.

## Implementation status — 2026-09-10 (verified on glass)

Landed in `devices/planet-geminipda/kernel/delta/` (fork rev `06fd13e11`,
built in-repo, delta byte-verified) and flashed as boot.img sha256
`2fbca31446cf1f70e1b37a8a109c3737e59f8adec7fbdea2d08b47c6a6c3c1f8`.

- Both drivers bind: `mt6797-power 10007000.power: MT6351 RTC_BBPU
  readback 0x000d` + "MT6797 restart + MT6351 poweroff handlers
  registered", and `mtk-wdt 10007000.watchdog: Watchdog enabled
  (timeout=31 sec, nowayout=0)`.
- `systemctl reboot` → clean self-boot to a new boot_id in ~40 s
  (`70dd8a9d…` → `dff7d973…`). [verified 2026-09-10]
- `systemctl poweroff` → unit off: the USB gadget disappears with no
  preloader/gadget-NIC and no loop (no limbo). [verified 2026-09-10;
  that run's NIC enumerated as RNDIS — the gadget is CDC-ECM since
  2026-09-17, docs/usb-network.md]
- The userspace-WDT escape was not needed; the §7 primary paths pass.
- **Gotcha that cost a boot loop:** the shared TOPRGU block. See the
  [corrected 2026-09-10] note in §5.

## 1. The power hardware

| Component | Role |
|---|---|
| MT6797X SoC (2×A72 + 8×A53) | AP. WDT/TOPRGU block at `0x10007000`. PWRAP (AP→PMIC) at `0x1000d000`. |
| **MT6351 PMIC** | All SoC rails, the PWRKEY/STRUP start-up circuit, **PWRBB** (battery-backup power unit = the real "off" switch), the RTC block. AP access via PWRAP (SPI-like; mainline `mtk-pmic-wrap.c` in our delta exposes it as a **16-bit regmap, `max_register = 0xffff`** — the PMIC main space `0x0000-0x0fff` *and* the RTC space `0x4000-0x403c`). |
| BQ25896 charger (i2c0 @0x6b) | Battery charge path, **independent of the PMIC** (sysfs `bq25890-charger-0`). |
| NT36672 TDDI panel | Initialised **only by LK** (CORE RULE 5). A boot that skips LK = uninitialised panel = black screen (the limbo symptom). |
| Power ("Esc/On") + silver side key | Both terminate at the PMIC: `PWRKEY_DEB`/`HOMEKEY_DEB`, TOPSTATUS `0x220` bits 1/2 (our `mt6351-keys` driver polls these). Holding the power key ~8-10 s is the PMIC-level hardware reset (STRUP long-press → PWRBB; `MT6351_PMIC_RG_STRUP_LONG_PRESS_EXT_PWRBB_CTRL`, DTS comment in `mt6797-gemini-pda.dts` "mt6351-keys" block). This is the universal recovery and cannot be disabled in software. |

Boot chain: PMIC POR → BootROM → **LK** (`boot` partition; target
`aeon6797_6m_n`) → kernel. LK is the only panel initialiser, and the
PWRKEY long-press that releases boot after a cold POR is handled by the
PMIC/LK. A reset flagged "bypass power key" skips that wait and boots
straight through.

## 2. The power-state machine (what the hardware actually does)

| State | How reached | Panel | Power key | Notes |
|---|---|---|---|---|
| Cold boot | PMIC POR (power on, or the 10 s PWRBB reset) | LK initialises | Required long-press to release boot | Normal. |
| WDT EXRST reboot | TOPRGU SWRST (or WDT expiry) with `AUTO_RESTART` ("bypass power key") | LK re-initialises | Not required — self-boots | What `gemini-wdt-reboot` does; field-verified since 2026-08-31. |
| **Limbo (current `systemctl reboot`)** | arm64 `machine_restart` with no handler → boot CPU halted in `while(1)` | Dead (no LK) | **Dead** — PMIC still routes key events to the "running" AP | PMIC + panel logic keep drawing ≈1.6 W. Recovery: 10 s PWRBB reset or WDT EXRST. |
| True poweroff (target) | MT6351 `RTC_BBPU` = `KEY\|AUTO\|PWREN` (PWRBB pulled low) | Off | Works — cold boot | PMIC cuts main rails; RTC/charger/key-scan alive (µA-mA). Vendor "shutdown" = this write. |

## 3. Why the current behaviour is what it is (mainline v6.6, source-verified 2026-09-10)

Fetched from the `v6.6` tag (torvalds/linux):

- `arch/arm64/kernel/process.c:126` `machine_restart()`:
  `local_irq_disable(); smp_send_stop(); … do_kernel_restart(cmd);` and if
  that returns: `printk("Reboot failed -- System halted"); while (1);`
  — `do_kernel_restart()` (`kernel/reboot.c:223`) just calls the
  `restart_handler_list` notifier chain, which is **empty** in our kernel
  (nothing registers a handler). Secondary CPUs are already stopped by
  `smp_send_stop()` before the halt. **This is the limbo**: every CPU off,
  PMIC on, no LK, no panel init, power key un-serviced.
- `arch/arm64/kernel/process.c:110` `machine_power_off()`:
  `smp_send_stop(); do_kernel_power_off();` — `do_kernel_power_off()`
  (`kernel/reboot.c:639`) calls registered sys-off handlers; none exist.
  And it is never reached for `poweroff(2)`: `do_reboot()` gates
  `SYS_REBOOT_SHUTDOWN` on `kernel_can_power_off()`
  (`kernel/reboot.c:645`) = "a handler is registered or `pm_power_off` is
  set" → false → `-EINVAL`. **This is why shutdown is currently impossible.**
- The hooks we need exist in 6.6: `register_restart_handler()`
  (restart notifier, `kernel/reboot.c`) and
  `register_platform_power_off()` (`kernel/reboot.c`, exported; the legacy
  `pm_power_off` weak pointer is still wired through a sys-off shim at
  `kernel/reboot.c:618`). A driver using these makes `systemctl reboot` and
  `systemctl poweroff` work with zero userspace involvement.

Caveat: the `Reboot failed -- System halted` printk goes to the console
(`console=` on the bring-up cmdline is the UART, not a journalled tty), so
it may not appear in the journal — it is verifiable on the serial console.
The mechanism above is from source and matches every observed symptom.

## 4. What the vendor (Android) does on this exact SoC

### Reboot

LK reboots **via the WDT**, never via a bare CPU reset:

- `gemini-lk lk/platform/mt6797/mtk_wdt.c:338` `mtk_arch_reset(mode)` →
  `mtk_wdt_reset(mode)` (`:34`):
  1. `writel(0x1971, BASE+0x08)` — `WDT_RESTART` key
  2. `WDT_MODE`: clear `AUTO_RESTART(0x10)`, `IRQ(0x08)`, `ENABLE(0x01)`,
     `DUAL_MODE(0x40)`; then set `KEY(0x22000000)|EXTEN(0x04)|AUTO_RESTART(0x10)`
     (mode 1) — `AUTO_RESTART` is the "bypass power key" flag
     (`mtk_wdt.h:66`: `/* Reserved */` — repurposed; comment at
     `mtk_wdt.c:46`: "autoretart: 1, bypass power key")
  3. `udelay(100)`
  4. `writel(0x1209, BASE+0x14)` — `WDT_SWRST` key → **immediate external
     reset** (PMIC power-cycles the SoC; `EXTEN` = external reset enabled)
- All LK reboots call it: `lk/app/mt_boot/sys_commands.c:228,326,355,363`,
  `lk/app/mt_boot/mt_boot.c:215,230,1225,1441` — all with mode 1
  ("bypass pwr key when reboot").
- The vendor 3.18 kernel's WDT driver has the identical sequence:
  `GeminiPDA/repos/gemini-linux-kernel-3.18 drivers/watchdog/mediatek/wdt/mt6797/mtk_wdt.c:335-375`
  (`wdt_arch_reset`).
- WDT register map: `mtk_wdt.h:43-51` (MODE `+0x00`, LENGTH `+0x04`,
  RESTART `+0x08`, STATUS `+0x0C`, INTERVAL `+0x10`, SWRST `+0x14`);
  `project.h:657` `TOPRGU_BASE = 0x10007000`; keys `mtk_wdt.h:80,94`.
- LK leaves the WDT **running** for the kernel (`mtk_wdt_init`,
  `mtk_wdt.c:246`: mode `IRQ|EXTEN|DUAL|ENABLE|AUTO_RESTART`, 10 s, kicked
  by LK's loop) — IRQ mode, so its expiry is harmless to the kernel (which
  never kicks it). The kernel never touches the WDT today except our
  userspace scripts.
- Note (uncertainty): the vendor "proper" reboot path on other MTK SoCs is
  an SMC/SPM call in a `mt-plat-reboot` driver — **no such driver exists in
  any tree we have** (the 3.18 tree is stripped of it), so the SPM SMC
  numbers are unknown. The WDT SWRST path is the only reboot path verifiable
  from source here, and it is what LK itself uses.
- Encoding discrepancy (irrelevant to the SWRST design): LK's
  `mtk_wdt_set_time_out_value()` (`mtk_wdt.c:171`) scales seconds ×2048,
  while the field-verified userspace encoding is `(SECS<<5)|0x08` — 1 count
  = 1 s (`services/scripts/gemini-wdt-reboot`, verified 2026-08-31 and
  repeatedly since, incl. `cl2-up.sh`'s 15 s guard). The timer path is not
  used by the design below (SWRST resets immediately).

### Poweroff

Both LK and the vendor kernel power off by **pulling PWRBB low via the
PMIC RTC BBPU register**, reached over the same pwrap interface the mainline
kernel already uses:

- LK: `lk/platform/mt6797/mt_rtc.c:26-35` — `RTC_Read/Write` are
  `pwrap_read/pwrap_write` (same 16-bit address space as the kernel's pwrap
  regmap); `mt_rtc.c:109` `rtc_bbpu_power_down()`:
  1. `rtc_disable_2sec_reboot()` — clear `2SEC_EN(bit8)|AUTO_PDN_SEL(bit6)`
     in `RTC_AL_SEC` (`RTC_BASE+0x0018`), write trigger
  2. unlock: `RTC_PROT` (`RTC_BASE+0x0036`) ← `0x586a`, trigger; ← `0x9136`,
     trigger (`RTC_WRTGR = RTC_BASE+0x003c` ← 1)
  3. `RTC_BBPU` (`RTC_BASE+0x0000`) ← `KEY|AUTO|PWREN`
     = `(0x43<<8)|0x8|0x1` = **`0x4309`**, trigger
  with `RTC_BASE = 0x4000` (`mt_reg_base.h:473`) — i.e. PMIC addresses
  `0x4000/0x4018/0x4036/0x403c`. Bit meanings: `PWREN`(b0) "BBPU=1 when
  alarm occurs", `AUTO`(b3) "BBPU=0 when xreset_rstb goes low",
  `KEY`(b8-15) write key (`mt_rtc_hw.h:5-12,68,74-76,235-237,255`).
  Comment: "pull PWRBB low".
- LK's target poweroff (`lk/target/aeon6797_6m_n/power_off.c:11-27`
  `mt6575_power_off`): BBPU pwdn, then every 100 ms — if the charger is
  detected (or after ~1 s) → `mtk_arch_reset(0)` (WDT reset **without**
  bypass-power-key → the device enters the PMIC's off-mode-charging state,
  LK's `mt_kernel_power_off_charging.c:86` only boots to kernel on powerkey /
  WDT-bypass / 2-sec window). Without a charger the AP is dead — the loop
  never runs.
- Vendor kernel: `pm_power_off = mt_power_off`
  (`drivers/misc/mediatek/base/power/mt6797/mt_pm_init.c:620`);
  `mt_power_off` (`drivers/misc/mediatek/rtc/mtk_rtc_common.c:397`) does the
  same BBPU write (`hal_rtc_bbpu_pwdn` → `rtc_bbpu_pwrdown(true)`,
  `drivers/misc/mediatek/rtc/mtk_rtc_hal_common.c:138` — `RTC_BBPU =
  KEY|AUTO|PWREN`) plus the same chrdet→`machine_restart("charger")`
  fallback. Charger-detect register: PMIC `CHR_CON0` = `0x0F78`,
  `RGS_CHRDET` = bit 5 (`upmu_hw.h:968,11135-11137`).

So "Android shutdown on the Gemini PDA" == `RTC_BBPU = 0x4309` over pwrap,
with a WDT reset into off-mode-charging when USB is attached. That is the
behaviour to replicate.

## 5. Design — one small delta driver

New files in `devices/planet-geminipda/kernel/delta/`:

- `drivers/power/reset/mt6797-power.c` (+ `Kconfig`/`Makefile` entries in
  the base tree's `drivers/power/reset/`, `CONFIG_MTK6797_POWER=y` in the
  lean config)
- DTS node in `mt6797-gemini-pda.dts`:

  ```dts
  mtk6797_power: power@10007000 {
      compatible = "mediatek,mt6797-power";
      reg = <0 0x10007000 0 0x100>;            /* TOPRGU/WDT */
      mediatek,pmic = <&pwrap>;                /* MT6351 regmap (RTC space 0x4000+) */
  };
  ```

  **[corrected 2026-09-10]** The first cut called the
  `watchdog@10007000` node in `mt6797.dtsi` inert. It is not: mainline
  `mtk_wdt` binds it through the `mediatek,mt6589-wdt` compatible and
  **kicks the LK-armed watchdog**. So the driver must map the shared
  TOPRGU block with `devm_ioremap()` — *not*
  `devm_platform_ioremap_resource()`, which calls
  `devm_request_mem_region()`. `mt6797_power_driver_init` is linked
  before `mtk_wdt_driver_init`, so the claim made `mtk_wdt` fail
  `-EBUSY`, the watchdog went unkicked, and the SoC reset ~20 s into
  every boot (the flash looked like a brick). Both drivers map the
  shared block; only `mtk_wdt` claims it. See
  `docs/session-log.md` 2026-09-10e.

**Restart handler** (`register_restart_handler`, must not return):
replicate `mtk_wdt_reset(1)` verbatim:

```
writel(0x1971, base + 0x08)                      /* WDT_RESTART */
mode = readl(base + 0x00)
mode &= ~(0x10 | 0x08 | 0x01 | 0x40)             /* AUTO_RESTART,IRQ,ENABLE,DUAL */
mode |=  0x22000000 | 0x04 | 0x10                /* KEY|EXTEN|AUTO_RESTART */
writel(mode, base + 0x00)
udelay(100)
writel(0x1209, base + 0x14)                      /* WDT_SWRST -> immediate EXRST */
```

Result: PMIC power-cycle → BootROM → LK (panel re-init, rule-5 safe) →
self-boot (bypass-power-key). `systemctl reboot` works with no userspace
involvement; `gemini-wdt-reboot` stays as an independent fallback.

**Poweroff handler** (`register_platform_power_off`, must not return):

1. Over the pwrap regmap (16-bit reads/writes, same pattern as
   `mt6351-regulator.c`'s `dev_get_regmap(parent)`):
   - `0x4018` (RTC_AL_SEC): clear bits 8|6, write `0x403c`=1
   - `0x4036` (RTC_PROT) ← `0x586a`, `0x403c`=1; ← `0x9136`, `0x403c`=1
   - `0x4000` (RTC_BBPU) ← `0x4309`, `0x403c`=1
2. If still alive ~1 s later (charger attached keeps the AP up — the vendor
   situation): WDT SWRST **mode 0** (same sequence without `AUTO_RESTART`)
   → LK off-mode-charging (charges on USB; powerkey boots the OS). Never
   fall through to the arm64 WFI loop.

Result: `systemctl poweroff` = true powerdown on battery (PMIC quiescent +
key-scan only — the battery-guard's `exec systemctl poweroff`
(`services/scripts/battery-guard.sh`, CRIT < 3.50 V) finally does what it
says), and a defined off-mode-charging state on USB.

## 6. Risks / uncertainties (all on-glass testable)

1. **SWRST from the kernel is a new code path** — LK and the vendor kernel
   do the identical writes, but our 6.6 kernel has never done one.
   Mitigation: §7 test protocol (the 10 s PWRBB reset is always available as
   escape; a background-armed userspace WDT adds a second escape).
   **[resolved 2026-09-10]** — `systemctl reboot` self-boots cleanly.
2. **pwrap regmap → RTC space from the kernel is unexercised** — the kernel
   has only touched the PMIC main space (`0x0220`, `0x0a0c`, `0x0f78`); LK
   proves the same pwrap interface reaches `0x4000+`. Mitigation: the
   driver's probe should first *read* a known RTC register (e.g. the
   RTC second counter `0x401a`) and sanity-check it before any write.
   **[resolved 2026-09-10]** — probe reads `RTC_BBPU` = `0x000d`
   (reachable), and the BBPU write path shuts the unit down.
3. **Post-BBPU behaviour with USB attached is not verified on this unit.**
   We copy the vendor/LK fallback (WDT reset mode 0 after ~1 s if alive).
   Record actual behaviour in the test. **[resolved 2026-09-10]** —
   `systemctl poweroff` on USB takes the unit down (USB vanishes, no
   preloader/RNDIS, no loop); a full off-mode-charging display was not
   separately confirmed (screen not observed at the time).
4. **Why exactly the power key is dead in the limbo** is an inference
   (PMIC key routing assumes a live AP; STRUP auto-boot only follows POR /
   WDT-bypass resets). The observables (limbo after `reboot(2)`, 10 s combo
   recovers, WDT EXRST self-boots) are field-verified.
5. **Panic path not covered**: `machine_emergency_restart` does not go
   through the restart-handler chain, so a panic still ends in limbo.
   The 10 s combo / userspace WDT remain the recovery; wiring the emergency
   path is a follow-up.
6. `console=` is UART, so kernel halt messages may not reach the journal —
   verify on the serial console during testing.

## 7. On-glass test plan + results (WDT-escape protocol)

Prereq: build the kernel with the driver (delta + config), flash the
boot.img via `bin/flash-nixos.sh boot` with **para = boot-recovery** (TWRP
sticky) until verified; keep a `stock-dump/` boot backup current.

**Results (2026-09-10):** steps 1 and 2 pass; step 3 (poweroff on USB)
passes in the sense that the unit turns off (no limbo, no loop) but the
screen state was not observed. The first flash of the driver
boot-looped — root cause and fix in §5 / `docs/session-log.md`
2026-09-10e (shared TOPRGU block must be mapped without claiming it).
`gemini-wdt-reboot` is now marked fallback-only (its script header);
flipping `bin/device-reboot.sh` to a plain `systemctl reboot` over ssh is
a follow-up (its WDT-EXRST mechanism still works, so it was left alone
rather than changed untested).

1. **Reboot**: from the device, arm a userspace escape
   (`(sleep 60; busybox devmem 0x10007004 32 $((60<<5|8))) &`), then
   `systemctl reboot`. Expected: clean self-boot within ~5 s, panel
   initialised (no flicker — rule 5), journal starts fresh with
   "Restarting system" on the UART console. Failure → 10 s power+side.
2. **Poweroff on battery**: `systemctl poweroff`. Expected: device truly
   off (no backlight; VBAT flat for ≥5 min — no 1.6 W drain), power key
   cold-boots normally. Failure → 10 s combo.
3. **Poweroff on USB**: `systemctl poweroff` with a charger. Expected:
   off-mode charging (LK charging display / auto-charge, powerkey boots the
   OS) — record whatever actually happens; the mode-0 WDT fallback defines
   the acceptable worst case (a normal boot).
4. **Battery guard end-to-end**: with the driver in, let
   `gemini-battery-guard` reach CRIT on battery (or `BATTERY_GUARD_CRIT_MV`
   raised + fake supply) — confirm the guard's poweroff is a real powerdown.
5. After all pass: flip para back to NixOS default, retire
   `gemini-wdt-reboot` to "fallback only" in its header, and update
   `bin/device-reboot.sh` to plain `systemctl reboot` over ssh.

## 8. What this fixes downstream

- `systemctl reboot` — clean reboot, no limbo, no 10 s combo.
- `systemctl poweroff` — true powerdown; **battery safety** (guard's CRIT
  poweroff becomes real; an idle "off" unit no longer drifts below the safe
  voltage).
- The userspace WDT hack becomes a fallback, not the primary path.
- Prerequisite for any suspend/s2idle work (`docs/power-sleep.md`): a
  working poweroff handler is the same `register_platform_power_off` slot.

## Source index (all read 2026-09-10)

- LK fork: `/home/cjdell/Projects/GeminiPDA/repos/gemini-lk/` —
  `lk/platform/mt6797/{mtk_wdt.c,mt_rtc.c,mt_kernel_power_off_charging.c,
  mt_pmic.c}`, `lk/platform/mt6797/include/platform/{mtk_wdt.h,mt_rtc_hw.h,
  project.h,upmu_hw.h}`, `lk/target/aeon6797_6m_n/power_off.c`,
  `lk/app/mt_boot/{mt_boot.c,sys_commands.c}`
- Vendor 3.18 kernel: `/home/cjdell/Projects/GeminiPDA/repos/gemini-linux-kernel-3.18/` —
  `drivers/misc/mediatek/base/power/mt6797/mt_pm_init.c`,
  `drivers/misc/mediatek/rtc/{mtk_rtc_common.c,mtk_rtc_hal_common.c,
  mt6391/mtk_rtc_hal.c}`, `drivers/watchdog/mediatek/wdt/mt6797/mtk_wdt.c`
- Mainline v6.6 (fetched from tag): `arch/arm64/kernel/process.c`,
  `kernel/reboot.c`
- This repo: `devices/planet-geminipda/kernel/delta/drivers/soc/mediatek/
  mtk-pmic-wrap.c` (pwrap regmap, MT6351 slave),
  `devices/planet-geminipda/kernel/delta/arch/arm64/boot/dts/mediatek/
  {mt6797.dtsi,mt6797-gemini-pda.dts}`, `services/scripts/
  {gemini-wdt-reboot,battery-guard.sh}`, `docs/power-sleep.md`
