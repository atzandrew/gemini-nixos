# AGENTS.md — gemini-nixos: the GOLDEN repo for the Gemini PDA (Mobile NixOS + bring-up knowledge)

**Repo status: GOLDEN (declared 2026-09-07).** This repo is the primary,
self-contained home for the Gemini PDA (MT6797X) project going forward —
both the Mobile NixOS port and the hardware/boot knowledge base that
used to live in the sibling project `/home/cjdell/Projects/GeminiPDA`.
The intent is that gemini-nixos **completely replaces GeminiPDA**.
GeminiPDA is NOT reverted, deleted or edited-for-new-work; it is the
legacy source that is being folded into this repo (knowledge, receipts,
tooling, artifacts), after which it can be archived.

**Transitional rule (until the migration below completes):** a topic's
source of truth is wherever its latest content is. New knowledge is
written HERE first, with dates and receipts. Where this repo does not
yet carry a GeminiPDA doc's content, GeminiPDA remains the reference
for THAT topic — cite it with its path and port it here per the
migration plan so the pointer decays. Do not put new work in GeminiPDA;
mirroring a correction there is optional (legacy).

Read this file first, then `README.md`,
`docs/mobile-nixos-port-feasibility.md` and `docs/disaster-recovery/`.

## Migration status (GeminiPDA → gemini-nixos)

| # | Item | Status (2026-09-07) | Destination here |
|---|---|---|---|
| M1 | Hardware/boot receipt docs (boot-chain.md, hardware.md, flashing.md, wifi-consys.md, roadmap.md) | ⬜ planned — `docs/boot-process.md` + repartition doc are the in-repo seeds; carry `file:line` receipts + dates when porting | `docs/` |
| M2 | **Disaster-recovery knowledge + image ledger** | ✅ done 2026-09-07 | `docs/disaster-recovery/` (README · inventory · gather · drills) |
| M3 | stock-dump blobs (partition dumps, firmware zip, boot backups) | 🟡 partial — boot-critical set copied 2026-09-07; full bulk pending (see `docs/disaster-recovery/inventory.md` "pending copy") | `stock-dump/` (gitignored) |
| M4 | Recovery tooling (patched-mtkclient launcher, USB watcher, devshell pkg) | ✅ done 2026-09-07 | `bin/run-mtk.sh`, `bin/usb-watch.sh`; devshell `mtkclient` |
| M5 | Kernel + LK source trees (linux-6.6, gemini-lk, mesa fork work) | ✅ kernel done 2026-09-08 (published v6.6 base + tracked delta in `devices/planet-geminipda/kernel/`; source no longer lives in GeminiPDA); gemini-lk + mesa fork still legacy until vendored | `devices/planet-geminipda/kernel/`, `kernel/base` (submodule ptr) |
| M6 | Device services/rootfs files | ✅ (already ported as derivations/scripts — the port repo's original job) | `services/`, `pkgs/`, `bin/` |
| M7 | Session history | ⬜ new sessions log HERE only; old history stays in GeminiPDA | `docs/session-log.md` |

## Status (device, unchanged by the pivot)

Phases 0/1/3 done at the **build level** — the flake's `boot.img` +
`rootfs.img` build green and match the bring-up boot contract — and
**phase 2 is now on glass (2026-09-07 milestone: NixOS boots, sshd over
 g_ether from p32)**; the rootfs `growfs`, several services and the
`nixos-rebuild` round-trip were the remaining on-glass work (TODO:
`docs/phase-2-on-glass.md`). **2026-09-08: the round-trip is closed
BOTH ways** — host `bin/deploy.sh` (gens ≤ 28) and device-native
build/switch (`bin/device-rebuild.sh`; gen30 was built + switched
entirely on the PDA from a repo clone at `/root/gemini-nixos`;
`nix-shell -p` works, pinned to the flake's nixpkgs rev — see README
"On-device build/switch"). **2026-09-09: the flake exposes
`nixosConfigurations.gemini` (single eval shared with the toplevel
package), so the STOCK loop works from the device clone too —
`cd /root/gemini-nixos && nixos-rebuild switch --flake .` (the Python
nixos-rebuild-ng; in the device closure by default; nixpkgs dropped
the bash nixos-rebuild at this pin).** **2026-09-10: the unit was
repartitioned one-way to TWRP + NixOS only** — the old Android + Debian
+ p32 `userdata` layout is gone; the NixOS rootfs now lives on ONE
**58.0 GiB p27 `linux`** partition (ext4 label `NIXOS_SYSTEM`), with p1
`recovery` (TWRP) the only other bootable partition
(`bin/repartition-nixos.sh`; `docs/repartition-android-space.md` §12).
**2026-09-11: the custom Rust desktop (`gemshell`) is built (aarch64
build green, `js6rq8h7v1xfg61q1fhyzg4sadqixshm-gemshell-0.1.0`) and
wired in as a fifth desktop mode (`gemcli session set gemshell`);
on-glass bring-up owed — `docs/gemshell.md` + the Where-things-live
gemshell row.**
**2026-09-12: gemshell reworked — the separate `gemsettings` wl_shm
client was REMOVED and replaced by an in-process, GPU-tessellated
egui settings panel (`src/shell.rs` + `render.rs::draw_egui`); the
hand-rolled font atlas blank-text bug and a `build_program`
vertex-shader mix-up were fixed; the compute present() left-right
mirror was fixed (scene-Y flip on the texelFetch); and all system
data now goes through the new `gemdata::DataProvider` trait
(`crates/gemdata{, -device,-dummy}`), which also now owns every
gemcli device function — `gemcli` is a thin frontend over it (ONE
implementation). Builds green (x86_64 checked + built; aarch64
on-device build/glass owed).**

**2026-09-17 (c): the USB gadget NIC moved from RNDIS to CDC-ECM**, so
macOS — which ships no RNDIS driver at all — can use the same
`10.15.19.82` link as the Linux workstation, which is what unblocks the
**full Mac reboot/flash cycle over the USB cable**. `CONFIG_USB_ETH_RNDIS`
is off (`bin/prune-kernel-config.sh` §9c), so the precomposed `g_ether`
gadget registers ONE configuration — CDC-ECM, `0525:a4a1` — instead of
RNDIS-first-`0525:a4a2` (receipt: `drivers/usb/gadget/legacy/ether.c`,
"register our configuration(s); RNDIS first, if it's used"). Linux
(`cdc_ether`) and macOS (`com.apple.driver.usb.cdc.ecm`, whose
personalities match exactly the descriptors `f_ecm` emits) both bind it
natively. The host scripts gained a darwin branch through the new
**`bin/lib/host.sh`** (MAC-based interface discovery, `ifconfig` vs `ip`,
platform ping, `lsusb` ⇄ `system_profiler`, devshell re-exec) —
`net-up`/`device-ssh`/`device-reboot`/`boot-switch`/`flash-nixos`/
`usb-watch` no longer need `lsusb`, and `flash-nixos`/`boot-switch`
re-exec into the darwin devshell for `adb` + GNU coreutils. Only
preloader/BROM recovery stays Linux-only (no USB passthrough into Apple's
container). **Kernel config + host scripts are built-but-not-flashed
(boot.img reflash owed).** Receipts + verification + rollback:
**`docs/usb-network.md`**.

**2026-09-26 (d): gemshell Wayland-app TOUCH FIXED on glass** — the
compositor's `local_coords` omitted `wl_surface.set_buffer_scale`, so at
UI scale 1.5 (`wl_output.scale = 2`) input was delivered in 2× buffer
pixels and every app ignored touch; now tracked and applied (verified at
150% with the fixed binary run transiently). **Residual:** a slight
tap-offset from the ignored `xdg_surface.set_window_geometry` (CSD shadow
margin) — logged for a future session, not fixed. **Open app-hosting
gaps:** no session D-Bus for clients, client EGL `dri2 screen` failures.
Receipts: `docs/gemshell.md` §"2026-09-26" + `docs/session-log.md`
2026-09-26 (d).

**2026-09-26 (e): gemshell FREEZE fixed — the compositor never sent
`wl_buffer.release`.** GTK4/wl_shm clients could not recycle buffers and
allocated a fresh shm pool every frame: one `gnome-calculator` reached
**920 MB RSS in ~27 s** (verified on glass), so a few apps exhausted RAM
and the no-swap reclaim thrash wedged userspace (kernel still pinged).
Now the attached `wl_buffer` is kept on the surface and released from
`surface_commit` right after the pixels are copied into a GL texture;
the same calculator plateaus at ~107 MB. Verified on glass with
`wasa7dasvxm15kihi2xv8m2nznjglk8x-gemshell-0.1.0` run transiently
(user-confirmed under sustained multi-app use: 7 clients, `Shmem:` ~200 MB,
gemshell ~175 MB, all flat). **Deployed as generation 55** via the new
Mac path `bin/macos/deploy.sh` (container `toplevel` build + `nix copy`
from the VM + profile switch), so it survives reboots (reboot-verified).
Receipts: `docs/gemshell.md` §"2026-09-26 (e)" +
`docs/session-log.md` 2026-09-26 (e).

⚠️ **Boot.img cmdline field: KEEP `bootopt=64S3,32N2,64N2` in it** — LK
consumes it via `platform_parse_bootopt`; without it the boot hangs on
the LK logo (~15 s WDT loop) before any kernel output (discovered
2026-09-07; `config/gemini.nix` kernelParams + `docs/phase-2-on-glass.md`
§2a).

## Golden rules

0. **Version & commit hygiene.** Every artifact that goes to the device
   carries an identity, and every session records what it flashed:
   - the kernel is **built in-repo** (since 2026-09-08: published
     Linux v6.6 base + tracked delta + lean config — see the session
     log; the #329 borrow was retired and `kernel/borrowed/` is only
     an unflashed reference until glass verification);
   - Mesa 26.2.2 (nixpkgs thin override + T880 delta), wlroots 0.18.2,
     gemwl: exact pins in
     `flake.nix`/the pkgs derivations.
   Log one version line per flash in `docs/session-log.md` (kernel,
   boot.img hash, mesa/wlroots pins). Never flash a build you cannot
   identify. [added 2026-09-07, adapted from GeminiPDA rule 0]
1. **Record before you forget.** Any fact learned from source, a build,
   an eval error, or (eventually) a boot attempt belongs in a doc — with
   a date. This repo's log is `docs/session-log.md`; port/build facts go
   to `README.md` / `docs/mobile-nixos-port-feasibility.md`;
   *hardware/boot* facts go HERE (this repo is golden) — into
   `docs/boot-process.md` and the receipt docs being ported (M1); when
   you must cite a receipt not yet ported, quote it with its original
   `file:line` + date + source path so it survives the port.
2. **Never silently contradict a doc.** Fix it *and* note the correction
   (`[corrected YYYY-MM-DD]`). If unsure, add rather than replace, and
   mark it `unverified`. Claims about *upstream* state ("is in
   mainline") must carry the date they were verified.
3. **Prefer evidence over opinion.** Keep receipts: `file:line` for LK
   behaviour (carry the source path + commit when citing legacy docs),
   derivation/commit ids for nix behaviour (ours — nixpkgs floats, so
   pin + date every claim about it).
4. **Small, dated deltas beat big rewrites.** Append to the tables and
   logs; don't restructure docs casually.
5. **NEVER abuse the LCD panel/matrix (CORE RULE).** The unit's NT36672
   TDDI is initialized ONLY by LK; a badly-initialized panel shows the
   "uninitialised" flicker and can BURN the LCD matrix (permanent damage
   — observed 2026-09-04). For THIS repo:
   - The kernel must stay the **fbcon/exclude-display build** — the
     borrowed #329 payload is exactly that. When the in-repo kernel
     derivation (`devices/planet-geminipda/kernel/`) is finally used,
     build with the fbcon verb (EXCLUDE_DISPLAY=1 FBCON=1) and never
     merge the display-landmine drivers (mediatek-drm*, mtk-mmsys,
     phy-mtk-*, tps65132-regulator) into the image.
   - If the glass EVER shows the flicker (or a kernel/display change
     touches the stack), stop, get the device to TWRP (LK/TWRP drive the
     panel correctly) and fix the image before continuing.
   - The fb buffer is NOT the panel state: a valid framebuffer can sit
     behind a badly-initialized LCD. Judge the glass by eyes/TWRP, not
     by a capture. (Full story: legacy GeminiPDA session-log 2026-09-04;
     port pending M7.)
6. **Scripts over long ad-hoc commands (CORE RULE).** Any multi-step
   host/device operation needed more than once becomes a committed
   script in `bin/` the FIRST time (headers: what/why/usage; reuse
   `bin/device-ssh.sh` / the devshell helpers; fail loudly on missing
   tools). Long inline `ssh '...'` chains and host for-loops are
   throwaway EXPLORATION only — promote them before the session ends.
   The host side is **Linux + macOS** since 2026-09-17: never hardcode
   `enp*`/`ip`/`lsusb` in a new script — source **`bin/lib/host.sh`**
   (platform detection, MAC-based USB-NIC discovery, net-up, ping,
   `lsusb` ⇄ `system_profiler`, devshell re-exec); receipts in
   `docs/usb-network.md`.
7. **Devshell-only CLIs — NEVER bare (CORE RULE).** The bare host PATH
   carries only git/nix/coreutils/ssh/usbutils-ish basics — verified
   NO python3/adb/make there (2026-09-07). Every project CLI (python3,
   adb, fastboot, mtkclient, …) lives ONLY in the flake devshell
   (`flake.nix` → `devShells.x86_64-linux.default`: python3, git,
   android-tools, usbutils, mtkclient). Reach them with
   `nix develop --command <tool> …` **from the repo root** (nix does NOT
   search upward for flake.nix). The device scripts self-re-exec inside
   the devshell when adb/lsusb are missing from the host PATH
   (`GEMINI_DEVSH_REEXEC` guards the loop). Never a bare `python3`/`adb`
   and never hard-code /nix/store paths (drift after GC).
8. **Long operations: `bash bin/run-job.sh start|wait`, NEVER inline
   nohup/pgrep loops (CORE RULE).** Anything >~30 s or that must keep
   working across tool calls: `bash bin/run-job.sh start NAME -- CMD`
   (returns immediately; setsid-detached) then
   `bash bin/run-job.sh wait NAME` (rc 0 ok / 1 failed / 2 still
   running → re-run wait). The old `nohup … & for … pgrep -f …` pattern
   stalls sessions: `pgrep -f` matches the polling shell's own cmdline →
   the loop spins until the tool timeout. Job state: `logs/jobs/<name>/`
   (gitignored); `wait-file LOG END_REGEX` salvages already-running ops.
   Long flash ops (rootfs push+dd) MUST run under run-job.
8b. **Poll, never long-sleep (agent rule).** While a job/build runs,
   poll with `bash bin/run-job.sh wait NAME` (it returns immediately with
   rc=2 while running) — do NOT `sleep 60`/`sleep 90` between checks
   (wasted wall-clock + stalled tool calls). If a tool needs a short
   settle between polls, cap sleeps at ~5–10 s. The `run-job.sh wait`
   rc protocol is the poll primitive: rc=0 done-ok, rc=1 done-failed,
   rc=2 still-running → re-run wait.
9. **Cache-healthy nixpkgs pins (CORE RULE, added 2026-09-08).** Every
   nixpkgs pin must be one hydra built the FULL closure for — always the
   nixos-unstable (or a release) CHANNEL snapshot, never a raw
   master/random commit: channel revs are only cut after hydra's
   complete build, so the big closures (Qt6/LXQt — the multi-hour
   compiles) substitute from cache.nixos.org. Raw commits get per-commit
   trunk-combined (base) coverage only — verified 2026-09-08: the old
   npins rev `0bb7ec54c848` qtbase 404'd for x86_64 AND aarch64 (Qt6/
   LXQt compiled every time); the channel rev `dc5d91f84032`
   (26.11pre1068949) has qtbase/qtwayland/lxqt-*/systemd/pipewire
   narinfos all 200. Pin the rev behind
   `https://channels.nixos.org/nixos-unstable/git-revision` (or the
   release-branch equivalent) + narHash from `nix flake prefetch
   github:NixOS/nixpkgs/<rev>`. VERIFY before committing with
   `nix path-info --store https://cache.nixos.org <outPath>` on the
   heavy packages eval'd at the candidate rev (qtbase first) — curl
   narinfo 404s are NOT evidence (proxy); use nix's own HTTP client.
   Also: don't let workaround overlays diverge from the channel
   defaults — each override forces non-cached drv hashes down its
   subtree (the systemd/ffmpeg/openblas/libfm overrides pruned with the
   2026-09-08 repin cost 137+ builds; receipts in config/gemini.nix +
   docs/session-log.md).
10. **Builds go through `bash bin/build.sh` — never a hand-rolled `nix
   build` for this repo's aarch64 outputs (CORE RULE, added 2026-09-17).**
   It detects the platform and runs the model that works there:
   - **Linux** → `bin/build-linux.sh`: the native aarch64 model, unchanged
     (`sudo nix build --store local … --option builders @/etc/nix/machines
     --fallback`, Pi compiles + cache substitutes); `toplevel` routes to
     `bin/deploy.sh build` because that also pins the GC roots.
   - **macOS** → `bin/macos/build.sh`: the same drvs built inside an Apple
     `container` aarch64 NixOS VM. Nix-on-darwin can *evaluate* and
     *substitute* aarch64-linux drvs but can never *run* their builders,
     so `nix build .#packages.aarch64-linux.<x>` on a Mac substitutes what
     it can and then fails on the custom drvs — never do it. Nix-on-darwin
     IS used for the mac-native parts (the darwin devshell in
     `flake-macos.nix`, the CA bundle the VM mounts).
   Same verbs both sides: `start [TARGET]` (detached, rule 8) → poll
   `wait [TARGET]` (rc 0 done-ok / 1 failed / 2 still running / 3 no job —
   rule 8b) → `log`/`status`; `shell` = the platform devshell (`nix
   develop`); `vm` = a shell in the mac build VM. Mac-only code lives in
   `bin/macos/`; darwin-only flake outputs in `flake-macos.nix` (merged by
   `mergeOutputs` — `//` is shallow); the map, target list and artifact
   locations are **`docs/building.md`**. Flashing is NOT part of this yet.

## Where things live

| Concern | File |
|---|---|
| Repo purpose, layout, boot chain, current status | `README.md` |
| Build model + commands, kernel source model, manual flash flow, on-device rebuild, pins maintenance | `AGENTS.md` (this file, below) |
| Feasibility study + the phased plan (phase table = roadmap) | `docs/mobile-nixos-port-feasibility.md` |
| Plain-language boot explainer (receipt pointers) | `docs/boot-process.md` |
| **Phase-2 on-glass knowledge + TODO** (2026-09-07 milestone: bootopt discovery, recovery receipts, not-quite-working list) | `docs/phase-2-on-glass.md` |
| **NixOS rootfs layout** — TWRP + NixOS only since 2026-09-10 (single 58 GiB p27 `linux`; supersedes the 2026-09-07 dual-boot plan §9/§10) | `docs/repartition-android-space.md` (§12) |
| "Published base + in-repo delta" pattern (mesa done; kernel next) | `docs/library-deltas.md` |
| **What was actually tried / happened** (dated entries; golden log) | `docs/session-log.md` |
| **Long-standing goals** (persistent, not one session's work) — **G1: stop chronic battery depletion; a real deep sleep + a real, measured poweroff** | `docs/standing-goals.md` |
| **Disaster recovery** — full-flash-erase → TWRP playbook (levels 0–2), image ledger + sha256, gather checklist, drills | `docs/disaster-recovery/` (README · inventory · gather · drills) |
| Flake entry; Mobile NixOS pin (`2c132754`); **nixpkgs pinned in-flake** (`26.11pre1068949` — see README Versions + this file's "Pins (maintenance)"); devShell (host x86_64, MNX npins) | `flake.nix` |
| Out-of-tree device definition (boot.img geometry, borrowed kernel, minimal initrd wiring) | `devices/planet-geminipda/` |
| Stage-2 system config (headless + USB-NIC (CDC-ECM) SSH, services, mesa fork, systemd-BPF/cudaLLVM overlays) | `config/gemini.nix` |
| SoC fragment (out-of-tree MT6797) | `modules/hardware-soc-mediatek-mt6797.nix` |
| Device services: GPU poweron / A72-up / battery-guard / WDT reboot | `services/gemini-pda.nix`, `services/scripts/` |
| **Silver-button clamshell sleep/wake** — light sleep (backlight/A53 cpus/inputs/services off, **A72 cluster down when up** [added 2026-09-10q]) + KEY_SLEEP daemon; deep-sleep (s2idle) follow-up. **systemd `suspend` is DISABLED (2026-09-11, deployed gen 22):** it is a different mechanism (kernel s2idle, no wake source) and locks the system up; `systemd.suppressedSystemUnits` removes the sleep targets/services and GNOME's power dconf keys are locked to `nothing`, so `gemcli sleep`/`gemini-sleepd` is the ONLY sleep path | `services/gemini-pda.nix` (`gemini-sleepd`) + `pkgs/gemshell/crates/gemdata-device/src/sleep.rs` + **`docs/power-sleep.md`** (see the `systemctl suspend` vs `gemcli sleep` section) |
| **Power modes — GNOME "Power Mode" → the A72 cluster (2026-09-10q, ON GLASS)** — PPD's placeholder driver is patched to advertise `performance` (GNOME hides it otherwise); `gemini-power-profile` (`gemcli profile watch`) maps performance ⇒ `a72 up`, balanced/power-saver ⇒ `a72 down`; 20 s per-boot settle (a change applies immediately); sleep-aware; A72 ops flock-serialized | **`docs/power-modes.md`** + `services/power-profiles.nix` + `patches/power-profiles-daemon-placeholder-performance.patch` + `pkgs/gemshell/crates/gemdata-device/src/profile.rs` |
| **Speakers vs headphones / L-R swap (2026-09-10q, ON GLASS)** — internal speakers are wired L↔R; a filter-chain virtual sink `gemini_speakers` crosses the pair; `gemini-speakerd` (`gemcli speaker watch`) drives amp pads 243/244 to match the PipeWire default sink, so the GNOME Sound/Quick-Settings output picker is the amp toggle; `audio-output` also sets the default sink. Pad state is read side-effect-free via pinctrl /dev/mem. **[fixed 2026-09-11, on glass gen 17]:** the watcher's `wpctl` parse never stripped the default node's `* ` marker → `default_sink()` was always None, so selecting Headphones left the amp ON; the parser is fixed, `/etc/gemini/audio-output-mode` now persists (tmpfiles + writer `mkdir -p`), and `gemini-speakerd` rewrites it from the observed sink so GNOME-only choices survive reboot | **`docs/desktop-plumbing.md` §Speakers** + `services/pipewire/60-gemini-speakers.conf` + `services/audio.nix` + `pkgs/gemshell/crates/gemdata-device/src/speaker.rs` |
| **Power states — reboot/poweroff FIXED (research 2026-09-10, on glass 2026-09-10)**: `systemctl reboot` limboed (arm64 `machine_restart` no-handler halt) and poweroff was refused; now the delta driver `drivers/power/reset/mt6797-power.c` registers LK's full WDT SWRST restart (priority 200) + the MT6351 `RTC_BBPU`=0x4309 poweroff over the pwrap regmap. Fork rev `06fd13e11`, boot.img `2fbca314…`; both drivers bind and the transitions verified. **Gotcha:** the shared TOPRGU block at `0x10007000` must be mapped with `devm_ioremap()`, not `devm_platform_ioremap_resource()` (the latter claims it and makes `mtk_wdt` fail `-EBUSY` → watchdog boot loop) | **`docs/power-states.md`** |
| Audio (PipeWire S16 path), Wi-Fi (CONSYS+USB), **GNOME (vanilla, the DEFAULT desktop since 2026-09-10, GPU-accelerated via KMS+kmsro → panfrost)**, legacy nested `gemwl` (no session client since the nested desktops were removed) | `services/gnome.nix` (default), `services/desktop.nix`, `services/gemshell.nix`, `services/audio.nix`, `services/wifi.nix` |
| **Desktop plumbing — DE-agnostic system services (2026-09-10)**: battery/AC via **UPower** (kernel Battery supply `bq25890-battery-N` — no fuel gauge, capacity voltage-derived; upower policy Ignore because gemini-battery-guard owns poweroff), Wi-Fi via **NetworkManager** (`services.geminiWifi.useNetworkManager`, default true; wlan0 CONSYS + wlan1 dongle, usb0 unmanaged, home networks as NM profiles; **2026-09-11 smartphone-like autoconnect: `gemini-wifi-smart`/`services/scripts/wifi-smart` + `autoconnect-retries=0` prefers the strongest known network and never gives up**), backlight **udev chmod 0666** + `brightnessctl` (no logind session for the system-service desktops). Bluetooth already standard (bluez). **2026-09-10p: GNOME audio + Fn media keys wired** — the GNOME session redirects `PULSE_SERVER`/`PIPEWIRE_RUNTIME_DIR` to the one system PipeWire session (`/run/gemwl-audio`; Settings→Sound + gsd volume keys), and the Fn layer is bound by **xkb keycode** in mutter (`<Mod5>0x36/0x37/0x38/0x39/0x1c` — a *keysym* accelerator resolves at level 0 and can never match Fn; **2026-09-11 added `/0x18/0x19/0x1a` = Fn+Q play-pause / Fn+W prev / Fn+E next, verified on glass**; the gsd `-static` overrides ride `NIX_GSETTINGS_OVERRIDES_DIR`, a **login-time** session variable — after a deploy the RUNNING session keeps the OLD overrides until re-login, so restarting `gsd-media-keys` is not enough; live fix: `systemctl --user set-environment NIX_GSETTINGS_OVERRIDES_DIR=<new /etc/set-environment value>` + `systemctl --user restart org.gnome.SettingsDaemon.MediaKeys.target`; the service is `RefuseManualStart` so restart the **.target**). **Regional settings (2026-09-11): `time.timeZone=Europe/London` + `i18n.defaultLocale=en_GB.UTF-8` + all `LC_*` GB; GNOME `org.gnome.system.locale` locked to `en_GB.UTF-8` at the **`system/locale`** dconf path, not the schema id** (config/gemini.nix, services/gnome.nix). **gen62 deployed + NM/upower/backlight verified on glass 2026-09-10** (battery icon still needs the new boot.img). **Touch (2026-09-10; gemshell confirmed 2026-09-12): a real 10-point multitouch `wl_touch`** — gemwl forwards every finger (`wlr_seat_touch_notify_*`, `GEMWL_TOUCH_POINTER_EMU=1` restores the old pointer emulation); the nested compositor's wlroots wayland backend re-emitted it, and the native `gemshell` compositor now takes touch directly (the `ABS_MT_SLOT` fix, 2026-09-12). GNOME uses the `geminipda-drm` KMS path with the DT touch transform | **`docs/desktop-plumbing.md`** (§Touch), `services/plumbing.nix`, `services/wifi.nix`, `pkgs/gemwl/gemwl.c` |
| **Touch verification tools** (2026-09-10): `bin/touch-inject.c` (writes raw Protocol-B events into the NT36772 evdev node — tap/swipe/pinch; **aarch64 `input_event` is 24 bytes, not 8**) + `bin/touch-probe.c` (WIP wl_touch client; blocked on the wayland ≥1.23 unified-`wl_interface` header change) | `bin/touch-inject.c`, `bin/touch-probe.c` (gotchas: `docs/session-log.md` 2026-09-10c) |
| **Keyboard/keybinding verification tool** (2026-09-10p): `bin/kb-inject.c` — writes raw EV_KEY chords (`fn+c`, `fn+v`, `fn+b`, `fn+n`, …) to the keyboard's evdev node, so the Fn media-key bindings can be exercised over ssh (v6.6 `evdev_write` DOES reach libinput/mutter) | `bin/kb-inject.c` (+ `docs/desktop-plumbing.md` §Volume) |
| **Keyboard verification tool** (2026-09-10m): `bin/wl-keymap-dump.c` — binds wl_seat→wl_keyboard and prints the xkb keymap the compositor actually ships clients (proved GNOME was sending plain US despite correct gsettings/env; `--grep` for one key). Build: `gcc $(pkg-config --cflags --libs wayland-client) bin/wl-keymap-dump.c` | `bin/wl-keymap-dump.c` |
| **Bluetooth bring-up + tools** (MT6630 CONSYS BT half: hci_stp port of the vendor 3.18 drv_bt → 6.6, WAK-line wake fix, rx-stall root cause; **persistent service + GUI/CLI on glass 2026-09-09m** — bluetoothd on the system bus auto-powered at boot, bluetoothctl/btmgmt CLIs, blueman-manager + SNI tray applet) | **`docs/bluetooth-bringup.md`** (+ `docs/session-log.md` 2026-09-09k/l/m; driver in the kernel delta `drv_bt/`; service module `services/bluetooth.nix`) |
| **A2DP audio — stutter + after-playback crackle FIXED (2026-09-10r)** — stutter = the **STP PSM**: its deep-idle backend is an unimplemented stub (`NULL function pointer`, −1), so it saved no power but gated STP TX past `MTKSTP_TX_TIMEOUT` (180 ms) → `0x7f` resync → BTIF desync ~every 1.5 s (measured: 12 resyncs + 41 TX timeouts / 20 s idle; PSM off = 0/0 and 1×577 B/80 ms ≈57 kbps → full ~265 kbps). Delta `wmt_lib.c` forces `gPsEnable=0` + `wmt_lib_ps_ctrl()` always-disables (runtime: `/proc/driver/wmt_dbg` `0 0`). Wi-Fi unaffected. The **crackle also needs the sink to idle** (`node.pause-on-idle=false` + GNOME volume-control monitor stream) — follow-up pending | **`docs/bluetooth-a2dp.md`** (receipts + measurement table); code `common_main/core/wmt_lib.c` (+ `psm_core.c`, `stp_core.c`) |
| Keyboard layouts — VT console map + desktop xkb: console `console.keyMap` (config/gemini.nix); desktop layout "gemini" packaged as an xkbcommon include dir for the gemwl/gemshell units via `XKB_CONFIG_EXTRA_PATH` (+ `XKB_DEFAULT_LAYOUT=gemini`). **GNOME needs more** (2026-09-10m): gnome-shell validates the input source against the **xkb registry**, so it also needs `pkgs/gemini-xkeyboard-config.nix` (xkeyboard-config copy + `symbols/gemini` + a `<layout>` entry in `rules/evdev.xml`) selected via `XKB_CONFIG_ROOT` — see the GNOME row | `config/keymaps/gemini-uk.map`, `config/xkb/symbols/gemini`, `pkgs/gemini-xkb.nix`, `pkgs/gemini-xkeyboard-config.nix` |
| **Rust device-control CLI** (backlight/battery/guard/a72/wdt/boot/gpu/speaker; script-parity; units NOT flipped until the on-glass `selfcheck` pass). **2026-09-12: the device functions moved to `pkgs/gemshell/crates/gemdata-device/`; `gemcli` is a thin clap frontend there and gemshell uses the same functions — one implementation** | `pkgs/gemcli.nix` + `pkgs/gemshell/crates/gemcli/` + `pkgs/gemshell/crates/gemdata-device/`; migration plan + parity recipe: `docs/gemcli.md` |
| **Windows PEs on the PDA** (wine-wow64 11.0 + box64; wow64 stack deployed to `/var/lib/wine-x86` so the desktop user can run it; system `wine`/`wine64` wrappers; per-user prefix `$HOME/.wine-x86`) | `bin/wine-x86-deploy.sh`, `pkgs/wine-{cli,x86}.nix`, `docs/wine-d3d.md` |
| **DOSBox-X with the Gemini keyboard fix** (2026-09-11; deployed gen 20): DOSBox-X's SDL2 input is scancode/position based and ignores the host XKB layout, and Fn is level-3 only (no scancode) → wrong UK symbols and no Fn layer. Wrapper forces `[dos] keyboardlayout=uk`, drops `key_ralt` (a held guest Alt made the guest layout skip normal/shift planes and killed the symbols) and seeds a complete mapper binding Fn+key → the level-3 symbol, with guest-Shift synthesis (`:` = Fn+O or Fn+`'`; `\` = Fn+3; `|`=Fn+1; `@`=Fn+K), and F1..F10 on Shift+Fn+number (level 4). **Mapper section must be `[SDL2]`, not `[sdl]`** (`SDL_STRING`). Overlay verified against the real binary on x86_64; interactive on-device click-through owed | `pkgs/dosbox-x-gemini.{nix,sh}`, `config/dosbox-x/mapper-dosbox-x.map`, `bin/gen-dosbox-x-mapper.sh`, **`docs/desktop-plumbing.md` §DOSBox-X** |
| Mesa 26.2.2 (nixpkgs thin override + the T880 polygon-list delta; the 25.0.7 fork retired 2026-09-10) / wlroots 0.18.2 pin / gemwl pkgs (**legacy**) | `pkgs/{mesa-geminipda,wlroots-geminipda,gemwl}.nix` + `patches/` |
| **Phosh desktop bring-up design + receipts** (removed 2026-09-12 — nested Phosh/LXQt deleted; docs/phosh.md deleted, receipts survive in git history + `docs/session-log.md` 2026-09-09o) | `docs/session-log.md` |
| **Vanilla GNOME — now the DEFAULT desktop, ON GLASS 2026-09-10, GPU-accelerated** (was only a feasibility study earlier that day): GNOME 50 `mutter` is KMS-only (X11 + nested backends removed), so `gnome-shell` cannot run on the LK framebuffer — not a Phosh-style protocol gap. Solved by exposing the LK fb as a KMS device + Mesa kmsro. App suite + session + receipts | **`docs/gnome-feasibility.md`** + `services/gnome.nix` + `services/gnome-apps.nix` |
| **`geminipda-drm` — LK framebuffer as DRM/KMS** (2026-09-10): delta `drivers/gpu/drm/tiny/geminipda-drm.c` (simpledrm-modelled) → `/dev/dri/card0`, DSI connector + panel-orientation 90, shadow-plane blit + alpha `0xff` fix; config emitted by `bin/prune-kernel-config.sh` (`CONFIG_DRM_GEMINIPDA=m`); rule-5 gate still passes. **Flashed + verified on glass 2026-09-10** (boot.img sha256 `1d2f350a…`) | `devices/planet-geminipda/kernel/delta/drivers/gpu/drm/tiny/` + `kernel/config` |
| **Vanilla GNOME desktop** (2026-09-10, **default ON**): standard NixOS `services.desktopManager.gnome` + `displayManager.gdm` on the KMS device; force-disables the legacy `gemwl` compositor, keeps custom PipeWire, loads panfrost after GPU power-on. Rendering is **GPU-accelerated** via Mesa `kmsro` (already in the mesa fork: pairs the display-only `geminipda-drm` card with panfrost). On glass 2026-09-10: mutter primary = card0, unattended boot, 0 failed units. **Sluggishness FIXED 2026-09-10k** (the ~99 % kworker was `geminipda-drm`'s redundant per-pixel alpha loop in the DRM commit worker — deleted; gemdemo 6→74 fps). **Keyboard FIXED 2026-09-10m**: gnome-shell's `XkbInfo` (libxkbregistry) needs gemini registered in `rules/evdev.xml`, not just an include dir — hence `pkgs/gemini-xkeyboard-config.nix` + `XKB_CONFIG_ROOT` + a locked dconf source; the keymap mutter ships now has `AE01=[1,!,|,F1]`, `AE03=[3,£,\,F3]`, `RALT=ISO_Level3_Shift`. **Touch**: kernel now reports raw portrait, sensor is 180° to the panel (DT inverted-x+y) — **confirmed on glass 2026-09-10m** | `services/gnome.nix` (`services.gnomeDesktop.enable`, set true in `config/gemini.nix`) |
| **Desktop/session selector — GNOME + gemshell + console** (2026-09-10; +gemshell 2026-09-11; COSMIC/niri removed 2026-09-12): GNOME is a GDM Wayland *session*, `gemshell` is the native Rust compositor as a system service (same sentinel mechanism as console — GDM skipped, no tty1 getty), `console` stays on fbcon tty1; a persistent marker `/var/lib/gemini/desktop` resolved before GDM picks the owner. `gemcli session set gnome\|gemshell\|console [--apply\|--reboot]`; `--reboot` = clean switch. `displayManager.defaultSession` is deliberately unset/`mkForce null` (the selector owns AccountsService) | **`docs/desktop-selection.md`** + `services/desktop-select.nix` + `services/scripts/gemini-desktop-apply` + `pkgs/gemshell/crates/gemdata-device/src/session.rs` |
| **gemshell — the custom Rust Wayland compositor + desktop** (2026-09-11 ON GLASS; **2026-09-12: egui settings panel + `gemdata` abstraction, deployed gens 39–42; on-glass confirmed: rotation 270 correct + TOUCH WORKS** — the dead-touch cause was `ABS_MT_SLOT` defined as 57 (= `ABS_MT_TRACKING_ID`) so every down was swallowed as a slot event; also fixed the Protocol-B emit-at-SYN order, the missing `O_NONBLOCK`, and the present() mirror; `GEMSHELL_TOUCH_TRAIL=1` is a finger-drawing touch-test mode): single-process compositor (wayland-server xdg-shell/wl_shm + EGL/GBM on the panfrost render node, GPU-direct present into the LK fb via /dev/gemfb) + an **in-process egui settings panel** (Wi-Fi/BT/Audio; GPU-tessellated meshes, real font shaping — the old `gemsettings` wl_shm client is gone); evdev keyboard (xkb, `gemini` layout) + 10-finger touch, shell UI (status bar / launcher / taskbar / gestures). All system data goes through **`gemdata::DataProvider`** (`crates/gemdata`; `gemdata-device` real, `gemdata-dummy` for nested); the same crate now owns every gemcli device function. **Landscape:** the product is landscape but the LK fb is PORTRAIT 1080x2160, so the logical scene is 2160x1080 and `present()` counter-rotates 90 (matching gemwl `WL_OUTPUT_TRANSFORM_90` / `geminipda-drm` LEFT_UP); touch is un-rotated with the matching inverse; `GEMSHELL_ROTATE`/`GEMSHELL_TOUCH_ROTATE` calibrate. **The 2026-09-12 present() fix flips the scene Y in the compute texelFetch** — without it the 90° rotation was really a transpose (the on-glass left-right mirror). **2026-09-11 glass-report fixes (deployed gen44):** the REAL no-text cause was the glyph **advance** (`h_advance_unscaled * px` ≈ 16 000 px/glyph — only the first char showed / centred text went off-screen); **SVG app icons** (resvg, lazy, text off — most apps had blank letter tiles); launcher scroll clamp sign + `*=0.8` decay removed + tile taps + a **Close** button; **idle CPU ~60 % → 0.2 %** (render only when dirty; 1 s status tick); settings **Display** tab (brightness) + `gemini_speakers` (Built-in Speakers) in the Audio sink list. **2026-09-11 (b) fixes (gens 46–48):** GTK apps launched nothing because GTK4 requires **`wl_data_device_manager`** (added); settings froze/queued because every snapshot/mutation ran on the UI thread (new `compositor::data::CachedData` worker, coalesced); **one** window chrome only — GTK4 ignores xdg-decoration and uses the KDE protocol, so toplevels default to CSD; **variable UI scale 100/150/200%** (logical `lw`×`lh`, `Renderer::ui_scale`, egui `pixels_per_point = PPP × scale`, glyph atlas at `26×SUP`); Fn brightness/volume verified with `kb-inject` (Fn+B/N brightness, Fn+C/V volume). **2026-09-11 (c) (gen51):** apps open **maximized to the work area** (no desktop padding), CSD header bars are the drag handle (`xdg_toplevel.move` moves the window *while still forwarding* the touch; un-maximizes to a floating size), and **`wl_output.scale = ceil(ui_scale)`** so scaling applies to app content too; `buffer_rect` stretches a near-matching CSD buffer to fill (GTK reserves a shadow margin). **2026-09-12 (d):** the status bar now carries **window controls for the focused window** (**minimize · restore/maximize · close**, left of the clock) so a maximized app can always be shrunk/closed; `request_close()` actually sends `xdg_toplevel.close` (the old `close_window()` only dropped the resource, so ✕ did nothing while the app kept running); the settings modal now renders **first** and `render_frame` skips the hidden scene under it (the opaque full-screen panel), with `dirty` re-armed only by `settings_open && shell.wants_repaint()` (settings tap latency); `Input::process_key` reconciles a stuck Level-3/Mod5 Fn against `EVIOCGKEY` so plain `c/v/b/n` self-heal to letters. **Nested x86_64 dev loop:** `bash bin/gemshell-nested.sh` (`GEMSHELL_NESTED=1`; host EGL/GBM + wl_shm present, host input forwarded; dummy data; opens the panel) — no flash needed; `GEMSHELL_SCREENSHOT` writes the scene FBO PNG. Boots via `services.gemshellDesktop` (system service, marker-gated — `gemcli session set gemshell --reboot`). Fast loops: `bin/gemshell-nested.sh` (x86_64), `bin/gemshell-dev.sh` (on-device transient unit), `bin/gemshell-host-check.sh` (cargo check/test) | **`docs/gemshell.md`** + `pkgs/gemshell/` (workspace incl. `crates/gemdata*`) + `pkgs/gemshell.nix` + `services/gemshell.nix` |
| **gemshell platform backends + macOS preview** (2026-09-11): `bash bin/gemshell-nested.sh` now works on macOS too — a **native Cocoa window + desktop OpenGL 3.3 core** preview (winit 0.29 / glutin 0.31 / glutin-winit 0.4) for iterating on the shell chrome + egui panel. **macOS has no Wayland at all** (no libwayland/socket/session), so the nested-Wayland trick is impossible there; the OS-specific half was moved behind traits in `pkgs/gemshell/src/platform/` — `InputSource`, `Presenter`, `Backend` — and **the only `#[cfg]` fork in the program is `create_backend`**. Linux keeps the EGL/GBM/LK-fb/evdev backend (moved, not rewritten). Host Rust build (`cargo build` in `pkgs/gemshell`); renderer picks the desktop-GL shader set via a `Glsl` dialect + core-profile VAO. Verified: macOS `cargo check`/`cargo build` green + renders a frame; Linux modules type-checked on macOS via `RUSTFLAGS='--cfg gemshell_check_all' cargo check`. Device/nix rebuild still owed. | `pkgs/gemshell/src/platform/{mod,linux,macos}.rs` + `docs/gemshell.md` §"Platform backends" |

| **GNOME perf/touch handover** (2026-09-10; **Issue 1 = perf RESOLVED 2026-09-10k**: the old "two Mesa versions + idle panfrost" guess was demoted — the real cause was the `geminipda-drm` alpha loop, now deleted from the kernel delta; touch 90° off = gemwl-era DT touch transform vs KMS panel-orientation, still open) | **`docs/handover-2026-09-10-gnome-perf-touch.md`** |
| **gemdemo minimal GLES 3.1 + ALSA template** (0.3.0, 2026-09-09: the 0.2.0 “GEMINI: EXODUS” spacesynth demoscene — ~5200 lines/10 modules, 60 fps on glass — was judged useless except as hw-interaction proof and stripped to ONE Rust file: spinning shaded triangle + 440 Hz sine via the verified EGL→panfrost + `gemini16` S16@44.1k paths; the receipts that survive it (DSA→glGen, single-interleaved-VAO rule, wl_egl_window, S16 wire state) are the template's doc; **deployed as gen34**) | `pkgs/gemdemo.nix` + `pkgs/gemdemo/src/main.rs` (the single file) + **`docs/gemdemo.md`** (template guide + receipt list; 0.2.0/0.1.0 sources live in git history) |
| **GEMINI: EXODUS (Director's Cut) — cinematic spacesynth + GPU stress test** (1.0.0, 2026-09-11): the 0.2.0 demoscene restored from git `32ba356` as a first-class package *and* upgraded — `--stress 0..3` load model (more of the same single-pass layers, never a new pass), `--bench SECS` per-chapter frametime percentiles (p1/worst), `--stats` frametime overlay, GEMINI constellation + twin-sun motif, enriched anthem (harmony lead + timpani), A72 render-thread pinning, chapter-jump race fix. Single-pass GLES 3.1, no FBOs — the 0.2.0 engine held 60 fps on glass; this build's on-glass pass is owed | `pkgs/gemini-exodus.nix` + `pkgs/gemini-exodus/` + **`docs/gemini-exodus.md`** (+ `bin/gemini-exodus-host-check.sh`; in the rootfs via `services/gemini-pda.nix`) |
| Borrowed kernel #329 reference (kept until the self-built kernel is glass-verified; NOT wired into any build since 2026-09-08) | `kernel/borrowed/` (payload, DTB, module tree, sramldo-smc.ko, config) + `devices/planet-geminipda/kernel-borrowed.nix` |
| Kernel source: published v6.6 base (fetch-pinned) + tracked delta + lean config | `devices/planet-geminipda/kernel/` (`default.nix`, `delta/`, `config`, `config.full-329`) + `kernel/base` submodule pointer |
| **DRM/KMS driver for the LK framebuffer** (the standard-device enabler; GNOME prerequisite) | `devices/planet-geminipda/kernel/delta/drivers/gpu/drm/tiny/geminipda-drm.c` |
| Kernel config pruning + delta sync tools (**§9c = the RNDIS-off / CDC-ECM gadget rule — re-run after any regeneration**) | `bin/prune-kernel-config.sh`, `bin/sync-kernel-delta.sh` (replaces the retired `bin/snapshot-kernel.sh`) |
| **Rootfs ext4 image builder shim** (R13: growable mke2fs geometry; **2026-09-10: re-execs under fakeroot + `chown -R 0:0` so the image gets root-owned inodes — without it a fresh install has NO WiFi (NM rejects non-root plugin files) and logrotate fails**) | `pkgs/make-ext4fs-shim.nix` (wired via `config/gemini.nix` nixpkgs overlay) |
| **SSH identity, in the repo** (2026-09-17): `keys/gemini_ed25519` is BOTH root/cjdell's login key (declared in `config/gemini.nix`) AND the device's **pinned sshd host key** (activation installs it 0600; `hostKeys` is a single ed25519 entry) — so a from-scratch reflash needs no provisioning and raises no "REMOTE HOST IDENTIFICATION HAS CHANGED". All host scripts default to it (`bin/lib/host.sh` `gemini_ssh_key`, which also fixes a fresh clone's 0644). Deliberate public key: single-user lab device | `keys/` + `config/gemini.nix`, `bin/lib/host.sh` |
| boot.img header inspection | `bin/dump-bootimg-header.sh` |
| **Recovery tooling** — patched-mtkclient launcher (preloader/BROM), USB-state watcher | `bin/run-mtk.sh`, `bin/usb-watch.sh` (+ devshell `mtkclient` = store pkg + DAs) |
| **USB gadget NIC: RNDIS → CDC-ECM** (2026-09-17; **✅ flashed + verified on glass 2026-09-26, entirely from a MacBook — build → TWRP/adb flash → boot → `0525:a4a1` + ping/ssh + reboot cycle; `docs/session-log.md` 2026-09-26 (b)**): the precomposed `g_ether` gadget registers ONE CDC-ECM configuration (`0525:a4a1`) instead of RNDIS-first (`0525:a4a2`), so **macOS** (no RNDIS driver) uses the same `10.15.19.82` link Linux does — the enabler for the full Mac reboot/flash cycle. ⚠️ **rootfs-generation caveat:** the device rootfs on 2026-09-26 was gen 54 (< repo-key commit `1020d4e`), so the root/repo-key ssh paths (`device-ssh.sh`/`device-reboot.sh`/`flash-nixos.sh`'s converge hop) do not log in until the device is redeployed/reflashed ≥ `1020d4e`; TWRP/adb flashes are unaffected. `bin/prune-kernel-config.sh` §9c holds the config rule (RNDIS symbols off); `bin/lib/host.sh` holds the host-side Linux/macOS differences (MAC discovery, `ip`/`ifconfig`, ping, `lsusb`/`system_profiler`, devshell re-exec). Not switched to configfs/legacy-gadget semantics on purpose (fail-safe: usb0 is the only debug link) | **`docs/usb-network.md`** + `bin/lib/host.sh` |
| **g_ether net-up / SSH / WDT-EXRST reboot** (host side; **Linux + macOS** since 2026-09-17 — no `lsusb`, NIC found by MAC; mac cycle: `bin/build.sh` → `bin/flash-nixos.sh`) | `bin/net-up.sh`, `bin/device-ssh.sh`, `bin/device-reboot.sh`, `bin/lib/host.sh` |
| **Boot-target switching + boot-partition flash** (adb/TWRP; twrp/android/flash/restore; the `debian` verb was removed 2026-09-10) | `bin/boot-switch.sh` |
| **Full NixOS flash orchestration** (converge-to-TWRP from any state, boot + stream rootfs → p27 `linux`; **`preflight` = the read-only readiness check** — artifacts + sha256 ledger, bootopt/fbcon in the boot.img, toolchain, ssh key, sudo, device state — and it works on Linux **and macOS** since 2026-09-17) | `bin/flash-nixos.sh` (verbs incl. `grow-rootfs` — offline p27 fs growth from TWRP, R13) |
| **One-way repartition to TWRP + NixOS only** (2026-09-10; plan/backup/apply/verify/boot; byte-verified GPT + streamed rootfs) | `bin/repartition-nixos.sh` |
| **Build/switch/rollback generations like a workstation** (native-aarch64 distributed build → delta `nix copy` → device profile switch + activate; NO reflash) | `bin/deploy.sh` (status/build/deploy/rollback) |
| **Build entry point — platform-dispatched** (2026-09-17, rule 10): `bash bin/build.sh <verb> [TARGET]` is the ONE way to build this repo's aarch64 outputs; it detects the platform and runs the model that works there, with the same verbs + rc protocol on both (`start`/`wait`/`log`/`status`/`shell`, `vm` on a Mac). Artifacts, targets and the never-do-this list: **`docs/building.md`** | `bin/build.sh` (dispatcher), `bin/build-linux.sh` (Linux: the unchanged native model — `nix build --store local` + Pi builder; `toplevel` → `bin/deploy.sh build`) |
| **Build images from a macOS host** (2026-09-17: a real `bootimg` built on the M5 Mac — 9.5 MiB, sha256 `b2404b13…`, header + appended DTB verified against the boot contract; **2026-09-26: built, TWRP/adb-flashed + booted from the Mac — CDC-ECM image sha256 `2e8f43ad…`**): nix on darwin can only *evaluate/substitute* foreign drvs, so Apple `container` + the aarch64 NixOS VM `rzmapp/nixos-vm:26.05` supplies the missing aarch64-linux builder (repo staged read-only at `/build/src`, persisted `/nix`, artifacts + manifest/sha256 in `~/.cache/gemini-macos/out`). Nix-on-darwin is used for what it *can* do, in its own flake file: `flake-macos.nix` adds `devShells.aarch64-darwin.default` (python3/rsync/cacert/adb/coreutils/findutils — adb + GNU coreutils are the DEVICE-script tooling too, since 2026-09-17 the Mac drives the flash cycle over the CDC-ECM NIC) + `packages.aarch64-darwin.caBundle`, merged via a one-level-deep `mergeOutputs` (a bare `//` is shallow — it broke `packages.aarch64-linux` once, 2026-09-17). **Egress gotcha:** with a Tailscale exit node owning the Mac's default route, container vmnet NAT gives the VM no internet (measured 2026-09-17; a runtime restart does not fix it) — `net` auto-detects and lends the VM the Mac's egress via `bin/macos/proxy.py` (proxy env only, **the VPN is untouched**). **Deploy (not flash) from a Mac** since 2026-09-26: `bin/macos/deploy.sh` builds `toplevel` in the VM, has the VM `nix copy` the closure delta to the device and switches the system profile (first used to ship the gemshell `wl_buffer.release` fix as device gen 55 — session-log 2026-09-26 (e)) | `bin/macos/{build.sh,vm-build.sh,deploy.sh,proxy.py}`, `flake-macos.nix`, **`docs/macos-build.md`**, `docs/usb-network.md` |
| **Same loop, run ON the PDA** (2026-09-08, gen30): build straight into the device store (cache substitutes; custom drvs compile locally when changed) + profile switch/activate, from a repo clone at `/root/gemini-nixos`; `channels` pins the `nix-shell -p` nixpkgs to the flake rev. **2026-09-09: the flake's `nixosConfigurations.gemini` makes the stock `nixos-rebuild switch --flake .` work from the clone — the script is now the convenience wrapper (dirty gate, status/rollback, channels)** | `bin/device-rebuild.sh` (status/build/switch/rollback/channels/gc); clone sync: `bin/device-repo.sh` (seed/push/pull over g_ether as a git bundle) |
| **GC-pin builds** (host `nix-collect-garbage` protection — every deploy pins itself; list/unpin) | `bin/gc-pin.sh` |
| **Detached job runner** (rule 8) | `bin/run-job.sh`; state `logs/jobs/` |
| **GNOME session-env refresh** — after a deploy, push `NIX_GSETTINGS_OVERRIDES_DIR` from `/etc/set-environment` into the RUNNING `systemd --user` + restart gsd targets, so schema-override changes (Fn keys, volume) work without a logout/reboot (2026-09-11) | `bin/gnome-session-env-refresh.sh` (`--all`) |
| Device partition backups pulled over adb/dd (gitignored; nvram/IMEI private — never commit) | `stock-dump/` (ledger + copy status: `docs/disaster-recovery/inventory.md`) |
| Legacy hardware/boot receipts (port pending M1) | `/home/cjdell/Projects/GeminiPDA/docs/` (read-only reference until ported) |

## When to update what

- **Port decision / build-level delta** (eval error, derivation quirk,
  new overlay) → this repo's docs + derivation comments; a dated entry
  in `docs/session-log.md`.
- **Milestone status** (phase table in the feasibility doc, README
  status) → update both; keep "build-level vs on-glass" explicit.
- **New hardware/boot fact or correction to one** → THIS repo
  (golden): `docs/boot-process.md` or the receipt doc being ported
  (M1), with `file:line` + date; if the same claim exists in a legacy
  GeminiPDA doc, port-fix it here and optionally leave a one-line
  `[superseded — see gemini-nixos …]` pointer there.
- **A flash/boot attempt happened** → `docs/session-log.md` (MANDATORY:
  versions flashed, hashes, exact behaviour, console output) +
  feasibility doc §9 phase notes.
- **Pins moved** (Mobile NixOS rev/narHash, **nixpkgs flake pin** (channel rev, flake.nix), kernel
  snapshot) → `flake.nix`/README "Pins" + date it (npins drift breaks
  the verified graphics stack — R2; the flake nixpkgs pin should track
  the nixos-unstable CHANNEL rev so hydra's full closure stays cached).
- **DR ledger changed** (a dump taken/verified/copied) →
  `docs/disaster-recovery/inventory.md` + a version line in the log.

## Device operations cheat sheet (mechanisms verified 2026-08-30/31 on
the legacy project; receipts still in GeminiPDA docs until M1 ports them)

**AFTER ANY DEVICE POWER-ON/POWER-CYCLE (GOLDEN RULE):** the host side
of the USB NIC link is DOWN (the gadget iface disappears on power-off).
`bash bin/device-ssh.sh '<cmd>'` auto-runs the net-up path (passwordless
sudo) — root shell @ **10.15.19.82** (host 10.15.19.1/24). The interface
is found by its **MAC** (`42:00:15:19:82:*`, the g_ether host/dev pair),
so USB-port renumbering no longer matters; the device auto-configures its
side at boot. Hand-rolled ssh needs `sudo bash bin/net-up.sh` first.
[2026-09-17: this is **Linux + macOS** now, and the gadget is **CDC-ECM**
(`0525:a4a1`) — `bin/lib/host.sh`, `docs/usb-network.md`. The admin key is
IN the repo (`keys/gemini_ed25519`, `keys/README.md`) and the device
declares it (login key + pinned sshd host key), so there is nothing to
copy and a reflash does not change the device's SSH identity; on a Mac
the host link is **passwordless** since 2026-09-26 (root LaunchDaemon
`com.gemini.usb-nic`, installer `bin/macos/install-usb-nic-daemon.sh`,
one-time admin prompt) — `docs/usb-network.md` §"Passwordless USB-NIC
link".]

**Remote reboot of a running Linux:** software resets POWER THE UNIT OFF
(verified 2026-08-31) — only the LK-configured WDT EXRST path
self-boots: `busybox devmem 0x10007004 32 0x48` (2 s WDT). From the host:
`bash bin/device-reboot.sh`. Device-side (NixOS rootfs): the
`gemini-wdt-reboot` unit does the same.

**Boot targets** (adb state machine in `bin/boot-switch.sh`; para = p2
of the largest mmcblk, 32-byte command at offset 0):
- `boot-recovery\0` + 18 zero bytes → **TWRP on every power-on (sticky —
  TWRP does not clear it)**; 32 zero bytes → NORMAL boots the `boot`
  image's **NixOS branch (p27 `linux` — the default)**. Since the
  2026-09-10 repartition TWRP + NixOS are the only systems, so
  `boot-debian` is gone (the initrd still treats it as non-NixOS and
  falls back). Only `para` and `boot` are ever written; never
  nvram/proinfo/protect*.
- `bash bin/boot-switch.sh status|twrp|android|flash [img]|restore`.
  NOTE: `android` = para-clear + reboot → NORMAL → boots the NixOS p27
  default (the historical name is kept). "Linux" is not a separate slot:
  our kernel goes in
  `boot` itself (boot2/boot3 are gone).
- Device running Linux (no adbd — the current real state): converge via
  `bin/flash-nixos.sh` (para write over ssh + WDT EXRST → TWRP); a fresh
  System is flashed with `flash-nixos.sh rootfs` (p27), or the whole
  layout redone with `bin/repartition-nixos.sh`.
- A hung boot image has NO software path back (para cleared = normal
  boot) → recovery = mtkclient preloader mode (`bin/run-mtk.sh`;
  full playbook `docs/disaster-recovery/drills.md`) — this is why the
  SAFE test cycle keeps para = boot-recovery (TWRP sticky) until the
  image is verified, and why `stock-dump/boot-*.img` backups are kept
  (restore = one adb command from TWRP).

**Flash pipeline (no fastboot on this device):** images go to partitions
from the patched no-swipe TWRP (root adbd) by-name paths:
`/dev/block/platform/mtk-msdc.0/11230000.msdc0/by-name/{boot,linux,para}`
(and `recovery`).
NixOS rootfs → p27 `linux` (58.0 GiB, ext4 label `NIXOS_SYSTEM` — the
single system partition since 2026-09-10). Boot image → p22 `boot`
(16 MiB).
Orchestrated by `bin/flash-nixos.sh
status|preflight|boot|rootfs|all|boot-nixos|grow-rootfs` (see its header for the safety
model + run-job usage; `preflight` is read-only and answers "could a flash
run from THIS host?" — it is the one to run before touching the device).

**Battery/charger truth** (OS-dependent; verified live on the legacy
project): TWRP sysfs is STALE — read dmesg `[PE+]Ibat=..`; Linux:
`/sys/class/power_supply/bq25890-charger-0` (+ `bq25896-raw.sh` raw ADC
over i2c). ICHGR = 0 is NORMAL when load ≥ input power (power-path);
scale 50 mA/step. No fuel gauge → voltage thresholds only (guard
<3.65 V, orderly poweroff <3.50 V). `i2cget -f -y 0 0x6b` — the `-f` is
MANDATORY (kernel driver claims 0x6b).

## Build model & commands (native aarch64)

Builds are **native aarch64** (canonical since the 2026-09-08
native-aarch64 merge; the x86_64 cross toplevel is ABANDONED — it hit
nixpkgs cross walls, last one Qt6CoreTools missing for the lxqt scope;
`docs/handover-2026-09-07-lxqt-native.md`). Every drv is
system=aarch64-linux; builds run as root against the LOCAL store with
`--option builders @/etc/nix/machines --fallback` so the 192.168.49.191
remote builder (8-core Pi) compiles and the host pulls finished paths
back over ssh (`bash bin/deploy.sh build` = exactly this; run long
builds under `bash bin/run-job.sh start <name> -- bash bin/deploy.sh
build`):

```sh
sudo nix build --store local .#packages.aarch64-linux.default   # boot.img + rootfs.img (+ flash script)
sudo nix build --store local .#packages.aarch64-linux.bootimg   # boot.img only (self-built lean kernel)
sudo nix build --store local .#packages.aarch64-linux.kernel    # the kernel package alone (Image.gz+dtbs+modules)
sudo nix build --store local .#packages.aarch64-linux.rootfs    # rootfs.img (→ `linux` partition)
sudo nix build --store local .#packages.aarch64-linux.initrd    # minimal initrd (size measurement, docs R1)
sudo nix build --store local .#packages.aarch64-linux.mesa      # patched nixpkgs Mesa 26.2.2 (panfrost + T880 delta)
```

Since 2026-09-08 the kernel is compiled in-nix (lean config: ~6 min
on the aarch64 builder; the full #329 config takes much longer — the
lean config drops 61 % of enabled symbols). Mesa is the other heavy
build (cached in the local store once built).

**On a Mac (no Linux host involved at all):** `bash bin/build.sh
start bootimg` (the same dispatcher — rule 10) runs this native aarch64
build inside an Apple
`container` VM — nix on darwin can evaluate/substitute foreign drvs but
can never *run* an aarch64-linux builder, so the VM is the builder
(first real `boot.img` 2026-09-17, 9.5 MiB, verified; **docs/macos-build.md**
— includes the egress-proxy gotcha for a VPN/exit-node host).

aarch64 note (historical): under the OLD npins rev,
`ffmpeg`/`ffmpeg-headless` defaulted to `withCudaLLVM = true` and failed
for aarch64, so `config/gemini.nix` carried an overlay forcing
`withCudaLLVM = false`. **Removed 2026-09-08 with the repin**: the new
pinned channel rev's aarch64 ffmpeg builds are on the cache. Same story
for the systemd `withLibBPF` and openblas `dynamicArch` workaround
overlays (old-rev/cross-era). If a real build of this rev re-hits one of
the old bugs, re-add the specific override with a date + receipt.

### Mesa ICD runtime wiring

**Historical (verified in the rootfs 2026-09-05), then superseded
2026-09-10:** originally the fork's manifest landed in `/etc` via an
explicit `environment.etc` entry (NixOS does not merge a package's
`$out/etc`), with a RUNPATH to its own lib dir so `libgallium-25.0.7.so`
resolved without an ldconfig cache. Since the single-Mesa change
(`hardware.graphics.package` = patched nixpkgs Mesa 26.2.2) there is one
ICD at `/run/opengl-driver/share/glvnd/egl_vendor.d/50_mesa.json`. The
on-glass extension check worth keeping: `eglQueryString(EGL_EXTENSIONS)`
must list `EGL_EXT_image_dma_buf_import`.

### Rootfs integration details (from the outstanding.md audit)

`sramldo-smc.ko` is loaded at boot via `boot.kernelModules` (the kernel
derivation builds it in `postInstall` and ships it under `extra/`,
depmod-indexed); `busybox` + `i2c-tools` are system packages for the
scripts and hand use on the serial console. `boot.kernelModules` also
loads the DRM chain (`drm`/`drm_shmem_helper`/`gpu-sched`/`panfrost`) +
`mt6351-keys` early (major-226 race with `wlan_gen3`), the B-19 USB
host-PM udev rules keep USB autosuspend off, and
`gemini-backlight-default` dims the display to 10 % at boot.
`gemini-wdt-reboot` / `gemini-boot-recovery` exist as CLIs *and*
hand-started systemd units. The bash-shebang rewrite (feasibility §7
R10) is applied at package time so the verbatim Debian scripts exec on
the NixOS rootfs.

## Kernel source model (published base + in-repo delta; self-contained since 2026-09-08)

The kernel is **built in-repo** — no borrowed #329 artifacts, no
GeminiPDA dependency, no vendored source (full receipt:
`docs/session-log.md` 2026-09-08).

- **base** = the published upstream Linux **v6.6** release tarball
  (kernel.org, fetched by hash) — byte-identical to `git archive v6.6`
  of the geminipda-bringup base commit `ffc253263a…`, which is also
  pinned as a git submodule pointer at `kernel/base` (fetch on demand:
  `git submodule update --init --depth 1 kernel/base`);
- **delta** = `devices/planet-geminipda/kernel/delta/` — the 512 plain
  files the bring-up branch changes over v6.6 (457 added + 55 modified,
  no deletions; NO patch files — edit source directly).
  `v6.6-base + delta == geminipda-bringup @ 188aade69` (the on-glass
  #329 tree), verified byte-for-byte; refresh with
  `bin/sync-kernel-delta.sh`;
- **config** = the LEAN device config (`./config`, generated by
  `bin/prune-kernel-config.sh` from `config.full-329`, the exact #329
  config kept for A/B). Prunes hardware that can never exist on this
  unit: 4,213 → 1,655 enabled after olddefconfig (61 % fewer symbols);
  result: Image.gz 8.0 MiB (was 13.5), 388 modules (was 1,165), DTB
  byte-identical to #329.

`kernel/borrowed/` (the #329 payload/DTB/module-tree/config) and
`devices/planet-geminipda/kernel-borrowed.nix` remain on disk as the
pre-verification reference/rollback.

## Flashing (manual — no fastboot)

**Tooling is in-repo and ready** (see the cheat sheet above):
`bin/flash-nixos.sh` orchestrates the whole pipeline — it converges the
device to TWRP from **any** state (running Linux via para-write + WDT
EXRST self-boot over ssh, Android via adb, or POC/offline with prompts),
then flashes. `bin/boot-switch.sh` is the adb/TWRP boot-target state
machine underneath; device state over ssh = `bin/device-ssh.sh` /
`bin/net-up.sh` (g_ether, 10.15.19.82).

1. Build: `nix build .#packages.aarch64-linux.default` → `result/`
   with `boot.img` + `system.img` (the NixOS rootfs, ~8 GB with GNOME).
2. `bash bin/flash-nixos.sh status` — device state + local artifacts.
3. `bash bin/flash-nixos.sh boot` — backs up the current `boot`, flashes
   `boot.img` → p22 `boot` (16 MiB). Stays in TWRP.
4. `bash bin/flash-nixos.sh rootfs --yes` — **streams** `system.img` →
   p27 `linux` (58 GiB ext4, label `NIXOS_SYSTEM`; destroys the current
   NixOS rootfs). First boot auto-resizes the fs to fill p27 and
   rehydrates the Nix store. (Run under `bin/run-job.sh` — ~8 GB.)
5. `bash bin/flash-nixos.sh boot-nixos` — clear para + reboot: LK →
   initrd → p27 `linux` → NixOS stage-2.

**A repartition (TWRP + NixOS only) is a one-way operation** handled by
`bin/repartition-nixos.sh` — see `docs/repartition-android-space.md`
§12. After it there is only one OS, so no para-based OS switching.

**Safety model:** an unverified boot image that hangs has no software
path back (recovery = mtkclient preloader mode), so the scripts default
to para = boot-recovery (TWRP sticky) until you explicitly boot the new
image, and every `boot` flash is backed up to `stock-dump/` first
(`bin/boot-switch.sh restore` rolls back). Rollback of the rootfs =
reflash `system.img`; p27 `linux` is the only partition these scripts
write.

## On-device build/switch — `nixos-rebuild switch --flake .`

Since 2026-09-09 the flake exposes `nixosConfigurations.gemini`, so the
STOCK NixOS tool works on the device. The PDA is a first-class flake
target: a repo clone at `/root/gemini-nixos` (sync with this host via
`bin/device-repo.sh` seed/push/pull or the github origin) can iterate the
config and add programs with NO host involved:

```sh
# on the device (root, in the repo clone at /root/gemini-nixos):
nixos-rebuild list-generations --flake .     # what is installed / selectable
# edit config/gemini.nix, git add + commit, then:
nixos-rebuild build --flake .                # eval + build on the PDA (no activation)
nixos-rebuild switch --flake .               # profile switch + activate (no reflash)
# ...and the classic ad-hoc shell:
nix-shell -p pkgname        # pinned to the same nixpkgs rev as the flake (rule 9)
```

`--flake .` resolves to `.#nixosConfigurations.gemini` (the device
hostname). The flake exposes that configuration (single eval shared
with `packages.aarch64-linux.toplevel` — the SAME toplevel derivation,
verified 2026-09-09), so `nixos-rebuild` builds, sets the system profile
and runs switch-to-configuration exactly as `bin/device-rebuild.sh` /
`bin/deploy.sh` do by hand. `nixos-rebuild` (the Python nixos-rebuild-ng
— the bash one is gone from nixpkgs at this pin) lands in the system
closure by default, and MNX's rootfs postBootCommands created
`/etc/NIXOS` + the system profile at first boot. Every toplevel records
the git rev of the tree it was built from
(`system.configurationRevision` → `nixos-rebuild list-generations` /
`nixos-version --configuration-revision`; a dirty tree shows as
`<sha>-dirty`) — commit before switching so the generation is a clean
commit (rule 0). `bin/device-rebuild.sh` stays as the convenience
wrapper (status/rollback/gc + the `channels` re-pin for `nix-shell -p`);
rollback without it = `nixos-rebuild --rollback switch --flake .`.

Builds are native aarch64 into the device store (store writes flow
through the socket-activated nix-daemon — `/nix/store` is bind-mounted
ro in the main namespace by design). The pinned nixpkgs rev substitutes
from cache.nixos.org over the device's wifi/USB NAT; only the custom
drvs (mesa, kernel, wlroots/gemwl, firmware) and config glue
compile locally. Kill the in-tree build via run-job or the daemon
(cap: `max-jobs = 2`, `cores = 2` — 3.6 GiB RAM bound; a zram/swapfile
is the open improvement).

## Pins (maintenance)

- **Mobile NixOS**: commit `2c132754` (branch `development`), fetched
  as a pinned tarball by `flake.nix` (it is not a flake, so nix 2.34
  cannot use it as a flake input). Bump the SHA in `flake.nix`.
- **nixpkgs**: pinned by **this flake** (`flake.nix`) — chosen rev =
  the nixos-unstable **channel snapshot** `dc5d91f84032`
  (`26.11pre1068949`). Bump = take the rev behind
  `https://channels.nixos.org/nixos-unstable/git-revision`, re-verify
  the narHash (`nix flake prefetch github:NixOS/nixpkgs/<rev>`), and
  re-pin the device's `nix-shell -p` channel
  (`bin/device-rebuild.sh channels`). See rule 9.
- **Kernel**: `bin/sync-kernel-delta.sh` folds a new fork rev into the
  delta (default `06fd13e11`); the delta now deliberately DIVERGES from
  the fork in five files, so the script ABORTS and lists them — pass
  `FORCE=1` only after folding local work into the fork.

## Conventions

- **Statuses:** ✅ done/verified · 🟡 built but not on hardware/partial ·
  ⬜ planned · 🔴 blocked. Update the phase table
  (`docs/mobile-nixos-port-feasibility.md` §9) + README status.
- **Dates:** `YYYY-MM-DD`; header of each doc carries "Last updated".
- **Nix files:** keep formatted (`nixpkgs-fmt` in the devshell if added);
  comment derivations like the existing ones (they carry the quirks —
  R2/R8/R9-style receipts).
- **Kernel delta hygiene:** the kernel builds from the published v6.6
  base + `devices/planet-geminipda/kernel/delta/` (source of truth;
  nix-build-only fixes live in `kernel/default.nix` so the delta stays
  byte-identical to the fork). To fold new fork commits in, point
  `bin/sync-kernel-delta.sh` at the new fork rev (it now defaults to
  `06fd13e11`) and update the rev in the derivation header. **Since
  2026-09-10k the delta deliberately DIVERGES from the fork** in five
  files (the delta-only `drivers/gpu/drm/tiny/{geminipda-drm.c,Kconfig,
  Makefile}`, the modified `mt6797-gemini-pda.dts` DRM node, and
  `common_main/core/wmt_lib.c` — STP PSM forced off, the A2DP fix, see
  `docs/bluetooth-a2dp.md`), so the
  script now ABORTS and lists them instead of clobbering — pass `FORCE=1`
  only if you have folded the local work into the fork. Do not hand-edit
  the delta and then blindly sync. The old `bin/snapshot-kernel.sh` +
  225 MB tarball are retired (git history has them).
- **Device backups:** `stock-dump/` is gitignored — boot/para backups
  live there (ledger + copy status in
  `docs/disaster-recovery/inventory.md`); nvram (IMEI) is private,
  never commit it.
- **Host has NO python3/adb on PATH** (rule 7) — `bin/*.py`-style tooling
  would need `nix develop --command python3 …`; `bin/run-mtk.sh` finds
  its own store python (sudo context) but needs the devshell's
  `mtkclient` closure to exist (build once: `nix develop --command true`).

## Before you start a session

1. Read `docs/session-log.md` (last entries) + the phase table in
   `docs/mobile-nixos-port-feasibility.md` §9 + README status.
2. If the session may touch device behaviour and the topic's receipts
   are not yet ported (M1/M7), glance at the legacy
   `GeminiPDA/docs/session-log.md` tail — it is the ground truth of what
   was tried until the history port lands.
3. Note the pinned versions (README "Pins") and doc "Last updated" dates.
4. Decide what needs updating as you go — don't wait until the end.

## Before you end a session

1. Append a dated entry to `docs/session-log.md` (what was tried, what
   flashed/changed, versions, hashes, next action).
2. Update every doc whose claims the session touched (statuses, dates) —
   including the phase table, README and the DR ledger.
3. If the session changed the kernel delta or its fork rev, re-run
   `bin/sync-kernel-delta.sh` and update the references.
4. If the session ran a flash/boot cycle, leave the version line +
   outcome in the session log and say whether the glass/TWRP state was
   left safe (prefer para = boot-recovery until images are verified).
5. New knowledge goes HERE (golden) — if you also touched a legacy doc,
   say so in the log so the port can pick it up.
