# Stock MT6797 suspend vs `mt6797-suspend` (research 2026-10-20)

Source: the vendor 3.18 kernel (github.com/gemian/gemini-linux-kernel-3.18:
`drivers/misc/mediatek/base/power/spm_v2/mt_spm_sleep.c`, `mt_spm.c`,
`mt_spm_internal.c`, `mt_spm_pmic_wrap.c`, `lcm/aeon_*`,
`arch/arm64/boot/dts/aeon_gpio.dtsi`), compared with
`devices/planet-geminipda/kernel/modules/mt6797-suspend` (c80be76), plus
read-only register reads on the Gemini (2026-10-20). Nothing below has been
tested yet. Status and the hang data: [`spm-sleep.md`](spm-sleep.md).

## Panel: stock removes the bias supply in suspend
- Stock `lcm_suspend` (the SSD2092 and NT36672 drivers both do it): DCS 0x28
  (display off), 50 ms, DCS 0x10 (sleep in), 120 ms, panel reset low, then the
  LCD bias supply off: ENP = GPIO60, ENN = GPIO251 (`aeon_gpio.dtsi`, driven
  low). Resume = bias on, reset, full init table.
- Ours: DCS 0x28 only. GPIO60 and GPIO251 read output-high (bias on) while
  awake, and nothing changes them for sleep. So the LCD sits biased and
  unrefreshed through every sleep and every hang. This matches the screen
  burns (long sleeps, hangs, long display-off series) and is the strongest
  lead for them.
- A real panel-off needs a panel re-init on resume, which today only LK does
  (CORE RULE 5; DCS 0x10 alone leaves the panel black until a reboot).

## Safety net: the PCM watchdog
- Stock arms the SPM's PCM watchdog for every suspend: `PCM_WDT_VAL =
  PCM_TIMER_VAL + 30 s`, `PCM_CON1.PCM_WDT_EN`, wake mode off
  (`__spm_set_pcm_wdt(1)`), and sets the TOPRGU SPM request to reset mode
  (`wd_spmwdt_mode_config(WD_REQ_EN, WD_REQ_RST_MODE)`). It is disabled
  again after the wake.
- Ours: never armed (live `PCM_CON1 = 0x6C00`, bit 8 clear). TOPRGU is
  already set to reset on the SPM request (`REQ_MODE = 0x00380003`, bit 1;
  `REQ_IRQ_EN` bit 1 clear).
- Expected effect if armed: a stuck sleep resets the SoC ~30 s late without a
  power-hold, with any cycle length, and doesn't need the 31 s TOPRGU WDT.
  Not yet verified, including whether NONRST_REG2 survives.

## Other stock suspend steps we skip (hang leads, unranked)
1. **Thermal chunks.** `SPM_THERMAL_TIMER = 23`: stock never runs one PCM
   period longer than 2^23 ticks (256 s). A longer sleep is split inside the
   firmware: `SPM_PASR_DPD_3` (0x1000663C) = number of chunks,
   `SPM_SW_RSV_2` (0x10006610) / `SPM_BSI_D0_SR` (0x1000641C) = Vcore
   temperature window (0–70 °C as thermal-ADC values), `SPM_SCP_MAILBOX`
   (0x10006098) counts chunks. `pcm_flags` bit 19 (0x80000) is set only when
   an efuse disables this. Ours: `flags = 0x5fc102ff` (bit 19 clear), and
   all four registers read 0.
2. **PMIC-wrapper SPM command table** (`DVFS_ADR/WDATA` 0–15 at 0x1000D10C).
   Stock loads its SUSPEND phase before sleep (Vcore sleep 0.6/0.7 V,
   VSLEEP_SEL, VSRAM_CORE, Vcore low-IQ) and NORMAL after. Ours: all 32 words
   read 0, and the SPM's arbiter channel is on (`HIPRIO_ARB_EN = 0x3FFF`,
   bit 4 DVFSINF/SPMINF). Any table command the firmware issues therefore
   writes 0 to MT6351 register 0x0000 (STRUP_CON0). Our flags set
   DIS_VCORE_DVS/DFS, which probably keeps the firmware from issuing them
   (unconfirmed).
3. **PMIC preparation.** `__spm_pmic_low_iq_mode(1)` (VGPU_ANA_CON7 bit 1);
   `__spm_pmic_pg_force_on` (STRUP_DIG_IO_PG_FORCE = 1,
   RG_STRUP_VIO18_PG_ENB = 1, so the VIO18 power-good monitor is off in sleep);
   `spm_pmic_power_mode(PMIC_PWR_SUSPEND)`; `clk_buf_write_afcdac`.
4. **IRQs and clocks.** `mt_irq_mask_all` + CIRQ clone (latches edge IRQs
   while the GIC is off); `spm_set_sysclk_settle` (3 ms); UART sleep
   handshake; BSI top init and PMIC clock power-down backup when
   MD/C2K/CONN are off.
5. **DRAM.** Stock suspend puts DRAM in self-refresh; ours keeps it active
   (DDREN blocked on connsys, see `spm-sleep.md`). The vendor DRAMC driver
   does no periodic software upkeep, so a missed DRAM job is unlikely.
- **Wake period.** Stock takes it from the fuel gauge (time per 1 % battery,
   at most 36 h), so one suspend is one long sleep. The same firmware is
   known to sleep for hours when set up the stock way.

## Still unexplained
- "woke: pmic read failed after 1 cycles of 60000 ms" (2026-10-20 12:40:52).
- High drain with lid EINT armed (1 % gauge only).

## Documents and other sources
- 96boards (public, for the Helio X20 dev board):
  <https://www.96boards.org/documentation/consumer/mediatekx20/additional-docs/>
  - *MT6797 Functional Specification* v1.0 (291 pp): pins, electrical, EINT
    (3.2), TOPRGU (4.2), PMIC wrapper (4.3), GPIO, DSI.
  - *MT6797 Register Table* Parts I and II (1637 + 1460 pp): TOPRGU, PWRAP
    (including the arbiter and DVFS table), EINT, GPIO, clocks, infracfg.
    **No SPM block and no PMIC.**
- MT6351 PMIC datasheet: not public (MediaTek NDA). The register map is in the
  vendor `upmu_hw.h`.
- No mainline or community MT6797 SPM/suspend implementation was found.
  TF-A's MT8195 SPM suspend (C, in ATF) is a modern MediaTek reference for a
  different SoC.
- Other trees: planet-community/android_kernel_planetcom_mt6797 (Planet);
  android.googlesource.com kernel/mediatek `android-mtk-3.18` (spm_v1/v2,
  mt8173).
