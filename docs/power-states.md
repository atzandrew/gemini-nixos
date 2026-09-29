# Power states of the Gemini PDA — reboot, poweroff, and the limbo state

**Last updated:** 2026-09-29 (G1 evidence: an on-USB poweroff is
**not** a true off — LK's POC trap boots the OS; the 2026-09-10
"poweroff verified" result is corrected below)

Original task (2026-09-10; **[status corrected 2026-09-29]**): `systemctl
reboot` left the device in a black-screen limbo (PMIC on, power key dead,
only a 10 s power+side-button hold recovered it) and `systemctl poweroff`
was impossible. We needed a real reboot (without the userspace WDT hack)
and a real powerdown (battery safety: an off device must not keep draining
below the safe voltage when USB is disconnected).

**Status:** **reboot is FIXED** (the delta driver's WDT SWRST restart
handler; `systemctl reboot` self-boots cleanly — on glass 2026-09-10, §
"Implementation status"). **poweroff is NOT fixed** — it is still an open
standing goal (G1): the 2026-09-10 result was not a true off, and a
2026-09-29 measurement found the unit alive (100 mA @ 5 V) after a
poweroff. `systemctl poweroff` on battery is unproven; on USB it is
structurally impossible (LK POC boots the OS). See below and
`docs/standing-goals.md` G1.

## TL;DR (the 2026-09-10 pre-implementation analysis; see "Status" above for what was actually built)

- The limbo is **arm64 mainline behaviour, not a bug in our config**:
  Linux 6.6 arm64 `machine_restart()` with no registered restart handler
  prints `Reboot failed -- System halted` and halts the boot CPU in
  `while(1)`. The PMIC stays fully powered (≈1.6 W floor, per
  `docs/power-sleep.md`), LK never re-runs (so the panel is uninitialised —
  black screen, no backlight), and the power key is routed to the dead AP —
  hence dead. Only the PMIC's hardware long-press reset (10 s power+side) or
  the WDT EXRST recover it.
- Poweroff failed earlier: `poweroff(2)` was **refused** (`-EINVAL`) because
  `kernel_can_power_off()` was false — no poweroff handler existed. A
  handler now exists, so the call is accepted; whether it truly cuts the
  rails is the open G1 question (see "Status" and `docs/standing-goals.md`).
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
- `systemctl poweroff` → the USB gadget disappears with no
  preloader/gadget-NIC and no loop (no limbo). [observed 2026-09-10;
  that run's NIC enumerated as RNDIS — the gadget is CDC-ECM since
  2026-09-17, docs/usb-network.md]
  - **⚠️ [corrected 2026-09-29] This was NOT evidence the unit switched
    off.** "USB went silent" is equally produced by an orderly shutdown
    followed by a halted AP with its USB transceiver off (the limbo
    class). A 2026-09-29 measurement on the same class of state found
    the unit drawing **100 mA @ 5 V ≈ 0.5 W** continuously with VBAT
    flat at 4.00 V — i.e. **alive, not off**. Worse, with USB attached a
    true off is *architecturally impossible*: our fallback's mode-0 WDT
    reset hands over to LK, and LK sends any charger-present boot into
    `KERNEL_POWER_OFF_CHARGING_BOOT`, which **loads the `boot` image and
    boots a kernel** (`gemini-lk lk/platform/mt6797/platform.c:801`,
    `lk/app/mt_boot/mt_boot.c:1490`; the vendor `mt_power_off` →
    `machine_restart("charger")` → `wd_sw_reset(0)` does the same). On
    this unit that kernel is NixOS, so "off-mode charging" boots Linux —
    which is exactly what TWRP's poweroff does. **Only an on-battery
    measurement can test "off".** Full receipts: docs/session-log.md
    2026-09-29.
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
| **Limbo (pre-fix `systemctl reboot`, historical)** | arm64 `machine_restart` with no handler → boot CPU halted in `while(1)` | Dead (no LK) | **Dead** — PMIC still routes key events to the "running" AP | PMIC + panel logic keep drawing ≈1.6 W. Recovery: 10 s PWRBB reset or WDT EXRST. Fixed by the restart handler (2026-09-10). |
| True poweroff (target) | MT6351 `RTC_BBPU` = `KEY\|AUTO\|PWREN` (PWRBB pulled low) | Off | Works — cold boot | PMIC cuts main rails; RTC/charger/key-scan alive (µA-mA). Vendor "shutdown" = this write. |

## 3. Why that limbo happened (mainline v6.6, source-verified 2026-09-10; fixed for `reboot`, still true for the *unproven* poweroff)

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
   (reachable). [corrected 2026-09-29] Reachability is proven; that the
   *write* actually clears `RTC_BBPU_BBPU` bit 2 (power down) is still
   unverified — see `docs/session-log.md` 2026-09-29.
3. **Post-BBPU behaviour with USB attached is not verified on this unit.**
   We copy the vendor/LK fallback (WDT reset mode 0 after ~1 s if alive).
   Record actual behaviour in the test. **[resolved 2026-09-10]** —
   `systemctl poweroff` on USB made USB vanish (no preloader/RNDIS, no
   loop); a full off-mode-charging display was not separately confirmed.
   **[re-opened + corrected 2026-09-29]** "USB vanished" does **not**
   prove off. With USB attached this fallback hands to LK, and LK's POC
   path **boots the `boot` image in charger mode** — i.e. an on-USB
   poweroff is an off-mode-charging *boot*, not an off (and on this unit
   it boots NixOS). Measured state days after one such poweroff: 100 mA
   @ 5 V, VBAT flat — alive. See §7 and `docs/session-log.md` 2026-09-29.
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

**Results (2026-09-10):** step 1 passes (reboot self-boots). Step 2
(poweroff on battery) was **not actually run with a measurement**. Step 3
(poweroff on USB) was recorded as "the unit turns off", but **[corrected
2026-09-29] that was not a true off** — with a charger attached LK routes
the fallback reset into POC and *boots a kernel*; the "USB vanished"
observation is also produced by a halted AP. The 2026-09-29 100 mA
measurement confirms the unit stayed alive. A valid poweroff test must be
**on battery with USB detached, and measured** (see §7 step 2 and
`docs/session-log.md` 2026-09-29). The first flash of the driver
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
2. **Poweroff on battery**: `systemctl poweroff` **with USB data
   detached**. Expected: device truly off (no backlight; VBAT flat for
   ≥5 min — no 1.6 W drain), power key cold-boots normally. **This is the
   only meaningful poweroff test** [2026-09-29]. Failure → 10 s combo.
3. **Poweroff on USB**: `systemctl poweroff` with a charger. Expected:
   off-mode charging (LK charging display / auto-charge, powerkey boots the
   OS) — record whatever actually happens. **[corrected 2026-09-29] Do
   not read "off" from this**: LK's POC path *boots the kernel* (here =
   NixOS) whenever a charger is present, so the observable is a normal
   boot, not an off. The mode-0 WDT fallback's "acceptable worst case"
   is exactly this boot. Only step 2 tests off.
4. **Battery guard end-to-end**: with the driver in, let
   `gemini-battery-guard` reach CRIT on battery (or `BATTERY_GUARD_CRIT_MV`
   raised + fake supply) — confirm the guard's poweroff is a real powerdown.
5. After all pass: flip para back to NixOS default, retire
   `gemini-wdt-reboot` to "fallback only" in its header, and update
   `bin/device-reboot.sh` to plain `systemctl reboot` over ssh.

## 8. What this fixes downstream

- `systemctl reboot` — clean reboot, no limbo, no 10 s combo.
- `systemctl poweroff` — *intended* true powerdown; **battery safety**
  (guard's CRIT poweroff becomes real; an idle "off" unit no longer drifts
  below the safe voltage). **[corrected 2026-09-29] Not yet demonstrated —
  see §6 item 3 / §7: an on-USB poweroff is a POC boot, and the on-battery
  poweroff has not been measured.**
- The userspace WDT hack becomes a fallback, not the primary path.
- Prerequisite for any suspend/s2idle work (`docs/power-sleep.md`): a
  working poweroff handler is the same `register_platform_power_off` slot.

## 9. Making `poweroff` real — instrumented next-session plan (2026-09-29)

Objective: make `systemctl poweroff` **on battery, USB detached** actually
cut the rails (G1 acceptance #1). The on-USB case stays "off-mode charging"
(§2/§6) and is out of scope for "off".

What we know going in (all source-verified — see the source index):
- the BBPU **read** path works (probe reads `0x000d`); the **write** has
  never been read back, so we do not know whether it sticks.
- the driver's poweroff = BBPU write → `mdelay(1000)` → **mode-0 WDT reset**
  → `while(1)` (`mt6797-power.c:138-179`). The mode-0 reset has never been
  independently observed to run.
- with a charger the mode-0 reset lands in LK POC, which **boots the
  kernel** (so it is a boot, not an off).
- the boot readback `0x000d` is power-**on** (`RTC_BBPU_BBPU` bit 2 = 1); a
  successful poweroff must leave bit 2 = 0.

**Step 0 — characterise the current "off" state** (cheap, no flash): with
the unit in the observed ~100 mA state, note (a) is the screen lit?
(b) does it enumerate on a host as `0e8d:2008` (LK POC) or nothing? (c) does
the power key boot it, do nothing, or need a 10 s hold? This distinguishes
**(A) LK POC / charging mode** from **(B) halted-AP limbo** (session-log
2026-09-29).

**Step 1 — instrument the driver** (a build, not necessarily flashed yet):
- read `RTC_BBPU` back immediately after the `0x4309` write and `dev_info`
  it (did bit 2 clear?);
- `pr_emerg`/log at entry to `mt6797_power_off` and just before the mode-0
  WDT fallback (did we reach it? did the 1 s elapse?);
- leave the restart handler untouched.

**Step 2 — on battery (USB data detached), the three failure modes separate:**
- low/quiescent current + VBAT flat → the write worked → off, done;
- still alive → the BBPU write did not stick (or was overwritten) → Step 3;
- reality check: on battery LK's own `kernel_power_off_charging_detection()`
  → `mt6575_power_off()` is a second chance — it re-writes BBPU and, after
  ~1 s, does `mtk_arch_reset(0)`. So a true off on battery *should* be
  reachable even if our kernel's write fails; if it is not, the reset/BBPU
  path itself is broken, not just our ordering.

**Step 3 — if the BBPU write does not stick**, try the vendor extras
(candidates, not proven necessary — LK omits them):
- `hal_rtc_bbpu_pwdn` drives **SRCLKENA/SRCLKEN_IN GPIO low** and disables
  the **32 K export** before the BBPU write
  (`mt6351/mtk_rtc_hal.c:180`);
- the vendor warm-reset calls `pmic_pre_wdt_reset()` (PMIC sleep-mode buck
  voltages) before SWRST (`power/mt6797/pmic.c:203`);
- re-check the pwrap RTC-space write path (`mtk-pmic-wrap.c`) — confirm the
  regmap **write** actually reaches `0x4000+` (the read does).

**Step 4 — USB case (not "off"):** make "off-mode charging" actually charge,
so a plugged-in unit does not flat-line. Dump BQ25896 REG00/REG03/REG0B/
REG12/REG13 with `services/scripts/bq25896-raw.sh` while it is in that state.

Build/flash/safety: `bash bin/build.sh start bootimg` (rule 10; on macOS it
builds in the container VM), flash **only** the boot partition with
`bin/flash-nixos.sh boot` (keep `para = boot-recovery` sticky until
verified), keep `stock-dump/` boot backups, obey panel rule 5, run long ops
under `bin/run-job.sh`, and keep the 10 s power+side hold as the
always-available escape.

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

**[2026-09-29 additions]** LK: `mt6797/boot_mode.c:254` (POC dispatch),
`app/mt_boot/mt_boot.c:1490` (POC loads the boot image → boots the kernel),
`platform/mt6797/platform.c:801` (POC turns the display on),
`platform/mt6797/atags.c:921` (`mode=charger`). Vendor:
`rtc/mt6351/mtk_rtc_hal.c:180` (`hal_rtc_bbpu_pwdn`: SRCLKENA-low + 32K
export), `watchdog/mediatek/wdk/wd_api.c:583` (`arch_reset("charger")` →
`wd_sw_reset(0)`), `power/mt6797/pmic.c:203` (`pmic_pre_wdt_reset`),
`include/mt-plat/mt6797/include/mach/mt_rtc_hw.h` (`RTC_BBPU_BBPU` = bit 2
"1: power on, 0: power down"). Legacy receipts: GeminiPDA session-log
2026-08-30 (POC trap) + 2026-09-04 (`shutdown -r` = power off).
