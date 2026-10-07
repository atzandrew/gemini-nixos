# SoC thermal sensor (MT6797) — 2026-10-07

Last updated: 2026-10-07. Prerequisite for A72 DVFS, GPU devfreq and higher
A53 caps (docs/cpu-dvfs.md).

## Status

| Step | State |
|---|---|
| Driver ported (`thermal1-soc-sensor.patch`), compile-tested (clang, W=1, no warnings), DTB builds | done 2026-10-07 |
| Conversion checked bit-exact against the vendor formula (7.8 M raw/calibration combinations) | done 2026-10-07 |
| `bin/gemini-thermal-check.sh pre` on the device (LK devinfo + calibration valid) | pending |
| First boot: zone reads plausibly (ixoo saw 32–35 °C idle on his unit) | pending |
| Load test: temperature rises under load, falls after; 85 °C trip throttles the A53s | pending |

## What the patch adds

- **`drivers/thermal/mediatek/auxadc_thermal.c`** (copy of 6.6.157 + MT6797,
  `MTK_THERMAL_V4`): mainline `mediatek,mt8173-thermal` driver family with
  MT6797 data. Six banks via PTPCORESEL (vendor bank table: 0 big/A72 =
  TS_MCU1, 1 GPU = TS_MCU4, 2 SoC = TS_MCU2+3, 3 L, 4 LL, 5 CCI = TS_MCU2),
  AUXADC channel 11, vendor timing (AHBPOLL 0x30d, MSRCTL0 0x492 = 2-of-4
  filter, valid mask 0x2c), TS_CON1[5:4] cleared (sensor buffer on), mux
  values 0..3 + 0x10 (ABB). Init in the vendor order (pause + disable all
  banks, wait THAHBST0 idle, AUXADC ch11 off, program banks, ch11 on, enable
  sensing points, release pause). One thermal sensor = hottest of all banks
  (like the vendor `mtktscpu` zone). Refuses to probe without valid
  calibration (the defaults read far off). Logs one line:
  `calibration: ge … oe … degc … slope … vts …`.
- **Conversion** (`raw_to_mcelsius_v4`) = vendor `raw_to_temperature_roomt()`:
  gain from ADC GE, offset OE − 512, per-sensor VTS, slope 1663 ± 10·O_SLOPE,
  ×15/18. Includes ixoo's 2026-09-05 corrections (OE − 512 in both terms,
  VTS by sensor id via `vts_index`).
- **Calibration source: `drivers/nvmem/mtk-lk-devinfo.c`**
  (`mediatek,mt6797-lk-devinfo`, `CONFIG_NVMEM_MTK_LK_DEVINFO=y`). LK copies
  the efuse devinfo words into `/chosen/atag,devinfo` (103 LE words: size,
  tag 0x41000804, 100 words, count). The vendor kernel reads its
  calibration from that copy, never from the efuse block. The provider
  exposes the payload (byte offset = 4 × index); thermal cell = indexes
  31..33 = efuse 0x10206180/84/88 = `reg <0x7c 0xc>`. The MT6797 layout is the
  mainline V1 (MT8173) fuse layout plus ADC OE in word 1 bits 21:12.
- **Reset: `clk-mt6797.c` registers the INFRACFG_AO reset controller**
  (set/clear pairs 0x120/0x130/0x140/0x150, like MT8183);
  `include/dt-bindings/reset/mt6797-resets.h`: thermal = 0 (RST0 bit 0,
  vendor `tscpu_reset_thermal()`), PMIC wrap = 64 (not used).
- **DT**: `auxadc` node (disabled — channel 11 belongs to the thermal
  controller), `thermal` node, `#reset-cells` on infracfg, `#cooling-cells`
  on all CPUs; board: `/firmware/lk-devinfo`, zone `soc-thermal` (poll 1 s,
  passive 250 ms), trips **passive 85 °C** (step_wise → cpufreq cooling of
  policy0 and policy4) and **critical 105 °C** (orderly poweroff).
- **Config**: `MTK_THERMAL=y`, `MTK_SOC_THERMAL=y`, `NVMEM_MTK_LK_DEVINFO=y`
  (RESET_CONTROLLER and NVMEM_SYSFS were already on via select/default).
  Resolved-config diff = exactly these three built-in symbols, so the
  userdata modules still match (boot-only flash).

## Not done (deliberately)

- No IRQ, no hardware protection (TEMPPROT → RGU reset at 117 °C in the
  vendor kernel). Needs the watchdog's thermal-reset mode; Linux's critical
  trip is the protection for now.
- No per-bank zones (mainline driver has one sensor). An A72-only zone
  (bank 0) would be a later addition if the max-of-all zone throttles too
  eagerly.
- EEM/PTP (shares PTPCORESEL) is not used by our kernel.
- Standalone IIO AUXADC stays disabled (needs arbitration with thermal).

## Credits / sources

- ixoo/gemini-pda-mainline (Linux 7.1.3): MT6797 V4 data, LK devinfo
  calibration idea and wire format, V4 corrections (patches 0057, 0057a,
  0512, 0517, 0541; experiments `2026-07-13-mt6797-thermal-recovery`,
  `2026-09-03-mt6797-thermal-auxadc-transaction-audit`,
  `2026-09-04-mt6797-thermal-snapshot`). Their read-only observation passed
  on their Gemini (corrected V4: 32–35 °C idle across all banks).
- Vendor kernel gemian/gemini-linux-kernel-3.18:
  `drivers/misc/mediatek/thermal/mt6797/src/mtk_tc.c`,
  `.../mt6797/inc/tscpu_settings.h`, `.../common/thermal_zones/mtk_ts_cpu.c`
  (`init_thermal()`, `tscpu_thermal_initial_all_bank()`), trips in
  `.../mt6797/inc/tzcpu_initcfg.h` (117 sysrst / 95 / 85 / 65 °C).

## Test

On the Gemini (`to_gemini bin/gemini-thermal-check.sh`):

1. `sh gemini-thermal-check.sh pre` — before flashing. Must say
   `calibration VALID`.
2. After booting the thermal image: `sh gemini-thermal-check.sh post` —
   probe messages, `soc-thermal` temperature, trips, cooling devices.
3. `sh gemini-thermal-check.sh load 120` — temperature every 2 s under full
   load + 60 s cooldown. Expect a rise of several °C and a fall afterwards.
   Trip test without heat: `echo 90000 | sudo tee
   /sys/class/thermal/thermal_zoneN/emul_temp` → LL/L should step down;
   `echo 0 | sudo tee …/emul_temp` to end (CONFIG_THERMAL_EMULATION=y).
