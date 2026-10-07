# CPU clocks and voltages (DVFS) on the Gemini (MT6797X)

Started 2026-10-04. Status: **step 1 survey DONE (2026-10-04 19:12)** —
clocks, PLLs, VSRAM and the speed bin are known; the DA9214 bucks can't be
read reliably yet (step 1b, `bin/da9214-read.sh`). Nothing has been
written to any clock or voltage register.

## Where we start

- No cpufreq and no cpuidle driver: every core runs at whatever LK / ATF
  left it at, all the time (also when idle).
- Shell-loop benchmark (2026-10-03): CPU0 (A53) 19.9 s, CPU4 (A53)
  13.5 s, CPU9 (A72) 13.1 s. Same core type, so CPU0-3 run ~1.47x slower
  than CPU4-7; the A72s are far below their rating.
- Login and app start are CPU-bound (eMMC is HS200 at ~150 MB/s now).
- No SoC thermal driver (no temperature reading, no throttling).

## Hardware map (vendor 3.18 kernel)

| Cluster | CPUs | Core | Clock source | Voltage rail | SRAM rail |
|---|---|---|---|---|---|
| LL | 0-3 | A53 | ARMCAXPLL0 (MCUMIXED 0x1001A200) | **VPROC1** = DA9214 BUCKA (reg 0xD7) | VSRAM_L = CPULDO (INFRACFG_AO 0x10001F98..FA0) |
| L | 4-7 | A53 | ARMCAXPLL1 (0x1001A210) | **VPROC1** (shared) | VSRAM_L (shared) |
| CCI (MCSI bus) | — | — | ARMCAXPLL2 (0x1001A220) | **VPROC1** (shared) | VSRAM_L |
| B | 8-9 | A72 | own "iDVFS" PLL at 0x102224a0/a4, **only via ATF SMCs** | VPROC2 = DA9214 BUCKB (reg 0xD9) | A72 SRAM LDO (SMC 0xC20003BF) |

- DA9214 sits on i2c6 (0x1100e000), address 0x68. Voltage = 300 mV +
  n x 10 mV (7 bits). Vendor writes only VBUCKA_A (0xD7) / VBUCKB_A (0xD9);
  which of the A/B set is live is BUCKx_CONT (0x5D/0x5E) bit4, possibly
  pin-controlled (bits 6:5) — the survey reads both.
- **LL, L and CCI share one rail.** VPROC1 must always be at least the
  highest voltage any of the three needs (vendor `_calc_new_cci_opp_idx`:
  VPROC1 = max(V_LL, V_L, V_CCI); CCI target = max(cluster freq)/2).
- ARMPLLDIV_MUXSEL (0x1001A270): 2-bit source per cluster (B 1:0, LL 3:2,
  L 5:4, CCI 7:6): 0 = 26 MHz, 1 = ARMPLL, 2 = MAINPLL, 3 = UNIVPLL.
  ARMPLLDIV_CKDIV (0x1001A274): 5-bit divider per cluster (B 4:0, LL 9:5,
  L 14:10, CCI 19:15): 8 = /1, 9 = 3/4, 10 = /2, 11 = /4, 17 = 4/5,
  18 = 3/5, 19 = 2/5.
- ARMCAXPLLn_CON1: bit31 CHG (apply), 26:24 posdiv (0 = /1, 1 = /2,
  2 = /4), 20:0 DDS. VCO = DDS x 26 MHz / 2^14. f = VCO / posdiv x ckdiv.

### Access protocol for 0x1001Axxx (important)

Vendor comment: "Everest 0x1001AXXX bus access issue. All access
0x1001AXXX driver should use the lock". ATF, the CPU-DVFS processor (CSPM)
and the kernel must never touch MCUMIXED at the same time. Vendor
`mt6797_0x1001AXXX_lock()`: IRQs off + spinlock, take **HW semaphore 3,
master 0** (write 1 to 0x11015440, read bit0 back; 2 ms timeout), 200 ns
delay before reads/after writes, release by writing 1 again. Owners:
SEMA3_M0 0x440 = kernel/FH, M1 0x444 = CSPM, M2 0x448 = ATF. The vendor
also writes 0x0b160001 to CSPM_POWERON_CONFIG_EN (0x11015000, "enable
internal CG bit") once at core_initcall and before every lock.

### How Android did it: "hybrid" DVFS

`mt_cpufreq_hybrid.h`: on MT6797 `CONFIG_HYBRID_CPU_DVFS` is on
(`ENABLE_IDVFS` in mt_cpufreq.h). The kernel loads a firmware blob
(`mt_cpufreq_hybrid_fw.h`) into the **CSPM** (a PCM sequencer at
0x11015000) and only sends it OPP indices; the CSPM does the PLL changes,
the DA9214 I2C writes (it drives i2c6 itself, with a semaphore against the
kernel's I2C driver) and the VSRAM tracking. The A72 PLL goes through ATF
(iDVFS SMCs). On our 6.6 kernel nothing loads that firmware, so the CSPM
should be idle — **the survey checks this** (PCM_FSM_STA bit21). The
non-hybrid path is still in the vendor source and is what we follow:

### Vendor DVFS sequence (non-hybrid path, mt_cpufreq.c)

`_cpufreq_set_locked()`:
1. **Voltage up first** (if the target needs more): `set_cur_volt_extbuck()`.
2. Frequency (`set_cur_freq()`), in this order:
   - posdiv increase (1 -> 2) before, clkdiv increase before;
   - new DDS: via MCU FHCTL hopping (LL/L/CCI, `mt_dfs_armpll`), or
     without hopping (`adjust_armpll_dds`): switch the cluster's mux to
     **MAINPLL** (CLK_MISC_CFG_0[5:4] = 3, MUXSEL field = 2), write CON1 =
     posdiv | DDS | CHG, wait 20 µs, switch back to ARMPLL (field = 1,
     CLK_MISC_CFG_0[5:4] = 0);
   - clkdiv decrease after, posdiv decrease after.
   - Sync DCM (MCUCFG 0x10220744): lowered before a frequency drop,
     raised after a rise (div = MHz/70). Leaving it unchanged on a rise is
     the conservative side.
3. CCI frequency/voltage.
4. **Voltage down last** (if the target needs less).

While a cluster runs from MAINPLL during the switch, VPROC1 must also
cover MAINPLL's rate (vendor raises B to its OPP 9 voltage for this).

`set_cur_volt_extbuck()` VSRAM tracking (all in mV): VSRAM = VPROC + 100,
clamped 1000..1200; invariant VSRAM >= VPROC and VSRAM - VPROC <= 300.
Going up: VSRAM first, then VPROC, in steps of at most 275 mV. Going down:
VPROC first, then VSRAM. Settle: DA9214 ~1 µs per 10 mV; LDO up
(Δ/12.5 mV + 5) µs, down (Δ x 2 / 6.25 mV + 5) µs.

CPULDO (VSRAM_L): CTRL_0[7:0] = 0xFF (8 LDOs on), CTRL_1 and CTRL_2 hold
four 4-bit vosel fields each (bits 3:0, 11:8, 19:16, 27:24), all set to the
same value: 0 = 1.05 V, 1 = 0.6 V, 2 = 0.7 V, n >= 3 = 0.9 V + (n-3) x
25 mV.

### Speed bin (which table)

`_mt_cpufreq_get_cpu_level()`: eFuse devinfo[22] bits 3:0 (function code
1): 0/3 -> level 0 "FY", 1/6 -> level 1 "SB", 4 -> level 1 + "TT segment"
(A72 OPP0/1 = 2587/2431 MHz), 2/7 -> level 2 "M", 15 -> level 3 "L". A DT
A72 `clock-frequency` of 1989 MHz forces level 2. A72 tables also depend on
the date code: devinfo[61] bits 7:4 < 7 -> "1221", else "0119". LK passes
the devinfo words in `/chosen/atag,devinfo`; `bin/cpu-clocks.sh` decodes
them. (The vendor DTB's per-cluster clock-frequency values — 1391 / 1950 /
2288 MHz — are LK fix-up placeholders, not the bin.)

### OPP tables (sign-off voltages, before EEM/PTP lowering)

The vendor's EEM ("PTP") driver lowers these per chip at runtime from
eFuse calibration; we use the unadjusted table values (more margin).

Level 0 (FY) — LL / L / CCI on VPROC1:

| idx | LL MHz | L MHz | CCI MHz | VPROC1 mV |
|---|---|---|---|---|
| 0 | 1391 | 1846 | 923 | 1200 |
| 1 | 1339 | 1781 | 884 | 1180 |
| 2 | 1287 | 1703 | 858 | 1150 |
| 3 | 1222 | 1625 | 819 | 1130 |
| 4 | 1118 | 1495 | 754 | 1090 |
| 5 | 1092 | 1417 | 715 | 1070 |
| 6 | 949 | 1274 | 637 | 1020 |
| 7 | 897 | 1209 | 611 | 1000 |
| 8 | 806 | 1092 | 533 | 970 |
| 9 | 715 | 949 | 481 | 940 |
| 10 | 624 | 832 | 416 | 900 |
| 11 | 559 | 741 | 377 | 880 |
| 12 | 481 | 650 | 325 | 850 |
| 13 | 416 | 559 | 286 | 830 |
| 14 | 338 | 468 | 234 | 800 |
| 15 | 221 | 325 | 169 | 780 |

Level 1 (SB) — LL / L / CCI on VPROC1:

| idx | LL MHz | L MHz | CCI MHz | VPROC1 mV |
|---|---|---|---|---|
| 0 | 1547 | 2002 | 988 | 1200 |
| 1 | 1495 | 1950 | 975 | 1190 |
| 2 | 1443 | 1885 | 936 | 1170 |
| 3 | 1391 | 1820 | 910 | 1150 |
| 4 | 1339 | 1755 | 884 | 1140 |
| 5 | 1274 | 1703 | 845 | 1130 |
| 6 | 1222 | 1625 | 819 | 1100 |
| 7 | 1118 | 1495 | 754 | 1070 |
| 8 | 1014 | 1352 | 676 | 1040 |
| 9 | 897 | 1209 | 611 | 1000 |
| 10 | 806 | 1092 | 533 | 970 |
| 11 | 715 | 962 | 481 | 940 |
| 12 | 624 | 832 | 416 | 900 |
| 13 | 481 | 650 | 325 | 850 |
| 14 | 338 | 468 | 234 | 800 |
| 15 | 221 | 325 | 169 | 770 |

posdiv / clkdiv per OPP (LL FY): /1 down to 1092 MHz; posdiv /2 from
949 MHz; clkdiv /2 from 481; /4 at 221. L FY: posdiv /2 from 949; clkdiv
/2 from 468. CCI: posdiv /2 always; clkdiv /2 from 533 (FY). VCO stays
within ~1.0-2.2 GHz. Levels 2 (M) and 3 (L) are lower (L-cluster max 1846
/ 1599 MHz).

A72 (B) on VPROC2, OPP 0 / 6 / 9 / 13 (full tables in mt_cpufreq.c):

| table | OPP0 | OPP6 | OPP9 | OPP13 |
|---|---|---|---|---|
| FY 1221 | 2145 @ 1180 | 1690 @ 1070 | 1287 @ 980 | 793 @ 880 |
| FY 0119 | 2314 @ 1200 | 1781 @ 1070 | 1378 @ 980 | 845 @ 880 |
| SB 1221 | 2340 @ 1180 | 1963 @ 1110 | 1404 @ 1000 | 793 @ 880 |
| SB 0119 | 2522 @ 1200 | 2093 @ 1110 | 1495 @ 1000 | 845 @ 880 |

`DEFAULT_B_FREQ_IDX` = 13 (the A72s start at ~793-845 MHz), which fits
the benchmark (A72 ~ CPU4).

## Approach (revised 2026-10-04 19:50: follow mainline conventions)

Not aiming for upstream, but every piece follows mainline conventions so
governor, thermal (cpufreq cooling), cpuidle/energy model and kernel bumps
come for free. No all-in-one board driver. Pieces, each testable alone
(read-only first via standard sysfs/debugfs), developed as modules where
possible (only DTS changes need a boot flash):

1. **DA9214 under mainline `da9211-regulator`** (`dlg,da9214` on i2c6),
   after the APPM I2C fix (step 1b) is confirmed. Catch: it would own BUCKB
   (A72 rail) that `cl2-up.sh` toggles via i2c-dev — move the A72 bring-up
   into the kernel (vendor did it in PSCI CPU_ON, `cpu_power_on_buck`) or
   keep BUCKB untouched (always-on / no consumer) first.
2. **Clock driver for MCUMIXED** (ARMCAXPLL0-2, MUXSEL, CKDIV) with the
   vendor HW-semaphore lock inside the regmap/accessors. `clk_summary` then
   shows every cluster clock.
3. **Small regulator driver for CPULDO** (VSRAM_L).
4. **DTS OPP tables** (speed bin SB/TT, sign-off voltages) + MT6797
   platform data in `mediatek-cpufreq` (intermediate clock = MAINPLL, the
   vendor switch trick; `sram-supply` tracking +100 mV / <= 300 mV; shared
   VPROC1 aggregated by the regulator core). CCI via the mainline
   `mtk-cci-devfreq` link.
5. **A72 last:** small clock provider wrapping the iDVFS SMCs + VPROC2;
   only after a temperature sensor is wired up (no thermal driver today).

Throwaway test modules are still fine for one-off measurements (e.g. "does
a VBUCKA_A write take effect"), never as the driver.

The A72 5-minute delay is a port workaround, not SoC behaviour (Android
brings the cluster up in the kernel's CPU_ON path). Revisit after the I2C
fix — the hang it guards against is not yet explained.

Battery: a governor (schedutil) drops idle clusters to the bottom OPPs
(~0.78 V), the actual battery win; cpuidle on top.

## Step 1 — read-only survey (tools)

- `bin/cpu-clocks.sh` (run on the Gemini as root, after the A72s are up):
  eFuse bin, DA9214 registers (reads, 3x each), measured MHz per CPU,
  then the probe module in three separately logged loads (stage A, B, C).
  Writes `~atzero/cpu-clocks-<time>.txt` (in $HOME, survives a hang).
- `bin/cpumhz/cpumhz` — freestanding aarch64 binary: times a chain of
  dependent ADDs (1 cycle each on A53 and A72) pinned to each CPU = the
  real core clock, independent of any register decode. Rebuild with
  `bin/cpumhz/build.sh`.
- `devices/planet-geminipda/kernel/modules/mt6797-dvfs-probe/`
  (`nix build .#packages.aarch64-linux.mt6797-dvfs-probe`):
  - stage A (default): CSPM state (bus CG, PCM kicked?, semaphore owners),
    CPULDO VSRAM_L, CLK_MISC_CFG_0, sync DCM. Plain reads.
  - stage B (`mcu=1`): ARMCAXPLL0-3, MUXSEL, CKDIV, MCU FHCTL — under the
    vendor semaphore protocol (its only writes: take + release the
    semaphore). Skips itself if the CSPM bus CG is off, unless
    `cspm_cg=1` allows the vendor's own CG write.
  - stage C (`smc_b=1`, only with cpu8/9 online): A72 PLL + SRAM LDO via
    the vendor secure-read SMC 0xC200035F.
  - Re-read any time: `cat /sys/kernel/debug/mt6797_dvfs_probe/status`.

Run:

```sh
# Hydra
nix build .#packages.aarch64-linux.mt6797-dvfs-probe --print-out-paths --no-link
scp <out>/mt6797-dvfs-probe.ko bin/cpu-clocks.sh bin/cpumhz/cpumhz atzero@192.168.0.139:/tmp/
# Gemini (wait until `cat /sys/devices/system/cpu/online` says 0-9)
ssh -t atzero@192.168.0.139 sudo bash /tmp/cpu-clocks.sh /tmp/mt6797-dvfs-probe.ko
# Hydra
scp 'atzero@192.168.0.139:cpu-clocks-*.txt' logs/
```

If stage B or C hangs the device: hold power ~10 s, boot, fetch the
partial report, and rerun with `STAGES=A` (or `STAGES=AB`).

## Step 1 results (2026-10-04 19:12, `logs/cpu-clocks-20261004-191254.txt`)

| Cluster | Measured (cpumhz) | PLL registers | Vendor table for this chip (SB) |
|---|---|---|---|
| LL cpu0-3 | 841-893 MHz | VCO 1794 /2 = **897 MHz** | idx 9 = 897 @ 1000 mV; max 1547 @ 1200 |
| L cpu4-7 | 1271 MHz | VCO 1274 /1 = **1274 MHz** | not a table point (1209 @ 1000 / 1352 @ 1040); max 2002 @ 1200 |
| CCI | — | VCO 1260 /2 = **630 MHz** | between 611 @ 1000 and 676 @ 1040 |
| B cpu8-9 | 743-748 MHz | SMC: VCO 1500 /2 = **750 MHz** | below OPP13 (845 @ 880); max **2587** (TT) @ 1200 |

- **Speed bin: level 1 "SB", TT segment (function code 4), A72 date code
  0119** -> the MT6797X/Helio X27 tables: LL max 1547, L max 2002, A72
  2587 / 2431 MHz at OPP0/1 (TT override), CCI max 988.
- cpumhz matches the PLL decode within ~0.5 % (cpu1 841 = noise, best-of-7
  still disturbed); the old shell-loop ratio CPU0/CPU4 (1.47) matches
  1274/897 (1.42). Headroom: LL 1.7x, L 1.6x, A72 3.4x.
- Muxes: all clusters on their ARMPLL (MUXSEL 0x55); CKDIV 0x8 = LL/L/CCI
  code 0 (= /1, vendor default case; probe decode fixed), B code 8 (/1).
- VSRAM_L = 1100 mV (all 8 CPULDOs vosel 11, enable 0xff). Vendor rule
  VSRAM = VPROC + 100 -> **VPROC1 is probably 1000 mV** (not yet read).
  A72 SRAM LDO = 1100 mV.
- CSPM: bus CG on, PCM **not kicked** (FSM 0x48490) — the vendor DVFS
  firmware is not running, as expected. SW_RSV = 0xbabebabe (untouched).
  MCU semaphore 3 free; taken on the first try and released cleanly.
- MCU FHCTL: HP_EN = 0 (no hopping). Its CFG/DSSC registers read
  differently between the two loads (e.g. FHCTL0 CFG 0x00ff00ff then 0),
  and ARMCAXPLL0_CON0 bit21 flipped — treat FHCTL and CON0 upper bits as
  unreliable reads; CON1 (the frequency) was identical both times and
  matches the measurement.
- Sync DCM 0x00070707 (div 7 = ~490-560 MHz class); MP2 0.
- `thermal_zone0` exists (type not yet logged; cpu-clocks.sh now prints it).
- **DA9214: reads unusable.** i2cget byte-data on i2c-2 failed ~1/3 of
  the time and returned 0x00 otherwise, including BUCKB_CONT bit0 = 0
  while the A72s run on that buck — so the zeros are wrong. Writes ACK
  (cl2-up.sh). Until VPROC1 can be read, no frequency is raised.

## Step 1b — DA9214 reads (2026-10-04 19:16, `logs/da9214-read-20261004-191647.txt`)

`bin/da9214-read.sh`, 10 reads each of 0x5e/0x5d/0xd7/0xd9:

- A (i2cget byte-data) and B (i2ctransfer w1+r1, repeated START): the same
  pattern — 0x00 or FAIL, alternating. Both are a single combined
  write-then-read (the driver's WRRD mode).
- C (send-byte, then receive-byte): always returns **the register address
  just written** (0x5e -> 0x5e, 0xd7 -> 0xd7) — a stale FIFO echo, not
  chip data.
- Nothing in dmesg. Other buses read fine with the same driver (BQ25896,
  touch), so it is specific to i2c6.

**Cause (vendor i2c-mtk.c):** i2c6 is the "APPM" controller
(`mediatek,appm_used`, also driven by the CSPM in Android). Its variant
(`mt6797_compat.idvfs_i2c = 1`) has **no TRANSFER_LEN_AUX register**: for
WRRD the read length goes into TRANSFER_LEN[12:8]. Our delta maps every
`mediatek,mt6797-i2c` to mt8173_compat (`aux_len_reg = 1`), so on i2c6 the
read length was written to a register that doesn't exist and the read
phase got length 0. Writes (single message) are unaffected — hence
cl2-up.sh's i2cset works. The vendor also takes the CSPM semaphore
(SEMA_I2C_DRV) around i2c6 transfers; with the CSPM idle that is moot.
Method C's echo is not explained by this (plain reads are set up the same
way in both drivers); recheck it after the fix.

**Fix (written 19:20, compile-checked, not built):** i2c-mt65xx delta gets
`mt6797_appm_compat` (= mt8173 but `aux_len_reg = 0`, i.e. mainline's
packed `len | aux_len << 8` path), selected in probe when the node is
`mediatek,mt6797-i2c` **and** has `mediatek,appm_used`; DTS `&i2c6` gains
`mediatek,appm_used;`. Only i2c6 changes. Logs
`mt6797 APPM variant: WRRD read length in TRANSFER_LEN[12:8]` at probe.
Boot-only flash.

Test: Dragon backup → flash boot → check the probe line in dmesg → wait
for the A72s (cl2-up.sh must still bring them up: its i2cset writes are
single-message) → `da9214-read.sh`: expect A and B to agree, 10/10, with
0x5e bit0 = 1. Fallback: `gemini-backup/boot-hs200-192-20261004.img`.

## A72 bring-up: root cause of the old "wedge" + in-kernel fix (2026-10-06)

Vendor reference: gemian/gemini-linux-kernel-3.18 `arch/arm64/kernel/psci.c`
`cpu_power_on_buck()` + `cpu_psci_cpu_boot()`.

| Step | Vendor (kernel, before PSCI CPU_ON) | cl2-up.sh (userspace) |
|---|---|---|
| 1 | SPM 0x10006218 \|= bit0 | DA9214 BUCKB on (i2cset) |
| 2 | dummy read 0x102224a0 | SPM 0x218 |
| 3 | SWSYSRST 0x10007018 key\|bit11 = **PWRAP_SPI_CTL_RST latch** | latch |
| 4 | DA9214 page 0 + BUCKB on, 1 ms | — |
| 5 | EXT_BUCK_ISO 0x10006290 &= ~3 | ISO clear |
| 6 | unlatch | unlatch |
| 7 | 240 us, SRAM LDO SMC 1.1 V, 240 us | SRAM LDO SMC, 0.1 s |
| 8 | CPU_ON (+ MP2 sync DCM, iDVFS init) | sysfs online |

Root cause (confirmed by test): cl2-up.sh switched the A72 rail on OUTSIDE
the PMIC-wrapper (pwrap) SPI reset latch and then held the latch for tens
of ms across devmem process launches while the kernel kept using the
MT6351. Explains "wedge while boot is busy" (RCU stalls, eMMC/I2C
timeouts). The "SCP contends i2c6" theory was wrong: vendor i2c-mtk only
arbitrates i2c0/i2c1 with the SCP; i2c6 is shared with the (idle) CSPM.
Vendor also brings the A72s up late (HPS hotplug after drivers), but in
the kernel.

Also found: cl2-up.sh's WDT "arm" writes LENGTH = 20 << 5 = 20/64 s, and
its "disarm" writes MODE = key only = WDT OFF for the rest of the boot
(the old reboot trap). The mainline mtk_wdt driver already owns the WDT
(CONFIG_WATCHDOG_HANDLE_BOOT_ENABLED; LK leaves MODE 0x5D, LENGTH 0xF800
= 31 s) — do not touch it.

**Test module `devices/planet-geminipda/kernel/modules/mt6797-cl2-on/`**
(flake `mt6797-cl2-on`): vendor order, register steps IRQs-off, I2C
inside the window, add_cpu(8/9), WDT untouched (default wdt=0), logs
"cl2-on:". Results:
- settled system: latch 1.38 ms (I2C 375 us), power-on 1.9 ms,
  add_cpu(8) 25 ms, add_cpu(9) 4 ms -> 0-9.
- full load (8 busy loops + continuous power_supply reads = pwrap traffic):
  OK, add_cpu(8) 58 ms; no stall/pwrap/mmc/i2c errors.
- **at boot** (systemd `cl2-on-test.service`, ~3 s after kernel start),
  4/4 boots OK: latch 1.36-2.03 ms, A72 online at ~3 s, no errors;
  graphical.target 21.1-21.7 s userspace (24.7 s on 2026-10-04 with the
  A72s off until ~5 min).
- gemini-a72-up.timer is DISABLED on the device during these tests;
  `cl2-on-test.service` (insmod /home/atzero/mt6797-cl2-on.ko wdt=0) is
  the interim bring-up.

Next: in-kernel, mainline-shaped: DA9214 under da9211-regulator owning
both bucks + a small MT6797 platform driver for the SPM/latch/SRAM-LDO
steps that onlines cpu8/9 once its regulator is up; then retire
cl2-up.sh, the timer and sramldo-smc.

## Log

- 2026-10-04 ~19:00: vendor DVFS extracted (this doc). Survey tools
  written; probe compile-checked (clang arm64, W=1, our config: no
  warnings); cpumhz built (static aarch64, 4.8 KB). Not yet run.
- 2026-10-04 19:12: **survey run** (above). Probe ckdiv decode fixed (code
  0 = /1); cpu-clocks.sh logs thermal zone type/temp. Next: step 1b
  (DA9214 read method), then the first write: VPROC1/VSRAM only.
- 2026-10-04 19:16: step 1b run: DA9214 combined reads broken on i2c6,
  separate reads echo the address. Cause found in the vendor I2C driver
  (APPM controller packs the WRRD read length; no AUX register). Fix
  written in the i2c-mt65xx delta + DTS (`mediatek,appm_used`); compiles
  clean (W=1), DTB contains the property. Not yet built/flashed.
- 2026-10-04 19:53: da9214-read.sh after the APPM I2C fix build: results
  identical to before (A/B 0x00/FAIL, C echoes the address;
  `logs/da9214-read-20261004-195351.txt`). Not yet confirmed that the new
  boot image was running: first check `dmesg | grep "APPM variant"` and
  `uname -v` (build date). If the line is there, the length packing alone
  is not the cause — next suspects: vendor push-pull/HS mode for this bus
  (I2C_PUSHPULL_FLAG, 3.4 MHz), and the method-C FIFO echo.
- 2026-10-06 13:33: **APPM I2C fix confirmed** (charger-ui boot image;
  `logs/da9214-read-20261006-133318.txt`). (The 10-04 "no change" run was on
  the old image; and `find /proc/device-tree` never searched anything —
  it is a symlink, use `ls /proc/device-tree/<node>/`.) Methods A and B now
  agree 10/10: 0x5d BUCKA_CONT 0x01, 0x5e BUCKB_CONT 0x01 (EN, VSEL A, no
  GPI), **0xd7 VPROC1 = 0x46 = 1000 mV, 0xd9 VPROC2 = 0x46 = 1000 mV**.
  Method C (receive-byte) still echoes the address — unused, ignore.
  Consequences: VSRAM_L 1100 = VPROC1 + 100 (vendor rule holds). LK runs
  L at 1274 MHz and CCI at 630 MHz on 1000 mV, slightly below the SB
  sign-off (1209 / 611 at 1000; 1352 / 676 at 1040), so the first write
  should be VPROC1 -> 1040 mV + VSRAM_L -> 1140 (margin at current clocks;
  also the voltage for L 1352). A72 at 750 MHz on 1000 mV has margin.
  Next (mainline-shaped plan): piece 1, DA9214 under da9211-regulator —
  first decide BUCKB ownership vs cl2-up.sh.
- 2026-10-06 13:57-14:32: A72 bring-up root-caused against the vendor
  psci.c (latch order/window) and fixed in a test module; settled, load
  and 4 boot-time runs all pass (section above). WDT handling in
  cl2-up.sh found wrong; module leaves the WDT to mtk_wdt.
- 2026-10-06 15:30-15:41: **in-kernel A72 bring-up works** (boot-only flash
  `boot-cl2power-20261006.img`; fallback `gemini-backup/boot-chargerui-20261006.img`).
  `CONFIG_REGULATOR_DA9211=y` (DT `dlg,da9214` on i2c6: BUCKA `vproc1`
  always-on 0.77-1.20 V, BUCKB `vproc2` 0.80-1.20 V; driver accepted the
  chip, "No IRQ configured" is expected), `mtk_wdt` delta with mt6797 toprgu
  data (SWSYSRST as reset controller, `#reset-cells`), and
  `drivers/soc/mediatek/mt6797-cl2-power.c` (`CONFIG_MTK_MT6797_CL2_POWER`;
  DT `cl2-power`: spm/mcucfg2 regs, `vproc-supply`, `resets = <&watchdog 11>`,
  `cpus`). 3 boots: VPROC2 on at 1.000 V, latch window ~1.3 ms, power-on
  ~1.8 ms, cpu8 ~3.5 ms, cpu9 ~2.4 ms, **cpu9 online at 8.0-9.6 s monotonic,
  before greetd (13 s)**. Both rails visible in /sys/class/regulator.
  Leftover: `vproc_fixed` placeholder regulator ("vproc", regulator.3) —
  remove with the DVFS work. Not handled yet: re-powering the cluster after
  BOTH A72s are offlined (keep one online).
  Cleanup: gemini-debian stops shipping cl2-up/cl2-down, gemini-a72-up
  (service+timer), the sramldo-smc load; new udev rule
  99-gemini-a72-workqueue.rules sets the workqueue cpumask 300 when cpu9
  comes online. cl2-up.sh / cl2-down.sh / mt6797-cl2-on marked superseded.
- 2026-10-06 16:15: **first voltage change: VPROC1 1.000 -> 1.040 V** via the
  da9211 regulator (test module `mt6797-vproc-set`, uv=1040000; VSRAM_L stays
  1.10 V, still >= VPROC1). dmesg `VPROC1 1000000 -> 1040000 uV ret 0`,
  sysfs vproc1 1040000. Stable: 8 busy loops 60 s, normal use + a game. Not
  persistent (LK sets 1.00 V each boot) — cpufreq will own it. Note:
  `i2cget 0x68` now says "Device or resource busy" (da9211 owns the
  address; use sysfs or `-f`). Next: MCUMIXED PLL clock driver, read-only first.
- 2026-10-06 16:57: **CPU clock driver v1 (read-only) works** (boot-only
  flash `boot-mcuclk-20261006.img`; fallback
  `gemini-backup/boot-cl2power-20261006.img`).
  `drivers/clk/mediatek/clk-mt6797-mcu.c` (`CONFIG_COMMON_CLK_MT6797_MCU`,
  DT `mcumixedsys: clock-controller@1001a000`, ids in
  `dt-bindings/clock/mt6797-mcumixedsys.h`): armpll_{ll,l,cci} ->
  cpu_*_sel -> cpu_{ll,l,cci}, every read under the vendor HW semaphore.
  clk_summary: cpu_ll 897000000, cpu_l 1274000000, cpu_cci 629999328 Hz —
  identical to the survey/cpumhz. Note: CSPM POWERON_CONFIG_EN read 0 at
  this boot (bus CG off; the 10-04 survey saw 1), the driver enabled it
  with the vendor key 0x0b160001 before using the semaphore. (The clk
  Makefile delta was delivered as Makefile.gemini — tool can't write
  "Makefile" — and renamed by the user.)
  Next: write ops (PLL DDS/posdiv, mux to MAINPLL during a change, ckdiv,
  vendor ordering), then the CPULDO regulator, then OPP tables +
  mediatek-cpufreq.
- 2026-10-06 17:10: **CPU clock driver v2 (write support), awaiting test.**
  Shape = mainline `mediatek-cpufreq` ("cpu" = cpu_X_sel mux,
  "intermediate" = apmixedsys mainpll): `cpu_X_sel` gets `.set_parent`
  (vendor `_cpu_clock_switch`: TOPCKGEN CLK_MISC_CFG_0[5:4]=3 before going
  to MAINPLL/UNIVPLL, cleared after returning — but only once no cluster
  incl. B still selects MAINPLL/UNIVPLL, since those bits are shared);
  `armpll_X` gets `.determine_rate/.set_rate` (vendor `adjust_armpll_dds`:
  CON1 posdiv+DDS+CHG in one semaphore-held RMW, 20 us settle) and refuses
  with -EBUSY while its own cluster runs from it, so a retune can only
  happen in the mux->mainpll / set_rate / mux->armpll sequence. posdiv =
  smallest /1,/2,/4 keeping VCO >= 1 GHz (matches the vendor tables);
  ceiling per cluster = SB max (LL 1547, L 2002, CCI 988 MHz); CKDIV stays
  read-only (every OPP >= 559 MHz is /1). No voltage logic in the clk
  driver. DT: mcumixedsys gets a third reg `misc-cfg` = <0x10000104 4>
  (mapped, not requested — topckgen owns the page).
  Test module `mt6797-cpuclk-step` (mhz=1209|1274|1352, L cluster only):
  refuses unless VPROC1 >= the SB voltage (1352 needs 1.04 V -> load
  `mt6797-vproc-set uv=1040000` first), then does the three clk calls.
- 2026-10-06 17:30: **first frequency step works: L cluster 1274 -> 1352
  MHz** (clk driver v2, boot-only flash `boot-mcuclk2-20261006.img`;
  fallback `gemini-backup/boot-mcuclk-20261006.img`). VPROC1 1.04 V first
  (mt6797-vproc-set), then `mt6797-cpuclk-step mhz=1352`: dmesg
  `cpu_l_sel -> mainpll (MUXSEL 0x65)`, `armpll_l -> 1352000000 Hz (CON1
  0xc00d0000, /1)`, `cpu_l_sel -> armpll_l (MUXSEL 0x55)`; clk_summary
  armpll_l/cpu_l_sel/cpu_l 1352000000; cpumhz cpu4-7 1340-1349. 5 min of 4
  busy loops on cpu4-7 + ~30 min use, no errors (the boot ended with an
  accidental power-key press, not a crash). Probe: MUXSEL 0x54 at 0.24 s
  (B field 0 = A72 not yet powered; 1 after cl2-power), CLK_MISC_CFG_0
  0xffff0000 (bits 5:4 clear). Note: a reboot returns to 1274 MHz / 1.00 V
  (LK).
- 2026-10-06 20:45: **VSRAM_L regulator** (awaiting test):
  `drivers/regulator/mt6797-cpuldo-regulator.c`
  (`CONFIG_REGULATOR_MT6797_CPULDO`, DT `vsram_l: regulator-vsram-l`,
  compatible `mediatek,mt6797-cpuldo`, `mediatek,infracfg = <&infrasys>`
  syscon regmap). Selectors 3..15 = 0.90..1.20 V (25 mV), DT 1.00..1.20 V;
  writes the vosel replicated to all 8 LDOs in CTRL_1 and CTRL_2 (vendor
  format, LK leaves 0x0b0b0b0b), forces CTRL_0[7:0]=0xff; settle = vendor
  (up 12.5 mV/us, down 3.125 mV/us, +5 us, min 25 us). No tracking in the
  driver (mainline puts that in mediatek-cpufreq's sram-supply logic).
  The unused fixed "vproc" placeholder regulator is removed.
  mt6797-vproc-set v2 moves VSRAM_L with VPROC1 (VPROC+100 mV clamped
  1.00..1.20 V; SRAM first going up, VPROC first going down; window now
  1.00..1.15 V).
- 2026-10-06 21:40: **cpufreq for the A53 clusters** (awaiting test). Mainline
  `mediatek-cpufreq` (delta copy) + `mt6797_platform_data`: min_volt_shift
  100 mV, max_volt_shift 275 mV (vendor steps 275 so the 25 mV SRAM grid
  can't pass 300), proc_max 1.20 V, sram 1.00..1.20 V, no ccifreq. Delta
  changes to the mainline driver: CPUs without `operating-points-v2` are
  skipped (cpu8/9), and init returns -ENODEV quietly for them;
  `cpufreq-dt-platdev` blocklists mediatek,mt6797. DT: every cpu0-7 node
  gets clocks <cpu_X_sel>, <MAINPLL> ("cpu", "intermediate"), proc-supply
  vproc1, sram-supply vsram_l, operating-points-v2 cluster0_opp / cluster1_opp
  (opp-shared). Shared VPROC1/VSRAM_L: each cluster is a separate consumer
  and the regulator core takes the max; each consumer keeps
  SRAM >= PROC and SRAM - PROC <= 275 mV (+25 mV grid) at every step, so the
  aggregate does too, even with both clusters changing at once.
  Tables (SB): LL 624/715/806/897/1014/1118, L 650/832/962/1092/1209/1352
  MHz; voltages floored at 1.00 V because the CCI stays at LK's 630 MHz
  (LK runs it at 1.00 V; SB says 611 @ 1.00, 676 @ 1.04) — floor goes when
  the CCI scales. LL includes 1118 @ 1.07 V because mediatek-cpufreq needs
  an OPP >= the intermediate rate (MAINPLL 1092) to pick the intermediate
  voltage; LL transitions therefore pass through 1.07 V (vendor avoided
  this with FHCTL hopping for LL/L/CCI; options later: FHCTL, or clk26m as
  intermediate). Clock driver: posdiv now /1 >= 1092 MHz, /2 below, never
  /4 (vendor tables exactly), so PLL >= 546 MHz; CKDIV for lower OPPs later.
  The test modules (vproc-set, cpuclk-step) must not be used with cpufreq
  running — they'd fight it.
- 2026-10-06 21:50: first cpufreq boot: `mtk-cpufreq: failed to initialize
  dvfs info for cpu0`, error -2. Cause: `dev_pm_opp_of_get_sharing_cpus()`
  walks every possible CPU of an opp-shared table and returns -ENOENT for
  one without `operating-points-v2` (cpu8/9). Mainline DTs give every CPU
  an OPP table, so: cpu8/9 get `cluster2_opp` (vendor A72 SB-0119 table,
  TT top 2587/2431 MHz; descriptive only) and mediatek-cpufreq now skips
  CPUs without a `clocks` property instead of without an OPP table. (Also:
  an earlier flash of this step put the previous image on by mistake —
  check `ls /proc/device-tree/cpus/cpu@0/` after flashing.)
- 2026-10-07 08:00: **cpufreq works** (boot-cpufreq3): policy0 = cpu0-3
  (624..1118 MHz), policy4 = cpu4-7 (650..1352 MHz), schedutil. Game test:
  ~391k LL and ~500k L transitions, no errors, no hang. time_in_state
  (10 ms units): LL 624 73768 / 1118 66037 (little in between), L 650 84040
  / 1352 58740. Rails sampled under load: VPROC1 1.00/1.04/1.07, VSRAM_L
  1.15/1.175 (single reads can mix before/after values of one transition).
  Problems: (1) the clk driver's pr_info on every switch flooded dmesg;
  (2) schedutil re-evaluated every 1 ms (mediatek-cpufreq leaves
  transition_latency 0) so LL ping-ponged 624 <-> 1118 about every
  millisecond, each time via 1.07 V and two DA9214 writes. Fix: clk logs
  -> pr_debug; mediatek-cpufreq sets cpuinfo.transition_latency from the
  OPP table; both tables get clock-latency-ns = 500 us -> schedutil
  rate_limit 10 ms. Next: FHCTL (no intermediate, no 1.07 V detour), CCI
  scaling (drops the 1.00 V floor), more OPPs.
- 2026-10-07 08:25: fix2 result: rate_limit_us 10000 but
  cpuinfo_transition_latency 4294967295 (CPUFREQ_ETERNAL fallback):
  `clock-latency-ns` is a per-OPP property in the opp-v2 binding, the
  table-level one was ignored. Fix3: clock-latency-ns in every OPP node;
  driver no longer falls back to ETERNAL (which would block ondemand /
  conservative), 0 = mainline behaviour. total_trans after the game:
  17476 (LL) / 10540 (L), no errors, dmesg quiet.
