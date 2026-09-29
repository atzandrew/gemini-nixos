# Standing goals — long-running objectives the project has not closed

**Last updated:** 2026-09-29

This is the ledger of goals that are *not* one session's work: they stay
open across flashes, sessions and refactors until an acceptance test
passes on glass. Keep entries short and dated. The phase table
(`docs/mobile-nixos-port-feasibility.md` §9) is the feature roadmap; this
doc is for the persistent problems behind it.

---

## G1 — Stop chronic battery depletion: make the device truly sleep and truly power off

**Status:** 🔴 OPEN — long-standing (known since at least 2026-09-08,
re-affirmed 2026-09-26, **evidence 2026-09-29**: the unit draws **100 mA @
5 V ≈ 0.5 W** hours-to-days after `systemctl poweroff`, with VBAT flat at
4.00 V — i.e. it never switched off). The single most important goal for
the unit's physical survival.

**The goal.** The unit must be able to sit unused **without draining its
battery below the safe voltage**. Concretely: a `systemctl poweroff`, and
a real deep sleep, must each drop the pack to a near-zero/quiescent draw
and stay there indefinitely. Until that is true, the device is one
forgotten week away from a deep discharge.

**Why it matters.** The battery has repeatedly been run below the safe
voltage and has needed a **bench power supply** to bring the pack back up
to a voltage where the PMIC/preloader will start the unit. Deep
discharge of a Li-ion pack risks permanent capacity loss and, at the
extreme, a preloader-only "brick" state (see `docs/disaster-recovery/`).
The recovery path is awkward and not always guaranteed; the real fix is
to never let the unit sit there.

**Current state (what exists).**

- **Light sleep** (`gemcli sleep` / `gemini-sleepd`, silver button) is
  implemented and on glass (2026-09-08). It is a *userspace* light sleep:
  the kernel stays fully up. Measured floor **≈ 400 mA @ 4 V ≈ 1.6 W**
  — the LCD panel logic + TDDI stay powered (rule 5: the fbcon kernel
  cannot blank the panel) and the SoC rails never gate (bring-up cmdline
  `clk_ignore_unused pd_ignore_unused regulator_ignore_unused`). See
  `docs/power-sleep.md`. At that draw the battery is flat in days, not
  weeks.
- **`systemctl suspend`** is deliberately **disabled** (2026-09-11): it
  enters kernel `s2idle`, which has **no wake source** on this unit
  (PMIC side keys are *polled* over pwrap, not IRQ-driven), so it locks
  the system up. It is suppressed in `config/gemini.nix` and GNOME's
  power plugin is dconf-locked off. Deep sleep (a s2idle wake source) is
  documented as kernel follow-up work in `docs/power-sleep.md`.
- **`reboot` / `poweroff`** are *claimed* working (2026-09-10): a delta
  driver `drivers/power/reset/mt6797-power.c` registers LK's WDT SWRST
  restart + the MT6351 `RTC_BBPU = 0x4309` poweroff over pwrap. The
  on-glass verification for poweroff was **shallow** — it observed the
  USB gadget disappear with no preloader/NIC loop, which a merely-halted
  AP with its USB transceiver off can also produce. It did **not**
  measure battery current after shutdown. See "The suspicion" below.
- **2026-09-29 — the suspicion is now evidence, and *why* on-USB can
  never be a true off.** Two source-verified facts (full receipts:
  `docs/power-states.md` §"Implementation status" + `docs/session-log.md`
  2026-09-29):
  1. A measured **100 mA @ 5 V ≈ 0.5 W** continuous draw hours-to-days
     after `systemctl poweroff`, screen/USB-radio off, compute warm,
     VBAT flat at 4.00 V. That is an alive SoC (rails up, halted or
     LK-charging) — not a PMIC-quiescent off. **The unit never switched
     off.**
  2. **With a charger attached, LK cannot power off at all.** LK's
     `kernel_power_off_charging_detection()` sends any charger-present
     boot that lacks a power-key/WDT-bypass/2-sec flag into
     `KERNEL_POWER_OFF_CHARGING_BOOT`, which LK implements by **loading
     the `boot` image and jumping to the kernel**. On this unit that
     image is NixOS → "off-mode charging" boots Linux. (Observed live:
     TWRP's poweroff just boots Linux.) `off-mode-charge=0` and every
     `is_force_boot()` flag choose `NORMAL_BOOT`, never off, so there is
     no LK configuration that yields "off with a charger". The vendor is
     byte-for-byte the same design. **Therefore every poweroff measured
     while plugged in is confounded; only an on-battery measurement can
     test "off".**
  3. A residual puzzle: VBAT is flat at 4.00 V rather than slowly
     rising, which the IINLIM=500 mA power-path should allow from a
     ~100 mA load — i.e. the BQ25896 may be stuck in the known B-19/B-22
     "VBUS present but NOT charging" state. Check REG00/REG03/REG0B/
     REG12/REG13 with `services/scripts/bq25896-raw.sh`.

**The suspicion (why this is still OPEN).** We are **not convinced the
device has ever truly switched off**. A halted CPU with the PMIC still
powered looks identical to a real poweroff from the host side (USB
vanishes either way). The 2026-09-10 "verified" result is not sufficient
evidence that `RTC_BBPU` actually cut the main rails. If it did not, the
unit keeps burning hundreds of mW indefinitely with the screen "off"
(**measured 2026-09-29: 100 mA @ 5 V ≈ 0.5 W**; the light-sleep awake
floor is ~1.6 W) — exactly the chronic-depletion mechanism.

**What "done" looks like (acceptance criteria).**

1. **Poweroff is real.** After `systemctl poweroff` with **USB
   disconnected**, measured pack current is at the PMIC-quiescent level
   (mA, not W) and **VBAT is flat over ≥ 30 min** — or RTC_BBPU readback
   + a current probe on a bench supply confirms the rails are cut. The
   power key cold-boots the unit normally afterwards. (The 2026-09-10
   test must be redone with a measurement, not just "USB disappeared".)
2. **Deep sleep is real.** `systemctl suspend`/`mem` (or an equivalent
   kernel path) survives entry *and* resumes, waking on a defined source
   (the silver/ESC key via the PMIC INT → pwrap EINT route), and drops to
   the deep-sleep target **< 50 mA**. Then the 2026-09-11 suppression is
   lifted and the light-sleep pieces move into
   `systemd-suspend.service` ExecStartPre/Post.
3. **Bench-supply recovery is retired.** A normal user flow (sleep, then
   long idle, then wake) never approaches the 3.65 V warn / 3.50 V crit
   thresholds on its own.

**Next steps.**

- [ ] **Re-verify poweroff with a measurement — ON BATTERY, USB DATA
      DETACHED.** Put the unit on a bench supply (or an inline current
      meter); run `systemctl poweroff`; confirm the current collapses and
      VBAT holds. Short-circuit the suspicion first — everything else
      depends on it. **Do not test this on a charger: 2026-09-29 proved
      an on-USB poweroff is architecturally an off-mode-charging boot,
      not an off** (see "Current state").
- [ ] If poweroff does **not** cut the rails: instrument the driver
      (RTC_BBPU readback *after* the 0x4309 write — boot readback is
      `0x000d`, so the write must clear bit 2, `RTC_BBPU_BBPU` "0: power
      down"; and log whether the mode-0 WDT fallback actually executes)
      and re-check the pwrap RTC-space write path against
      `docs/power-states.md` §4/§5 receipts.
- [ ] **Because a true off on USB is impossible, make the VBUS-present
      idle state safe/charging.** Dump the BQ25896 (REG00/REG03/REG0B/
      REG12/REG13 via `services/scripts/bq25896-raw.sh`) and fix the
      "VBUS present but NOT charging" state, or "off-mode charging" still
      drains the pack while the unit appears off.
- [ ] Build the s2idle wake source: PMIC HOMEKEY/PWRKEY INT enable +
      pwrap `INT_EN`, and a wake-capable IRQ the `mt6351-keys` driver arms
      in suspend (`docs/power-sleep.md` §Deep sleep).
- [ ] Probe s2idle entry under a WDT-escaped test (a driver's `.suspend`
      may hang on this bring-up kernel); recover via ~30 s WDT EXRST or
      the 8-10 s power+side PMIC hardware reset.
- [ ] Once (1)+(2) pass: lift the suspend suppression, unify the silver
      button and logind onto one path, and update the acceptance results
      here with dates + measured numbers.

**Receipts / related docs.** `docs/power-states.md` (reboot/poweroff
driver + the shallow 2026-09-10 result), `docs/power-sleep.md` (light
sleep, the ~1.6 W floor, deep-sleep follow-up), `config/gemini.nix`
(suspend suppression), `services/scripts/battery-guard.sh` (3.65 V warn /
3.50 V crit orderly poweroff — currently the last line of defence),
`services/plumbing.nix` (UPower policy), `docs/disaster-recovery/`
(deep-discharge recovery).
