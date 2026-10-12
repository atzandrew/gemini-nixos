---
name: gemini-nixos architecture
alwaysApply: true
---
# Project architecture: gemini-nixos

Kernel, boot image and device support for the Planet Gemini PDA (MediaTek MT6797X: CPU0-3 + CPU4-7 Cortex-A53, CPU8-9 Cortex-A72; Mali-T880 via panfrost; Solomon SSD2092 DSI video-mode panel, 1088x2176; US keyboard). Built with Nix flakes on Hydra (Fedora Asahi, aarch64).

Current role: the device runs **Debian 13** (sibling repo `../gemini-debian`). This repo still produces the **kernel 6.6.157, boot.img + initrd (with the charging screen), out-of-tree modules, firmware, and the device scripts/keymaps** that gemini-debian copies in. The NixOS userland parts (`services/*.nix`, gnome, gemshell, wine, dosbox) are legacy; don't extend them unless asked.

## Where things live
- `flake.nix` — all build outputs (`packages.aarch64-linux.bootimg`, `.kernel`, `.initrd`, module packages). Flakes only see git-tracked files.
- `devices/planet-geminipda/` — the device:
  - `default.nix`, `initrd.nix`, `charger.sh` (off-mode charging decision in the initrd), `charger-ui/charger-ui.c` (charging screen).
  - `kernel/default.nix` — kernel build; `kernel/config` — the kernel config (`CONFIG_CMDLINE` with `CONFIG_CMDLINE_FORCE=y` is the real command line).
  - `kernel/delta/` — every file changed vs. upstream 6.6.157, same paths as the kernel tree.
  - `kernel/modules/<name>/` — out-of-tree modules (`mt6351-gauge` is the fuel gauge; the others are test/probe modules).
- Key kernel files under `devices/planet-geminipda/kernel/delta/`:
  - DT: `arch/arm64/boot/dts/mediatek/mt6797-gemini-pda.dts`, `mt6797.dtsi`
  - CPU clocks: `drivers/clk/mediatek/clk-mt6797-mcu.c`, `clk-mt6797.c`; cpufreq: `drivers/cpufreq/mediatek-cpufreq.c`
  - CPU rails: `drivers/regulator/mt6797-cpuldo-regulator.c`, `fan53555.c`; A72 power-on: `drivers/soc/mediatek/mt6797-cl2-power.c`
  - Thermal: `drivers/thermal/mediatek/auxadc_thermal.c`, `drivers/nvmem/mtk-lk-devinfo.c`
  - Display: `drivers/gpu/drm/tiny/geminipda-drm.c` (the DRM driver in use), `drivers/video/fbdev/geminipda-fb.c`, `drivers/gpu/drm/panel/panel-solomon-ssd2092.c`
  - eMMC: `drivers/mmc/host/mtk-sd.c`; charger: `drivers/power/supply/bq25890_charger.c`; RTC: `drivers/rtc/rtc-mt6351.c`; watchdog: `drivers/watchdog/mtk_wdt.c`
- `*.patch` in the repo root — kernel changes as delivered (already applied to `kernel/delta/` once committed). Name = feature + step (e.g. `a72dvfs1b-rate.patch`).
- `services/scripts/` — device scripts copied into the Debian image (`battery-guard.sh`, `backlight`, `wifi*`, `audio-output`, ...). Never run `cl2-up.sh`/`cl2-down.sh`.
- `config/` — `gemini.nix` (NixOS config), `keymaps/`, `xkb/symbols/gemini`.
- `pkgs/` — Nix packages (firmware, Mesa/wlroots for the Gemini, misc tools); `patches/` — userland patches (Mesa panfrost, wlroots).
- `bin/` — host and device helpers: `gemini-env.sh` (IPs + `to_dragon`/`flash_boot` helpers), `gemini-thermal-check.sh`, `plasma-trace.sh`, `cpu-clocks.sh`, `gemini-disp-probe.py`.
- `docs/` — one doc per subsystem: `cpu-dvfs.md`, `thermal.md`, `display.md`, `emmc-hs200.md`, `boot-process.md`, `building.md`, `power-*.md`, `usb-network.md`.

## Never read these whole (too big for the context window)
`AGENTS.md`, `docs/session-log.md`, `files.txt`, `out.txt`, `devices/planet-geminipda/kernel/config*`, `config/keymaps/gemini-uk.map`, `charger-ui/stb_*.h`, `delta/net/bluetooth/hci_sync.c`, `delta/drivers/mmc/host/mtk-sd.c`, `mt6797-gemini-pda.dts`, `docs/cpu-dvfs.md`. Use grep_search and read only the matching part. Ignore `logs/`, `spm-fw/`, `result*`, `outputs/`, `e2fsprogs-1.47.4/`.

## Conventions
- Kernel work follows mainline conventions (DT bindings, regulator/clk/cpufreq frameworks), no board-local hacks.
- A config change that alters structures modules use needs boot + userdata flashed together; DTS/cmdline-only changes are boot-only.
- Builds run as `atzero`, never `sudo nix`. The user runs all builds, git and flashing.
