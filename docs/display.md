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
