# Gemini PDA — framebuffer, vblank and tearing

Started 2026-10-07 (evening). Goal: tear-free display on Debian/Plasma without touching panel init (CORE RULE 5).

**Status 2026-10-07 20:35: vsync1 WORKS — tearing gone (user: "so much smoother", the darkening artifacts on the power-off/Leave screen are gone too).** Boot image `boot-vsync1-20261007.img` (= bthid1 + vsync1) + the matching `geminipda-drm.ko` (old module backed up as `~/geminipda-drm.ko.bak-bthid1` on the Gemini). Next: zero-copy (step 3).

## vsync1 on the device (2026-10-07 20:30)

- dmesg: `ovl0 L0 … addr=0x7dfb0000 <- LK framebuffer`, `ovl0 L3 … addr=0x7e8b8000` (page 1), `rdma0: global_con=0x00000101 size=1080x2160 int_en=0x3f`, **`hardware vblank: rdma0 bit 2, 12 irqs in 200 ms, period 16899 us (59.17 Hz), line at irq 2160`**.
- /proc/interrupts: `MT_SYSIRQ 217 Level geminipda-drm-vblank`. CmaTotal 98304 kB.
- Stats after ~45 s of Plasma: stat_chased 2131 / stat_commits 2132 (all full frames — KWin sends full damage), **stat_chase_line_max 2160** (OUT_LINE_CNT holds 2160 through the blank → every copy started inside the blank, none late), copy ~2.9 ms typical, max 11.0 ms.
- The only kernel WARN in that boot was the known wlan `dev_addr_check` one (old wlan_gen3.ko), unrelated.
- Follow-ups: try dropping `KWIN_DRM_OVERRIDE_SAFETY_MARGIN=12000` (flip events now come at the real vblank, not after the copy); A/B `chase=0` for the record; worst-case copy 11 ms is still under the 16.9 ms scan but a heavy-load late start could tear — zero-copy removes that.

## How it works today (geminipda-drm, gemini-nixos `kernel/delta/drivers/gpu/drm/tiny/geminipda-drm.c`)

- LK initialises the panel and leaves the whole pipe running: OVL0 → OVL0_2L → OVL1_2L → COLOR → CCORR → AAL → GAMMA → OD → DITHER → RDMA0 → UFOE → DSI0 (mutex0 mask `0x05fcb400`, SOF/EOF `0x41` = DSI0 video, from ixoo's 2026-07-12 capture of stock Gemian).
- One scanout buffer in LK's vram (1088-px pitch, ARGB8888, alpha must be 0xff because LK left layer alpha enabled).
- KWin renders (panfrost via kmsro) into a shmem GEM buffer; every commit the driver **CPU-copies the damaged rows into the live scanout buffer** (~7 ms for a full frame at ~2.8 GB/s, plus a range cache sync).
- vblank = an hrtimer at 60 Hz, **not phase-locked to the real scan**. Nothing in the kernel touches a display register.
- → **Tearing cause:** the copy writes the same buffer RDMA0 is reading, at an arbitrary point in the scan. With the panel mounted rotated, the tear shows as a vertical line in landscape.

## Correction to earlier notes

- **The panel runs in DSI *video* mode, not command mode** (status.md said "command mode, frames pushed on update only"). Evidence: vendor LCM `aeon_ssd2092_fhd_dsi_solomon.c` `LCM_DSI_CMD_MODE 0` → `params->dsi.mode = SYNC_PULSE_VDO_MODE`; the NT36672 LCM uses `BURST_VDO_MODE`; our DTS comment also says video mode (sync pulse). Timings (SSD2092): VSA 1 / VBP 43 / VFP 76 lines, HSA 4 / HBP 20 / HFP 26, PLL 502 MHz, 4 lanes. The probe will confirm from DSI0 `MODE_CTRL`.
- Consequence for idle power: in video mode RDMA0 keeps reading the frame from DRAM at 60 Hz (~560 MB/s) even while blanked. The "display scanout ~0 mA" measurement in idle-power.md was taken with the scan still running, so it can't have measured switching it off. Re-check if panel sleep is pursued.
- The panel on this unit is the **Solomon SSD2092** (LK detects `aeon_ssd2092_fhd_dsi_solomon`, per the DTS comment); the geminipda-fb/-drm headers still say NT36672. Same 1080×2160 geometry. The probe prints the LCM name from the atag.

## How Planet/Gemian's Debian did it (vendor kernel 3.18 + libhybris)

Sources: `gemian/gemini-linux-kernel-3.18` (`drivers/misc/mediatek/video/mt6797/`, `lcm/`), `gemian/xf86-video-hwcomposer`.

- **Userspace:** Xorg with `xf86-video-hwcomposer` (Android HWComposer through libhybris). X renders with GLES into an Android native-window buffer (rotation done in the shader, `Option "Rotate"`), then `hwc prepare()/set()`; it waits on the previous **retire fence** before the next frame → one frame per vsync. Zero copies in the X driver.
- **Kernel:** the Android HWC calls `/dev/mtk_disp_mgr`; `primary_display_config_input_multiple()` points an **OVL layer's address register at the new buffer** (page flip, no copy). Even the fbdev path flips: `mtkfb_pan_display` changes the layer address by `yoffset` across **3 fb pages** (`MTK_FB_PAGES = 3`); FB layer = OVL0 layer 0.
- **When the write lands:** in video mode the register writes go through CMDQ/GCE, which waits for `CMDQ_EVENT_DISP_RDMA0_EOF` + `CMDQ_EVENT_MUTEX0_STREAM_EOF` (end of the frame), so the new address is written in the vertical blank and the next frame reads it in full.
- **vsync:** `DISP_PATH_EVENT_IF_VSYNC` is mapped to **`DDP_IRQ_RDMA0_DONE`** (RDMA0 frame-done interrupt, `INT_STATUS` bit 2; bit 1 = frame start; cleared by writing 0). DSI0 frame-done only when low-frame-rate mode is on.
- Summary: **real vsync IRQ + page flip of the OVL layer address during vblank, no CPU copy.** That is exactly what geminipda-drm lacks.

## Register facts (vendor `ddp_reg.h`, matches mainline MT8173 generation)

| Block | Base | Notes |
|---|---|---|
| OVL0 | 0x1400b000 (SPI 213) | 4 layers; SRC_CON 0x2c (layer enables); L*n* CON 0x30+0x20n (CFMT [15:12], AEN bit 8, BTSW bit 24), SRC_SIZE 0x38+0x20n, PITCH 0x44+0x20n, **ADDR 0xf40+0x20n**. No shadow registers found in the vendor code. |
| OVL0_2L / OVL1_2L | 0x1400d000 / 0x1400e000 | 2 layers each, same layout |
| RDMA0 | 0x1400f000 (**SPI 217**) | INT_ENABLE 0x0, INT_STATUS 0x4 (bit1 frame start, bit2 frame done), GLOBAL_CON 0x10 (bit1 = memory mode), OUT_LINE_CNT 0xfc (beam position) |
| DSI0 | 0x1401c000 (SPI 229) | MODE_CTRL 0x14 (0 cmd / 1 sync-pulse / 2 sync-event / 3 burst), VSA/VBP/VFP/VACT 0x20–0x2c |
| MUTEX | 0x1401f000 (SPI 202) | mutex*n* EN 0x20+0x20n, MOD 0x2c+0x20n, SOF 0x30+0x20n |

`CONFIG_DEVMEM=y`, `STRICT_DEVMEM` doesn't block MMIO; the DT nodes for these blocks are `okay` but no driver binds (DRM_MEDIATEK is hard-banned), so nothing claims the regions.

## Plan (each step boot-image only, no panel init, CORE RULE 5 respected)

0. **Probe (read-only)** — `bin/gemini-disp-probe.py` on the Gemini: `sudo python3 gemini-disp-probe.py 2`. Answers: which OVL layer holds the LK fb address (expect OVL0 L0), pitch/format/AEN, **vram size (how many frames fit → double buffering inside LK's reservation?)**, DSI mode, real refresh rate and vblank length from RDMA0's line counter, RDMA mode (direct-link vs memory). If everything reads 0 → stop (unclocked/DEVAPC).
1. **vsync1 — real vblank + beam-chasing copy.** Map RDMA0, `request_irq(SPI 217)`, enable only the frame-done bit in RDMA0 `INT_ENABLE` (the only register write), clear status in the handler, call `drm_crtc_handle_vblank()` from it (hrtimer stays as a fallback, module param). Do the copy right after frame-done: a full-frame copy (~7 ms) outruns the scan (~15.8 ms for 2160 lines), so if it starts inside the ~0.85 ms blank the beam never overtakes it → no tear, no extra memory. Costs up to one frame of latency. Risk: a late start under heavy CPU load can still tear occasionally.
2. **flip1 — double buffering in LK's vram** (if the probe shows ≥ 2 frames). Copy into the back buffer, then write the layer's ADDR register in the frame-done IRQ (like the vendor CMDQ path). Copy = this commit's damage + the previous commit's (buffer age 2). Restore LK's address on remove/shutdown. Tear-free regardless of load.
3. **zero-copy — scan out KWin's buffer directly.** GEM objects from `drm_gem_dma` (CMA, contiguous, below 4 GiB for the 32-bit OVL address); flip = ADDR + PITCH writes in vblank; XRGB needs layer alpha off (L_CON AEN = 0) or keep forcing 0xff. Removes the ~7 ms copy and ~560 MB/s of CPU memory traffic per 60 fps. Needs CMA raised from 32 MB to ~64–96 MB (3 KWin buffers ≈ 28 MB). This is the Gemian design in mainline form. KWin's `KWIN_DRM_OVERRIDE_SAFETY_MARGIN` / `cache_sync` tweaks may become unnecessary.
- Not planned: full mediatek-drm/DSI ownership (re-drives the panel; rule 5).

## Probe results (2026-10-07 20:08, boot-bthid1)

- atag: fb_base **0x7dfb0000**, vram **0x1f90000 = 31.6 MiB = 3.52 pages** of 1088×2176×4 (page = 0x908000), LCM `aeon_ssd2092_fhd_dsi_solomon`, fps field 5921 (59.21 Hz).
- **OVL0 L0** = LK fb page 0 (pitch 4352, CON 0x21ff: fmt 2, AEN 1, plane alpha 0xff) — this is what geminipda-drm writes.
- **OVL0 L3 is ALSO enabled**, on top of L0: page 1 (fb + 0x908000), pitch 4320, same format, AEN 1. Probably LK's assert/DAL layer and transparent (alpha 0) — otherwise nothing we draw would show. It costs a second full-frame DRAM read every frame (~560 MB/s). Pages 0 and 1 are taken; **page 2 (fb + 0x1210000) is free** → double buffering fits. Disabling L3 (after checking it is transparent) would free page 1 and the bandwidth.
- OVL0_2L / OVL1_2L: no layers enabled (pass-through).
- RDMA0: **direct-link** (fed by OVL, GLOBAL_CON 0x101), 1080×2160, **INT_ENABLE 0x3f left on by LK**, INT_STA 0x26 pending (bits 1, 2, 5). → the driver must own INT_ENABLE (fixed in the patch: only its own bit, or 0).
- DSI0: **VIDEO sync-pulse** (MODE_CTRL 1) confirmed; VSA/VBP/VFP/VACT 1/43/76/2160.
- Mutex0 EN 1, MOD 0x05fcb400, SOF 0x41 (same as stock Gemian).
- RDMA0 OUT_LINE_CNT 0..2160; **59.17 Hz, 16.900 ms frame (±0.05 ms)**; 2280 lines total → 7.41 µs/line, blank 120 lines ≈ 0.89 ms.
- Copy cost today: stat_copy_us_last 3003, **max 14903 µs** (cache_sync off) — the worst case is close to a whole scan, so the beam-chasing copy can still lose occasionally; double buffering (page 2) or zero-copy is the robust fix.

## vsync1 — what the patch does (`vsync1-hw-vblank.patch`)

- `geminipda-drm.c`: DT phandles `mediatek,rdma = <&disp_rdma0>` / `mediatek,ovl = <&disp_ovl0>`. Probe logs OVL0 L0–L3 (en/con/pitch/addr, marks the layer holding the LK fb; read-only), RDMA0 global_con/size/int_en/line, then a **200 ms self-test** of RDMA0's frame interrupt (SPI 217, `INT_STATUS` bit `vblank_irq_bit`, default 2 = frame done). Pass = 5–30 IRQs with an 8–34 ms period → hardware vblank; otherwise (silent, storm, RDMA not running, no phandle) it logs why and keeps the old hrtimer vblank. Only RDMA0 `INT_ENABLE` (own bit) and `INT_STATUS` (write-0-to-clear) are written; restored on remove/shutdown; masked while the CRTC's vblank is off (DPMS).
- **chase** (0644, default 1): each page-flip commit waits for the next frame-done, sends the flip event with that vblank's timestamp, then copies — the copy (≤ ~7 ms) stays ahead of the scan (~16 ms). Modesets and `refresh_hz=0` are not chased.
- New read-only params: `stat_vblank_irqs`, `stat_hw_period_us` (measured refresh), `stat_irq_line` (RDMA0 OUT_LINE_CNT in the IRQ), `stat_chase_line` / `stat_chase_line_max` (line counter when a chased copy starts — small = in time; write 0 to reset max), `stat_chased`. Load-time params: `hw_vblank` (default 1), `vblank_irq_bit` (2; try 1 = frame start if 2 is silent).
- DTS: the two phandles. Config: `CONFIG_CMA_SIZE_MBYTES` 32 → 96 (for step 3; boot-only, no module ABI change).
- Old module + new DT → ignores the phandles; new module + old DT → software vblank. Both safe.
- Compile-tested in Claude's workspace (v6.6.157 + delta + config, clang 18, `W=1` clean; DTB builds, phandles verified in the decompiled DTB). Module link/modpost not run (needs the full build).

### Test (tonight)

1. Probe on the current image (Gemini): `sudo python3 ~/gemini-disp-probe.py 2` → paste output. If everything reads 0, stop.
2. Hydra: `git apply vsync1-hw-vblank.patch && git add -A && git commit -m "geminipda-drm: RDMA0 hardware vblank + vblank-aligned copy (vsync1)"`; `git add bin/gemini-disp-probe.py` too. Build boot image + kernel (module): `IMG=$(nix build .#packages.aarch64-linux.bootimg --print-out-paths --no-link)`; `to_dragon "$IMG" boot-vsync1-20261007.img`; `KER=$(nix build .#packages.aarch64-linux.kernel --print-out-paths --no-link)`; `find -L $KER -name 'geminipda-drm.ko*'` → `to_gemini <that file>`.
3. Gemini: back up and replace the module at `/sbin/modinfo -n geminipda_drm` (same compression as the existing file). Dragon: `flash_boot boot-vsync1-20261007.img` (fallback: `boot-bthid1-20261007.img`).
4. After boot: `sudo dmesg | grep -i geminipda`; `grep geminipda /proc/interrupts`; `cd /sys/module/geminipda_drm/parameters && grep . stat_*`; `grep Cma /proc/meminfo` (CmaTotal 98304 kB). Eyes: drag/scroll fast in landscape; A/B with `echo 0 | sudo tee chase`.
5. If it still tears with chase on: look at `stat_chase_line_max` (late starts) and `stat_irq_line` (is bit 2 really frame done?); try `vblank_irq_bit=1` via `/etc/modprobe.d/`. KWin's `KWIN_DRM_OVERRIDE_SAFETY_MARGIN=12000` can probably drop now (flip events come at the vblank, not after the copy) — test later.

## Open questions for the probe

- Is the LK fb on OVL0 L0, and is `vram` ≥ 2 × 9.40 MB (1088 × 2160 × 4)? `MTK_FB_PAGES = 3` in the vendor kernel suggests LK reserves ~3 pages.
- Does OVL apply ADDR writes immediately or at frame start? (Decides whether the IRQ-time write needs to land inside the blank.)
- Refresh rate: 60 Hz expected (LK `fps` field in the atag).

## Panel off/on (stock sequence) — research 2026-10-21 (read-only, nothing tested yet)

Goal: the panel is truly off (bias off, reset held) for the whole of every sleep, as stock does, so the LCD is never
left biased and unrefreshed (6 burns so far). Sources: vendor 3.18 kernel gemian/gemini-linux-kernel-3.18 8cfe659
(`drivers/misc/mediatek/lcm/aeon_ssd2092_fhd_dsi_solomon/`, `gpio/lp3101.c`, `video/mt6797/dispsys/ddp_dsi.c`,
`videox/primary_display.c`, `arch/arm64/boot/dts/aeon6797_6m_n.dts`).

### Facts
- **Panel**: SSD2092 (cmdline LCM `aeon_ssd2092_fhd_dsi_solomon`, ID 0x01572098). AGENTS.md rule 5 still says NT36672;
  that driver also exists in the vendor tree but is not the fitted panel. SSD2092 is a TDDI: touch is the same chip.
- **Stock suspend** (`primary_display_suspend`): `dpmgr_path_stop` (DSI → command mode, wait INTSTA bit 31 BUSY clear;
  OVL/RDMA stopped) → `lcm_suspend` → `dpmgr_path_power_off` (DSI clock lane LP, lanes to ULPS, clocks/PHY off).
- **Stock `lcm_suspend`**: DCS 0x28, 50 ms, DCS 0x10, 120 ms, +10 ms, RESET low, ENN (GPIO251) low, ENP (GPIO60) low.
- **Stock resume** (`primary_display_resume`): DSI power on + path reset/config (DSI still in command mode) →
  `lcm_resume` → `dpmgr_path_start` (DSI → video mode, HS clock, START).
- **Stock `lcm_resume`** = `lcm_poweron` + `lcm_init`:
  RESET low, ENP high, ENN high, 20 ms, `lp3101_poweron()`, RESET high, 10 ms, low, 10 ms, high, 20 ms, then the init
  table. **The table is 186 commands** (the "369" in the handover counted the commented-out copy above it too); the
  last two are 0x11 + 120 ms and 0x29 + 50 ms. Longest packet: cmd + 22 bytes.
- **Init table in the repo**: `kernel/delta/drivers/gpu/drm/tiny/geminipda-ssd2092-init.h`, generated from the vendor
  file. The older mainline port `drm/panel/panel-solomon-ssd2092.c` (written for the banned mediatek-drm stack) has the
  same 186 commands byte for byte, plus three **non-stock** extras at the end (0x51 0xFF, 0x53 0x24, 0x55 0x00, a July
  CABC workaround). Don't send those. No LK source or dump to compare against; LK builds the same lcm file
  (`#ifdef BUILD_LK` branches), so the table is the same.
- **Packet types** (vendor `DSI_set_cmdq_V2`, and the July lesson in panel-solomon-ssd2092.c): cmd < 0xB0 → DCS
  (0x05 no param / 0x15 one param / 0x39 long); cmd ≥ 0xB0 → **generic** (0x13 / 0x23 / 0x29 long). Sending the
  0xB0+ registers as DCS leaves the booster off (black screen).
- **LCD bias (LP3101, TPS65132-class)**: I2C1 (`i2c@11008000`), addr 0x3E, already in our DT as `bias@3e`
  (`ti,tps65132`, no driver bound — banned). `lp3101_poweron()` = write reg 0x00 = 0x0F, reg 0x01 = 0x0F
  (VPOS/VNEG, 4.0 V + 0.1 V·n → 5.5 V). Our DT comment claims LK writes 0x0E (5.4 V): **read 0x00/0x01 live before
  writing anything** and use what LK left. Registers are volatile across an EN power cycle (DT comment, boot.md
  capture 250), so they are rewritten on every resume, as stock does. Also on I2C1: FUSB301A @0x25 (driver bound).
- **Reset**: GPIO180 in its LCM_RST function; stock drives it with **MMSYS_CONFIG + 0x150 bit 0**
  (0x14000150, `MMSYS_LCM_RST_B`; 1 = released). Not a GPIO write; the pin mux stays as LK left it.
- **Bias enables**: ENP GPIO60, ENN GPIO251, plain GPIO outputs, active high (live 2026-10-20: both high).
- **DSI0** (0x1401c000): START 0x00 (bit 0 DSI_START, bit 16 VM_CMD_START), INTSTA 0x0c (bit 1 CMD_DONE, bit 5
  VM_CMD_DONE, bit 31 BUSY), MODE_CTRL 0x14 (MODE 0 = command, 1 = sync-pulse video), CMDQ_SIZE 0x60,
  PHY_LCCON 0x104, VM_CMD_CON 0x130, VM_CMD data 0x134–0x143 (**16 bytes only**, vendor `DSI_VM_CMDQ_REGS data[4]`; 0x144 = CKSM_OUT,
  0x148 = STATE_DBG0), STATE_DBG1 0x14c,
  CMDQ data 0x200.

### Two ways to send the panel commands
1. **Video mode, VM_CMD** (what our 0x28/0x29 tests used). Vendor `DSI_set_cmdq_V2` video branch, per command:
   VM_CMD_CON = VM_CMD_EN | LONG_PKT(len>2) | TS_VFP_EN (bit 5, already set) | ID<<8 | (short: d0<<16 | d1<<24;
   long: wc=len<<16), long payload byte-wise into 0x134+ (byte 0 = cmd); START bit 16 0→1; wait INTSTA bit 5; clear it.
   The video stream and RDMA/OVL keep running, so nothing else in the pipeline has to restart. **Not stock**: stock
   never sends the panel off/on sequence with video running, and the panel would see video frames while in reset and
   during init.
2. **Command mode, CMDQ** (stock). MODE_CTRL = 0, wait BUSY clear, send through CMDQ (0x200 + CMDQ_SIZE + START,
   wait CMD_DONE), then back to video: MODE_CTRL = 1, START 0→1. This also needs the RDMA0 (and mutex) stop/reset/start
   that `dpmgr_path_stop/start` does, or the restarted frame can come out shifted. More register writes outside the
   panel, so more to test.
- **Recommendation**: panel side exactly stock in both cases. DSI side: start with (1) for the awake tests (proven
  path, smallest change), and move to (2) plus ULPS once the panel sequence is proven, if the user wants the full
  stock behaviour. The burn fix is the bias/reset part, which both give.

### Where the code goes
- **geminipda-drm**, not mt6797-suspend or a userspace script. It already owns the screen state (CRTC
  enable/disable, backlight, OVL L0, RDMA0 maps). Add `dev_pm_ops` suspend/resume (`drm_mode_config_helper_suspend/
  resume`) so every system suspend runs the panel off from the kernel, before the SPM code, and resume runs the full
  stock init. A hang mid-sleep then leaves the panel unbiased. Panel off on DPMS blank (stock does this on every screen
  off) as a second step behind a module parameter.
- New DT links from the geminipda-drm node: `&dsi0` (regs, of_iomap like RDMA/OVL), `&mmsys` (syscon, reg 0x150),
  `&lcd_bias` (I2C client: the core already instantiates `bias@3e` with no driver, `of_find_i2c_device_by_node`),
  ENP/ENN GPIOs requested with **GPIOD_ASIS** (no change at probe).
- Order on resume: panel init in DRM resume (kernel), touch re-init after (today: the userspace rebind stopgap in
  gemini-suspend-prep / s2.sh; later ssd2092 touch PM ops). The prep's DCS 0x28/0x29 can go once the driver does it.
- Rule 5: this is targeted panel writes with LK's DSI setup kept, not mediatek-drm/tps65132 (still banned). Needs
  the AGENTS.md text updated (panel name, the panel-off exception) when the code lands.

### Read-only checks still to do on the Gemini (it was offline 2026-10-21)
- `i2cget -y -f <i2c1 bus> 0x3e 0x00` / `0x01` / `0x03` → LK's bias values (0x0E or 0x0F?).
- 0x14000150 = 1? GPIO180 mode = LCM_RST? GPIO60/251 high? DSI0 MODE_CTRL 0x1, VM_CMD_CON 0x21, INTSTA.
- Which `/dev/i2c-N` is i2c@11008000.

### Implementation: panel-pm in geminipda-drm (2026-10-21, built, NOT yet run on the device)
- Choice (user, 2026-10-21): option 1 (video mode, VM_CMD), panel side exactly stock, safeguards 1–6:
  fail-to-off; no automatic retry; LK's bias values read at probe + read back after writing; every command waits
  for VM_CMD_DONE (100 ms timeout); guard hrtimer (`panel_guard_ms`, 6000) forces reset + bias off if "on" stalls;
  backlight only after a good init and only with the CRTC on. Status read-back (0x0A = 0x9C) left out (needs
  command mode); add it if the tests show anything odd.
- Probe only reads. `panel-pm: LK state: …` in dmesg logs LCM_RST_B, bias VPOS/VNEG, DSI0 MODE/START/VM_CMD_CON;
  any surprise → "unavailable", the panel stays as LK left it. Requesting GPIO60/251 sets their mux to GPIO, which
  is what LK already uses.
- Params: `panel` (read on/off/failed/unavailable; write off|on), `panel_suspend` (default 0: suspend not hooked
  until the awake tests pass), `panel_guard_ms`, `stat_panel_on_ms`, `stat_panel_err`, `stat_panel_err_cmd`,
  `stat_panel_cycles`.
- VM_CMD sends at most one packet per frame's front porch, so "on" may take up to ~186 × 16.9 ms ≈ 3.3 s
  (+ ~230 ms of stock delays); `stat_panel_on_ms` measures it. The guard is 6 s for that reason.
- Needs both the new DTB (boot image) and the new module. Old DT → "no mediatek,dsi in DT, unavailable".
- Test helper: `bin/sleep-tests/panel-cycle.sh state | off | on | cycle [secs] [count]` (prints state, errors,
  init time, GPIO60/251, 0x14000150). gemini-suspend-prep / s2.sh still send DCS 0x28/0x29 themselves; drop that
  once panel_suspend is on by default.
- Live read-only checks 2026-10-21 (Gemini, 6.6.157, busybox i2cget/devmem/debugfs): LP3101 on i2c-1 (client
  `1-003e` exists, no driver) VPOS 0x0F, VNEG 0x0F (5.5 V, = vendor `lp3101_poweron`; the old DT comment's 0x0E was
  wrong), reg 0x03 0x33. 0x14000150 = 1 (reset released). DSI0 START 0x1, INTSTA 0x80000792, MODE 0x1, VM_CMD_CON
  0x21. GPIO60/251: mux 0 (GPIO), output, high; GPIO180: mux 1 (LCM_RST), line high. All three unclaimed.

### Test 1 (2026-10-21 ~10:44) and the 16-byte fix
- Boot with boot-panelpm-20261021.img (sha256 e4d532be…) + module: `panel-pm: LK state: LCM_RST_B 0x1, bias VPOS 0xf
  VNEG 0xf, DSI0 MODE 0x1 START 0x1 VM_CMD_CON 0x21` → ready. User checked a mid-grey screen first: no ghosting.
- `panel-cycle.sh cycle 10 1`: off OK (reset 0, ENP/ENN low for 10 s), on OK by the driver (all commands
  VM_CMD_DONE, **861 ms**, not ~3.3 s: several packets per front porch). **On glass: ~70 % of colours hue-shifted
  ("rainbow"), no flicker, no ghosting**; the user forced a power-off.
- Cause: the VM_CMD data slot is 16 bytes, not 24 (misread). The two 23-byte gamma packets (0xC3) went out cut
  short (gamma wrong → hue shift; everything else was ≤ 11 bytes and landed); the extra words hit CKSM_OUT /
  STATE_DBG0 (read-only status, harmless).
- Fix (user chose "A" over command mode): each 0xC3 packet split in two with the start offset advanced
  (0x00 + 13 values, 0x0D + 8; 0x15 + 13, 0x22 + 8). The vendor's own pair (offset 0x00 then 0x15 = 21 = the
  number of values in the first) shows param 1 is a start offset. Table struct is now `d[16]` (compile-time
  limit), send() refuses > 16, probe checks the whole table. If colours are still wrong → command mode (option 2).
- **Test 2 (2026-10-21 10:57, 6490f26 module): one cycle 10 s off → on in 844 ms, all 188 packets done; on glass
  (user): colours right, no artifacts, touch works.** Fix A confirmed. Next: 5 → 20 awake cycles, then short sleeps.
- Test 3 (2026-10-21 10:59–11:06): 5 + 20 awake cycles (10 s off, 5 s gap), all off with reset 0 + ENP/ENN low, all
  on err=0, 813–898 ms. User: no fog, colours right, touch works.
- Test 4 (11:09) failure path: `panel_guard_ms=100` → "on" → guard fired, bias off + reset held, `FAILED (-110 at
  init step 0)`, state "failed" held 10 s (bias low, reset 0), no retry; normal "on" recovered (868 ms). No
  atomic-context warnings from the hrtimer path (regmap/gpiod in hardirq OK).
- **Test 5 (2026-10-21 11:14–11:27) short sleeps:** `panel-sleep.sh 10 30 pcm_wdt=30` (test module b307168 with
  bias_check). Runs 1–9: bias before sleep `ENP 0 ENN 0`, wake rc 0, panel on 824–852 ms, err 0, 181–185 mA over
  the ~34 s kernel window (was ~208 mA with the bias on; unconfirmed). User: screen fine after every wake.
  Run 10: the known random sleep hang (breadcrumb b5010006 = PSCI suspend, cycle 6, ~12 s in); watchdog reset
  ~90 s after suspend start, no power-hold. The panel was off: bias_check refuses .prepare unless ENP/ENN read 0,
  and SPM cycles ran. First hang with the panel unbiased.
- **Test 6 (2026-10-21 11:34–11:39) wake paths**, everyday-style suspend via `bin/sleep-tests/wake-test.sh`
  (gemini-suspend-prep pre → echo mem → post; sleep.target stays masked; test module b307168 max_s=0
  wake_keys=1 wake_lid=1 pcm_wdt=30; panel_suspend=1 at runtime). All four: bias `ENP 0 ENN 0` before sleep,
  rc 0, panel on 813–845 ms, err 0. A Esc/On key (21 s), B silver key (27 s), C lid shut → opened
  (`last_wake=lid opened`, 28 s), D from the lock screen → back on the lock screen (28 s). User: screen back each time.
  Not covered: Plasma/logind starting the suspend (needs sleep.target unmasked).
- Test 7 (11:41) reboot with the panel off (bias low, reset held): LK re-initialised it, driver "ready" with the
  usual LK state; defaults back (installed module without bias_check, panel_suspend=0, suspend masked).
- **Test 8 (2026-10-21 11:43–12:37) soak:** `panel-cycle.sh cycle 10 200`: 200/200 off with reset 0 + ENP/ENN low,
  200/200 on err=0 (≤ 892 ms, typ. ~850), no kernel warnings. Plasma idle-blanked the screen mid-run: the driver kept
  the backlight off (CRTC off) and lit it at the next panel-on after a key press, as designed.
- 12:45 mid-grey check after all tests (user): clean, no ghosting. **Awake + short-sleep acceptance done.**
