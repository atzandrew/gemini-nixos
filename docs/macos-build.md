# Building this repo's images on a Mac (no Linux machine involved)

Last updated: 2026-09-17

Status: ✅ **WORKS** — on 2026-09-17 a complete, flash-ready `boot.img`
was built **entirely from an M5 Mac** (macOS 26.6.2, 10 CPU / 32 GiB),
verified against the boot contract below. **Nothing was flashed**; this
doc is about the build path only.

Entry point: `bash bin/build.sh start bootimg` (it detects the platform
and picks this path on a Mac). `docs/building.md` is the map for humans
and agents; **this** doc is the macOS receipts, gotchas and limits. The
mac-only code lives in `bin/macos/`, and the darwin-only flake outputs in
`flake-macos.nix` — the Linux build model is untouched by both.

This is the promoted version of the 2026-09-12 probe (dry-run only,
rc 0 — receipts in `docs/session-log.md`), which established that the
pi5 `container`-VM technique transfers to this repo. The probe's
predictions were exact: `bootimg` = **17 drvs to build locally + 380
substituted**; the real build did exactly that.

## What was built (2026-09-17, receipts)

| | |
|---|---|
| Artifact | `mobile-nixos_planet-geminipda_boot.img` |
| Store path | `/nix/store/gman244d9xaikqw2gzrc2nr90fw103gp-mobile-nixos_planet-geminipda_boot.img` |
| Size | 9,988,096 B (**9.5 MiB** — fits the 16 MiB `boot` partition with ~6.5 MiB headroom) |
| sha256 | `b2404b13b8dbc861dbe9e0de43cd3bdb08f1b99ab2de6477b844256610eef997` |
| Source rev | `ad4eb9d33eb9e7dfef58bbaaa5b31f5061f0bd38-dirty` (this tree) |
| Wall clock | ≈6.5 min (380 paths substituted, 486 MiB download; 17 drvs compiled on 10 cores) |
| Host | macOS 26.6.2 (25G83), 10 CPU, 32 GiB — Apple `container` 0.12.3 + `docker.io/rzmapp/nixos-vm:26.05` (aarch64, Nix 2.34.8, kernel 6.18.15) |

Verified with `bash bin/dump-bootimg-header.sh <img>` (now macOS-clean)
and an FDT scan of the appended DTB:

```
magic:          ANDROID!
kernel:         size=8037167 (7.66 MiB)  addr=0x40200000
ramdisk:        size=1947027 (1.86 MiB)  addr=0x45000000
second:         size=0  addr=0x40f00000
tags_addr:      0x44000000
page_size:      2048
cmdline:        console=tty1 bootopt=64S3,32N2,64N2 … fbcon=rotate:3 …
total size:     9988096 bytes
```

- **`bootopt=64S3,32N2,64N2` is present in the cmdline** — the LK
  requirement (`platform_parse_bootopt`; without it the boot hangs on
  the LK logo — AGENTS.md, docs/phase-2-on-glass.md §2a).
- `fbcon=rotate:3` is present, i.e. this is the **fbcon/exclude-display
  build** the panel rule demands (rule 5) — no display-landmine driver
  was merged into it.
- All five geometry fields match the measured bring-up contract.
- The DTB is appended to the (gzip) kernel payload 25,264 B from its
  end — well inside the last-2 MiB window LK scans for the FDT magic —
  and its strings are the right tree: `planet,gemini-pda`,
  `mediatek,mt6797`, plus this repo's delta nodes (`planet,geminipda-drm`,
  `geminipda-fb`).

## How it works

Two separable things, only one of which transfers from the pi5
technique:

1. **Build environment — adopted.** Nix on macOS is `aarch64-darwin`.
   It can *evaluate* foreign-system derivations and *substitute* cached
   ones, but it can never **run** an `aarch64-linux` builder — so every
   custom drv in this repo (kernel, mesa, wlroots/gemwl, gemshell, the
   NixOS config glue) is unbuildable on the bare Mac. Apple's
   `container` runtime + a persisted aarch64 NixOS VM supplies the
   missing builder: same silicon, a native aarch64 nix, and a `/nix`
   store that survives between runs (7.1 GiB after the `bootimg` build),
   so iteration only rebuilds what changed.
2. **Image shape — does NOT transfer.** The Gemini boots through
   MediaTek LK (MTK-header `boot.img` + rsync'd rootfs on p27), with no
   FAT firmware partition and no extlinux/U-Boot, so the pi5 repo's
   `system.build.sdImage` is inapplicable. This repo already produces
   the right artifacts (`outputs.android.android-bootimg`,
   `outputs.generatedFilesystems.rootfs`).

### Scripts (rule 6 — no ad-hoc command chains)

The mac-specific half lives in **`bin/macos/`**; the cross-platform entry
point is `bin/build.sh` and the Linux half is `bin/build-linux.sh`.
`docs/building.md` is the map; this doc is the receipts.

| File | Runs on | Job |
|---|---|---|
| `bin/build.sh` | either | the dispatcher — platform detection + the one verb surface (`docs/building.md`) |
| `bin/macos/build.sh` | the Mac | stage the tree, drive the container, `start`/`wait`/`log`/`status`/`net`/`stage`/`shell`/`vm`/`stop` |
| `bin/macos/vm-build.sh` | inside the VM | the actual native build (`nix build … packages.aarch64-linux.<TARGET>`) + artifact/identity collection |
| `bin/macos/proxy.py` | the Mac | stdlib CONNECT proxy for the VM's egress (see below) |

```sh
bash bin/build.sh start bootimg     # stage + DETACHED build (rule 8)
bash bin/build.sh wait  bootimg     # rc 0 done-ok / 1 failed / 2 running (rule 8b)
bash bin/build.sh status            # container, egress, artifacts, hashes
```

By hand (below the dispatcher) is also fine, and useful for the mac-only
verbs: `bash bin/macos/build.sh net|stage|stop|shell|vm`.

### What nix-on-darwin does here (flake-macos.nix)

Darwin nix cannot build the images, but it is used for everything it *can*
do, and that part is kept in its own flake file so the Linux outputs are
untouched:

- **`devShells.aarch64-darwin.default`** — `nix develop` on a Mac lands
  here (the default devshell for the current system, exactly as
  `devShells.x86_64-linux.default` does on the workstation). It supplies
  python3 (the proxy), rsync (real rsync, not macOS's openrsync),
  cacert, adb, git and curl, so nothing depends on Homebrew or the Xcode
  CLT. Verified on the M5: python3 3.14.7, rsync 3.5.0, adb 37.0.0, all
  substituted from cache.nixos.org. `bin/macos/build.sh` re-execs into it
  automatically if the host lacks python3/rsync.
- **`packages.aarch64-darwin.caBundle`** (`cacert`) — the CA bundle the
  script mounts into the VM at `/etc/ssl/certs`, preferred over scraping
  the macOS keychain (which remains the fallback).
- These are merged into the flake's `outputs` by `mergeOutputs`, which is
  one level deep *because* a bare `//` is shallow: the first attempt
  replaced the whole `packages` attrset with the mac one and broke
  `packages.aarch64-linux` (caught by the dispatch test on 2026-09-17).

**Gotcha (mac, nix-side):** a local path/git flake evaluates the
*committed* tree, so a new file must be `git add`ed before `nix develop`
/ `nix eval` can see it (`Path 'flake-macos.nix' … is not tracked by
Git`). The build path is immune — the wrapper rsyncs the working tree
into the VM.

Artifacts + identity land in `~/.cache/gemini-macos/out/` (mounted at
`/out` in the VM): the image itself plus `<target>.paths`,
`<target>.log`, `<target>.sha256` and `<target>.manifest` (target,
attribute, host rev, egress, build time, VM/nix versions, store paths) —
rule 0: never flash something you cannot identify. The Mac-side copy is
re-hashed and matches the in-VM sha256.

The staging tree is `~/.cache/gemini-macos/src` (rsync, excluding
`.git`, `logs/` and `pkgs/gemshell/target/` — 38 MiB, mounted
**read-only** at `/build/src`); the Mac side is always the source of
truth. The container's geometry is created by the script if missing
(`--cpus 10 --memory 12g`, repo ro, macOS CA bundle over the VM's stale
certs, `out` rw) — that create path was verified with a throwaway
container (src ro + out rw + certs mounted).

## The one real gotcha: the VM's internet egress

**Symptom:** the first build died in evaluation —
`unable to download '…/mobile-nixos/…tar.gz': Timeout was reached (28)`,
i.e. the VM could not reach github.com at all.

**Cause (measured):** this Mac's default route belongs to a **Tailscale
exit node** (`netstat -rn` → `default … utun4`; `tailscale status` →
`grafton-router … active; exit node`). Apple's `container` vmnet NAT
does not survive that: the VM reaches its gateway and the Mac itself,
but every NAT'd connection to the internet times out — while the Mac's
own `curl` is fine.

```
VM → 192.168.64.1 (bridge gw)   OK      ping 0.4 ms
VM → 192.168.1.136:22 (the Mac)  OK      the Mac's sshd
VM → github.com:443              TIMEOUT (curl 28, 25 s)
VM → 1.1.1.1:443                 TIMEOUT
VM → cache.nixos.org             TIMEOUT
Mac → github.com                 200 in 0.9 s
Mac → cache.nixos.org            200 in 0.3 s
```

Restarting the container — and the whole `container system stop/start`
runtime, including `container-network-vmnet.default` — did **not** fix
it, so it is the host's routing, not stale container state.

**Fix — lend the VM the Mac's egress, do not touch the VPN.**
`bin/build.sh start` first tests the VM (`net` verb). If the VM is
online it uses the direct path; if not it starts
`bin/macos/proxy.py` (stdlib only; `python3`, from the darwin devshell
when the host has none) bound to
the container bridge gateway **only** (`192.168.64.1:3128`, clients
restricted to `192.168.64.0/24`) and runs the build with
`http_proxy`/`https_proxy`/`all_proxy` set. Nix is proxy-aware end to
end: its downloader uses those variables for substituters
(cache.nixos.org narinfo/nar), and nixpkgs' `fetchurl` declares
`impureEnvVars = proxyImpureEnvVars`, so the same variables reach the
fixed-output builders (the flake's two `fetchTree` inputs, the kernel
tarball) under nix's sandbox. Verified through the proxy in the VM:
github 200, cache.nixos.org 200, cdn.kernel.org 200.

Alternatives if you would rather not use the proxy: pause the exit node
(`tailscale set --exit-node=`) so the Mac (and therefore vmnet NAT) uses
the LAN router. The proxy is preferred here because it changes nothing
about the host's VPN state.

## What a Mac still cannot do (2026-09-12 analysis, unchanged)

Flashing/device cycles are analysed in `docs/session-log.md`
2026-09-12 — summary: **adb works** (this Mac already has it, and macOS
drives the TWRP/Google gadget `18d1:4ee2`), **reboot works** (ssh over
the LAN; only the gadget-drop *detection* is Linux-specific), the
**g.usb RNDIS `0525:a4a2` link does not** (macOS has no RNDIS driver) so
reach the device over Wi-Fi/LAN instead, and **preloader/BROM recovery
has no Mac path** (Apple's container has no USB passthrough). Host
scripts that assume `ip`/`lsusb`/`nix develop` need darwin branches —
still owed (the darwin devshell now exists, so it is the `ip`/`lsusb`
assumptions in `bin/device-ssh.sh`, `bin/boot-switch.sh`,
`bin/flash-nixos.sh` that remain).

## Cost of the other targets

From the 2026-09-12 dry-run (same flake, same VM):

| Target | drvs built locally | paths substituted | download |
|---|---|---|---|
| `kernel` | 6 | 290 | 292 MiB |
| `bootimg` | 17 | 380 | 486 MiB |
| `rootfs` | 552 | 1538 | 2.9 GiB |
| `toplevel` | 549 | 1532 | 2.9 GiB |

So `bootimg` (the firmware) is the cheap one; `rootfs`/`toplevel` compile
mesa, wlroots, the Rust workspace and the whole NixOS glue — feasible
(the M5's 10 cores versus the Pi builder's 8), but a long first run, and
the Mac's free disk (~35 GiB at the time of writing) is the thing to
watch, since the VM's disk image lives on it.

## Owed / next

- **Nothing has been flashed from the Mac** — deliberately. The image
  above is built and verified, not deployed (rule 5: a flash needs the
  TWRP/para safety cycle).
- A `rootfs`/`toplevel` build from the Mac (`bash bin/build.sh start rootfs`).
- **The mac flash workflow** (later, explicitly out of scope on
  2026-09-17): darwin branches for `bin/device-ssh.sh`,
  `bin/boot-switch.sh`, `bin/flash-nixos.sh` so the Mac can drive the
  adb + LAN-ssh half, with the adb/reboot mechanism from the 2026-09-12
  analysis. `bin/macos/` is the natural home for the mac-only pieces;
  the darwin devshell already carries adb.
