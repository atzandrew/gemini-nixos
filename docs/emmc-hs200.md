# eMMC HS200 / HS400 on the Gemini (MSDC0)

Started 2026-10-04. Status: **attempt 2 at 192 MHz FAILED (read errors);
next = the same driver at 96 MHz (DTS max-frequency 100 MHz).** The device
runs the HS52 boot image again.

## Where we start (2026-10-04)

- HS52: 48 MHz actual, 8-bit, 1.8 V, 42 MB/s read. Card `DF4064` supports
  HS200 + HS400 (EXT_CSD DEVICE_TYPE 0x57).
- First HS200 try (DTS `mmc-hs200-1_8v` only, compatible `mediatek,mt2701-mmc`):
  192 MHz, 71 MB/s read, 104 MB/s write, then `msdc_auto_cmd_done: AUTO_CMD23
  ... cmd_error=-84` (CRC) and I/O errors during a 1 GiB write + read-back.
  Reverted; see `claude/status.md` (project) eMMC section.

## What the vendor 3.18 kernel does differently

Source: `gemian/gemini-linux-kernel-3.18` (GitHub, read 2026-10-04),
`drivers/mmc/host/mediatek/mt6797/` and `arch/arm64/boot/dts/aeon6797_6m_n.dts`.
Mainline: `drivers/mmc/host/mtk-sd.c` at v6.6.157.

### Pads (IOCFG_B = 0x10002400; mainline does nothing here)

`msdc_io.h:223-240`, `msdc_io.c` `msdc_set_driving_by_id` / `msdc_set_smt_by_id` /
`msdc_set_tdsel_by_id` / `msdc_set_rdsel_by_id`, called from `sd.c msdc_init_hw()`:

| Reg | Offset | Vendor value | Note |
|---|---|---|---|
| DRV | +0x1a0 | [11:0] = 0x249 | 3-bit fields dat3-0, dat7-4, cmd/dsl/rstb, clk; all **1** (`mmc0_pins_default` drive-strength 1 for every group; `msdc_io.c:933` debug says "should: 1001001001b") |
| SMT | +0x060 | [3:0] = 0xf | Schmitt on all groups |
| TDSEL | +0x0d0 | [15:0] = 0xcccc | |
| RDSEL | +0x080 | [17:0] = 0 | |
| PUPD/R0/R1 | +0x100/0x110/0x120 | clk/dsl pull-down 10k, cmd/dat pull-up 10k, rstb pull-up 50k | `msdc_pin_config_by_id(PULL_UP)`; not changed by our driver |

Note the vendor eMMC drive strength is the **weakest-but-one** setting (1),
not a strong one. If LK already left these values, drive strength is not
the HS200 problem. The new driver logs LK's values at probe
(`mt6797 pads bootloader:`); `bin/emmc-pads.sh` reads them read-only on the
current HS52 system.

### Controller registers

| Setting | Vendor (`autok.c`, `msdc_tune.c`) | Mainline mtk-sd (mt2701 compat) |
|---|---|---|
| PATCH_BIT0 CKGEN_MSDC_DLY_SEL | 0 (`autok_path_sel`) | 1 |
| PATCH_BIT1 WRDAT_CRCS_TA / CMD_RSP_TA | 0 / 0 | 1 / 1 (0xffff4089) |
| PATCH_BIT1 GET_BUSY_MA / GET_CRC_MA | 1 / 1 | 0 / 1 |
| PATCH_BIT2 RESPSTSENSEL / CRCSTSENSEL | **3 / 3 in HS200**, 5 / 5 in HS400 (`autok_init_hs200/hs400`) | 2 / 2 always |
| EMMC50_CFG0 bit 4 (CRC_STS_SEL) | 0, 1 only in HS400 | 1 always |
| CMD / DAT delay tuning | separate (autok) | one shared delay (`msdc_tune_together`) |
| Clock source | `MSDC0_CLKSRC_400MHZ` = msdcpll 400 MHz, HS200 = src/2 = 200 MHz | msdcpll_d2 192 MHz (our DTS), HS200 = src (mode 1) = 192 MHz |

The AUTO_CMD23 CRC error is a command-response error, so the response latch
(RESPSTSENSEL) and the shared CMD/DAT delay are the first suspects; pads second.

## The driver change (kernel delta, 2026-10-04)

`devices/planet-geminipda/kernel/delta/drivers/mmc/host/mtk-sd.c` = v6.6.157
`mtk-sd.c` + a `mediatek,mt6797-mmc` compatible (mt2701 layout plus
`mt6797_quirks`). Nothing changes for other compatibles (msdc1 keeps mt2701).
Compile-checked 2026-10-04 (clang arm64, W=1, our kernel config): no warnings.

Every vendor setting is a module parameter, default = vendor value.
[corrected 2026-10-04] They can NOT be set from the boot.img cmdline: the
kernel config has `CONFIG_CMDLINE_FORCE=y` (CONFIG_CMDLINE is the whole
command line; `config/gemini.nix` kernelParams are inert, as its own comment
says). Changing one = edit the default in mtk-sd.c, or CONFIG_CMDLINE, or
(for the clock) the DTS — all mean a kernel rebuild.

| Parameter | Default | Meaning |
|---|---|---|
| `mt6797_drv` | 1 | MSDC0 drive strength 0-7 (all groups); -1 = leave LK's |
| `mt6797_pad` | 1 | SMT/TDSEL/RDSEL vendor values; 0 = leave |
| `mt6797_ckgen` | 0 | CKGEN_MSDC_DLY_SEL; -1 = mainline (1) |
| `mt6797_pb1` | 1 | vendor PATCH_BIT1 TA/MA; 0 = mainline |
| `mt6797_latch` | 3 | RESP/CRC latch in HS200; -1 = mainline (2) |
| `mt6797_latch_hs400` | 5 | same in HS400 |
| `mt6797_crcsts_sel` | 0 | EMMC50_CFG0 CRC_STS_SEL outside HS400; -1 = mainline (1) |
| `mt6797_split_tune` | 1 | tune CMD and DAT separately; 0 = mainline tune-together |
| `mt6797_max_hz` | 0 | cap the bus clock (0 = DT max-frequency) |

Logged (`dmesg | grep mt6797`): parameters + source clock at probe, pad
registers before/after, controller registers after init and after every
tuning, and each tuning window (`map` bitmap of passing delays, `maxlen`,
`final`). A wide window (maxlen ≥ ~10 of 32) = margin; a narrow one = marginal.

The pad block is mapped with `devm_ioremap` (no region request: pinctrl
owns it) and only for the instance at 0x11230000.

## Clock options for a slower HS200 step

msdc50_0_sel parents (clk-mt6797.c): clk26m, msdcpll, syspll_d3, univpll1_d4,
syspll2_d2, syspll_d7, msdcpll_d2, univpll1_d2, univpll_d3.

- **96 MHz** — keep msdcpll_d2 (192) and cap with DTS
  `max-frequency = <100000000>` (mode 0, div 0 = src/2).
- **156 MHz** — DTS `assigned-clock-parents = <&topckgen CLK_TOP_UNIVPLL1_D4>`
  (univpll/8, mode 1 = no divider). HS52 then runs at 39 MHz. Check the real
  rate in `clk_summary` first (`bin/emmc-pads.sh` prints it).
- **182 MHz** — CLK_TOP_SYSPLL2_D2.
- 192 MHz — current msdcpll_d2.

HS400 note: mainline needs src ≥ 400 MHz for a 200 MHz HS400 clock
(`hz >= src/2` path). With msdcpll_d2 (192) HS400 would run at 96 MHz DDR
(same throughput as HS200 at 192). Getting 200 MHz HS400 means sourcing
msdcpll itself at ~400 MHz (it reads 384 today) — later.

## Test procedure

1. Dragon: `mtk r boot ~/gemini-backup/boot-<date>.img` (already have
   `boot-hs52-20261004.img`).
2. Flash boot only.
3. `bin/emmc-check.sh 1024 3` → needs `VERDICT: PASS` (no mismatch, no new
   errors, **no retunes during the run**). Then, before any power-off,
   `scp atzero@192.168.0.139:/tmp/emmc-check-dmesg.txt logs/` (the script
   saves the full kernel log there; `ssh host 'sudo dmesg'` without `-t`
   captures nothing — sudo has no terminal for the password).
4. Drop the cap (192 MHz), repeat step 3 at least twice, across a reboot.
5. If it fails: flip one parameter default at a time (e.g. `mt6797_latch`,
   `mt6797_split_tune`, `mt6797_drv`) — kernel rebuild each.
6. HS400 only after HS200 passes repeatedly.

## Log

- 2026-10-04: vendor settings extracted (this doc); driver change written
  and compile-checked; `bin/emmc-check.sh` gained ROUNDS + mt6797 log lines
  + kernel-error delta and a PASS/FAIL verdict; new read-only
  `bin/emmc-pads.sh`. Not yet built or flashed.
- 2026-10-04 ~15:50: LK's MSDC0 pads read on the HS52 system
  (`bin/emmc-pads.sh`, `logs/emmc-pads-hs52.txt`): DRV 0x249 (drive 1 on
  every group), SMT 0xf, RDSEL 0 = **same as vendor**; TDSEL 0 (vendor
  0xcccc) = the only pad difference. Drive strength is not the cause.
  Clocks: msdcpll 383999878, msdcpll_d2 191999939, univpll1_d4 156000000,
  syspll2_d2 182000000, msdc50_0_hclk 273000000.
- 2026-10-04 ~16:05: **attempt 2 flashed (boot only) — ran at 192 MHz**, not
  96: the `mtk_sd.mt6797_max_hz` kernelParam never reached the kernel
  (CMDLINE_FORCE; `/proc/cmdline` = CONFIG_CMDLINE). Vendor register values
  confirmed applied (pb1=ffff40c0, pb2=748b180d → latch 3/3, pb0 CKGEN 0,
  emmc50_cfg0 bit4 0, pad_tune0=002a3400 → CMD delay 10, DAT delay 20).
  Tuning windows: CMD map=ffffffff (32/32 pass), DAT map=ffffff01/ffffff03
  (fail only at delays 1-7; 24 wide; final 20). **7 retunes at 47-55 s**
  (during boot/login; a retune follows a transfer error) — already marginal.
  `emmc-check.sh`: read 119 MB/s (attempt 1: 71), write 100 MB/s, then round 1
  **INTEGRITY: MISMATCH** with `I/O error, dev mmcblk0, sector 39837696 op
  READ` (+ 39838720). Failure moved from the command path (attempt 1:
  AUTO_CMD23 CRC) to sustained data reads. The full kernel log was lost
  (non-tty sudo). Restored `boot-hs52-20261004.img`; afterwards userdata
  clean, `dpkg --verify` only the 4 known config files.
- 2026-10-04 ~16:25: next build = same driver, DTS `max-frequency` 100 MHz
  (96 MHz); `config/gemini.nix` kernelParam reverted; `emmc-check.sh` now
  counts retunes (any during the run = FAIL) and saves the full kernel log
  to `/tmp/emmc-check-dmesg.txt`.
- 2026-10-04 ~16:40: **96 MHz build failed to boot**: `I/O error, dev mmcblk0`,
  `EXT4-fs: I/O error while writing superblock`, root mount failed. Restored
  `boot-hs52-20261004.img` (Dragon: `mtk w boot gemini-backup/boot-hs52-20261004.img`;
  then unplug, hold power ~22 s). Afterwards: userdata `clean`, no errors,
  `dpkg --verify` only the 4 known config files.
  **Cause (driver bug):** the HS200 latch 3 was keyed on timing only. The
  vendor (`autok.c:60-69`, `autok_init_sdr104`) uses 3 only above 100 MHz and
  `AUTOK_*_LATCH_EN_HS_VALUE` = 2 at ≤ 100 MHz. Fixed: latch now chosen by the
  actual clock, and logged (`mt6797 timing T clk C: resp/crc latch L`).
  Compile-checked; not yet built.
- Vendor DTS enables HS200 + HS400 with a 400 MHz source, so stock Android
  very likely ran HS400 (200 MHz DDR) on this eMMC (not verified from an
  Android log).
- 2026-10-04 ~17:05: **96 MHz HS200 PASSES** (latch fix build, DTS
  max-frequency 100 MHz, source msdcpll_d2): `emmc-check.sh 1024 3` = 3x
  INTEGRITY OK, 0 retunes, 0 errors; read 84 MB/s, write 70-72 MB/s (HS52:
  42 MB/s). Latch 2 at 96 MHz (pb2=548a180d). Tuning: CMD map=cfffffff
  (maxlen 28, final 9), DAT map=7fffffff (maxlen 31, final 10) — wide.
  Logs: `logs/emmc-hs200c-96mhz-check.txt`, `logs/emmc-hs200c-96mhz-dmesg.txt`.
  The "5 errors before the run" were false positives: the driver's own
  "mt6797 ... crc" info lines matched `msdc.*crc`; `emmc-check.sh` now ignores
  mt6797 lines (also explains the "1 error before" in the 192 MHz run; its
  retunes and read errors were real).
- 2026-10-04 ~17:15: next = 192 MHz the vendor's way: DTS
  `assigned-clock-parents = <&apmixedsys CLK_APMIXED_MSDCPLL>` (384 MHz) +
  max-frequency 200 MHz → mtk-sd mode 0, div 0 = src/2 = 192 MHz (vendor:
  msdcpll 400, src/2 = 200). The failed 192 MHz run used msdcpll_d2 + mode 1
  (no divider). DTB diff vs the 96 MHz build = these two properties only
  (plus phandle renumbering). Same driver (latch 3 above 100 MHz).
- 2026-10-04 ~17:20: **change of plan for the 192 MHz retry — latch-ck, not
  the clock path.** Vendor `execute_online_tuning_hs200()` (autok.c ~2420-2517)
  also tunes INT_DAT_LATCH_CK_SEL (PATCH_BIT0[9:7]): starts at
  AUTOK_LATCH_CK_VALUE = 1 and raises it until *multi-block* reads pass
  (`autok_execute_tuning_latch_ck`, tune_latch_ck_cnt). Mainline uses
  `host->latch_ck` (DT `mediatek,latch-ck`, default 0) and tunes with one
  CMD21 block — consistent with "tuning passes, sustained reads fail" at 192.
  Build 2e: the 96 MHz DTS + max-frequency 200 MHz + `mediatek,latch-ck = <1>`;
  clock source unchanged (msdcpll_d2, mode 1). DTB diff vs the passing 96 MHz
  build = exactly those two properties. The msdcpll-source idea is parked
  (needed for HS400 later). Expect `pb0=403c00c6` (latch_ck 1) after tuning.
- 2026-10-04 ~17:40: **192 MHz + latch-ck 1 PASSES** `emmc-check.sh 1024 3`:
  3x INTEGRITY OK, 0 errors/retunes during the run; read **134 MB/s**, write
  103-106 MB/s (HS52 42; failed 192 run 119/100). `pb0=403c00c6` confirms
  latch_ck 1. Logs: `logs/emmc-hs200e-192latch-{check,dmesg}.txt`.
  **But one retune at 3.17 s** (as `/init` started reading root), nothing
  logged before it — likely one recovered data CRC error (mainline only
  dev_dbg()s bus errors). First tuning: CMD map=7ffffbff (final 21), DAT
  map=fff01fff (fail 13-19; final 4); after the retune: CMD fffffffc (final
  17), DAT fffffe07 (fail 3-8; final 20). The data fail band moves between
  tunings. (The 96 MHz boot tuned once only; the failed 192 run retuned 7x.)
  The `dev_addr_check` WARNING at ~24 s is the Wi-Fi driver's MAC, unrelated
  (also in the 96 MHz log).
- 2026-10-04 ~17:50: driver now logs real (non-tuning) bus errors:
  `mt6797 bus error: [data] cmd=.. err=..` (ratelimited, cmd and data paths);
  `emmc-check.sh` counts them and lists them in its header. Compile-checked.
  Verdict so far: 192 MHz is much better but not yet proven — needs several
  boots with the error log before daily use.
- 2026-10-04 ~18:00: build f (bus-error logging), 192 MHz, boot 1: read
  **150 MB/s**. The boot retune is explained: `mt6797 bus error: data cmd=18
  arg=00008040 blocks=32 err=-84` at 3.132 s → retune at 3.14 s (recovered;
  CRC caught it). The cmd=5/55 -110 "errors" at 0.92-0.96 s are the core's
  SDIO/SD probe before MMC (expected). Pattern on both 192 MHz boots: first
  tuning (~1 s) DAT fail band at taps 12/13-18/19, mainline picks DAT delay 4
  (earliest window); by ~3.1 s the band has moved to taps 3-8 → delay 4 fails
  → retune picks 20. CMD window also shifts (7ffffbff/3ffffdff →
  fffffffc/fffffffe). A one-time step during early boot (consys/Wi-Fi power-up
  runs 2.4-3.06 s — unconfirmed), not thermal drift. Then stable for 3 GiB.
- Driver (written, NOT yet built — finish the soak on build f first):
  widest-window delay choice for mt6797 (map fff80fff → 25 instead of 4;
  fff01fff → 6, still not robust), and bus-error logs only once a card
  exists (hides the probe timeouts). Candidate real fix if the soak shows
  errors only at boot: one forced retune after boot settles.
- 2026-10-04 ~18:15: **soak, build f at 192 MHz, boots 1-3: all PASS**
  (9 x 1 GiB OK, 0 retunes / 0 errors during every run; read 143-158 MB/s,
  write 95-106 MB/s). Logs `logs/emmc-192-boot{1,2,3}{,-dmesg}.txt`.
  **The boot error is deterministic:** every boot, init tuning (~1.0 s) DAT
  map fff80fff/fff01fff → delay 4; at ~3.13-3.15 s the same read
  (`CMD18 arg=00008040 blocks=32`) gets CRC -84; the retune finds the same
  windows every time (CMD fffffffe → 16, DAT fffffe07 → 20); then stable.
  Not drift. Between init tuning and the failing read the core switches the
  card's power class and enables its cache (`mmc.c` mmc_init_card after
  mmc_hs200_tuning) — the tuning may simply be measured in a different card
  state; CONSYS power-up (2.4-3.06 s) is the other candidate.
- Build g (written, compile-checked): one retune requested
  `mt6797_post_init_retune_ms` = 500 ms after the init tuning (delayed work →
  mmc_retune_needed; performed on the next request), logged as
  "post-init retune requested"; dump labels "tuned (init)" vs "tuned".
  Plus the widest-window delay choice and quiet card-probe errors from
  build f+. Expected: the ~3.13 s CRC error disappears if the card-state
  hypothesis holds; if it persists, CONSYS is the suspect.
- 2026-10-04 ~18:35: **build g, 192 MHz, boot 1: PASS with 0 bus errors
  since boot** (read 156 MB/s, write 98-104 MB/s; 3x 1 GiB OK, 0 retunes in
  the run). Post-init retune requested at 1.44 s but performed at 3.17 s (the
  card is idle until `/init` at 3.12 s; a retune runs on the next request):
  CMD fffffffe → 16, DAT fffffc07 → 21, then the first big read was clean.
  Init tuning this boot: DAT fff00fff → 6 (widest-window rule; tie 12/12 kept
  the first). Card-state vs CONSYS cause still not separated (retune ran after
  CONSYS); either way the first post-boot I/O now uses fresh delays and any
  residual error would be recovered as before. Logs
  `logs/emmc-192g-boot1{,-dmesg}.txt`. Remaining: 1-2 more boots, then commit.
