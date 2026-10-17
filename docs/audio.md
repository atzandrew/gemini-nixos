# Audio: speaker amps + headphone jack under DAPM (2026-10-16)

Status: done and on glass (boot image `boot-hpidle-20261016.img`, gemini-nixos
`48003ea`; gemini-debian `df4b916` + `210bc55`). Speakers and headphones work,
no pop, jack silent at idle, ~74 mA less than the old userspace scheme.

## Hardware (receipts)

- Card `mt6797-mt6351`: MT6797 AFE (`sound/soc/mediatek/mt6797/`) + MT6351
  PMIC codec (`sound/soc/codecs/mt6351.c`, under `&pwrap`).
- The built-in speakers hang off the codec **HPL/HPR** (headphone) drivers
  through two external amps, enable = **SoC GPIO 243 + 244**. Speakers and
  jack are electrically in parallel; there is no jack detection.
- Vendor 3.18 kernel (gemian/gemini-linux-kernel-3.18,
  `sound/soc/mediatek/mt_soc_audio_6797/AudDrv_Gpio.c`,
  `mt_soc_codec_63xx.c`) + the vendor DTB from the TWRP recovery partition
  (p1, appended DTB):
  - GPIO 243/244 sit in MediaTek's `hpdepop-pullhigh/low(_e2)` pinctrl
    slots (`pins = <0xf300>` / `<0xf400>`), but Planet reused them as the
    speaker amps: `AudDrv_GPIO_EXTAMP_Select` / `EXTAMP2_Select` drive
    those states. The real HP-depop function (`AudDrv_GPIO_HPDEPOP_Select`)
    is `#if 0` — **no idle-ground hardware on this board**.
  - Amp enable = low, 1 ms, then **3 low/high pulses of ~2 µs** (AW87xx
    one-wire mode select, mode 3), then **25 ms** warm-up. Off = low, 0.5 ms.
  - `headphone_en` = GPIO 234 (switched only with the mic ADC);
    `headphone_cs` = GPIO 235 (a `/proc/headphone_cs` toggle, purpose
    unknown).

## Kernel (delta tree)

`sound/soc/mediatek/mt6797/mt6797-mt6351.c` (machine driver, now in delta):

- DT `speaker-amp-gpios = <&pio 243 ..>, <&pio 244 ..>` on the `sound` node.
- `SND_SOC_DAPM_SPK("Speaker")` fed from codec `Headphone L/R`; its event
  runs the vendor pulse sequence (irq-off for the µs pulses; pio GPIOs are
  MMIO). DAPM: amps on after the codec path is up, off before it goes down.
- `SND_SOC_DAPM_HP("Headphones")`, always connected (`Headphones Switch`).
- `Speaker Switch` = custom control: **on** = Speaker pin on + `HP Idle`
  off; **off** = Speaker off + `HP Idle` on, in one DAPM sync (the shared
  HP drivers never drop out, the amps can't be powered by the idle path).

`sound/soc/codecs/mt6351.c`:

- `SND_SOC_DAPM_INPUT("HP Idle")` → DACL/DACR, supplied by
  `DL Digital Clock`. When enabled, DAPM keeps the DAC, HP drivers and the
  codec's digital DL clock gates on with **no stream** (SoC AFE/DMA off).

### Why HP Idle (the idle buzz)

With the stream closed the jack lines float and pick up board noise (an
audible buzz in headphones, the "crackling" users report). Register A/B on
the device (debug module `kernel/modules/mt6351-audpoke`, writes limited to
the audio blocks):

| Idle state | Result |
|---|---|
| everything off (DAPM idle) | buzz |
| + analog HP chain (clocks, LDOs, NCP, HP drivers) | buzz |
| + DAC bits (`AUDDEC_ANA_CON0` = 0xf46f) | buzz |
| + digital DL (`AUDIO_TOP_CON0` 0x2014 c0e5→c020, AFE_ON, DL_SRC) | **silent** |
| − DL_SRC, − AFE_ON (only the 0x2014 clock gates) | silent |
| re-gate 0x2014 | click + buzz again |

So the codec's digital clock gates are what keep the outputs quiet.

## Userspace (gemini-debian)

- PipeWire's ACP sees `Speaker`/`Headphones` and offers **Speakers /
  Headphones** ports on the built-in sink; the Headphones port turns
  `Speaker Switch` off. The sound settings choice *is* the switch
  (WirePlumber saves it per user).
- `gemini-audio` → `/usr/local/bin`, runs as the user (no sudo):
  `speaker|headphone|toggle|status` = `pactl set-sink-port`; `route` (boot
  unit) sets the AFE/codec route controls.
- Removed: `gemini-audio-session` user unit, `gemini-sleep-gpio` (+ unit),
  the never-suspend WirePlumber rule `51-gemini-no-suspend.conf` (it kept
  the AFE + amps on forever), the sudoers drop-in. `bin/apply-files.sh`
  gained `files.remove` + `units.user.disable` so update bundles clean
  these off old installs.

## Power (gauge A/B, battery, screen on, 90 s phases)

| State | Current |
|---|---|
| Speakers, idle (new) | 818 / 804 mA |
| old behaviour (Speakers + open stream = amps + codec + AFE) | 885 mA |
| Headphones, idle (HP Idle hold) | 812 mA |

≈74 mA saved; the HP Idle hold is within noise (±7 mA).

## Sleep

- Widgets are not `ignore_suspend`, so on system suspend DAPM powers the
  whole path (amps, codec, HP Idle) off and restores it on resume. The AFE
  is runtime-PM and idle without a stream.
- To verify once suspend works: after resume the port/`Speaker Switch` is
  unchanged, HP Idle is back in Headphones mode, playback OK.
- Watch: Headphones mode keeps the codec's audio clock request
  (`CLKSQ Audio`) on while awake — may matter for deep runtime idle with
  the screen off. Speakers mode is unaffected.

## Gotchas

- Over SSH without a session lingering, each login starts the user's
  PipeWire fresh: wait for the `alsa_output.platform-sound.*` sink before
  running `gemini-audio`, or it reports "built-in sink not found".
- If a PipeWire started under the old never-suspend rule (e.g. the SDDM
  greeter's) is still running, it holds the PCM: the user session gets
  "Dummy Output" and the amps stay on. Restart it or reboot.
