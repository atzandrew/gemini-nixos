# CPU clocks and voltages (DVFS) on the Gemini (MT6797X)

Started 2026-10-04. Status: **step 1 (read-only survey) ready to run** —
probe module, `bin/cpu-clocks.sh` and `bin/cpumhz` written and
compile-checked; nothing has been written to any clock or voltage
register yet.

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

## Approach

**Recommended: a board-local driver, developed as a loadable module first.**

- Mainline-style (cpufreq-dt / mediatek-cpufreq + DTS OPP tables) would
  need: a clock driver for the MCUMIXED ARM PLLs/mux/divider that honours
  the HW semaphore; the DA9214 under the mainline da9211 regulator driver
  (which would also own BUCKB, the A72 rail that `cl2-up.sh` drives over
  i2c-dev — a conflict to solve first); a regulator for CPULDO with VSRAM
  tracking; mediatek-cpufreq platform data for a shared VPROC1 plus the
  CCI rule; and the A72 iDVFS SMCs don't fit the clk framework at all.
  That is the right end state if this is ever upstreamed, but it is five
  new pieces before the first measurement.
- A single board-local `mt6797-cpufreq` (kernel delta, like the mtk-sd
  change) does the vendor sequence explicitly: per-cluster OPP table
  chosen by the eFuse bin, VPROC1 = max rule, VSRAM tracking, semaphore-
  protected PLL writes, every step logged, every knob a parameter.
- **Test vehicle: a module, not a boot image.** Each experiment is an
  `insmod` that performs one logged step. If the device hangs, a power
  cycle runs the boot chain's own clock setup again — Linux persists
  nothing, no flash is needed, and the known-good boot image is never
  touched. (Caveat to check: the DA9214 is an external chip and may keep a
  voltage we set across a warm reset until preloader/LK set it again.
  We only ever raise it first, so that is the safe direction; the survey
  run after a reboot shows what the boot chain leaves.) Only a proven driver goes into the boot image (then
  the usual Dragon backup / boot-only flash / fallback loop applies).
- Not planned: porting the CSPM firmware blob (opaque; hard to debug).

Order: **L cluster first** (CPU4-7; A53, highest A53 table, already the
faster cluster). First write = VPROC1 (+VSRAM_L) up only, no frequency
change (more margin, nothing else). Then one L OPP step at the voltage
already set. LL follows on the same rail. The A72s last (ATF SMCs + the
`cl2-up.sh` DA9214/bus-safety sequence, which stays untouched), and only
after there is a temperature reading: the vendor ran the A72s at
2.1-2.5 GHz with thermal throttling and a power budget (PPM); we have
neither.

Battery: today every core sits at its LK clock and voltage even when idle.
A real governor (schedutil) would drop idle clusters to the bottom OPPs
(221-325 MHz at ~0.78 V), which should lower idle drain, while a fixed
higher clock without a governor would raise it. So the battery win comes
with step 5 (governor), and cpuidle on top of that.

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

## Log

- 2026-10-04 ~19:00: vendor DVFS extracted (this doc). Survey tools
  written; probe compile-checked (clang arm64, W=1, our config: no
  warnings); cpumhz built (static aarch64, 4.8 KB). Not yet run.
