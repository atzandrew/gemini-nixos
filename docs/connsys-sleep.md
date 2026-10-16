# Connsys sleep — status

**Last updated: 2026-10-15 (night).** Status: **a real CONN power-off now works** (glue
`conn_pwr_off`, cef35fd). With connsys powered off, PCM_REG13 bits 13/14/27 clear and the SPM
firmware runs the **26M-off step for the first time** (SW_DEBUG bits 0/1, SPM sleep ~347 mA vs
~390–400 mA). DDREN still not run. The re-power-on timeout is fixed (13af906, see below). Connsys still
never *sleeps* while a function is on. Running log: `~/Build/notes/connsys-sleep.md` on Dragon.

Why it matters: connsys holds PCM_REG13 (0x10006134) bit 14 CONN_SRCCLKENA high, which blocks the
SPM's 26 MHz-off and DDREN (DRAM self-refresh) steps (see `docs/spm-sleep.md`) and keeps the
connsys rails on (~43 mA).

Success criterion (stock): `spm_is_conn_sleep()` = `!(PCM_REG13 & R13_CONN_SRCCLKENA)`
(vendor `spm_v2/mt_spm_sleep.c`; bit 13 CONN_STATE, bit 14 CONN_SRCCLKENA per
`pcm_def_mt6797.h`).

## What was tested (2026-10-15, r13 sampled every 100 ms, Gemini on Wi-Fi)

Every state below kept bits 13/14 high in **every** sample (≈3000 samples in total).

| # | state | result |
|---|---|---|
| E0 | `bluetoothctl power off` | high |
| E1 | + WAK heartbeat stopped (`btif_wak_hb_ms=0`) | high |
| E2 | + Wi-Fi Fast-PS (`iw dev wlan0 set power_save on`) | high |
| E3a | + `nmcli radio wifi off` | high |
| E3b | stock STP PSM on (test hook, see below) — firmware ACKs sleep, host INACT | high |
| E3c | PSM + Wi-Fi PS, PSM + radio off | high |
| E4 | BT WMT function really off (`b:0`) + PSM + Wi-Fi PS / radio off | high |
| E5 | all WMT functions off (`b:0 w:0 wmt:0`, `HW_PWR_OFF ok`) | high |

Runtime controls used (all stock WMT debug paths; `/proc/driver/wmt_dbg` takes hex):
- PSM: `echo 1 > /sys/module/mtk_wcn/parameters/wmt_psm_allow`, then
  `echo "0 1e" > /proc/driver/wmt_dbg` (on, 30 ms idle = stock) / `"0 0"` (off).
- WMT function off/on: `"7 <type> 0"` / `"7 <type> 1"` (type 0 = BT, 3 = Wi-Fi).
  Wi-Fi func off/on recovered without a reboot (one 3 s wmtd timeout during re-power-on).

## Findings

1. **The "NULL deep-idle backend" never stopped connsys sleeping.** `psm_core.c` sets the PSM state
   INACT (firmware sleep handshake done) *before* calling `mt_combo_plt_enter_deep_idle()`, and
   ignores its return value. In stock the backend (`mtk_wcn_btif_dpidle_ctrl`) only gates the
   host-side BTIF/DMA clocks. The PSM itself works on this port. This corrects the reasoning in
   `docs/bluetooth-a2dp.md`.
2. **Likely real cause of the A2DP stutter: the chip→host wake interrupt is not wired.** Stock
   requests the BGF EINT (`MT_CONN2AP_BTIF_WAKEUP_IRQ`, DT `mediatek,mt6797-consys`,
   `wmt_plat_bgf_irq_isr`). In `wcn_hw_glue.c`, `wmt_plat_eirq_ctrl`/`wmt_plat_irq_cb_reg` are no-ops,
   so a PSM-asleep chip can't wake the host for RX. A `MTKSTP_RESYNC2` appears ~1 s after every PSM
   sleep entry. Not yet verified with A2DP.
3. **Bug (fixed in d5967d5): Bluetooth off never turned the BT core off.** On 6.6 the core clears
   `HCI_RUNNING` before `->close()`; `hci_stp_close()` used the old `test_and_clear_bit` guard, so
   it always returned early and `mtk_wcn_wmt_func_off(BT)` never ran.
4. **The WAK heartbeat** (`btif_wak_hb_ms`, 30 ms) runs whenever the BTIF link is open, which is from
   WMT power-on, not just while BT is used. It is non-stock. It did not turn out to be the holder.
5. **No real power-off:** `mtk_wcn_cmb_hw_pwr_off()` in the glue is a no-op, so "all functions off"
   leaves the consys MCU powered and running.
6. **Rails:** stock puts VCN28 and VCN33 (BT/Wi-Fi) in PMIC HW mode (`RG_*_ON_CTRL=1`, follow
   SRCCLKENA). Ours: VCN28 HW mode ✔ (0x0a0c=0xda6a), VCN18 SW (as stock),
   VCN33_BT/WIFI SW (0x0a98/0x0a9a=0x1802, ON_CTRL=0) — differs from stock.
7. Not levers: 0x10001f00 bit 9 (AP2CONN_WAKEUP, an active-low force-assert debug pin — high is
   normal) and `BTIF_SLEEP_EN` (stock never calls `btif_sleep_ctrl`).

## Real CONN power-off (E6, E7, SPM A/B — 2026-10-15)

- E6: during the spike's B-27 power cycle (CONN domain off ~120 ms, SPM PWR_CON 0x10d → 0x112)
  r13 reads 0x00000000 / 0x00040000 → bits 13, 14 and 27 clear. The 26M request is inside connsys.
- WMT order: power-off = STP deinit (BTIF released) → `HW_PWR_OFF` → `wmt_plat_pwr_ctrl(FUNC_OFF)`;
  power-on = `HW_PWR_ON` → `wmt_plat_pwr_ctrl(FUNC_ON)` → STP init → BTIF hand-off (B-27).
- cef35fd: `conn_pwr_off` (mtk_wcn param, default 0). 1 → FUNC_OFF does `pm_runtime_put_sync` on the
  spike's consys device (CONN domain off, same mechanism as B-27); FUNC_ON gets it back before STP
  init so B-27 stays balanced. CONN register reads in the glue are skipped while off.
- E7: all functions off with `conn_pwr_off=1` → domain off for 30 s, r13 clear in 300/300 samples;
  Wi-Fi back afterwards.
- SPM stage 6, run-B config of 2026-10-14 (`pdn=1 acinactm=0 clk_init=1 settle=1`, flags 0x5fc102ff,
  src_mask 0x10aa007e, Wi-Fi off, 25 × 2 s), USB unplugged:

| run | CONN | SPM sleep | dbg | r13 at wake |
|---|---|---|---|---|
| C1 | off | **347 mA** | **0x10ff** (26M bits 0/1) ×25 | 0x00060000 |
| B | on | 402 mA | 0x10fc | 0x00066000 |
| C2 | on (power-off skipped, see below) | 387 mA | 0x10fc | 0x00066000 |

- Re-power-on (fixed in 13af906): every domain power-on after an off hit `wait completion timeout opId(3)`
  (3 of 3, E7 included). opId(3) = WMT_OPID_FUNC_ON; stock `wmt_exp.c` caps the **Wi-Fi** func-on wait at
  4 s, and after `wmt:0` that call must do the whole power-on incl. the ROM patch push, which took ~10 s here
  (~40 ms/fragment). Cause: the spike's live STP reader (`consys_wmtrxd`) used `btif_rx_drain()` with a 30 ms
  idle wait after every reply. With the idle wait removed for that reader: patch push ~1 s (boot and
  re-power-on), HCI round trips 4–11 ms (were 40–50), Wi-Fi func-on incl. power-on 2.8 s; E8 (3 off/on
  cycles) clean, Wi-Fi back 4 s after radio on. Earlier, in the C1→B case the link later failed
  (`Ack Miss trigger firmware assert` during run B's sleep) → whole-chip reset, which never recovers on this
  port (`wifi_reset_start` no-op); not yet re-tested with the fix.
- Procedure rule: take BT down through BlueZ (`bluetoothctl power off`) before a WMT BT func off; turning
  the BT core off under an UP hci0 leads to tx timeouts → whole-chip reset (happened once).

## Next steps

1. Re-run the SPM A/B/A with the rx-idle fix (does the ack-miss assert recur?), then whole-chip reset
   recovery. Then the rails in the off state (stock also turns VCN18/28/33 off)
   and DDREN (SW_DEBUG bits 8/9), which still does not run with CONN off.
2. For sleep while Wi-Fi is on: wire BGF EINT (stock), drop the WAK heartbeat, enable the PSM, and
   retest A2DP (resyncs / `stp_do_tx_timeout` / throughput as in `docs/bluetooth-a2dp.md`).
3. Then VCN33 HW mode and SPM stage 6 (`pdn=1 io_en=0x81 acinactm=0 src_mask=0x10aa007e
   clk_init=1 settle=1`, look for SW_DEBUG bits 0/1 and 8/9).

## Code state

- `463e816` — `wmt_psm_allow` module param (default 0 = A2DP force-off unchanged).
- `d5967d5` — hci_stp close fix (not yet deployed).
- `cef35fd` — glue `conn_pwr_off` (deployed, default 0).
- `13af906` — spike: no RX idle wait in the live STP reader (built in; flashed as boot-rxidle-20261015.img).
- The Gemini runs a cef35fd `mtk_wcn.ko` (previous: `mtk_wcn.ko.psm-463e816`); the original is kept as
  `/lib/modules/6.6.157/kernel/drivers/misc/mediatek-connectivity/mtk_wcn.ko.orig-20261015`.
