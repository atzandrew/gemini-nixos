# Bluetooth A2DP audio — stutter + after-playback crackle (ROOT-CAUSED)

**Last updated: 2026-09-10 (r).** Status: **root cause found, fixed in the
kernel delta and VERIFIED ON GLASS** (`wmt_lib.c`: STP PSM forced off;
toplevel `94bw47dh6bhr1rk4jafva9nimz3avwjb`, gen7, after a WDT reboot).
Kernel rebuild/deploy receipts are in `docs/session-log.md` 2026-09-10r.

> **Correction (2026-10-15, `docs/connsys-sleep.md`):** the PSM's missing
> deep-idle backend did *not* stop the chip sleeping — the firmware sleep
> handshake completes and the backend only gates host-side BTIF clocks. The
> more likely cause of the stutter is the unwired chip→host wake interrupt
> (BGF EINT), so a PSM-asleep chip can't wake the host for RX. The force-off
> fix below still stands until that is wired and A2DP is retested.

Related: `docs/bluetooth-bringup.md` (the HCI-STP/WMT bring-up) ·
`services/audio.nix` (the one system PipeWire/WirePlumber session) ·
kernel delta `devices/planet-geminipda/kernel/delta/drivers/misc/mediatek-connectivity/`.

Headset under test: **Sony MDR-ZX330BT** (`00:18:09:28:45:FB`), A2DP **sink**
(the device is the A2DP *source*), negotiated codec **SBC**.

## Symptom (reported)

- A2DP playback stutters **predictably** even with the device otherwise idle.
- **Crackling after playback**, like a ring buffer that was never purged.

## Root cause — the STP power-save mode (PSM)

The **STP PSM** (`wmt_lib_ps_enable()` → `mtk_wcn_stp_psm_enable()` →
the `psm_core.c` monitor) gates the STP/BTIF TX while it "sleeps". On this
port its sleep action, `mt_combo_plt_enter_deep_idle(COMBO_IF_BTIF)`, has **no
backend**: the Android combo stub logs `NULL function pointer` and returns
`-1` (`common_detect/mtk_wcn_stub_alps.c`), so the chip **never actually
enters deep idle**. The PSM therefore saves no power, but it still stalls the
link long enough to trip `MTKSTP_TX_TIMEOUT` (180 ms); the host then injects a
`0x7f` STP resync, the peer follows, and the BTIF link desyncs about **every
1.5 s**. A2DP then underruns (stutter) and, while the sink is left running
(silence), the glitches are heard as the after-playback crackle.

### Decisive measurement (idle, no stream at all, headset connected)

| PSM | `RESYNC2` | `stp_do_tx_timeout` | `deep idle fail` | per |
|---|---|---|---|---|
| **on** (default) | 12 | 41 | 198 | 20 s |
| **off** (`echo "0 0" > /proc/driver/wmt_dbg`) | **0** | **0** | **0** | 20 s |

PSM on resyncs even with **zero** audio traffic, at every idle time tried
(30 ms, 1 s, 5 s), and with every `btif_wak_hb_ms` (0, 5, 10, 20, 30, 50, 60,
100) — the WAK heartbeat and the idle time are not the lever. The PSM is.

### Effect on the A2DP transport

| PSM | ACL frames / ~6 s | frame size | throughput |
|---|---|---|---|
| on | 45 | fixed 572 B | ~57 kbps (≈1 frame / 80 ms) |
| **off** | 237 | 818–887 B | **~265 kbps** (correct SBC @44.1 k) |

`btmon`: with PSM off the controller reports `Number of Completed Packets`
~every 60 ms with several packets per event; with PSM on it was a metronomic
80 ms, Count 2–3, and the host's `mtk_wcn_stp_send_data()` blocked ~80 ms per
frame. The 80 ms bubble was the PSM, not the audio path.

## Fix (kernel delta)

`common_main/core/wmt_lib.c`:

- `gPsEnable` **defaults to 0**;
- `wmt_lib_ps_ctrl()` now **always disables** (never sets `gPsEnable=1`);
- `wmt_lib_ps_enable()` is documented as intentionally off (it calls
  `mtk_wcn_stp_psm_disable()` if somehow reached).

The force-off in `wmt_lib_ps_ctrl()` is the important one: both
`mt6630_sw_init()` and `mtk_wcn_wmt_func_off(BT)` (`wmt_exp.c`) ask to
re-enable the PSM, so a one-shot disable at init would not survive a BT
off/on cycle.

Runtime equivalent while testing (not persistent across BT power cycles):
`echo "0 0" > /proc/driver/wmt_dbg` (the `wmt_dbg` proc commands are hex
`<id> <arg>`; id `0x0` = `wmt_dbg_psm_ctrl`, arg `0` = disable).

### Not a regression / verified with the fix

**Verified on glass 2026-09-10** after building toplevel
`94bw47dh6bhr1rk4jafva9nimz3avwjb` and deploying it (gen7) + a WDT reboot
(module hot-reload does **not** work: after `rmmod`/`modprobe` the CONSYS
stayed `POWER_OFF` and wlan0/hci0 never returned — reboot required). With
the PSM now off **by default** (no `/proc` write):

- idle 12 s → `RESYNC2` 0, `stp_do_tx_timeout` 0, `deep idle fail` 0;
- 30 s playback → 0 / 0;
- 25 s idle after stop → **0 A2DP frames** (sink `state: "suspended"`);
- `wlan0` stayed associated on 5 GHz and pinged throughout;
- the MDR-ZX330BT reconnected and is the default sink.

- **Wi-Fi unaffected**: `wlan0` stays associated on 5 GHz (“The Lab”) and
  pings the gateway *and* `1.1.1.1` while the PSM is off, before and after a
  BT audio stream.
- The noisy `[STP] mtk_wcn_stp_send_data: ... to inform WMT to wakeup chip`
  messages (≈10/s, PSM-driven) drop to **0**.
- No power is lost: the PSM never deep-idles the chip (backend absent).
  Revisit only when a real MT6630 deep-idle implementation exists.

## Finding 2 — the after-playback crackle: gone once the PSM stopped feeding silence

Before the fix the A2DP sink could stay **running** while idle
(`node.pause-on-idle = "false"` and/or
`gnome-control-center --gapplication-service` (`org.gnome.VolumeControl`)
holding a recording stream on the default sink's monitor ports), so the
bluez sink kept sending silent SBC; every link glitch then garbled that
stream — the after-playback crackle. `pw-link -d`-ing
`…:monitor_FL/FR → GNOME Settings:input_*` made `pw-top` drop to all-`C`
immediately, confirming the sink could idle.

**After the PSM fix** the post-stop measurement shows the sink now reaches
`state: "suspended"` on its own and emits **0** A2DP frames over 25 s idle
(`node.pause-on-idle` is still `false`; WirePlumber's suspend wins because
nothing is driving the graph). So no further action was needed here. If the
crackle ever returns, the follow-up is a WirePlumber rule for
`~bluez_output.*` (`session.suspend-timeout-seconds`,
`node.suspend-on-idle = true`) and/or stopping a persistent
`org.gnome.VolumeControl` monitor stream.

## Evidence log (how it was pinned)

1. `dmesg` during playback: `[STP] stp_do_tx_timeout` (3 unacked seqs,
   `Resend STP packet`), each preceded by
   `stp_parser_data_in_full_mode: MTKSTP_SYNC: go to MTKSTP_RESYNC2, buff = 7f`
   and `mtkstp_process_packet: expected_rxseq = X, parser.seq = Y`.
2. HCI-STP TX timestamps: the stall is exactly **1.052 s**.
3. `btmon`: `Number of Completed Packets` every **80 ms**, Count 2–3 →
   ~57 kbps, i.e. ~2× A2DP underrun.
4. Resyncs present with **no stream** (connected, idle) ⇒ not load-driven.
5. Resync period ~1.5 s, invariant under `btif_wak_hb_ms` and the PSM idle
   time ⇒ not the WAK heartbeat.
6. `/proc/driver/wmt_dbg` PSM off ⇒ resyncs/timeouts to **0** and throughput
   to full A2DP rate. ⇒ **PSM is the cause.**
7. PSM's deep-idle backend confirmed absent: `NULL function pointer` /
   `deep idle fail(-1)` ⇒ disabling it costs nothing.

## Device state left (2026-09-10)

Audio verified clean (0 resyncs/timeouts) over repeated 6–30 s streams, and
the sink idles to `suspended` with 0 frames after stop, on the deployed
toplevel. The MDR-ZX330BT is connected as the default sink. `wlan0` is
associated on 5 GHz. No boot.img/para change (only the rootfs closure moved).
