# Porting the Gemini PDA bring-up work to Mobile NixOS — feasibility study

**Author:** cjdell (investigation carried out 2026-09-05)
**Scope:** Assess the feasibility, gaps, risks and effort of porting the
working mainline-Linux bring-up of the Planet Computers Gemini PDA
(`/home/cjdell/Projects/GeminiPDA`, "GeminiPDA") onto NixOS, using
Mobile NixOS (`/home/cjdell/Projects/gemini-nixos/repos/mobile-nixos`,
commit `2c132754`, development branch) as the base.

> **GOLDEN-REPO note [2026-09-07]:** this doc is historical (written
> when gemini-nixos was only the port repo). gemini-nixos is now the
> GOLDEN repo for the whole project (AGENTS.md M1–M7); GeminiPDA is
> legacy and being folded in. The feasibility analysis and phase table
> remain valid; only the "sibling is the knowledge authority" framing
> is obsolete.

---

## 1. Executive summary

**Verdict: feasible — no fundamental blocker, but with one headline
engineering constraint (boot-image size budget) and one reproducibility
concern (pin the exact GPU/desktop software combo).**

The Gemini PDA boots a stock MediaTek LK bootloader from a standard
**Android `boot.img` v0** in a 16 MiB partition, with a **gzip'd
`Image.gz` + appended DTB** kernel payload and a CPIO initramfs. Mobile
NixOS's `android` system type was built for exactly this shape: it
assembles `boot.img` with `mkbootimg` at configurable base/offsets,
supports appended-DTB kernels (`isImageGzDtb` / `appendDTB`), builds
arbitrary kernels from source (including a 6.6 fork like the
`geminipda-bringup` branch), and can emit **TWRP-flashable zips** — the
Gemini has no fastboot, but the project already flashes via a patched
TWRP, so its existing flash tooling applies unchanged.

The work to port is almost entirely **declarative re-packaging** of
already-solved bring-up knowledge (kernel branch is committed, custom
userspace is small C/shell + stock Plasma/LXQt), plus a handful of
small Mobile NixOS extensions (an MT6797 SoC fragment, out-of-tree
device definition, `flashingMethod` is cosmetic).

What does *not* port trivially:

1. **16 MiB `boot` partition budget.** Current boot.img = kernel
   payload 13.34 MiB + initramfs 1.26 MiB + headers ≈ **14.66 MiB**,
   leaving ~**2 MiB** for Mobile NixOS's stage-1 initrd, which is far
   larger than the current hand-rolled 1.3 MiB initramfs. This needs a
   deliberate size plan (see §7, risk R1).
2. **No DRM device** on the working graphics path (LK-framebuffer
   based: `geminipda-fb` + dma-buf + custom `gemwl` wlroots compositor
   + nested KWin/LXQt + patched Mesa). None of the standard
   mobile/Phosh assumptions apply; logind reports `CanGraphical=no`
   (already fought in the current work). All of this must be packaged
   as Nix derivations/services rather than Debian files dropped on the
   rootfs.
3. **Version pinning.** The on-glass-verified stack is very specific
   (kernel 6.6 `geminipda-bringup`, Mesa 25.0.7 fork, wlroots 0.18.2,
   kwin 6.3.6 / labwc 0.8.3, Plasma 6). Mobile NixOS tracks nixpkgs
   unstable, which will carry newer (unverified-on-this-hardware)
   versions; reproduce the verified set by pinning nixpkgs/derivations.

Everything else (boot geometry, cmdline, storage on the 27.7 GiB
`linux` partition, USB gadget networking, TWRP deployment, WDT reboot
quirk, keyboard/touch, backlight/charger) maps cleanly onto NixOS
configuration. Effort estimate and a phased plan are in §9.

---

## 2. Background: the two projects

### 2.1 GeminiPDA (source of the work to port)

Goal: run modern mainline Linux on the Planet Computers Gemini PDA
(MT6797X "aeon", Helio X25 family, 10-core big.LITTLE, Mali-T880 MP4,
no DRM display driver — the *only* display path is the framebuffer LK
initialises and hands over via `atag,videolfb`).

Current milestone (per README/roadmap, 2026-09-04):
the device **auto-boots into a GPU-accelerated desktop** (Plasma 6
and/or LXQt/labwc nested on `gemwl`, a custom wlroots 0.18 Wayland
compositor) **rendering at 60 fps straight into the LK framebuffer**
via a dma-buf (zero CPU pixels), over a Debian 13 rootfs on the eMMC
`linux` partition. Touch, keyboard (custom xkb layout), backlight,
charger/battery-guard and all 10 CPU cores work; audio/PMIC/DRM
display are the remaining open roadmap items.

Artifact inventory that would need to move (see §4 for the mapping):

| Artifact | Where it lives |
|---|---|
| Kernel 6.6 fork, branch `geminipda-bringup` (all bring-up committed in-tree incl. `geminipda-fb` LK-fb driver + dma-buf export, board DTS `mt6797-gemini-pda.dts`, Novatek NT36772 touch, PSCI/A72 bring-up, config fragments) | `repos/linux-6.6` |
| Kernel `.config` (3077 `=y` / 1018 `=m`, `CONFIG_CMDLINE_FORCE=y` with the full working cmdline) | `build/out-6.6/config` |
| Custom initramfs (~1.26 MiB; partition-scanning `/init`) | `build/out-6.6/initramfs-stage/`, built by `build/build-initramfs-6.6.sh` |
| Boot.img packer (v0 header, page 2048, kernel 0x40200000, ramdisk 0x45000000, tags 0x44000000, appended DTB) | `other-work/gemini-linux/scripts/pack-boot-img.py` + `build/pack-gemini-6.6.sh` |
| `gemwl` compositor (single C file, links wlroots 0.18 + EGL) + services | `build/wayland/` (`gemwl.c`, `*.service`, `start-plasma-nested.sh`) |
| Mesa fork patches (panfrost dma-buf import/export, desktop flags) | `mesa/` submodule + `patches/mesa-panfrost-*.patch`, `build/mesa-cross/` |
| wlroots experiment patch (2-draw split; likely superseded by kernel #286 SH_OS fix) | `build/patches/wlroots-0.18.2-pass-2draw.patch` |
| GPU power-on (devmem LDO/MFG sequence), A72 bring-up service, battery-guard, backlight/power CLIs, xkb layout, soak-monitor, gltests suite, gemwin | `build/rootfs-files/*`, `build/a72-bringup/`, `build/gltests/`, `build/gemwin/`, `build/gpu-poweron.sh` |
| Flash/deploy scripts (TWRP para-switch, no-swipe flash, WDT reboot) | `build/flash-nohelp.sh`, `build/boot-switch.sh`, `build/device-reboot.sh` |
| Knowledge base (boot chain, hardware, gpu-rendering, handovers) | `docs/*.md`, `feasibility.md`, `AGENTS.md` |

### 2.2 Mobile NixOS (the target base)

"Superset of NixOS for mobile devices" (`README.adoc`): abstracting
device differences (boot loaders, kernels, storage, system images)
behind NixOS modules, while stage-2 remains ordinary NixOS.

Relevant architecture (verified in the repo, commit `2c132754`):

- **Device model** (`doc/in-depth/devices.adoc`): *system* (boot-chain
  family) → *platform/SoC* fragment → *family* → *device*. Devices
  live in `devices/`; a device can also be given to the evaluator as a
  **path** (`lib/release-tools.nix`: `builtins.isPath device` → import
  that module) — so a Gemini device definition can live entirely
  out-of-tree in the `gemini-nixos` repo.
- **System types** (`modules/system-types/`): `android` is the relevant
  one (boot.img via `mkbootimg`, `bootimg.flash.*` geometry options,
  appended-DTB support, recovery image, flash scripts, and
  **TWRP-flashable zips** via `flashable-zip.nix` — see §3.2).
- **Kernel builder** (`overlay/mobile-nixos/kernel/builder.nix`, ~700
  lines; docs in `doc/in-depth/kernel-builder.adoc`): builds arbitrary
  kernel sources from a `configfile`; supports compressed
  `Image.gz`-dtb outputs (`isImageGzDtb`), `isCompressed "gz"/false`,
  module vs monolithic, Werror removal for new compilers, DTB builds
  (`buildDTBs`), menuconfig/normalization helpers (`bin/menuconfig`,
  `bin/kernel-normalize-config`).
- **Boot**: stage-1 (initrd built by NixOS machinery + Mobile NixOS
  additions: mruby boot GUI, recovery menu, optional initrd SSH/USB
  gadget/firmware) → mounts the real rootfs **by filesystem label**
  (`modules/rootfs.nix`: default `NIXOS_SYSTEM`, `fileSystems."/" =
  /dev/disk/by-label/...`) → switch_root to the NixOS generation →
  ordinary NixOS stage-2 (systemd).
- **Rootfs image**: ext4 image populated with the system closure
  (`modules/rootfs.nix`, `mobile.generatedFilesystems.rootfs`, first-
  boot store re-hydration via `nix-store --load-db`), flashed to a
  partition (`system_partition_destination`, default `system`).
- **SoC fragments** (`modules/hardware-mediatek.nix` etc.): small
  options setting `mobile.system.system = "aarch64-linux"` plus kernel
  config (e.g. `ARCH_MEDIATEK`) and quirks. Known MTK fragments:
  mt6755, mt6785, mt8127, mt8183 — **no mt6797 yet** (trivial to add;
  the mt8183 fragment is ~5 lines of config effect).
- **Build**: cross-compiles from x86_64 (`examples/hello` documented as
  working via cross). nixpkgs pin: `npins/sources.json` →
  `nixos-unstable` (~26.11pre, 2026-08 era) — same generation as the
  GeminiPDA devshell's nixpkgs (2026-08-26).

---

## 3. What fits out of the box

The Gemini PDA's boot contract (deep knowledge in `GeminiPDA/docs/
boot-chain.md`) maps almost 1:1 onto Mobile NixOS features:

### 3.1 `boot.img` geometry — exact match

Measured from the current artifact `build/out-6.6/cleanup-boot.img`
(and `other-work/release-269/new_kali_boot.img`) by direct header
parse (AOSP v0 header):

| Field | Measured value | Mobile NixOS option |
|---|---|---|
| page size | 2048 | `mobile.system.android.bootimg.flash.pagesize = "2048"` |
| kernel load addr | 0x40200000 (2 MiB-aligned; arm64 rule) | `offset_base` + `offset_kernel` (e.g. base 0x40000000 + kernel 0x200000) |
| ramdisk addr | 0x45000000 | `offset_ramdisk` |
| tags addr | 0x44000000 | `offset_tags` |
| second addr | 0x40f00000 (unused, size 0) | `offset_second` |
| cmdline | from `config.boot.kernelParams` | `bootimg.nix` writes `cmdline = concatStringsSep " " config.boot.kernelParams` |

`mkbootimg` computes `addr = base + offset`, so any combination
reproducing the three load addresses is expressible. Header v0 (no
vendor extensions) — matches Mobile NixOS's `bootimg.nix` (mkbootimg
v0 output).

### 3.2 Appended-DTB kernel payload — exact match

LK requires the DTB appended after the gzip kernel payload (scans the
last 2 MiB for the FDT magic; ignores the header `dt_size`). Two
equivalent Mobile NixOS paths:

- kernel-builder `isImageGzDtb = true` → kernel target
  `Image.gz-dtb` (kernel source tree already builds
  `mediatek/mt6797-gemini-pda.dtb`; Makefile entry present), or
- `appendDTB = [ "dtbs/mediatek/mt6797-gemini-pda.dtb" ]` with plain
  `Image.gz` (exactly what `pack-gemini-6.6.sh` does today with
  `cat Image.gz dtb`).

`mkbootimg` then packs it as the `kernel` blob — byte-equivalent to the
current `pack-boot-img.py` output.

### 3.3 Boot mode / kernel cmdline

LK assembles the final cmdline as
`LK_base + <boot.img cmdline> + lcm=… + earlycon=uartmtk… +
androidboot.…`, then overwrites `/chosen/bootargs`. Consequences (per
`boot-chain.md` §5) all handled declaratively in NixOS:

- our `console=ttyS0,921600n1` and `earlycon=mtk8250,mmio32,
  0x11002000` must ride in the boot.img cmdline → `boot.kernelParams`
  (first-valid-wins ordering already guaranteed because the LK base and
  its own additions are mainline-inert / rejected).
- The current kernel forces its cmdline with `CONFIG_CMDLINE_FORCE=y`
  (`build/out-6.6/config`). For Mobile NixOS the cleaner contract is to
  **drop `CMDLINE_FORCE`** and manage the full param list in NixOS
  (`CONFIG_CMDLINE=""`): fbcon rotation/font, g_ether MACs,
  `clk/pd/regulator_ignore_unused` (needed until the PMIC milestone),
  `maxcpus=8`, `consoleblank=0`, etc. A/B on hardware before adopting
  (cmdline ordering is the classic trap on this device).
- The ramdisk in boot.img is the initrd: kernel unpacks the CPIO and
  runs `/init`. Mobile NixOS stage-1 is such a CPIO initrd (its `/init`
  is what starts; LK's `root=/dev/ram` is inert once an initrd is
  mounted and stage-1's own `fileSystems."/"` device is used — exactly
  how the current custom initramfs boots too).

### 3.4 Storage: rootfs partition exists and is huge

`linux` partition p29 = **27.7 GiB** (currently Gemian/Debian rootfs).
Mobile NixOS flashes `system.img` (ext4, default label `NIXOS_SYSTEM`,
auto-resize on first boot) there; stage-1 mounts by label — immune to
the device's **mmc0/mmc1 numbering quirk** that forced the current
partition-scanning initramfs (by-label needs udev only, which stage-1
runs). Wiping p29 forfeits Gemian, not Android (`system` p27 is
untouched).

### 3.5 Deployment without fastboot — existing tooling applies

`flashingMethod` (fastboot/lk2nd/odin) only drives generated scripts
and docs, not the artifacts. The Gemini's no-fastboot reality is
already solved in-project:

- **TWRP-flashable zips**: `make-flashable-zip` produces
  `flashable-<device>.zip` (boot.img + system.img via standard
  update-binary `flash_partition` calls). The device's patched
  no-swipe TWRP (boot3 slot, entered via the `para` partition
  `boot-recovery` command) runs these — and the existing
  `flash-nohelp.sh` / `boot-switch.sh` pipeline (para-write → WDT
  EXRST self-boot → TWRP adb → dd) can also just `dd` the
  `android-bootimg` / `rootfs.img` artifacts straight to `boot`/`linux`
  as it does today.
- `boot.switch` semantics are unchanged: active kernel lives in the
  `boot` partition (p22); Mobile NixOS `boot.img` is written there the
  same way.

### 3.6 Kernel packaging

Mobile NixOS's kernel-builder takes any source + a config file:
- `src` = the `geminipda-bringup` branch (committed bring-up state;
  fetchGit from the local/private remote).
- `configfile` = `build/out-6.6/config` (full `.config`; builder does
  `cat ${configfile} > $buildRoot/.config` then `olddefconfig`), or a
  normalized `config.aarch64` produced by `bin/kernel-normalize-config
  <device>`.
- GCC-15-vs-6.6: already proven — the current pipeline cross-builds
  with nixpkgs GCC 15.2.0 / binutils 2.46 (docs/build-environment.md);
  builder has `enableRemovingWerror` if needed.
- `isCompressed = "gz"` (LK gunzips the payload; xz for the kernel is
  *not* assumed available in LK — see R1 mitigation).
- DTB: kernel target or `buildDTBs` (builder commit 92e54819 exposes
  dtbs) → `appendDTB`.
- Non-modular (`modular = false`) keeps stage-1 tiny; note
  `DRM_PANFROST=m`, `pwm-mtk-disp=m` etc. are modules → ship them to
  stage-2 via the kernel package's module tree (standard NixOS).

### 3.7 USB gadget networking / SSH

g_ether (CDC ECM) is the console-of-record for this device
(10.15.19.82). Mobile NixOS gadget support: `mobile.usb.mode =
"gadgetfs"` with configfs functions (`modules/initrd-usb.nix`,
`usb-gadget.nix`); stage-2 networking + `services.openssh` are plain
NixOS. The project does NOT use those: the precomposed `g_ether` gadget
is kept (auto-binds the UDC in kernel context — no userspace ordering to
get wrong on the only debug link) and the fixed MACs come from the
g_ether kernel params (proven).

**[corrected 2026-09-17]** This section used to read "g_ether (CDC ECM)"
while the *receipt* was RNDIS: `ether.c` registers RNDIS first when
`CONFIG_USB_ETH_RNDIS=y`, so every host picked it and the unit enumerated
as `0525:a4a2`. That symbol is now **off** (`bin/prune-kernel-config.sh`
§9c): the gadget registers a single CDC-ECM configuration
(`0525:a4a1`), which is what makes macOS — no RNDIS driver — able to use
the link and the reboot/flash cycle. The "tiny extension" this section
predicted turned out to be a kernel-config subtraction. Full receipts:
**`docs/usb-network.md`** (🟡 not yet reflashed/on glass).

### 3.8 Serial console, fbcon, framebuffer console

`geminipda-fb` + `fbcon=rotate:3` console output is kernel-side
(`FB_GEMINIPDA=y`) — no Mobile NixOS change needed; NixOS console font
(TER16x32) via `console.font` or the fbcon kernel param as today.

---

## 4. Mapping: GeminiPDA work → Mobile NixOS/NixOS construct

| GeminiPDA artifact | Mobile NixOS / NixOS equivalent |
|---|---|
| `repos/linux-6.6` branch `geminipda-bringup` + `build/out-6.6/config` | `mobile.boot.stage-1.kernel.package = pkgs.callPackage ./kernel {}` (kernel-builder wrapper in device dir), or an out-of-tree derivation |
| `pack-gemini-6.6.sh` / `pack-boot-img.py` | `mobile.system.type = "android"` + `bootimg.flash.*` + `appendDTB` (no custom packer needed) |
| Custom initramfs (scans for `/etc/os-release`) | Replaced by Mobile NixOS stage-1 (`outputs.initrd`); by-label rootfs mount; optional initrd SSH/USB-gadget for bring-up |
| Debian 13 rootfs + scp'd service files under `/root`, `/etc/systemd/system` | `rootfs.img` (system closure) + NixOS modules (systemd services/units, env, packages) |
| `gemwl` (wlroots-0.18 compositor + fb backend + compute-shader double-buffer) | Nix derivation: single C file; deps wlroots (0.18.x) + wayland + xkbcommon + EGL/GLESv2 — straightforward `stdenv.mkDerivation` |
| Patched Mesa 25.0.7 fork (panfrost dma-buf caps, `-Dopengl=true`, gbm) | nixpkgs `mesa` override/overlay with `patches/`, or fork derivation; keep `PAN_MESA_DEBUG=noafbc` env + runtime-PM `control=on` |
| KWin/Plasma 6 nested session (`plasma-nested.service`, start script) | nixpkgs `plasma5/plasma6` packages + custom unit; export the session env (`XDG_RUNTIME_DIR=/run/gemwl`, GBM backend path) |
| labwc/LXQt A/B session | nixpkgs `labwc`/`lxqt` + custom unit |
| `gpu-poweron.sh` (devmem LDO/MFG/PMU writes) | one-shot systemd service (`After=systemd-modules-load`, before compositor) |
| `a72-up` / `cl2-up.sh` (DA9214+SPM+SMC pre-sequence) | one-shot systemd service, `Before=multi-user.target`; kernel #294 already allows late-CPU caps |
| battery-guard / `power` / `backlight` CLIs (bash + sysfs/devmem) | small package or inline `systemd.services` + `environment.systemPackages` |
| xkb `gemini` layout + console keymap `gemini-uk.map` | NixOS `console.keyMap`/xorg/keyboard config; kernel KEY_APOSTROPHE fix is in the branch |
| WDT reboot quirk (`shutdown -r` powers off; `devmem 0x10007004 0x48` EXRST works) | custom `wdt-reboot` unit/alias or `systemd` override documented in the device README; `CONFIG_MEDIATEK_WATCHDOG=y` already set |
| gltests suite / soak-monitor / gemwin / capture tooling | dev tooling package(s) in the device's dev shell or `environment.systemPackages` (optional) |
| `flash-nohelp.sh`, `boot-switch.sh`, `device-reboot.sh` | stay as host-side scripts (distro-independent); reference the `android-flashable-zip` / `android-bootimg` / `rootfs` outputs |
| Kernel docs knowledge base | becomes device README + comments in the device Nix files |

---

## 5. Gaps / small Mobile NixOS extensions required

1. **SoC fragment `mediatek-mt6797`** — add to `modules/hardware-
   mediatek.nix` (mirror mt8183: `mobile.system.system =
   "aarch64-linux"`, `ARCH_MEDIATEK` kernel config). ~10 lines; either
   upstream PR or an out-of-tree module that declares
   `options.mobile.hardware.socs.mediatek-mt6797.enable` (the assertion
   in `hardware-soc.nix` checks `cfg.socs ? ${cfg.soc}`, so an
   out-of-tree option definition is sufficient).
2. **`flashingMethod`** enum has no "manual/TWRP" value — cosmetic;
   pick a value for docs and rely on the artifacts + existing flash
   scripts. (Worth an upstream PR adding `"manual"`.)
3. **Device definition + family** — new `devices/planet-geminipda/`
   (or out-of-tree path-based device under `gemini-nixos/`): identity,
   hardware (soc, RAM 4 GiB, screen 1080×2160 rotated), android system
   type config (§3.1/§3.2), kernel params (§3.3), stage-1 settings,
   usb gadget, services.
4. **nixpkgs pin choice** — decide: follow Mobile NixOS's npins
   nixpkgs (simplest, but version drift risk, §7 R2) vs pin an older
   nixpkgs that still carries the verified wlroots 0.18 / Mesa 25 /
   Plasma 6.3 / kwin 6.3 set, with Mobile NixOS overlaid.
5. **Custom Mesa/wlroots/gemwl packaging** — nixpkgs `mesa` and
   `wlroots` overrides need the fork patches + exact feature flags
   (gbm/wayland/opengl/panfrost). Small but version-sensitive; see R2.
6. **`linux` partition repurpose** — reformat p29 ext4 with label
   `NIXOS_SYSTEM` (destructive to Gemian only; Android p27 untouched).

No changes to Mobile NixOS *system types* are needed — the `android`
type and its boot.img assembly cover the Gemini's boot contract.

---

## 6. Boot-chain compatibility checklist (from `boot-chain.md`)

| LK requirement | Status under Mobile NixOS |
|---|---|
| boot.img magic `ANDROID!`, no signature check | ✅ mkbootimg v0 |
| DTB appended at end of kernel payload (last 2 MiB scan) | ✅ `isImageGzDtb` / `appendDTB` |
| kernel payload gzip, decompressed ≤ 28 MiB (LK MT6797: 50 MiB buffer) | ✅ kernel compressed `gz`; **watch decompressed size** (see R1) |
| kernel_addr 512 KiB/2 MiB aligned | ✅ 0x40200000 geometry |
| ramdisk at 0x45000000 | ✅ via offsets |
| DTB copied to 0x44000000 (`tags_addr`) then patched | ✅ header value; LK does the copy/patches itself |
| `/chosen` bootargs overwritten by LK → cmdline must come from boot.img cmdline | ✅ `boot.kernelParams` (drop `CONFIG_CMDLINE_FORCE`) |
| CPU nodes need `clock-frequency` (LK no-NULL-check) | ✅ in committed DTS |
| `/memory`, `/chosen`, reserved regions in DTS | ✅ in committed DTS |
| Screen console via `atag,videolfb` + `geminipda-fb` | ✅ kernel-side, unchanged |
| POC-charging / TWRP `para` boot switching | ✅ deployment-level, unchanged (device-independent) |

---

## 7. Risks and mitigations

### R1 (HEADLINE) — 16 MiB boot partition vs Mobile NixOS stage-1 size

**RESOLVED (build 2026-09-05).** Measured Mobile NixOS stage-1 for
this kernel/config: **9.66 MiB xz** (39 MiB unpacked — loader/ruby/
systemd/udev/GUI extra-utils). With the bring-up kernel payload
(13.45 MiB gz + DTB) that totals ~23.3 MiB: the stage-1 **cannot**
fit. The boot image now carries the partition-scanning busybox initrd
from the verified bring-up instead (1.26 MiB gzip):

```
boot.img = 15,433,728 B = 14.72 MiB  (cap: 16 MiB, headroom ~1.3 MiB)
  kernel: Image.gz + DTB      14,108,276 B (13.45 MiB)
  ramdisk:  gzip cpio (busybox + /init)   1,322,243 B (1.26 MiB)
```

The stage-1 trade-offs (no boot USB/GUI/SSH; serial-only early boot)
and the optional recovery-image escape hatch are documented in
`README.md`.

Original bring-up image reference: boot.img = 14,665,728 B
(~13.98 MiB) for a 16 MiB partition (`boot` p22 / `boot2` p30 /
`boot3` p31): kernel payload (Image.gz+DTB) 13,342,273 B ≈
**12.72 MiB**, initramfs 1,320,162 B ≈ **1.26 MiB**.

The current custom initramfs is 1.3 MiB by design (busybox-less, a few
shell tools). A Mobile NixOS stage-1 initrd is an order of magnitude
larger (busybox + udev + initrd tools + boot UI machinery) — several
MiB even with `xz` compression. Motorola-Potter (also 16 MiB `boot`)
is marked *broken* in-tree precisely because its boot image "does not
fit".

Mitigations (in increasing order of invasiveness):

1. **Shrink the kernel payload.** 12.7 MiB of gz is large for a
   bring-up kernel; the reference mainline 7.1.3 image was ~9.4 MiB.
   Drop unused `=y` bloat and bring-up debug (`CONFIG_DEBUG_INFO`?
   pstore keepers, unneeded built-in drivers), target ≤ ~10 MiB → frees
   ~3.7 MiB for stage-1.
2. **Minimise stage-1**: non-modular kernel (already effectively so for
   boot-critical paths; panfrost/pwm-mtk-disp stay modules loaded from
   stage-2), `mobile.boot.stage-1.compression = "xz"` (kernel has
   `CONFIG_RD_XZ=y`), no initrd firmware, no initrd USB/SSH extras in
   the default image, no splash/GUI in the *boot* image (the boot GUI
   and recovery menu can be kept in the separate recovery.img output).
3. **Measure first.** Before committing, build the minimal device
   config and record `outputs.initrd` compressed size; iterate on the
   budget table above. A NixOS non-modular stage-1 target of
   ~4–6 MiB xz is plausible but must be proven on this kernel/config.
4. **GPT surgery (fallback):** Android userdata (p32, 27.3 GiB) is
   FDE-encrypted and Android `system` (p27) is unused if Android is
   abandoned; a resized/re-created `boot`-adjacent partition or a GPT
   move would relax the cap, but breaks the "leave Android bootable"
   property and the boot-switch story — last resort.

   (Not needed: options 2+3 in the bring-up form above suffice.)

### R2 — version drift vs the verified graphics stack
**PARTIALLY RESOLVED (build level, 2026-09-05); nested-session stack (KDE) still open.**

The GPU path that works was reached through dozens of A/B cycles with a
specific combo: kernel 6.6 `geminipda-bringup`, **Mesa 25.0.7 fork**
(panfrost dma-buf caps, desktop GL), **wlroots 0.18.2**, kwin 6.3.6 /
labwc 0.8.3, Plasma 6.3.6. nixpkgs unstable (26.11pre) ships newer
Mesa/wlroots/KDE; behaviour on the T880-with-no-DRM path is
unpredictable (and historically very buggy). Mitigation: pin nixpkgs /
override derivations so the NixOS image ships the **exact verified
versions** first; only then evaluate upgrades one component at a time.
Note the recent kernel-side fixes (#286 SH_OS, #289 touch frame, #294
A72) mean some old Mesa/wlroots workarounds are redundant — a chance to
simplify, but re-validate on glass.

Mesa is now an in-tree derivation (`pkgs/mesa-geminipda.nix`): vanilla
25.0.7 + the 272-line fork patch (verified byte-identical to the
on-glass fork tree, submodule commit `ac19be0` = `mesa-25.0.7-1-gac19be0`),
surfaceless EGL/glavnd-ICD-only build with the exact verified meson
flag set, installed alongside `libglvnd` in the system closure. Since
2026-09-07 the base is the **published** upstream 25.0.7 archive
fetched by hash (no vendored tarball — the first library on the
“published base + in-repo delta” pattern, `docs/library-deltas.md`;
the kernel snapshot is next). Build quirks documented in the
derivation:

- the nixpkgs pin's Mesa (26.1.4) cannot take the 25.0.7 patch
  (standalone 25.0.7 is correct);
- the GitLab **API** archive endpoint (`fetchFromGitLab`) is
  byte-unstable (three fetches → three different tar bytes on
  2026-09-05), so the source is a `fetchurl` of the byte-stable
  canonical `/-/archive/` URL of tag `mesa-25.0.7` (hash-pinned,
  same bytes as the previously vendored tarball);
- nixpkgs' meson setup hook passes `-Dauto_features=enabled` by
  default. The verified reference build ran plain meson (auto
  features stay `auto`), so the derivation sets
  `mesonAutoFeatures = "auto"` — otherwise `microsoft-clc` force-ons
  and meson demands the `libclc` dependency (meson.build:850), and
  `xlib-lease` becomes a hard require() error (meson.build:472);
- mesa 25.0.7's meson.build requires python3 + mako + PyYAML +
  packaging at configure time; `find_program` falls back to meson's
  own interpreter when no python3 is on the build PATH (and that one
  has none of these modules), so the derivation provisions
  `python3.withPackages` on the PATH;
- build-host tools in a cross derivation: a bare `patchelf`/`python3`
  in the cross package set resolves to the **aarch64** version
  ("Exec format error" on the x86_64 build host); the derivation
  uses `buildPackages.patchelf`/`buildPackages.python3`
  (meson/ninja/bison/flex happen to be build-native in this set);
- **ICD manifest discovery** (found by rootfs verification, not the
  build): the compiled-in libglvnd scan list is
  `/run/opengl-driver/share/glvnd/egl_vendor.d` (only created by
  `hardware.graphics` — off here), `/etc/glvnd/egl_vendor.d`, and
  `/usr/share/glvnd/egl_vendor.d` (no `/usr` on NixOS). NixOS does
  **not** merge a package's `$out/etc` into the system `/etc`, so
  `config/gemini.nix` wires the manifest explicitly via
  `environment.etc."glvnd/egl_vendor.d/50_mesa.json"` →
  `${mesa}/etc/glvnd/...`. Without that entry the ICD is silently
  undiscoverable (libEGL.so.1 finds no vendor JSON → no display);
- **ICD RUNPATH** (same verification): `libEGL_mesa.so.0` has a
  `DT_NEEDED` on `libgallium-25.0.7.so` (same dir, no ldconfig cache
  on NixOS), so postInstall appends its own lib dir to the ICD's
  `DT_RUNPATH` with `patchelf --add-rpath` — `--set-rpath` would
  replace the build-time entries and make the ICD's other NEEDEDs
  (`libdrm.so.2`, `libm`, `libc`) unresolvable. `libgallium`'s own
  NEEDEDs are covered by its unmodified build-time RUNPATH.

### R3 — no-DRM desktop model is custom

No KMS/DRM device → stock mobile stacks (Phosh, genuine wlroots DRM
backends, display managers) cannot scan out; logind reports
`CanGraphical=no` (already documented). The working model is
"services, not display manager": `gemwl.service` owns the fb, nested
KWin/LXQt sessions run as units with a private `XDG_RUNTIME_DIR`.
Entirely expressible in NixOS, but it must be packaged faithfully —
this is the largest *porting-by-hand* item (see §9 phase 4). If/when
mtk_drm display support lands (roadmap milestone-4 Path B), the whole
gemwl layer becomes removable.

### R4 — cmdline contract changes (CMDLINE_FORCE removal)

Moving the cmdline from `CONFIG_CMDLINE_FORCE` into
`boot.kernelParams` changes nothing physically (LK appends boot.img
cmdline into `/chosen/bootargs`) but is a contract change — keep an
A/B fallback boot slot (boot2) and verify first boot on serial/screen
console before committing (the project's boot-chain doc is explicit
that ordering traps hang boot).

### R5 — module loading timing (GPU power, A72)
**RESOLVED (build level, 2026-09-05).** GPU LDO/PMU devmem writes and
the A72 pre-sequence are one-shot services that must run before
GPU/compositor or big-core use. Implemented in `services/gemini-pda.nix`
as systemd units: `gemini-gpu-poweron.service` (oneshot, `After=`
udevd, `RemainAfterExit`), `gemini-a72-up.service` (oneshot,
`After=` udevd + multi-user; `ExecStartPre` modprobes `sramldo-smc`
with an `insmod`-by-store-path fallback), `gemini-battery-guard.service`
(long-running, env-configured thresholds). The scripts are the
reference implementations verbatim (`services/scripts/`), each unit
gets an explicit `Path=` (systemd's default PATH is minimal — no
`devmem`/`i2cset`), and the `sramldo-smc` module is built **inside the
kernel derivation's `postInstall`** against the same build tree, so it
is ABI-identical to the boot kernel and lands in the same
`lib/modules/<ver>/extra/` tree that NixOS's wrapped `modprobe`
searches. CLIs `backlight`, `power`, `battstat`, `bq25896-raw.sh` and
a new `gemini-wdt-reboot` (WDT EXRST self-boot; plain `reboot`/TOPRGU
powers the unit off on this board) ship via a small `runCommand`
utils package. On-glass verification is still owed.

Hardware constraint (from the reference session log, learned by
freezing a device): the verified cmdline keeps **`maxcpus=8`
intentionally** — the kernel boots the 8 A53s only; the A72 cluster
(cpu8/9) is powered down and SPM-isolated at boot. A bare sysfs
hotplug of cpu8/cpu9 **hard-freezes** the unit (LK leaves the MTK
WDT disarmed, so no auto-reset — physical power cycle needed). The
safe bring-up is exactly what `gemini-a72-up.service` does:
`cl2-up.sh` runs the vendor pre-CPU_ON power sequence (DA9214 BUCKB
rail on i2c6, SPM un-isolate, SRAM-LDO 1.1 V SMC via `sramldo-smc`)
**with the WDT armed (15 s)** so a freeze self-recovers, and only
then does the PSCI CPU_ON via sysfs. The pinned kernel
(`733c0c7ea`) already carries the required late-cpufeature fix. Do
not raise `maxcpus` or hotplug the A72s outside that service.

**2026-09-07 addendum (sibling commit 738d19f):** the REVERSE path is
now proven on the same #329 kernel — per-core PSCI offline is clean
("psci: CPU9 killed (polled 0 ms)"), and the last-A72 teardown (secure
power_off_cl3 inside the controller's AFFINITY_INFO SMC: CCI/snoop
withdrawal, B mux/PLL, SPM MP2 power-down, B_EXT_BUCK_ISO re-assert
0x10006290 bit1, 0x10006218 bit0 clear) completes on mainline, after
which the external DA9214 BUCKB rail is dropped (vendor
cpu_power_off_buck) — the box returns to the byte-identical cold-boot
state `cl2-up.sh` was built for (re-enable ×2 cycles verified).
`services/scripts/cl2-down.sh` (verbatim port, WDT-armed 20 s) ships in
the same utils package as a hand-run CLI (`cl2-down.sh [cpu9|cpu8|
both]`) — deliberately NOT a unit: the power-saving down is on-demand
and must not race `gemini-a72-up` at boot. On-hardware exit for the
down path: same milestone-11/12 cycle as everything else (still nothing
flashed in this repo).

### R6 — first-boot store re-hydration on p29

`rootfs.img` is populated with the system closure and registers the
store on first boot (`nix-store --load-db`, postBootCommands). 27.7 GiB
is ample; slow eMMC write of a multi-GB image is a time consideration
for flashing, not feasibility (compressLargeArtifacts exists).

### R7 — Mobile NixOS maintenance burden

> [corrected 2026-09-08] The last line no longer states the working
> model: the x86_64 CROSS toplevel is ABANDONED (nixpkgs cross walls,
> Qt6CoreTools for the lxqt scope); the canonical build is NATIVE
> aarch64 drvs (flake buildSystem = aarch64-linux) built on the
> 192.168.49.191 remote builder from the x86_64 host — same store
> closure the device runs, see flake.nix header + bin/deploy.sh.

Mobile NixOS is a moving target against nixpkgs unstable; the
`gemini-nixos` repo should pin both (npins or flake lock) and treat
updates deliberately. The device is `aarch64` and cross-builds from
x86_64 (proven by `examples/hello`).

### R8 — aarch64 cross-build: systemd BPF programs fail to compile
**RESOLVED (build 2026-09-05).** The pinned nixpkgs
(`nixos-26.11pre1031299`) cannot cross-build the full `systemd` (261)
for aarch64 from x86_64: meson compiles the BPF programs with the
*host* clang and `-target bpf` but no sysroot → `fatal error:
'errno.h' file not found` (src/bpf/restrict-fs.bpf.c). Two subtleties:

- `config.systemd.package = pkgs.systemd.override { withLibBPF =
  false; }` is not sufficient — packages like **dbus-broker** depend
  on `pkgs.systemd` directly and pull the BPF-enabled build back into
  the closure.
- The fix is a package-set overlay (`nixpkgs.overlays` in
  `config/gemini.nix`) so *every* reference sees the no-BPF systemd.
  `nixpkgs.overlays` works in this eval even though `specialArgs.pkgs`
  is passed at the outer shim: Mobile NixOS's `evalWith` does not
  forward `pkgs` to `evalConfig`, so the `nixpkgs.*` module options
  (re-import with `localSystem`/`crossSystem`) are fully active, and
  list-typed options concatenate definitions across modules.

The systemd-bpf LSM units (restrict-fs, io_uring restrictions) are
irrelevant for a trusted single-user PDA; revisit if the nixpkgs pin
moves (an upstream fix may land).

### R9 — kernel-builder: `modules.dep` silently missing from the module tree
**RESOLVED (2026-09-05).** Mobile NixOS's kernel builder seds the
top-level `DEPMOD ?= ...` make variable to point at kmod's depmod, but
the `modules_install` rule runs `scripts/depmod.sh`, which does a
**PATH lookup** (`command -v $DEPMOD`) and silently no-ops when
depmod isn't on the build PATH — the installed tree has all 1166
`.ko` files but **no `modules.dep`**, so any `modprobe` by name
(panfrost, `sramldo-smc`, ...) fails to resolve. NixOS's
`aggregateModules` (kmod aggregator) does not re-run depmod because the
module dir is a read-only store symlink. Fix: add `buildPackages.kmod`
to the kernel derivation's `nativeBuildInputs` and re-run depmod
explicitly after the out-of-tree module install (see
`devices/planet-geminipda/kernel/default.nix`).

Related traps hit in the same step:

- the out-of-tree module source is itself a store path (read-only),
  and `make M=<dir> modules` writes `.o`/`.o.d` files into `<dir>` —
  so the source must be copied to a writable location in the build
  dir before building;
- `M=` must be an **absolute** path: a relative `M=sramldo-smc-src`
  is resolved against the kernel *source* tree (a read-only store
  path here), not the cwd — "../sramldo-smc-src/Makefile: No such
  file". `M="$PWD/sramldo-smc-src"` (cwd = the O= build dir) works.

### R10 — verbatim Debian service scripts shebang `#!/bin/bash` (no /bin/bash on NixOS)
**RESOLVED (2026-09-07) — package-level shebang rewrite.** The ported
bring-up scripts (`services/scripts/*`) keep the Debian-rootfs shebang
`#!/bin/bash` (`#!/usr/bin/env bash` in two), but a NixOS stage-2 only
creates `/bin/sh` (via `environment.binsh`) — never `/bin/bash`.
systemd `ExecStart=` execs the script **directly** (no shell), the
kernel resolves the `#!` interpreter by absolute path, so every
bash-shebanged unit (`gemini-gpu-poweron`, `gemini-a72-up`/`cl2-up.sh`,
`gemini-battery-guard`, `gemini-audio-defaults`, and the new
`gemini-backlight-default`) would fail on glass with status=203/EXEC
(ENOENT). Found while auditing the outstanding.md items (their closure
inspection read the units, not their execution). Fix:
`services/gemini-utils.nix` rewrites bash/env-bash shebangs to the store
bash (`#!${bash}/bin/bash`, the aarch64 one in the package set) at
package time — the NixOS-idiomatic `patchShebangs` equivalent.
`#!/bin/sh` scripts are untouched (/bin/sh exists).

Related latent issue (same root cause, still OPEN): any *other*
verbatim bash script that ends up executed outside the gemini-pda-utils
package needs the same treatment.

### R12 — systemd 261 removed the unit `Path=` key (status=127 on glass)

**FOUND + FIXED (2026-09-07).** Every service PATH declared as
`serviceConfig.Path = lib.makeBinPath [...]` was a silent no-op: the
device's systemd 261 rejects the `Path=` key outright (journal:
`Unknown key 'Path' in section [Service], ignoring`) — upstream removed
it (replaced by `ExecSearchPath=`, added v250) and nixpkgs migrated its
own modules to `serviceConfig.ExecSearchPath`/the module-level `path`
option. Result on glass: gemini-audio-defaults (amixer), wifi-internal
(modprobe), gpu-poweron (busybox) all failed with "command not found"
even though `systemctl cat` showed the dirs. Fix: all 11 service
`Path=` uses migrated to the module option `path = [ pkgs... ]` (list
of packages -> Environment PATH prepended to the unit default), the
nixpkgs-idiomatic form for this pin. Confirmed on glass: audio/GPU/wifi
units now find their tools.

### R13 — make_ext4fs image geometry cannot grow past 2x (growfs EINVAL)

**FOUND + FIXED (2026-09-07).** mobile-nixos builds ext4 rootfs images
with the Android make_ext4fs tool (image-builder ext4.nix). The kernel
can online-grow that geometry only to exactly 2x the image size, then
ext4_resize_fs fails -EINVAL (819200 blocks / 25 groups on the 1.5 GiB
Gemini image; fs stops there, systemd-growfs-root dies, "/" stays
~3.1 GiB). Mechanism: adding the next sparse_super backup group needs
reserved-GDT entries the resize inode was never given
(reserve_backup_gdb/verify_reserved_gdb in fs/ext4/resize.c). Reproduced
on the host kernel with the real image (geometry-intrinsic). Every
mke2fs geometry tested grows 1.5G->27G cleanly. Fix: `pkgs/make-ext4fs-
shim.nix` re-implements the make_ext4fs CLI on mke2fs (default ext4
features: flex_bg/64bit/metadata_csum), hooked via a `lib.mkAfter`
nixpkgs.overlays override (plain overlay defs lost to mnx's overlay
list — order matters). New images grow on first boot via
growfs-root. Offline `resize2fs` also rescues already-flashed
make_ext4fs fs (no geometry limits) — the grow-rootfs verb in
bin/flash-nixos.sh (static musl aarch64 e2fsprogs, TWRP).

### R14 — service Type/RemainAfterExit under unitConfig = ignored

**FOUND + FIXED (2026-09-07).** gemini-pda.nix put `Type`,
`RemainAfterExit`, `Restart`/`RestartSec` under `unitConfig`, which maps
to the unit's `[Unit]` section where systemd ignores those keys — the
units ran as Type=simple and deactivated the moment their script
exited (systemctl show: RemainAfterExit=no). battery-guard only worked
by accident (long-running simple service). Moved to `serviceConfig`
([Service]); oneshots now stay active(exited).

### Non-risks (checked, no action needed)

- **Serial/console**: ttyS0 921600 via kernel params; `earlycon` OK.
- **Watchdog**: `CONFIG_MEDIATEK_WATCHDOG=y` present; WDT-reboot quirk
  is user-space handled.
- **eMMC numbering quirk**: by-label mount sidesteps it.
- **No fuel gauge**: distro-independent (kernel + scripts already done).
- **Android partition layout**: untouched by the plan except p29.
  [superseded 2026-09-07 — under review: the repurpose target is now
  Android's p32 `userdata` instead of p29, keeping Debian on p29 for
  testing; see `docs/repartition-android-space.md`]
- **Touch/keyboard**: kernel-side + xkb config; nothing Mobile NixOS
  specific.
- **Flashing**: TWRP pipeline unchanged.

---

## 8. Decisions required up front

1. **Kernel source flow**: fetch the `geminipda-bringup` branch into
   the Mobile NixOS kernel derivation (recommended, hermetic) vs keep
   today's external build script and only feed artifacts in (not
   recommended — breaks Nix reproducibility).
2. **nixpkgs policy**: pin the verified-version nixpkgs for the GUI
   milestone (R2) vs float on Mobile NixOS's npins pin.
3. **Out-of-tree device** (device-by-path in `gemini-nixos`, upstream
   later) vs forking `repos/mobile-nixos` and adding the device +
   SoC fragment in-tree immediately.
4. **Android p27/p32 fate** — keep untouched (assumed yes) → p29-only
   repurpose. [superseded 2026-09-07 — NixOS rootfs on p32 `userdata`
   (Android erased), Debian kept on p29, dual-boot single boot.img +
   para marker] **[RESOLVED 2026-09-10 — repartitioned one-way to
   TWRP + NixOS only: Android + Debian reclaimed into a single 58 GiB
   p27 `linux` rootfs; see `docs/repartition-android-space.md` §12]**
5. **Scope of the first milestone** — see §9 phase 2 (headless NixOS
   over g_ether SSH) is the natural "definition of done" for the
   port's foundation.

---

## 9. Effort estimate and phased plan

Rough sizes (engineering sessions; device access assumed, per project
working style):

| Phase | Work | Size estimate | Exit criteria |
|---|---|---|---|
| 0 | Repo scaffolding: `gemini-nixos` flake/pins importing Mobile NixOS; out-of-tree device skeleton; kernel derivation for `geminipda-bringup` + config; boot.img geometry §3.1/§3.2 | 1–2 sessions | **DONE (2026-09-05).** `android-bootimg` header fields match `cleanup-boot.img` (kernel/ramdisk/second/tags addrs, 2048 pages, `ANDROID!` v0); kernel payload is Image.gz + appended DTB; eval clean |
| 1 | Stage-1 sizing (R1): build minimal initrd, measure, trim kernel config and stage-1 options to fit 16 MiB | 2–4 sessions | **DONE (2026-09-05) — see R1/R8.** Minimal busybox initrd (1.26 MiB gz) replaces stage-1 in the boot image; boot.img = 14.72 MiB (headroom 1.3 MiB); rootfs generation lookup + by-label symlink handled in the initrd; systemd-BPF cross-build fixed via overlay |
| 2 | Rootfs bring-up: flash `rootfs.img` to p32 `userdata` (dual-boot boot.img; Debian stays on p29 — repartition doc §10, 2026-09-07), g_ether networking, sshd, serial/fbcon console, WDT-reboot unit, boot-switch/flash docs | 1–2 sessions | SSH over g_ether to a NixOS shell on hardware; `nixos-rebuild` round-trip works. **✅ first half DONE on glass 2026-09-07** (NixOS boots + sshd over g_ether — see `docs/phase-2-on-glass.md`); boot needed `bootopt=64S3,32N2,64N2` in the boot.img cmdline field (LK consumes it — §4 correction). **✅ round-trip DONE 2026-09-08, BOTH directions**: host `deploy.sh` (gens ≤ 28) and **device-native build/switch** (`bin/device-rebuild.sh`, gen30 built + switched on the PDA from a /root/gemini-nixos clone; `nix-shell -p` pinned to the flake's nixpkgs rev). Remaining: `growfs` of / (TODO P0, done on the live install), vconsole/audio/wifi service fixes, cold-reboot check of gen30. **[2026-09-10: rootfs repartitioned to the single 58 GiB p27 `linux` (TWRP + NixOS only; Android/Debian reclaimed) — repartition doc §12]** |
| 3 | Kernel-adjacent services: gpu-poweron, a72-up, battery-guard, backlight/power CLIs, watchdog-reboot | 1–2 sessions | **DONE (build level, 2026-09-05).** Units + verbatim scripts in `services/`; sramldo-smc built in the kernel derivation; kernel depmod bug fixed (R9). **2026-09-07:** R10 added — bash shebang rewrite in `gemini-pda-utils` (see §7 R10); + `cl2-down.sh` verbatim (A72 cluster power-down CLI — see R5). Exit on hardware: same as current milestone-11/12 behaviour |
| 4 | Graphics port (R2/R3): ~~package Mesa fork~~ (done in-tree, R2) + ~~wlroots 0.18.x + gemwl~~ (in-tree 2026-09-07: `pkgs/wlroots-geminipda.nix` = pinned 0.18.2 from source, `pkgs/gemwl.nix` + `services/desktop.nix`; Mesa fork now also builds libgbm for wlroots' gles2) + ~~labwc 0.8.3 + nested LXQt~~ (**removed 2026-09-12**, together with Phosh; the GPU desktop is vanilla GNOME via the `geminipda-drm` KMS driver since 2026-09-10 — docs/gnome-feasibility.md), gltests suite as package | 2–5 sessions | GPU desktop on glass from a pure `nixos-rebuild`-deployed system — **DONE** (GNOME on glass 2026-09-10) |
| 5 | Input polish + docs: xkb layout, touch/libinput quirks, device README, TWRP zip artifacts | 1 session | user-verified desktop incl. keyboard/touch |
| 6 | Optional upstreaming: `mediatek-mt6797` SoC fragment, `planet-geminipda` device, `flashingMethod = "manual"`; kernel upstream watch (mtk_drm) | ongoing | PRs merged or presented |

**Total: ~10–18 focused sessions** (dominated by phases 1 and 4 —
sizing and the graphics re-validation), not counting any regression
hunting in new-version graphics components (R2).

---

## 10. Sources / evidence

Investigation performed against these trees (both at the same Mobile
NixOS development commit):

- Mobile NixOS: `/home/cjdell/Projects/gemini-nixos/repos/mobile-nixos`
  (commit `2c132754`, branch `development`; same commit as the clone in
  `GeminiPDA/other-work/mobile-nixos`). Read: `README.adoc`,
  `doc/porting-guide.adoc`, `doc/in-depth/{devices,stage-1,
  kernel-builder}.adoc`, `doc/boot_process.adoc`, `doc/getting-started.
  adoc`, `modules/system-types/android/*`, `modules/rootfs.nix`,
  `modules/hardware-{soc,mediatek}.nix`, `modules/stage-0.nix`,
  `overlay/mobile-nixos/kernel/builder.nix`, `lib/{
  eval-with-configuration,configuration,release-tools}.nix`,
  device examples `pine64-pinephone`, `motorola-potter`, `oneplus-
  fajita`, families `sdm845-mainline`, `mainline-chromeos-mt8183`.
- Gemini PDA bring-up: `/home/cjdell/Projects/GeminiPDA` — README,
  `docs/{boot-chain,mainline-support,hardware,roadmap}.md`,
  `AGENTS.md`, `feasibility.md`; artifacts `build/out-6.6/` (boot
  images parsed byte-level), `build/rootfs-files/`, `build/wayland/`,
  `build/configs/`, `patches/`, `repos/linux-6.6` (branch
  `geminipda-bringup`), `repos/pmaports-archived/device-planet-
  geminipda/deviceinfo`, `other-work/gemini-linux/scripts/
  pack-boot-img.py`.

No prior art found for Mobile NixOS on the Gemini PDA / MT6797 (web
search 2026-09-05: no results; no in-tree or open-PR references).
