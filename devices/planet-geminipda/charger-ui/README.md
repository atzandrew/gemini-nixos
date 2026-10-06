# charger-ui — the graphical off-mode-charging screen

`charger.sh` (in the initrd) calls `/bin/charger-ui` whenever something visible
changes. If the program is missing or fails, `charger.sh` falls back to the text
screen automatically. Background: project doc `claude/charger-mode.md`.

## Files

| File | What | In the initrd as |
|---|---|---|
| `charger-ui.c` | the renderer (layout constants in `struct layout` at the top) | `/bin/charger-ui` (static, musl) |
| `bg.png` | the background: everything that never changes | `/share/charger/bg.png` |
| `Nunito-Regular.ttf` | Nunito, static Regular (wght 400) instance of the variable font | `/share/charger/Nunito-Regular.ttf` |
| `Nunito-OFL.txt` | the font's licence (SIL OFL 1.1) | — |
| `stb_image.h`, `stb_truetype.h` | github.com/nothings/stb @ 2c980bb5 (public domain) | compiled in |

## The background (`bg.png`)

- **2160 × 1080 PNG, landscape** as you hold the Gemini. Any colour depth; 256
  colours is fine. Don't go much lower: the anti-aliased edges get jagged.
- Black background. Contains the title, the Gemini drawing, the hint text.
- **Leave these areas black** (the program draws them):
  - the battery circle: centre (546, 567), outer radius 264 px, plus a few px
    of margin. The program draws the white 3 px outline, the fill and the %.
  - the status line: centred on x 546, baseline y 908, roughly x 130–960,
    y 870–925.
- Replacing the art = replace `bg.png`, rebuild the initrd/boot image. No
  recompile. Moving the circle or text = edit `struct layout` in charger-ui.c.

The current `bg.png` is the user's final background (2026-10-05): title +
"Device connected to power source", the Gemini drawing, "Hold power button
to boot", the unplug arrow and "Unplug charger to power off".

## What the program draws

- Circle: white outline, filled from the bottom to the charge level with grey
  `#343434` (`#5a2222` below 15 %), anti-aliased.
- `NN%` in Nunito 130 px (em), white, centred in the circle (`--%` = unknown).
- Status line in Nunito 33 px: `Charging at 1.4 A` / `Charging at 650 mA`,
  `Fully charged`, `Charged`, `Charging paused`, `Charger too weak`,
  `Battery very low - hold power again to boot anyway`, `Starting...`,
  `Charger removed - switching off`.

Tearing: the framebuffer has no vblank. Each run composes the frame in RAM and
copies only the circle and status-line rectangles (the whole screen only on the
first frame), in panel row order, so a tear is at most a brief seam inside one
of them. Screen off/on is the backlight, which never tears.

## Preview on any Linux PC (no Gemini needed)

```
cc -O2 -o charger-ui charger-ui.c -lm
./charger-ui --out frame.ppm --bg bg.png --font Nunito-Regular.ttf --pct 80 --line "Charging at 1.4 A"
```
`frame.ppm` opens in most image viewers (or `convert frame.ppm frame.png`).

## Regenerating the font instance

```
pip install fonttools
python3 -c "from fontTools.ttLib import TTFont; from fontTools.varLib import instancer; \
  instancer.instantiateVariableFont(TTFont('Nunito[wght].ttf'), {'wght': 400}).save('Nunito-Regular.ttf')"
```
(`Nunito[wght].ttf` from github.com/googlefonts/nunito, `fonts/variable/`.)
