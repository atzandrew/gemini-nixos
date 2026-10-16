# Connsys sleep — status

**Last updated: 2026-10-15.** Status: **connsys never sleeps and never powers off on this port**,
in any WMT state. Root cause not found yet; the next step is a real (stock-shaped) consys
power-off. Running log: `~/Build/notes/connsys-sleep.md` on Dragon.

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

## Next steps

1. Implement the stock consys power-off (`mtk_wcn_consys_hw_reg_ctrl(0)`: AXI bus protect, ISO,
   MTCMOS off, VCN rails off) behind `mtk_wcn_cmb_hw_pwr_off`, then check r13 with all functions off.
   If bit 14 still stays high with connsys powered off, the holder is outside connsys.
2. For sleep while Wi-Fi is on: wire BGF EINT (stock), drop the WAK heartbeat, enable the PSM, and
   retest A2DP (resyncs / `stp_do_tx_timeout` / throughput as in `docs/bluetooth-a2dp.md`).
3. Then VCN33 HW mode and SPM stage 6 (`pdn=1 io_en=0x81 acinactm=0 src_mask=0x10aa007e
   clk_init=1 settle=1`, look for SW_DEBUG bits 0/1 and 8/9).

## Code state

- `463e816` — `wmt_psm_allow` module param (default 0 = A2DP force-off unchanged).
- `d5967d5` — hci_stp close fix (not yet deployed).
- The Gemini runs a 463e816 `mtk_wcn.ko`; the original is kept as
  `/lib/modules/6.6.157/kernel/drivers/misc/mediatek-connectivity/mtk_wcn.ko.orig-20261015`.
