# gemini-nixos — NixOS on the Planet Computers Gemini PDA

This repository builds and runs **NixOS on the Planet Computers Gemini
PDA** (MediaTek MT6797X "aeon"). It is a Mobile NixOS port with an
in-repo, self-built mainline **Linux v6.6** kernel and a
GPU-accelerated **vanilla GNOME** desktop. It is also the primary
("golden") home for the whole Gemini PDA project — both the port and the
hardware/boot knowledge base that previously lived in the sibling
`GeminiPDA` repo. See `AGENTS.md` for the migration plan and the working
rules.

The device boots a stock MediaTek **LK** bootloader from a 16 MiB
Android `boot` partition, and there is **no fastboot**. Since the
**2026-09-10 repartition**, TWRP + NixOS are the only systems on the
unit: TWRP is p1 `recovery`, the single NixOS boot image is p22 `boot`
(16 MiB), and the rootfs is one **58.0 GiB p27 `linux`** partition
(ext4 label `NIXOS_SYSTEM`). Android and the old Debian/`userdata`
layout were reclaimed into it — see
[docs/repartition-android-space.md](docs/repartition-android-space.md)
§12.

## What works today

**On glass (verified):**

- **NixOS boots** — the port milestone (2026-09-07), now on the single
  58 GiB `linux` rootfs with `growfs` and the `nixos-rebuild`
  round-trip both closed. Updates are generation switches, not
  reflashes: from the host with `bin/deploy.sh`, or entirely on the
  device with `nixos-rebuild switch --flake .` from a repo clone at
  `/root/gemini-nixos`. (SSH over the **USB gadget NIC** — CDC-ECM
  since 2026-09-17, so Linux *and* macOS hosts can use it — or the device
  on the LAN; both work.)
- **Vanilla GNOME is the default desktop** (since 2026-09-10) and is
  **GPU-accelerated**: the LK framebuffer is exposed as a normal KMS
  device by an in-kernel `geminipda-drm` driver, and Mesa's `kmsro`
  pairs it with **panfrost** on the Mali-T880. GDM auto-logs in, the
  GNOME app suite is installed, and a clean boot comes up with zero
  failed units.
- **Multiple desktop sessions** — vanilla **GNOME** (GDM, the default)
  and the custom Rust **gemshell** compositor (its own system service,
  with an in-process egui settings panel since 2026-09-12) are
  co-installed, plus a console-only mode;
  `gemcli session set gnome|gemshell|console`. COSMIC, niri and the
  older nested **Phosh**/**LXQt** desktops were removed 2026-09-12.
- **Wi-Fi** — internal MT6630 CONSYS (`wlan0`, with the factory NVRAM
  MAC preserved) plus USB dongles (RTL8821CU), managed by
  **NetworkManager**, with a smartphone-like autoconnect loop that
  prefers the strongest known network and keeps scanning when none is in
  range.
- **Bluetooth** — MT6630 CONSYS `hci_stp` (persistent bluetoothd, CLI
  and blueman GUI) and **A2DP audio**, with the stutter and
  after-playback crackle root-caused and fixed.
- **Audio** — the PipeWire system session on the S16-only analog path,
  with the internal L↔R speaker swap corrected by a virtual sink and the
  speaker amp following the chosen output. Fn media and volume keys work.
- **Power** — battery guard, silver-button light clamshell sleep/wake
  (`gemcli sleep` / `gemini-sleepd`; systemd `suspend` is deliberately
  disabled because it locks the unit up), GNOME "Power Mode" driving the
  A72 cluster, and working `reboot`/`poweroff` (custom reset/poweroff
  kernel drivers).
- **Touch** — a real 10-point multitouch device, no emulated cursor,
  correct 180° rotation.
- **Keyboard** — the Gemini UK layout registered with GNOME, including
  the Fn layer and Fn media keys.
- **Windows / DOS guests** — wine-wow64 for 32-bit Windows PEs and
  DOSBox-X with a Gemini-keyboard Fn overlay.
- **Graphics stack and demos** — one patched Mesa 26.2.2 (panfrost + the
  T880 polygon-list delta) shared by the whole system; `gemdemo` (a
  minimal GLES 3.1 + ALSA template) and **GEMINI: EXODUS** (a cinematic
  spacesynth and GPU stress test).
- **Recovery** — a full disaster-recovery playbook plus mtkclient
  preloader tooling and TWRP-flashable image backups:
  [docs/disaster-recovery/](docs/disaster-recovery/).

**Status shorthand:** ✅ done/verified · 🟡 built but not yet on hardware
· ⬜ planned · 🔴 blocked. The phase table lives in
[docs/mobile-nixos-port-feasibility.md](docs/mobile-nixos-port-feasibility.md)
§9; the dated record of what was tried is
[docs/session-log.md](docs/session-log.md).

## Boot chain

```
LK ──► boot.img ("ANDROID!" v0, 2 KiB pages)
       ├─ kernel: Image.gz + appended DTB → 0x40200000 (LK gunzips, finds FDT)
       └─ ramdisk: gzip cpio (busybox + /init) → 0x45000000 (copied verbatim;
          the kernel decompresses it, CONFIG_RD_GZIP=y; passed via ATAG_INITRD2)
            └─ /init: scans /dev/mmcblk* for the Mobile NixOS rootfs
               (ext4 with /nix/store + nix-path-registration or a system
               profile — the image is store-only, no /init at its root),
               mounts it rw, creates /dev/disk/by-label/NIXOS_SYSTEM (no udev
               in the initrd; fstab references / by label), then switch_roots
               to <generation>/init found via nix-path-registration (first
               boot) or /nix/var/nix/profiles/system (later boots)
                 └─ NixOS stage-2 (store rehydrated on first boot via
                    postBootCommands; / grows to fill the partition via
                    x-systemd.growfs)
```

The Mobile NixOS stage-1 is **not** in the boot image (it cannot fit —
see R1 under "Known constraints"); it is disabled for this device. There
is no stage-1 USB gadget, boot GUI or boot SSH — serial `ttyS0,921600`
and fbcon are the bring-up interfaces. The rootfs image contains only
the Nix store + `nix-path-registration`; the stage-2 init is the
generation's store-path init, which is why the initrd resolves the
generation explicitly.

> ⚠️ **The boot.img cmdline must keep `bootopt=64S3,32N2,64N2`.** LK
> parses it via `platform_parse_bootopt`; without it the boot hangs on
> the LK logo before any kernel output. Discovered 2026-09-07 — see
> [docs/phase-2-on-glass.md](docs/phase-2-on-glass.md) §2a.

## Layout

| Path | What |
|---|---|
| `flake.nix` | Flake entry point; imports Mobile NixOS eval directly (Mobile NixOS is not a flake). Exposes `nixosConfigurations.gemini` + the aarch64 packages |
| `devices/planet-geminipda/default.nix` | Out-of-tree device definition: SoC, boot.img geometry, DTB append, kernel, minimal-initrd wiring |
| `devices/planet-geminipda/initrd.nix` | Minimal partition-scanning busybox initrd (R1; replaces Mobile NixOS stage-1 in the boot image) |
| `devices/planet-geminipda/kernel/` | Kernel derivation (published v6.6 base + tracked delta + lean config); `postInstall` builds the out-of-tree `sramldo-smc.ko` (A72 bring-up SMC). The delta adds **`geminipda-drm`**, the KMS driver for the LK framebuffer |
| `modules/hardware-soc-mediatek-mt6797.nix` | Out-of-tree MT6797 SoC fragment (upstreaming = phase 6) |
| `keys/` | The device SSH identity, committed on purpose: `gemini_ed25519` is both the admin login key and the **pinned sshd host key** (so a reflash never changes the device's identity). Rationale + the accepted trade: `keys/README.md` |
| `services/` | Device services and desktop sessions (see the table below) |
| `config/gemini.nix` | Stage-2 system configuration (headless base + USB-NIC (CDC-ECM) SSH, device services, Mesa, GNOME default, on-device Nix) |
| `bin/lib/host.sh` | Host-side link helpers shared by the device scripts: platform detection, MAC-based USB-NIC discovery, net-up, ping, USB-id probes, devshell re-exec (Linux + macOS) |
| `pkgs/` | In-repo package forks/pins: `mesa-geminipda`, `wlroots-geminipda`, `gemwl` (legacy), `gemcli`, `gemdemo`, `gemini-exodus`, `gemini-firmware`, `gemini-xkeyboard-config`, … |
| `patches/` | The tracked Mesa panfrost (T880 polygon-list) patch |
| `bin/` | Host/device tooling: build/deploy, flash/repartition, recovery, repo sync — see `AGENTS.md` |
| `docs/` | Design notes, bring-up receipts and the dated session log (see "Documentation") |
| `kernel/base` | git submodule pointer to upstream torvalds/linux @ v6.6 (`ffc253263a…`) — fetch on demand for reading base sources |
| `kernel/borrowed/` | Pre-2026-09-08 borrowed-kernel scaffolding, kept as reference/rollback only; no longer wired into any build |
| `stock-dump/` | Device partition backups (gitignored; nvram/IMEI private) |

## Device services

A condensed view of what runs in the rootfs. The full per-unit detail
lives in the module headers under `services/` and the linked docs, and
the operational cheat sheet is in `AGENTS.md`.

| Unit / CLI | What it does |
|---|---|
| `gemini-gpu-poweron.service` | Powers the Mali-T880 (MFG MTCMOS domains via SPM devmem + VGPU rails via i2c RT5735) before anything uses the GPU |
| `gemini-a72-up.service` / `cl2-down.sh` | Brings cpu8/cpu9 (A72) online (DA9214 BUCKB, SPM pre-sequence, SRAM-LDO SMC, PSCI CPU_ON, WDT-guarded) and powers them back down to the cold-boot state |
| `gemini-battery-guard.service` | Battery safety daemon: low/critical VBAT alerts + orderly poweroff, USB-present-but-not-charging detection, history CSV |
| `gemini-backlight-default.service` | Boot-time backlight = 10 % (power-saving default) |
| `backlight`, `power`, `battstat`, `bq25896-raw.sh` | Backlight/charge control and battery/charger status CLIs (raw ADC bypasses stale driver latches) |
| `gemini-wdt-reboot` / `gemini-boot-recovery` | Device-side WDT EXRST self-reboot and boot-into-TWRP CLIs + units (fallbacks now that plain `reboot`/`poweroff` work) |
| `pipewire` / `wireplumber` / `pipewire-pulse` | The PipeWire media stack as one root system session; `pcm.gemini16` pins the analog path to S16_LE at the alsa-lib boundary |
| `gemini-audio-defaults`, `speaker`, `audio-output`, `gemini-speakerd` | Playback route at boot; speaker-vs-headphone output; the speaker amp follows the PipeWire default sink; the L↔R-correcting virtual sink |
| `gemini-power-profile.service` | GNOME "Power Mode" → A72 cluster (performance ⇒ A72 up) |
| `gemini-wifi-nvram`, `gemini-wifi-internal`, `gemini-wifi-auto`, `gemini-wifi-smart`, `wifi`/`wifi-internal`/`wifi-smart` | Factory NVRAM record (stable MAC), internal CONSYS stack with the correct load order, saved-profile auto-connect, smartphone-like strongest-known-network autoconnect (never gives up), and the USB/internal/smart CLIs |
| `gemini-sleepd.service` | Silver side-button light clamshell sleep/wake (`gemcli sleep key`); the only sleep path |
| `gemcli` | Rust device-control CLI (backlight/battery/guard/a72/wdt/boot/gpu/speaker/profile/status + a read-only `selfcheck` parity harness) |
| `gemini-utils.nix` / `services/scripts/` | The verified bring-up CLIs, ported verbatim |

## Documentation

| Doc | Contents |
|---|---|
| [docs/session-log.md](docs/session-log.md) | Dated record of what was actually tried — the project's ground truth |
| [docs/mobile-nixos-port-feasibility.md](docs/mobile-nixos-port-feasibility.md) | Feasibility study and the phased plan (phase table = roadmap) |
| [docs/boot-process.md](docs/boot-process.md) | Plain-language boot explainer with receipt pointers |
| [docs/phase-2-on-glass.md](docs/phase-2-on-glass.md) | The 2026-09-07 first-boot milestone, the `bootopt` discovery, and the recovery receipts |
| [docs/repartition-android-space.md](docs/repartition-android-space.md) | Repartition history, including the final 2026-09-10 TWRP + NixOS-only layout (§12) |
| [docs/disaster-recovery/](docs/disaster-recovery/) | Full-flash-erase restore playbook, image ledger + sha256, gather checklist, drills |
| [docs/gnome-feasibility.md](docs/gnome-feasibility.md) | Why vanilla GNOME needed a KMS driver, and the GPU-acceleration receipts |
| [docs/desktop-plumbing.md](docs/desktop-plumbing.md) | DE-agnostic plumbing: battery/UPower, backlight, audio/speakers, touch |
| [docs/desktop-selection.md](docs/desktop-selection.md) | GNOME/gemshell/console session selector |
| [docs/bluetooth-bringup.md](docs/bluetooth-bringup.md), [docs/bluetooth-a2dp.md](docs/bluetooth-a2dp.md) | MT6630 CONSYS Bluetooth port + the A2DP stutter/crackle fix |
| [docs/power-sleep.md](docs/power-sleep.md), [docs/power-modes.md](docs/power-modes.md), [docs/power-states.md](docs/power-states.md) | Sleep/wake, A72 power modes, reboot/poweroff |
| [docs/gemcli.md](docs/gemcli.md) | Rust device-control CLI + migration/parity plan |
| [docs/gemdemo.md](docs/gemdemo.md), [docs/gemini-exodus.md](docs/gemini-exodus.md) | GLES templates and the EXODUS GPU stress test |
| [docs/wine-d3d.md](docs/wine-d3d.md) | Windows (wine-wow64/box64) on the PDA |
| [docs/library-deltas.md](docs/library-deltas.md) | The "published base + in-repo delta" pattern |
| [docs/building.md](docs/building.md) | Which build system, when — the platform-dispatched build map |
| [docs/usb-network.md](docs/usb-network.md) | The USB gadget NIC: RNDIS → CDC-ECM, and the now-possible Mac reboot/flash cycle |
| [docs/macos-build.md](docs/macos-build.md) | Building the images from a Mac (container VM): receipts, gotchas, limits |
| [docs/handover-*.md](docs/) | Point-in-time handover notes (historical) |

## Versions

- **Kernel**: built in-repo from upstream Linux **v6.6** (kernel.org
  tarball, fetch-pinned) + the tracked delta in
  `devices/planet-geminipda/kernel/delta/` + the lean device config.
- **Mesa**: 26.2.2 (nixpkgs) + the tracked panfrost T880 polygon-list
  patch, wired as `hardware.graphics.package`.
- **wlroots** 0.18.2, **labwc** 0.8.3, **phoc** 0.54.0 (fork-gbm
  wlroots 0.19.3) — pinned from source where the nixpkgs versions differ.
- **Mobile NixOS**: commit `2c132754` (`development`).
- **nixpkgs**: the nixos-unstable **channel** rev `dc5d91f84032`
  (`26.11pre1068949`), pinned in `flake.nix` so hydra's full aarch64
  closure substitutes from cache.nixos.org.

Nothing goes to the device without a version line recorded in
[docs/session-log.md](docs/session-log.md) — the kernel, boot.img hashes
and graphics pins after every flash.

## Building and flashing

Builds are **native aarch64** (the x86_64 cross toplevel is abandoned) and
go through **one platform-dispatched entry point** — `bash bin/build.sh`
(rule 10); the map is [docs/building.md](docs/building.md):

| Machine | Implementation | Model |
|---|---|---|
| Linux (the workstation) | `bin/build-linux.sh` | native aarch64 drvs on the local store, Pi remote builder compiling, cache.nixos.org substituting — unchanged |
| macOS | `bin/macos/build.sh` | the same drvs built inside an Apple `container` aarch64 NixOS VM (nix-on-darwin can substitute those drvs, never build them) — [docs/macos-build.md](docs/macos-build.md) |

```sh
bash bin/build.sh shell             # that platform's devshell (nix develop)
bash bin/build.sh start [TARGET]    # detached build, default TARGET = bootimg
bash bin/build.sh wait  [TARGET]    # rc 0 ok / 1 failed / 2 still running
bash bin/build.sh status            # jobs/container, egress, artifacts, hashes
```

The Linux workstation keeps its deployment loop on top of the same build:

```sh
bash bin/deploy.sh build            # native aarch64 toplevel (Pi builder; gc-pinned)
bash bin/deploy.sh deploy           # copy the delta + switch the device generation
```

Flash images: `TARGET=default` gives `boot.img` + the rootfs image.
Flashing is manual (no fastboot) and is orchestrated by
`bin/flash-nixos.sh`, which converges the device to TWRP from any state
and then streams the rootfs to p27. A repartition is a one-way operation
via `bin/repartition-nixos.sh`. **Both building and flashing are
platform-dispatched now:** since the USB NIC is CDC-ECM (2026-09-17) a
Mac can drive the whole reboot/flash cycle over the cable
([docs/usb-network.md](docs/usb-network.md)) — the one thing that still
needs a Linux host is preloader/BROM recovery (`bin/run-mtk.sh`; Apple's
container has no USB passthrough — [docs/macos-build.md](docs/macos-build.md)).
Before touching the device, `bash bin/flash-nixos.sh preflight` answers
read-only whether a flash can run from this host (artifacts + sha256
ledger, the `boot.img` cmdline requirements, toolchain, ssh key, sudo,
device state).

Measured boot image (**Mac build 2026-09-17**, the self-built lean
kernel): 9,988,096 B = **9.5 MiB** — kernel payload 7.66 MiB + appended
DTB, minimal initrd 1.86 MiB — `bootopt=64S3,32N2,64N2` and
`fbcon=rotate:3` present, both verified. The same refactor rebuilt the
identical sha256 on both sides (`b2404b13…`). The RNDIS → CDC-ECM USB
NIC change rebuilt it once more (2026-09-17 (c): 9,984,000 B, kernel
payload 4,610 B smaller, sha256 `2e8f43ad…`) —
[docs/usb-network.md](docs/usb-network.md).

The operational details — build commands, the kernel base+delta model,
the step-by-step flash procedure and its safety model, and the on-device
`nixos-rebuild` loop — are in **`AGENTS.md`**.

Measured boot image (build 2026-09-05, the borrowed `#329` kernel):
15,431,680 B = **14.7 MiB** in
the 16 MiB partition (kernel payload 13.45 MiB + DTB, initrd 1.26 MiB
gzip, ~1.3 MiB headroom). The 2026-09-17 in-repo lean-kernel build above
is 9.5 MiB — see `docs/macos-build.md` for the field-by-field receipt.

## Known constraints

- **R1 — boot partition budget (resolved).** The 16 MiB `boot` budget
  cannot hold the Mobile NixOS stage-1 initrd, so the boot image carries
  a minimal partition-scanning busybox initrd instead: 14.7 MiB total,
  ~1.3 MiB headroom. Trade-off: serial-console-only early boot.
- **R2 — one Mesa (resolved 2026-09-10).** The Mesa delta is now a thin
  override of the pinned nixpkgs Mesa 26.2.2, shared as the single glvnd
  ICD across the device, rather than a separate 25.0.7 fork.
- **R3 — display path (superseded 2026-09-10).** Originally there was no
  DRM and the desktop ran through the `gemwl` wlroots compositor. The
  kernel now provides the `geminipda-drm` KMS device, GNOME runs on it
  GPU-accelerated, and `gemwl` + the nested desktops remain the
  buildable rollback.

Full receipts: [docs/mobile-nixos-port-feasibility.md](docs/mobile-nixos-port-feasibility.md) §7.
