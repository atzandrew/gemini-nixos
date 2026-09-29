# gemini-nixos session log

Dated entries of what was actually tried / decided in this repo.
Hardware/boot ground truth lives in the sibling project
(`/home/cjdell/Projects/GeminiPDA/docs/session-log.md`) — cross-reference
when a session touches device behaviour. Latest entry first.

## 2026-09-29 — G1 evidence: an on-USB poweroff is structurally NOT a poweroff (LK's POC trap boots the OS); 100 mA measured draw shows the unit stays alive

Ask: the unit sat on a USB charger for several days after
`systemctl poweroff` yet still draws **100 mA @ 5 V ≈ 0.5 W**
continuously; VBAT holds steady at **4.00 V (so not charging)**; the
compute area is slightly above room temperature. "Shutting down" from
TWRP just boots Linux. Read-only investigation — the device is not
enumerating on this host and `10.15.19.82` is unreachable, so all of this
is source/analysis against this repo plus the legacy tree on
`zen3-nixos:~/Projects/GeminiPDA`; no device or repo-code action taken.

**Findings.**

1. **The unit is not off.** 0.5 W of load with the screen/USB-radio off is
   the "limbo" class G1 suspected, not a PMIC-quiescent off.
2. **On USB, a poweroff cannot be a true off — LK turns it into
   off-mode charging, which *boots a kernel*.** [source-verified]
   - our fallback: `mt6797-power.c:138-179` — write `RTC_BBPU = 0x4309`,
     wait 1 s, then a **mode-0 WDT reset** while a charger holds the rails.
   - LK `mt_kernel_power_off_charging.c:97` + `boot_mode.c:254`: charger
     present + `is_force_boot()` false → `KERNEL_POWER_OFF_CHARGING_BOOT`.
   - `platform.c:801` turns the **display on**; `mt_boot.c:1490` **loads the
     `boot` image and jumps to the kernel**; `atags.c:921` passes
     `androidboot.mode=charger`.
   - our `boot` image is the normal NixOS image, so charger mode = **boot
     Linux** — exactly the TWRP observation (TWRP poweroff → LK POC →
     normal boot.img → NixOS).
   - the vendor is identical: `mt_power_off` (`mtk_rtc_common.c:397`) →
     `machine_restart("charger")` → `arch_reset("charger")` →
     `wd_api->wd_sw_reset(0)` (`wd_api.c:583-618`) = mode 0, no bypass.
   - **no LK path yields "off with a charger":** `off-mode-charge=0` and
     every `is_force_boot()` flag choose `NORMAL_BOOT`, never off. LK only
     powers off when no charger is present.
3. **The 2026-09-10 "poweroff verified" was not evidence of off** — it only
   saw "USB went silent", which is an orderly shutdown + halted AP. G1
   already suspected this; the 100 mA draw now confirms it. [corrects
   `docs/power-states.md` "Implementation status" / §7 step 3.]
4. **Legacy corroboration** (GeminiPDA `docs/session-log.md`): 2026-08-30
   documents the POC trap verbatim ("lands ANY boot without a live
   power-key press + WDT-bypass + rtc 2sec into
   KERNEL_POWER_OFF_CHARGING_BOOT when a charger is detected"); 2026-09-04
   records "`shutdown -r now` over ssh on this unit = POWER OFF".
5. **Not yet explained: VBAT flat at 4.00 V.** The BQ25896 power-path *does*
   drop charge current to zero when the load eats the input
   (`docs/hardware.md:855-877`), but IINLIM is the chip default 500 mA (and
   our kernel reports 500 mA), so a ~100 mA load should leave charge
   headroom and VBAT should rise. A flat VBAT therefore suggests the
   charger is *not in a normal charging state* — the known B-19/B-22 "VBUS
   present but NOT charging" failure mode. Unverified (device not
   reachable); check REG00/REG03/REG0B/REG12/REG13 with
   `services/scripts/bq25896-raw.sh`.
6. **Deltas vs the vendor poweroff path** (candidates, low confidence): our
   driver omits `hal_rtc_bbpu_pwdn()`'s SRCLKENA-GPIO-low step and 32 K
   export disable (`mt6351/mtk_rtc_hal.c:180`), and the vendor warm-reset
   calls `pmic_pre_wdt_reset()` (PMIC sleep-mode buck voltages) before
   SWRST — ours does not. LK omits them too, so not obviously required.

Two states fit the measurements and are distinguishable: **(A) LK POC /
charging mode** — display ON (`platform.c:801`), USB → `0e8d:2008`, power
key boots; **(B) halted-AP limbo** — dark, no USB, power key dead. That
TWRP's poweroff *does* reach LK (Linux) while ours appears not to hints our
path lands in (B), i.e. our mode-0 fallback may not reach LK — unproven.

**Next (G1 step 1).** `systemctl poweroff` **on battery, USB data
detached**, log VBAT ≥ 30 min (flat = off, falling = limbo), and read
`RTC_BBPU` back after the 0x4309 write (boot readback is `0x000d`; poweroff
must clear bit 2 — `mt_rtc_hw.h`: `RTC_BBPU_BBPU` "1: power on, 0: power
down"). Instrument the driver to log whether the mode-0 fallback executes.
Because a true off on USB is impossible, also make the VBUS-present idle
state actually charge (item 5), or "off-mode charging" still drains the
pack. **Full instrumented plan: `docs/power-states.md` §9.**

## 2026-09-26 (e) — gemshell wedge ROOT CAUSE + FIX: the compositor never sent `wl_buffer.release` (wl_shm clients leaked >900 MB/app → RAM exhaustion)

Ask: gemshell was "fine for a few minutes then suddenly extremely
slow", then "locked up again after a few interactions". Signature on
glass: **kernel alive (ping 0.25 ms) but userspace dead** (sshd banner
timeout) — the known hard-wedge shape (2026-09-09), reproduced under
gemshell while GNOME stayed stable.

**Diagnosis (read-only, over the interim password hop — the gen-54
rootfs predates the repo key).**
- The frozen boot's journal (`journalctl -b -1`; journald is
  persistent) ends with a kernel `Purging N bytes` storm from
  **panfrost.ko** (`pr_info_ratelimited` in `panfrost_gem_shrinker_scan`,
  v6.6 `panfrost_gem_shrinker.c` — verified by `strings` on the module),
  i.e. heavy memory reclaim, then silence. No OOM, panic, RCU stall or
  GPU fault was logged (`CONFIG_DETECT_HUNG_TASK`/`SOFTLOCKUP` are OFF;
  `/sys/fs/pstore` — ramoops IS wired — was empty: the freeze emitted
  nothing).
- Instrumented the live box: `gnome-calculator` launched under gemshell
  grew **384 KB → 920 MB RSS in ~27 s**, with `Shmem:` (its wl_shm
  pools) 37 MB → 843 MB. A few apps/interactions exhaust the 3.7 GB
  RAM; with **no swap** the reclaim thrashes → userspace starves → wedge.

**Root cause.** The compositor never sent `wl_buffer.release` — the
`Dispatch<WlBuffer>` impl was empty and `SurfaceInner` did not even keep
the buffer resource. A compositor MUST release a wl_buffer once it is
done reading it, so a wl_shm client may recycle it. Without it GTK4
allocates a fresh shm pool every frame → unbounded growth. (The legacy
gemwl compositor got this from wlroots; the hand-rolled wl_shm path in
gemshell regressed it.)

**Fix.** Keep the attached `wl_buffer` in
`SurfaceInner.pending_buffer_wl` and call `release()` from
`surface_commit` as soon as `window_texture()` has copied the pixels
into a GL texture (nothing reads the buffer afterwards). Also release on
the no-window/early-return paths so the client is never left holding it.

**Verified on glass** (`wasa7dasvxm15kihi2xv8m2nznjglk8x-gemshell-0.1.0`,
built via the macOS container, shipped with `nix-store --import`, run
transiently as `gemshell-dev.service` with the installed unit's env):
the same `gnome-calculator` now plateaus at **~107 MB RSS** and
`Shmem:` ~75 MB (flat), `MemAvailable` steady ~3.28 GB. ✅
**User-confirmed under sustained use** (7 clients open, heavy app
interaction): `Shmem:` ~200 MB, gemshell RSS ~175 MB, `MemAvailable`
~3.0 GB — flat over minutes (a host-side sampler; before the fix a single
app reached 843 MB shmem).

**Deployed properly (Mac → device, no Linux host).** `bash bin/build.sh
start toplevel` built
`33dr0yrpy9f3brjpjq6hbb6l2wc3hsyz-nixos-system-gemini-26.11pre-git` (its
closure carries `wasa7…-gemshell`); `nix copy` from the build VM shipped
the 22 s delta and `switch-to-configuration switch` activated it →
**generation 55**. That Mac-side flow is committed as
**`bin/macos/deploy.sh`** (`status`/`deploy`/`rollback`) — the macOS
counterpart of `bin/deploy.sh`. The device's root now accepts the repo
key, so `bin/device-ssh.sh` works directly (the gen-54 rootfs caveat is
gone).

**Persistence proven:** a WDT-EXRST reboot came back with a new
`boot_id`, `/run/current-system` = the gen-55 toplevel, and
`gemini-gemshell.service` active on `wasa7…` (2026-09-26).

**Device left:** gen 55 current, `gemini-gemshell.service` active on the
fixed binary, `para` clear. Nothing flashed. (A transient
`gemshell-dev.service` run + a GC root were used during bring-up.)

Source: `pkgs/gemshell/src/compositor/wayland.rs`,
`pkgs/gemshell/src/compositor/mod.rs`; docs/gemshell.md "2026-09-26 (e)".

## 2026-09-26 (d) — gemshell on glass: WAYLAND-APP TOUCH FIXED (buffer-scale input transform); app-hosting issues logged

Ask: *"bring gemshell up on the device so I can remind myself of the
issues … it currently does not host Wayland apps properly"* — then, on
glass: apps open but **ignore all touch**.

Led the device to the gemshell session (`sudo gemcli session set gemshell
--reboot`; marker now `gemshell`, GDM skipped, `gemini-gemshell.service`
active, own socket `/run/gemshell/wayland-0`, panel renders). Reached it
with the interim **password hop** (`cjdell` / `0000`, passwordless sudo):
the gen-54 rootfs predates the repo-key commit `1020d4e`, so
`bin/device-ssh.sh`'s pubkey path still fails (usb-network.md caveat).

**Root cause (found, fixed, verified).** `Compositor::local_coords`
omitted `wl_surface.set_buffer_scale`. Wayland input coordinates are
**surface-local** (buffer pixels ÷ buffer scale); gemshell sent buffer
pixels. The persisted UI scale is **1.5**, so `wl_output.scale=2`, every
client allocates a 2× buffer and calls `set_buffer_scale(2)` — e.g.
gnome-calculator: configure 1440×588, buffer **2880×1232** →
surface-local **1440×616** — while gemshell delivered `wl_touch.down` at
e.g. `(884, 1024)`: 2× too large, outside the surface, so GTK dropped
every event. The protocol path itself was correct:
`wl_seat.capabilities(7)` includes Touch, the client calls `get_touch`,
events arrive (receipt: `WAYLAND_DEBUG=1` client log).

Proof without a rebuild: set the UI scale to 100% (output scale 1 →
factor 1) → the same Calculator **responded** to touch.

**Fix.** Track `set_buffer_scale` (was in the ignored request arm) in
`SurfaceInner`, carry it on `Window`, and apply it in `local_coords`
(`k = bw/buf.w * buffer_scale`); also fixed the non-maximized auto-size
(buffer px → surface-local). `cargo check` clean (macOS host).

**Build + deploy.** `bash bin/build.sh start gemshell` (macOS container;
rev `6a5242f`-dirty) →
`/nix/store/agipxng1i25ygry2am7x20pppjxd16gb-gemshell-0.1.0`. Only that
path was missing on the device (all 30 closure deps already present), so
`nix-store --export` → scp → `sudo nix-store --import`, then the
`gemshell-dev.sh` transient model (env copied from
`gemini-gemshell.service`).

**Verified on glass at 150%:** touch now works in the app (user
confirmed). ✅

**Known imperfection (future session — user-flagged).** Taps respond but
land **slightly offset** from the button. Likely cause: gemshell ignores
`xdg_surface.set_window_geometry` (`wayland.rs`), so GTK's CSD shadow
margin (~14 surface-local px) is not excluded when mapping the buffer
rect to the window. Fix ≈ honour `set_window_geometry` (and/or the CSD
inset) in the buffer→surface transform. Logged, NOT fixed.

**Other app-hosting issues observed (open):**
- **No session D-Bus** — the system-service compositor starts none, so
  GTK/portal apps log `Cannot autolaunch D-Bus without X11 $DISPLAY`
  and portals fail (`org.freedesktop.portal.Desktop` unavailable).
- Client-side **MESA-EGL `failed to get driver name for fd -1` /
  `failed to create dri2 screen`** warnings.
- `resvg` SVG-icon noise (`Images decoding was disabled by a build
  feature`, `usvg … marker-* 'none'`) — cosmetic.

**Device left:** marker = `gemshell`; the fixed binary runs as the
transient `gemshell-dev.service` (installed `gemini-gemshell.service`
stopped). ⚠️ **A reboot falls back to the installed, unfixed gemshell**,
where touch is dead at 150%; the interim is UI scale 100% (usable).
Nothing flashed; `para` untouched.

## 2026-09-26 (c) — passwordless mac USB link: root LaunchDaemon re-applies 10.15.19.1 automatically

Follow-up to (b): the user was prompted for their Mac admin password twice
during that cycle (once per link-drop, via `osascript … with administrator
privileges` around the `ifconfig`). Ask: make the flow user-less.

**Cause.** The host end of the link loses `10.15.19.1/24` on every device
power cycle (golden rule); `ifconfig` needs root, and macOS `sudo`
timestamps are per-tty — so scripts/agents cannot re-apply it unattended.

**Fix — macOS-only root LaunchDaemon (rule 6, committed):**
- `bin/macos/usb-nic-up.sh` — polls for the gadget MAC
  (`42:00:15:19:82:`) and assigns `10.15.19.1/24` when it is missing;
  `--once` for a single pass.
- `bin/macos/com.gemini.usb-nic.plist` — `RunAtLoad` + `KeepAlive`, log
  `/var/log/gemini-usb-nic.log`.
- `bin/macos/install-usb-nic-daemon.sh` — install / uninstall / status /
  selftest; installs to `/usr/local/libexec/gemini-usb-nic.sh` +
  `/Library/LaunchDaemons/com.gemini.usb-nic.plist`.

Installed on this Mac with **one** admin prompt (`osascript … install`
with `GEMINI_SELFTEST=1`): the selftest deleted `10.15.19.1` and the
daemon restored it within the 5 s poll window. Receipts:
`launchctl print system/com.gemini.usb-nic` → `state = running`;
`/var/log/gemini-usb-nic.log` → `assigned 10.15.19.1/255.255.255.0 to
en12`; ping OK. From now the link comes up across device reboots/replugs
with no password. Linux unaffected (`sudo -n` already covers it).

Docs: `docs/usb-network.md` new §"Passwordless USB-NIC link" + the
`sudo -v` notes rewritten; `docs/macos-build.md`; `AGENTS.md` cheat-sheet
note + rule-6 host line.

## 2026-09-26 (b) — first kernel build + flash + verify from the MacBook: CDC-ECM proven on glass, no Linux host involved

Ask: *"try a kernel build/deploy/verify from this macbook now that we've
implemented USB ethernet compatibility. device is currently in twrp."*

Outcome: ✅ the whole macOS cycle ran end to end for a **boot.img** (the
kernel + minimal initrd), and the CDC-ECM USB NIC is now **verified on
glass from macOS**: build → TWRP flash over adb → boot → link up → reboot
cycle. No Linux host involved; nothing on the Linux side touched.

**Build (macOS → Apple `container` aarch64 NixOS VM).**
`bash bin/build.sh start bootimg` (host rev
`f542127b90d055f1db4afb43e459533706aa6e45`), egress **DIRECT** (no
Tailscale proxy needed this time), rc 0. It reproduced the CDC-ECM image
**byte-for-byte**:
`/nix/store/0h3hkydixy98r6vchianjib1nmrg8m59-mobile-nixos_planet-geminipda_boot.img`,
9,984,000 B, sha256
`2e8f43adccc3738eef36927bf9fe2618b4755d559efe09c7c94d9eca7f06d26e` —
identical to the 2026-09-17 build (the kernel code is unchanged since
`96f24b5`), so this rebuild proves reproducibility rather than adding a
new hash.

**Bug found + fixed (rule 6).** `bin/boot-switch.sh`'s `backup_para()`
ran `adb pull … stock-dump/para.bin` **without creating `stock-dump/`**,
so on a clone that has never flashed (this Mac) the first flash aborted
with `adb: error: cannot create file/directory '…/stock-dump/para.bin'`.
Added `mkdir -p "$(dirname "$PARA_BACKUP")"` (the boot-backup path already
had its own `mkdir -p`). No device writes had happened at that point.

**Flash (Mac, TWRP already up → adb only).** `bash bin/flash-nixos.sh boot`:
`preflight` first (all ok: artifact+ledger match,
`bootopt=64S3,32N2,64N2`, `fbcon=rotate:3`), then
- backed up `para` → `stock-dump/para.bin`,
- backed up current `boot` → `stock-dump/boot-20260926-175629.img` (16 MiB),
- pushed 9,984,000 B → p22 `boot` (`dd`, 9+1 records). rc 0, stayed in TWRP.

**Boot + link (macOS, CDC-ECM proven).** `bash bin/flash-nixos.sh
boot-nixos` (para cleared + reboot). macOS enumerated the gadget with **no
RNDIS**: `ioreg -p IOUSB` → `idVendor=0x0525 (1317)`,
`idProduct=0xA4A1 (42145)` ("Linux 6.6.0 with mtu3") — i.e.
**`0525:a4a1`, the CDC-ECM gadget** (the pre-change RNDIS id was
`0525:a4a2`). Interface `en12` came up carrying the host MAC
`42:00:15:19:82:00`, took `10.15.19.1/24` (root — macOS `ifconfig`), and
`10.15.19.82` answered ping AND ssh. On-device `dmesg`: `cdc_ether`
registered, `g_ether gadget.0: HOST MAC 42:00:15:19:82:00 / MAC
42:00:15:19:82:01 / g_ether ready`, `mtu3 … gadget (high-speed) pullup
D+`; `/proc/cmdline` carries the `g_ether.*` params and `fbcon=rotate:3`
(rule-5 build, display untouched). Kernel `6.6.0 #1-mobile-nixos`.

**Reboot cycle (golden rule).** `sudo systemctl reboot` (via a `cjdell`
login — see below) dropped the link; `en12` came back with **no address**
(as documented), re-applying `10.15.19.1/24` + ping recovered in <5 s. New
`boot_id` `f9bb2f93-f4f1-4d34-b3f2-419e7899e1fb` vs
`34774926-a8be-4c51-b1a1-0dab221ba6ae` before; product id still
`0x0525:0xa4a1`.

**⚠️ Gap surfaced: the running rootfs predates the repo-key commit.** The
device runs **gen 54 (2026-09-11)**; its
`/etc/ssh/authorized_keys.d/root` holds exactly one key — the **legacy
Linux-workstation key** whose private half is *not* in this repo (the
transitional key kept deliberately by `1020d4e`). So the canonical host
scripts that ssh as **root with `keys/gemini_ed25519`** —
`device-ssh.sh` / `device-reboot.sh`, and `flash-nixos.sh`'s
converge-to-TWRP-from-a-running-Linux hop — **cannot log in until the
device is redeployed/reflashed with a generation ≥ `1020d4e`**.
- `root` is `prohibit-password` → no password fallback.
- `cjdell` accepts the documented passcode (`0000`) and is in `wheel`
  with passwordless sudo, so the on-device verification above went through
  `cjdell` + an askpass script; `bin/device-ssh.sh` stays root-only.
- Flashing **from TWRP/Android needs no ssh at all**, so the Mac cycle is
  fully usable whenever the device is already in recovery (as here).

**Version lines (rule 0).** kernel built in-repo, `linux 6.6.0`; boot.img
sha256 `2e8f43ad…` (store `0h3hkydi…`); boot backup
`stock-dump/boot-20260926-175629.img`; device left booted into NixOS
(gen 54) on the new boot.img, para cleared, USB CDC-ECM link up on the
Mac.

**Repo changes:** `bin/boot-switch.sh` mkdir fix; this log;
`docs/usb-network.md` status → flashed/verified + the key gap;
`docs/macos-build.md` "Owed" → the mac flash cycle is exercised. Nothing
committed.

Next: bring the device rootfs to a generation ≥ `1020d4e` (a `toplevel`
deploy or rootfs reflash) so the root-key ssh paths — and the
from-running-Linux converge hop — work from the Mac; then drop the
transitional legacy key from `config/gemini.nix`.

## 2026-09-26 — G1 recorded: chronic battery depletion, and doubt that the unit ever truly powers off

Ask: *"note in the project a long standing goal. this device is suffering
chronic battery depletion issues, often going below the safe voltage and
requiring a bench power supply to recover a safe voltage. we need to make
the device actually deep sleep/shutdown as i'm not convinced we've ever
managed to switch it off fully"*.

**Device access (macOS host, no Linux involved).** Device was in **TWRP**
(adb, via the darwin devshell: `nix develop --command adb devices` →
serial `CQMJJREEINNRSKGE  recovery`). No flash/install of any kind.

**Battery reading (TWRP, USB attached).** TWRP sysfs is stale as
documented — `POWER_SUPPLY_CAPACITY=-1`, `status=Not charging` — so the
dmesg `[PE+]` line is the truth:

- `Vbat=3564 mV`, `AvgVbat≈3499 mV`, **charging**: `CHGEN=1`,
  `ChrStat=2`, `Ichg=400`, `Ibat≈+440–480 mA`.
- `Vbus=5100`, **`PE+ is not connected`** (plain-USB fallback, Ilim 500 mA
  — no high-voltage/PE+ negotiation).
- battery temp 26–27 °C, health Good. SMB side reported `CAPACITY_SMB=50`
  (untrustworthy — no fuel gauge).
- **VBAT 3.56 V is already below the 3.65 V warn threshold** while on the
  charger: the pack was substantially depleted. The guard's crit
  (orderly poweroff) is 3.50 V and the vendor hard-off 3.40 V.

**Decision.** Recorded the ask as a first-class long-standing goal in the
new **`docs/standing-goals.md`** — **G1: stop chronic battery depletion;
make the device truly sleep and truly power off**. Key point written there:
the 2026-09-10 "poweroff verified" result (`docs/power-states.md`) was
measured only as *"the USB gadget disappears, no preloader/NIC loop"*,
which a merely-halted AP with its USB transceiver off produces just as
well. **It did not measure post-shutdown battery current**, so it is not
evidence the PMIC actually cut the rails — consistent with the user's
suspicion that the unit never fully switches off. The ~1.6 W awake floor
(`docs/power-sleep.md`) draining "off" would exactly explain the chronic
deep discharge. G1's first action is therefore to **re-run the poweroff
test with a current measurement/bench supply**, then build the s2idle wake
source.

**Repo changes (docs only; nothing built or flashed):** new
`docs/standing-goals.md`; README power bullet + Documentation table;
AGENTS.md "Where things live" row.

**Device left:** in TWRP, on USB and charging (~3.56 V and rising, ≈+
0.44 A). No boot-image or partition writes. Safe.

Next action: G1 step 1 — poweroff with a measured current draw (USB data
detached), then decide whether the `mt6797-power` driver's RTC_BBPU path
needs instrumenting.

## 2026-09-17 (d) — the SSH identity moved into the repo (login key + pinned host key)

Ask: *"create a new pub/private key pair for the gemini and add to this
repo. security is not a concern for this device in its current state. also
use this for the host key so SSH doesn't complain on new connections after
reflashing from scratch. no important files should live outside of the
repo. when done, commit"*.

**What was wrong before.** The admin key lived at
`~/.ssh/id_ed25519_gemini` (outside the repo), its pubkey was pasted inline
into `config/gemini.nix`, and a from-scratch rootfs needed a manual
re-provision pass (password login + `cat >> authorized_keys`). On the Mac
that key does not exist at all, so the ssh half of the flash cycle was
dead on arrival. Separately, sshd's host key was the NixOS-generated one,
so **every reinstall changed the device's identity** — which is precisely
what produces `REMOTE HOST IDENTIFICATION HAS CHANGED` and stale
`known_hosts` lines after a reflash.

**Now.** New keypair committed at **`keys/`** (ed25519, fingerprint
`SHA256:5P4/WXMpdUQ0G0K5p69d1pCiYwoOwp7EWJykjxBnFKI`), and it plays both
roles:

1. **login key** — `config/gemini.nix` declares
   `users.users.{root,cjdell}.openssh.authorizedKeys.keyFiles =
   [ ../keys/gemini_ed25519.pub ]`, so a fresh rootfs is ssh-reachable with
   zero provisioning (the manual recipe is retired — the note in
   `bin/device-ssh.sh`'s header is gone). The legacy inline workstation
   pubkey STAYS, marked transitional: the rootfs currently on the device
   still trusts only it, and the workstation's private half is not in this
   repo. Drop it once everything in play has been reflashed.
2. **pinned sshd host key** — `services.openssh.hostKeys` is overridden to
   a single ed25519 entry at `/etc/ssh/ssh_host_ed25519_key`, and
   `system.activationScripts.gemini-ssh-hostkey` installs the repo key
   there as a real 0600 root file (verified: the activation text resolves
   to `/nix/store/n9r7…-gemini_ed25519` → `install -m 0600`). It must be an
   activation script and not `environment.etc`: /etc entries are symlinks
   into the world-readable store and **sshd refuses group/other-readable
   host keys**. Consequence (accepted, documented): the private key is in
   git *and* in the world-readable store. `generateHostKeys` is left at its
   default so the sshd-keygen unit is the failsafe — it is conditioned on
   the file being absent, so it no-ops while activation works.

**Nothing outside the repo.** All host scripts now default to the repo
key: `bin/lib/host.sh` gained `GEMINI_KEY_DIR` / `GEMINI_SSH_KEY` /
`GEMINI_KNOWN_HOSTS` + `gemini_ssh_key()`, which also fixes the fresh-clone
permission trap (git stores only the exec bit, so a clone is 0644 and ssh
refuses it — the helper chmods 600 and says so). Wired into `device-ssh`,
`device-reboot`, `flash-nixos` (non-fatally, so `preflight` still runs and
reports a missing key), `repartition-nixos`, `deploy.sh` (POSIX sh, so a
plain `${GEMINI_SSH_KEY:-$repo/keys/gemini_ed25519}` + chmod there) and
`gemshell-dev.sh`. `keys/known_hosts` ships the pinned host key for
`10.15.19.82` so a human/tool can verify against the repo; the scripts
keep their long-standing `StrictHostKeyChecking=no` +
`UserKnownHostsFile=/dev/null` (a flash must never be blocked by host-key
state — and with a pinned host key the steady state is clean anyway).
`preflight` now checks that `known_hosts` still matches the key's public
half, so the two cannot drift.

**Verified.** `nix eval`: `services.openssh.hostKeys` = the single pinned
ed25519 entry (NixOS's default rsa+ed25519 is genuinely replaced),
`authorizedKeys.keyFiles` = the repo pubkey for root *and* cjdell, and the
activation script text installs from the store path at 0600. `preflight`:
ssh key ✅ + hostkey ✅ (only `sudo -v` and the deliberately unbuilt
`system.img` remain as warnings). The chmod trap was tested by setting the
key 0644 and re-resolving it. Caveat worth knowing: the flake cannot see
`keys/` until it is `git add`ed (a git flake evaluates the committed tree)
— it is in this commit for that reason.

**Flagged for the future (in `keys/README.md`, not just here):** the
private key is public in git + world-readable in the store. That is fine
for this single-user lab device and is what the ask chose, but the README
lists what to do the day it stops being acceptable (new key outside the
repo, rotate `authorizedKeys` + host key in one change).

## 2026-09-17 (c) — USB NIC: RNDIS → CDC-ECM, so the Mac can drive the reboot/flash cycle

Ask: *"move from RNDIS USB NIC to a more cross-platform USB NIC driver to
enable the full mac auto reboot/flash cycle of the PDA. implement this"* —
i.e. do the thing the 2026-09-12 analysis recommended.

**Why (the receipt).** `drivers/usb/gadget/legacy/ether.c` (v6.6, the base
this repo builds from): when `CONFIG_USB_ETH_RNDIS=y`, the gadget registers
**two** configurations and RNDIS goes **first** — *"register our
configuration(s); RNDIS first, if it's used"* — so every host picks it and
the device descriptor is overridden to `0525:a4a2`. macOS has no RNDIS
driver (only CDC-ECM/NCM; HoRNDIS is dead on Apple Silicon), which made
`10.15.19.82` unreachable over USB from this Mac. With RNDIS off, ether.c
registers **one** configuration: CDC-ECM, NetChip ids `0525:a4a1`
(`CDC_PRODUCT_NUM`), `bDeviceClass = USB_CLASS_COMM`.

**Device change (2 symbols + generator).** `# CONFIG_USB_ETH_RNDIS is not
set`, plus `CONFIG_USB_CONFIGFS_RNDIS` and the promptless
`CONFIG_USB_F_RNDIS` (only those two select it; olddefconfig resolves it).
`bin/prune-kernel-config.sh` gained **section 9c** so a config regeneration
cannot silently bring RNDIS back, and the generated banner records the
policy. Everything else is unchanged on purpose: still the precomposed
`g_ether` (`CONFIG_USB_ETH=y`), still auto-instantiated by the kernel when
the mtu3 UDC appears, still MACs from the `g_ether.*` kernel params, still
`usb0`.

**Why not configfs/NCM.** configfs (`ecm.usb0`/`ncm.usb0`) would allow a
runtime choice and is the modern idiom, but it needs a userspace binder
service to wait for the UDC and bind — and `usb0` is this unit's ONLY
debug link, so a mis-ordered service costs ssh access, which is a strictly
worse failure than "the Mac can't see the NIC". Legacy-ECM also fails
safe: if `can_support_ecm()` were false the driver falls back to CDC
Subset (`049f:505a`, Linux-only) instead of losing the link. NCM (the
Windows-11-friendly function) stays compiled in (`CONFIG_USB_CONFIGFS_NCM=y`)
for a future configfs step; RNDIS is deliberately gone.

**macOS side (evidence, not assumption).** On this Mac (macOS 26.6.2):
`com.apple.driver.usb.cdc.ecm` (`AppleUSBECM.kext`) is loaded, and its
personalities match `IOUSBHostInterface` **class 2/subclass 6**
(`AppleUSBECMControl`) and **class 10** (`AppleUSBECMData`) — exactly the
control/data descriptor pair `f_ecm` emits. `AppleUSBDeviceNCM.kext`
exists too but its personalities key on an `IOPropertyMatch`
(`USBDeviceFunction`) rather than the CDC descriptors, and `lsusb` does
not exist on darwin. Linux's `cdc_ether` binds class 2/6 generically.

**Host scripts (Linux behaviour kept, darwin branch added).** New
**`bin/lib/host.sh`** — the platform differences in ONE place: platform
detection, **MAC-based interface discovery** (`42:00:15:19:82:*`; the host
end `…:00` arrives in the ECM functional descriptor and both host drivers
adopt it), `ip` vs `ifconfig` net-up, `ping -W seconds` vs `-W ms`,
`lsusb` vs `system_profiler SPUSBDataType`, and the devshell re-exec
(`gemini_need_devshell`: adb + `lsusb` on Linux, adb + GNU `timeout` on
macOS). Then: `net-up.sh`, `device-ssh.sh` (the `GEMINI_DEV_IP`
Wi-Fi/LAN override is preserved), `device-reboot.sh` (**`lsusb` removed**
entirely — the reset is proved by link/ssh drop + boot_id change, and the
host address is re-applied from the lib after re-enumeration),
`boot-switch.sh` and `flash-nixos.sh` (USB probes via the lib; the
post-WDT "TWRP appeared" wait is now adb-based instead of
`lsusb | grep 18d1:4ee2`; artifact defaults fall back to
`~/.cache/gemini-macos/out`), `usb-watch.sh` (VID map + lib).
`flake-macos.nix`'s darwin devshell comment now states its second job
(adb + GNU coreutils for the device scripts; it already carried
them).

**Built, not flashed.** The kernel config change was built through the
prescribed path — `bash bin/build.sh start bootimg` (macOS → the aarch64 VM)
— **rc 0**: boot.img `0h3hkydixy98r6vchianjib1nmrg8m59-…`, 9,984,000 B,
sha256 `2e8f43adccc3738eef36927bf9fe2618b4755d559efe09c7c94d9eca7f06d26e`,
kernel payload 8,032,557 B (**4,610 B smaller** than the RNDIS build) and
`bin/dump-bootimg-header.sh` still shows `bootopt=64S3,32N2,64N2` +
`fbcon=rotate:3` + the g_ether MAC params. Proof in the built kernel's own
output: `find $out -name '*rndis*'` → nothing (no `usb_f_rndis.ko` in the
gadget function dir), no `usb_f_ecm.ko` (ECM is built-in), `usb_f_ncm.ko`
kept. Also caught by the build: the mac artifact dir was publishing ONLY
hash-prefixed store names, so `bin/macos/vm-build.sh`'s collector now also
writes the conventional `boot.img`/`system.img` alias the flash scripts
default to.

**Reflash owed** (`bin/flash-nixos.sh boot`), then the on-glass
verification list — new doc **`docs/usb-network.md`**: the enumeration id
to expect (`0525:a4a1` good / `a4a2` stale image / `049f:505a` ECM
rejected), net-up + ssh on both hosts, the reboot cycle, and the rollback.
No display code touched (rule 5 not involved), but the usual para-sticky
safety applies.

**Flash readiness (made verifiable).** Added a read-only **`preflight`**
verb to `bin/flash-nixos.sh` (rule 6: the "could a flash run from THIS
host?" question is now one command, not an ad-hoc chain). It touches
nothing on the device and checks: the artifact set **and verifies every
artifact against the build's `.sha256` ledger** (mapping the ledger's
in-VM `/out/<name>` paths onto this host's artifact dir — the Rule-0 "the
copy we flash is the bytes the build produced" check), `bootopt=64S3,32N2,64N2`
+ `fbcon=rotate:3` inside the boot.img (LK hang trap + the rule-5 build),
the toolchain (adb + GNU coreutils), the SSH key, passwordless sudo, and
the device state if one is attached; exit non-zero only for a REQUIRED
miss. First run (M5, CDC-ECM boot.img built, device not attached):
**flash possible** — `boot.img` ✅ 9,984,000 B sha256 `2e8f43ad…` and
matches the ledger, both cmdline fields ✅, toolchain ✅ (adb 37.0.0 +
coreutils 9.11 from the darwin devshell — so the flash path's
`timeout`/`stat`/`sha256sum` assumptions hold on macOS), with two warnings:
no `~/.ssh/id_ed25519_gemini` on this Mac and no cached sudo. Both are
user actions, not blockers for a TWRP-adb flash (the key is only needed to
converge a device sitting at the NixOS desktop); `system.img` is missing
too but only a full reflash needs it. Recorded in `docs/usb-network.md`
§"Readiness on 2026-09-17".

**Docs:** new `docs/usb-network.md`; corrected the stale claims in
`docs/mobile-nixos-port-feasibility.md` §3.7 (it said "CDC ECM" while the
receipt was RNDIS), `docs/building.md` and `docs/macos-build.md` ("flashing
from a Mac is future work" → now done bar BROM recovery), README,
AGENTS.md (status + tables + cheat sheet), and bracketed `[resolved
2026-09-17]` notes on the 2026-09-12 analysis rather than editing it.

**Side effect worth knowing:** now that macOS HAS a driver for the NIC, the
QEMU-USB-passthrough idea from 2026-09-12 is closed *for the NIC
specifically* — macOS libusb cannot detach a kernel driver, so a guest
could not take it (noted in the log at that entry).

## 2026-09-17 (b) — build system reorganised: one platform-dispatched entry point, mac half isolated

Ask: "organise the build system such that the mac specific stuff is
cleanly separate from linux native… agents must know automatically when to
invoke which build system… nix-on-darwin is assumed to be available,
utilise this but keep the mac specific parts of the flake cleanly
separate… must not break what was already working linux native… commit".

**Shape now (rule 10 + `docs/building.md`):** `bash bin/build.sh <verb>
[TARGET]` is the ONE entry point; it detects the platform and dispatches,
with the same verbs and rc protocol on both sides:

| Piece | Scope |
|---|---|
| `bin/build.sh` | cross-platform dispatcher (`start`/`wait`/`log`/`status`/`shell`/`vm`) |
| `bin/build-linux.sh` | Linux: the untouched native model — `sudo nix build --store local` + `#packages.aarch64-linux.<T>` + `--option builders @/etc/nix/machines --fallback` under `bin/run-job.sh`; `toplevel` → `bin/deploy.sh build` (GC pins kept) |
| `bin/macos/{build.sh,vm-build.sh,proxy.py}` | everything macOS-specific (was `bin/build-macos*.{sh,py}`) |
| `flake-macos.nix` | darwin-only flake outputs, merged by `mergeOutputs` |
| `docs/building.md` | the decision map (targets, artifacts, gotchas) |

**Nix-on-darwin now carries the mac-native work** (`flake-macos.nix`,
imported by `flake.nix` and merged — the Linux outputs are otherwise
untouched):

- `devShells.aarch64-darwin.default` — plain `nix develop` on a Mac lands
  here (the current-system default, symmetric with
  `devShells.x86_64-linux.default`); python3 / rsync / cacert / adb / git /
  curl, so nothing needs Homebrew or the Xcode CLT. Verified on the M5:
  python3 3.14.7, **rsync 3.5.0** (real rsync, not openrsync), adb 37.0.0,
  all substituted. `bin/macos/build.sh` re-execs into the devshell
  (`GEMINI_MACOS_REEXEC` guard) when the host lacks python3/rsync.
- `packages.aarch64-darwin.caBundle` — the CA bundle the VM mounts;
  preferred over the old keychain scrape, which stays as the fallback.

**Two bugs/gotchas caught while wiring it (kept as receipts):**

1. **`//` is a SHALLOW merge.** Merging the mac outputs at the flake's top
   level swapped the *whole* `packages` attrset for the mac one and broke
   `packages.aarch64-linux` — the first dispatch run died with
   `error: attribute 'aarch64-linux' missing` at
   `f.packages.aarch64-linux.bootimg`. Fixed with the `mergeOutputs`
   helper (devShells/packages merged one level in). This is exactly why
   the merge is explicit + commented in flake.nix.
2. **Nix only sees *tracked* files.** `nix develop`/`nix eval` from the
   repo evaluate the committed tree, so the new `flake-macos.nix` was
   invisible until `git add`ed (`Path 'flake-macos.nix' … is not tracked
   by Git`). The Mac *build* path is immune — the wrapper rsyncs the
   working tree into the VM — but anything nix-side on the Mac is not.

**Verified the Linux side did not move (the ask was "must not break what
was already working"):**

- `nix eval .#packages.aarch64-linux.bootimg.outPath` →
  `/nix/store/gman244d9xaikqw2gzrc2nr90fw103gp-mobile-nixos_planet-geminipda_boot.img`
  — the *same* path as before the reorganisation;
- end-to-end through the new dispatcher,
  `bash bin/build.sh start bootimg` → `wait bootimg` rc 0 → identical
  sha256 `b2404b13b8dbc861dbe9e0de43cd3bdb08f1b99ab2de6477b844256610eef997`;
- `devShells.x86_64-linux.default` still evaluates on Linux.

**Files touched:** new `bin/build.sh`, `bin/build-linux.sh`,
`flake-macos.nix`, `docs/building.md`; moved `bin/macos/*`; updated
`flake.nix` (merge helper + header), `AGENTS.md` (new **rule 10**, the two
rows), `README.md` (build section + doc table), `docs/macos-build.md`
(dispatcher/devshell sections, gotchas, owed list).

**Not done (explicitly out of scope):** the flash workflow — building is
platform-independent now, flashing still is not (`docs/macos-build.md`
"Owed / next"). Nothing was flashed in this session either.

## 2026-09-17 — REAL macOS build: a flash-ready `boot.img` built from the Mac (nothing flashed)

Ask: "try to make a firmware build (but dont flash) using only this mac
just to see if its possible".

**Answer: yes — done.** `packages.aarch64-linux.bootimg` built end-to-end
on the M5 Mac (macOS 26.6.2, 10 CPU / 32 GiB) inside the aarch64 NixOS VM
(the 2026-09-12 pi5-technique probe is now an implemented, scripted path —
`docs/macos-build.md`).

| | |
|---|---|
| Artifact | `gman244d9xaikqw2gzrc2nr90fw103gp-mobile-nixos_planet-geminipda_boot.img` |
| Size | 9,988,096 B (9.5 MiB — 16 MiB partition, ~6.5 MiB headroom) |
| sha256 | `b2404b13b8dbc861dbe9e0de43cd3bdb08f1b99ab2de6477b844256610eef997` |
| Source rev | `ad4eb9d33eb9e7dfef58bbaaa5b31f5061f0bd38-dirty` (this tree) |
| Cost | ≈6.5 min: 380 paths substituted (486 MiB), 17 drvs compiled on 10 cores — **exactly the 2026-09-12 dry-run prediction** (17 local / 380 substituted) |
| Host VM | `docker.io/rzmapp/nixos-vm:26.05` — aarch64, Nix 2.34.8, kernel 6.18.15, `/nix/store` 7.1 GiB after the build (persisted) |

**Verified, not assumed** (`bin/dump-bootimg-header.sh`, now macOS-clean;
+ an FDT scan of the appended DTB): magic `ANDROID!`; kernel
0x40200000 (8,037,167 B = 7.66 MiB); ramdisk 0x45000000 (1,947,027 B);
second 0x40f00000 size 0; tags 0x44000000; page 2048; cmdline carries
both **`bootopt=64S3,32N2,64N2`** (the LK requirement — AGENTS.md,
phase-2 §2a) and **`fbcon=rotate:3`** (this is the fbcon/exclude-display
build of rule 5, no display-landmine driver merged). The appended DTB
sits 25,264 B from the end of the (gzip) kernel payload — inside LK's
last-2 MiB FDT scan — and carries `planet,gemini-pda` + `mediatek,mt6797`
plus this repo's delta nodes (`planet,geminipda-drm`, `geminipda-fb`).
**Nothing was flashed**; the glass/panel rule was never in play.

**The blocker hit first, and its receipts (worth keeping):** the initial
run died in eval — `unable to download '…/mobile-nixos/…tar.gz': Timeout
was reached (28)`. Cause: this Mac's default route belongs to a
**Tailscale exit node** (`netstat -rn` → `default … utun4`;
`tailscale status` → `grafton-router … active; exit node`), and Apple's
`container` vmnet NAT does not survive that. Measured: VM →
192.168.64.1 OK (ping 0.4 ms), VM → the Mac 192.168.1.136:22 OK, VM →
github:443 / 1.1.1.1:443 / cache.nixos.org **all timeout**, while the
Mac's own curl gets 200 from both. A container restart *and* a full
`container system stop/start` (incl. `container-network-vmnet.default`)
did **not** fix it → host routing, not stale container state.

**Fix (no VPN was touched):** one committed script lends the VM the
Mac's egress — `bin/macos/proxy.py` (stdlib, `/usr/bin/python3`),
a CONNECT/HTTP proxy bound to the container bridge gateway only
(`192.168.64.1:3128`, clients limited to `192.168.64.0/24`), with the
build run under `http_proxy`/`https_proxy`/`all_proxy`. Nix honours
those for substituters and, via `proxyImpureEnvVars`, for fixed-output
fetchers (the two `fetchTree` inputs + the kernel tarball) under the
sandbox. Verified through it: github 200, cache.nixos.org 200,
cdn.kernel.org 200. `bin/build.sh net` auto-detects and only
engages this when the VM genuinely has no egress.

**Committed this session (rule 6 — the probe was a throwaway
`logs/gemini-macos-probe.sh`, now superseded):** `bin/build.sh`
(stage/start/wait/log/status/net/sh/stop; the run-job rc protocol:
0 done-ok / 1 failed / 2 running — rule 8b), `bin/macos/vm-build.sh`
(the in-VM native build + artifact/identity collection to `/out`),
`bin/macos/proxy.py`, `docs/macos-build.md`.
`bin/dump-bootimg-header.sh` gained a BSD-`stat` fallback so it runs on
macOS. Staging is `~/.cache/gemini-macos/src` (38 MiB, rsync minus
`.git`/`logs`/`cargo target`), artifacts in `~/.cache/gemini-macos/out/`
with `<target>.manifest` + `.sha256` identity files (rule 0) — the
Mac-side re-hash matches the in-VM one.

**Not done / next:** nothing flashed from the Mac (a flash still wants
the TWRP/para safety cycle); `rootfs`/`toplevel` from the Mac (552 local
drvs, 2.9 GiB download — feasible, long, and the Mac's free disk is the
constraint); darwin branches for `bin/device-ssh.sh` /
`bin/boot-switch.sh` / `bin/flash-nixos.sh` + a darwin devshell, so the
Mac can drive the adb/LAN-ssh half too (the 2026-09-12 analysis still
stands: adb yes, RNDIS no, BROM no). Left running: the
`gemini-macos-builder` VM and the egress proxy (`bash bin/build.sh
stop` stops both).

## 2026-09-12 — macOS aarch64 build path: pi5 `container`-VM technique PROBED green (no implementation yet)

Ask: "are we able to build images/kernels using the same technique used
here: `~/Projects/gc-business/gc-rust-node/pi5`?"

**What the pi5 technique actually is** (two separable things):

1. **Build environment** — on Apple Silicon, build `aarch64-linux`
   derivations inside Apple's `container` runtime running the
   persisted aarch64 NixOS VM `rzmapp/nixos-vm:26.05`; the VM's
   `/nix` store survives runs, so iteration only rebuilds changes.
   (a staging copy of the repo is bind-mounted read-only at `/build/src`,
   the macOS CA bundle at `/etc/ssl/certs`, output dir at `/out`;
   build via `nix build --impure --expr 'builtins.getFlake "path:…"'`.)
2. **Image shape** — NixOS's `system.build.sdImage` (FAT firmware +
   ext4 root holding the whole store), i.e. an SD-card image for a
   standard bootloader/eeprom chain.

**Probe (host: M5 Mac, macOS 26.6.2, 10 CPU / 32 GiB, Nix 2.34.7;
`/usr/local/bin/container` present, image + `gc-pi5-builder` already
pulled/stopped).** Created `gemini-macos-builder` (10 CPU, 12 GiB) with
this repo staged read-only, and DRY-RAN the flake's real aarch64 outputs
inside the VM (aarch64, Nix 2.34.8, 15 GiB RAM, 443 GiB free disk on
`/dev/vdb`):

| Output | derivations to build locally | paths substituted |
|---|---|---|
| `packages.aarch64-linux.kernel` | 6 | 290 (292 MiB dl / 999 MiB unpacked) |
| `packages.aarch64-linux.bootimg` | 17 | 380 (486 MiB / 1.6 GiB) |
| `packages.aarch64-linux.rootfs` | 552 | 1538 (2.9 GiB / 9.2 GiB) |
| `nixosConfigurations.gemini` toplevel | 549 | 1532 (2.9 GiB / 9.2 GiB) |

All four evaluated and printed derivations with **rc 0** — eval, the
flake's two `builtins.fetchTree` inputs (MNX `2c132754` + nixpkgs
`dc5d91f84032`), TLS, and the binary-cache split all work in the VM.
The 552 "build locally" set is the expected mix: NixOS config glue
(unit/dconf/udev/etc derivations, never cached) plus exactly the
repo's custom/patched drvs — `linux-6`, `mesa`, `wlroots`, `gemshell`,
`gemcli`, `gemdemo`, `gemini-exodus`, `gemini-firmware`,
`power-profiles-daemon`, `glibc-locales`, `rustc`/`cargo` crates. The
heavy GNOME closure substitutes. So a macOS build is feasible and the
M5 (10 cores) should beat the 192.168.49.191 Pi builder.

**Image shape does NOT transfer.** The Gemini boots via MediaTek LK —
MTK-header `boot.img` (kernel+initrd+`bootopt=64S3,32N2,64N2`) on p22,
rootfs on p27 `linux` — with no FAT firmware partition, no eeprom, no
extlinux/U-Boot, so `system.build.sdImage` is inapplicable. The repo
already produces the analogous self-contained artifacts
(`outputs.android.android-bootimg` + `outputs.generatedFilesystems.rootfs`,
Mobile NixOS's Android image generator), and those stay as-is. Only
axis (1), the build environment, is worth adopting here.

**Device cycles from the Mac (flash/reboot/RNDIS/adb/preloader) —
analysis only; device not attached, nothing flashed/tested (2026-09-12).**

- **adb: YES.** This Mac already has `adb` (Homebrew,
  `1.0.41 / 37.0.1`) — macOS adb drives the TWRP/Google gadget
  (`18d1:4ee2`) natively, so the adb half of the flash cycle is viable.
  The flake has **no darwin devshell** (`devShells.x86_64-linux` cannot
  run here) — but brew covers adb, so a darwin branch is enough.
- **g_ether USB link: NO (platform block).** The device enumerates as
  `0525:a4a2`, the Linux **RNDIS** gadget (repo VID map `bin/usb-watch.sh`
  + session-log 2026-09-10e; `docs/mobile-nixos-port-feasibility.md §3.7`
  loosely calls it CDC-ECM — the receipt says RNDIS). macOS ships **no
  RNDIS driver** (CDC-ECM/NCM only), so `10.15.19.82` will not come up;
  HoRNDIS is an unmaintained kext and not an option on Apple
  Silicon/macOS 26. Workarounds: reach the device on the **LAN/Wi-Fi**
  (`bin/device-ssh.sh` already honours `GEMINI_DEV_IP`; the container VM
  reaches the LAN via vmnet NAT), or **switch the gadget to CDC-ECM**
  (kernel config + boot.img reflash — rule 5 applies).
  **[resolved 2026-09-17: the gadget IS switched to CDC-ECM
  (`0525:a4a1`) — session-log 2026-09-17 (c) + `docs/usb-network.md`;
  the LAN/Wi-Fi workaround is now the fallback, not the only path.]**
- **Preloader/BROM recovery: NO path today (the real loss).**
  `bin/run-mtk.sh` needs the linux devshell's store `mtkclient`
  (`/nix/store` glob), `/usr/local/lib/mtkclient-patched` and store
  python3.13 — none exist on macOS, and Apple's `container` has **no USB
  passthrough**, so the *verified* MTK toolchain cannot be reused from
  this host. A native aarch64-darwin mtkclient is theoretically possible
  (libusb) but unproven; BROM is the last-resort recovery for a hung
  boot.img, so it stays a Linux-host/device capability.
- **Reboot cycles: YES** (mechanism is just ssh — `bin/device-reboot.sh`
  arms WDT EXRST via `busybox devmem` — or `adb reboot`); only its
  gadget-drop *detection* greps `lsusb` and must be swapped for
  ping/ssh or `ioreg`/`system_profiler` on macOS.
- Host scripts are otherwise Linux-only: `ip` + iface names
  (`net-up.sh`, `device-ssh.sh`), `lsusb` (`usb-watch.sh`,
  `device-reboot.sh`, `boot-switch.sh`), and the `nix develop` re-exec
  (`boot-switch.sh`, `flash-nixos.sh`). A darwin branch + darwin
  devshell is the port (rules 6/7).

**USB passthrough (2026-09-12 follow-up):** Apple's `container` (what
runs `rzmapp/nixos-vm`) has **no USB passthrough** — CLI 0.12.3 exposes
only `--virtualization` (nested virt), ports, mounts and sockets; no
`--device`/`--usb`. So the aarch64 VM used for builds can never see the
device. **QEMU can**: this Mac has QEMU 11.1.0 built with libusb
(`usb-host`, `qemu-xhci` present — `vendorid`/`productid`/`hostbus`/
`hostport` options), so a NixOS aarch64 guest under QEMU could receive
USB via `-device qemu-xhci -device usb-host,…`. Caveat: macOS libusb
cannot `detach_kernel_driver`, so a device a kext has claimed can't be
taken. By device: `0525:a4a2` RNDIS and `18d1:4ee2` adb have no in-box
macOS driver (likely claimable — a QEMU guest would get `usb0`/adb);
`0e8d:2000` preloader is CDC-ACM (`AppleUSBCDCACMData` may claim it →
risky for mtkclient); `0e8d:0003` BROM (bulk) likely claimable. Only one
claimant at a time. Also the Apple container image is not QEMU-bootable
(Apple's own kernel/boot) — a real NixOS aarch64 qcow2 is needed.
**[2026-09-17: the gadget is CDC-ECM now (`0525:a4a1`), so this
particular trade has flipped — macOS DOES have an in-box driver for it
(`com.apple.driver.usb.cdc.ecm`, which claims the interface), so a QEMU
guest could no longer take that device via libusb. Irrelevant to the
normal path (the host uses the NICs directly), but it closes the
QEMU-as-USB-shim idea for the NIC specifically — see docs/usb-network.md.]**

**Verdict:** build (VM) + flash (adb) + reboot (ssh over LAN / adb) is
achievable from this Mac once the scripts get darwin branches; USB
ethernet (RNDIS) and BROM recovery are not, today.
**[2026-09-17: USB ethernet IS achievable now — the gadget is CDC-ECM
(`0525:a4a1`) and the scripts have the darwin branches via
`bin/lib/host.sh`; only BROM/preloader recovery stays Linux-only.
`docs/usb-network.md` has the receipts + the owed on-glass verification.]**

**Artifacts / state:** scratch staged at `~/.cache/gemini-macos/src`
(1.5 GiB), probe script `logs/gemini-macos-probe.sh` (gitignored),
container `gemini-macos-builder` created + stopped; `gc-pi5-builder`
left as found (stopped). **Not done:** no real build was run (dry-run
only), so no `.img`/kernel hash exists yet; no `bin/` script committed
(rule 6) and nothing in `flake.nix` parameterised (rule 5 not touched —
no build, no flash). Next action if wanted: promote the probe into a
committed `bin/build.sh` + a doc, then a real kernel/bootimg build.

## 2026-09-11 — macOS support: `bash bin/gemshell-nested.sh` now opens a native preview window

Ask: "i want this repo to also work on macOS. can you make this work:
`bash bin/gemshell-nested.sh`", clarified to "i just want to test the
desktop UI, doesn't need to be proper wayland" + "prefer traits over
compile time cfg blocks".

**The blocker, stated plainly:** macOS has no libwayland, no compositor
socket and no Wayland session, so the existing nested mode (a Wayland
*client* under the host compositor) cannot exist there at all. The host
is an Apple-Silicon Mac (macOS 26.6.2, Nix 2.34.7, host Rust 1.97.1) with
no Linux builder configured, so `.#packages.x86_64-linux.gemshell` cannot
run either.

**Design — platform backends behind traits.** The compositor core is now
platform neutral. New `pkgs/gemshell/src/platform/`:

- `mod.rs` — the `InputSource` / `Presenter` / `Backend` traits, the
  shared `InputEvent` enum, the shm helper (`memfd_create` on Linux,
  `shm_open` elsewhere) and `create_backend()` — **the only OS `#[cfg]`
  in the whole program**;
- `linux.rs` — the moved device/nested half: EGL/GBM context, the LK-fb
  dma-buf import + compute-blit `Presenter`, the evdev input wrapper, and
  the `poll()` loop (was `Compositor::run`);
- `macos.rs` — a **Cocoa window + desktop OpenGL 3.3 core** context
  (winit 0.29 / glutin 0.31 / glutin-winit 0.4), a
  `glBlitFramebuffer` `WindowPresenter`, and a winit event loop that maps
  mouse/keyboard onto the compositor's gestures/keysyms.

The renderer takes a `Glsl` dialect and now owns a `Box<dyn Presenter>`;
`Compositor` takes `(Renderer, Box<dyn InputSource>, Font, dummy_data)`
and exposes a small `pub(crate)` platform interface (`startup`,
`drain_background`, `advance`, `wants_frame`, `poll_timeout_ms`,
`render`, `poll_input`, `feed_input`). `mod.rs`/`ui.rs`/`shell.rs` are
otherwise untouched. The desktop-GL shaders (`*_CORE`) and a core-profile
VAO were added; `render.rs` no longer carries any EGL/GBM code.

Build wiring: `Cargo.toml` moves `system` (libwayland) into a
`[target.'cfg(target_os="linux")']` section and adds the macOS deps;
`build.rs` links `-framework OpenGL` on macOS and the Wayland/EGL/GLES
set on Linux; unused `xkbcommon`/`gl` crates dropped. `util::find_font()`
learned the macOS font paths, `gemdata-device`'s `reboot(2)` call is
`cfg(target_os="linux")`, and `bin/gemshell-nested.sh` branches on
`uname` (macOS: host `cargo build` + run).

**Verification (host-side only — no device touched):**
- `cargo check` clean on aarch64-darwin; the full `cargo build` links and
the binary **boots, creates the GL 3.3 core context and renders** —
`GEMSHELL_SCREENSHOT` wrote a detailed 2160x1080 scene-FBO PNG (~200 KB,
settings panel open).
- The Linux modules were type-checked on the Mac via a
`--cfg gemshell_check_all` escape hatch (`RUSTFLAGS='--cfg
 gemshell_check_all' cargo check`, check does not link) — clean. The
Linux backend was MOVED, not rewritten, so this is the strongest check
available without the aarch64 builder; a device/nix build is still owed.
- `bash bin/gemshell-nested.sh` runs end-to-end on macOS (builds + opens
the window).

**Not done / owed:** no on-glass or nix-Linux rebuild (the Linux `system`
Wayland backend differs from the rs backend used for the type-check);
`cargoLock` will re-fetch the new macOS deps on the next nix build.
Docs: `docs/gemshell.md` gained a "Platform backends" section.

## 2026-09-11 — Desktop cleanup: LXQt, Phosh, COSMIC, niri removed; light sleep made desktop-aware; device store GC'd

Ask: "remove lxqt, phosh and cosmic from the project, then GC the Nix
store on the device. also make sure the sleep mode is correct for the
currently running desktop environment", followed by "remove niri too".

**Support set is now GNOME + `gemshell` + fbcon console.** Removed:
`services/lxqt.nix`, `services/phosh.nix`, `config/lxqt/`,
`services/scripts/{start-lxqt-nested,start-phosh-shell,prepare-phosh-session}`,
`pkgs/labwc-geminipda.nix`, `pkgs/phoc-geminipda.nix`,
`docs/phosh.md`, the `services.desktopManager.cosmic.enable` and
`programs.niri.enable` blocks in `config/gemini.nix`, and the flake's
`labwc`/`phoc`/`phosh`/`squeekboard` outputs. `services/desktop-select.nix`
and `gemcli session` now enum `gnome|gemshell|console` (session.rs,
clap value-parser); `docs/desktop-selection.md` rewritten around the
three modes. `services/gnome.nix`/`services/gemshell.nix` still
force-disable the legacy `gemwl` compositor, which is now the only
in-tree nested (session-less) compositor.

**Sleep fix.** `gemdata-device/src/sleep.rs` `SERVICES` was still the
removed `gemwl.service` + `phosh-nested.service` + `lxqt-nested.service`
list. It now stops the LIVE panel owner — `display-manager.service`
(GNOME via GDM), `gemini-gemshell.service`, or legacy `gemwl.service` —
plus the three PipeWire units. `gemcli sleep status` reports the running
set and the marker/desktop is no longer consulted (the units are
authoritative).

**On glass (gen 54, `05b9a07`, toplevel
`bw374jj8g4ph317d8saqlw7bwf2nrx3j-nixos-system-gemini-26.11pre-git`):**
deployed with `bin/deploy.sh deploy` (no reflash; `display-manager` NOT
restarted — running GNOME session preserved, 0 failed units). Then a
full silver-button-equivalent cycle: `gemcli sleep on` → backlight off,
A53 1..7 offline, inputs unbound, **`display-manager.service` stopped**
(the running cjdell gnome-session ended), PipeWire stopped, wifi wlan0
down, state ASLEEP; `gemcli sleep off` → everything restored, GDM
restarted and **auto-logged cjdell back into gnome-shell** (session 2,
`gnome-shell-50.4` running, 0 failed units), backlight restored to 9 %.
So light sleep is correct for the currently-running GNOME desktop.

**Device GC:** `bash /root/gemini-nixos/bin/device-rebuild.sh gc` →
`nix-env --delete-generations old` (54 → only gen 54 left) +
`nix-collect-garbage -d`: **3553 store paths, 3.1 GiB freed**; store now
**15 G used / 43 G free (25 %)** on p27. Device repo clone pushed to
`05b9a07` (`bin/device-repo.sh push`) so on-device `nixos-rebuild`
matches.

Docs updated: `docs/desktop-selection.md` (3 modes + removal record),
`docs/power-sleep.md` (desktop-aware stop-list), `README.md`, `AGENTS.md`,
`docs/gnome-feasibility.md`, `docs/desktop-plumbing.md`,
`docs/mobile-nixos-port-feasibility.md`, `docs/gemcli.md`,
`docs/gemshell.md`, `config/xkb/README.md`.

Next: the gemshell on-glass pass after the 2026-09-12 rework is still
owed (docs/gemshell.md); no other follow-up from this change.

## 2026-09-11 — Wi-Fi: smartphone-like autoconnect (never give up + prefer strongest known network)

Ask: "the device is struggling to connect to the wifi network" → then
"the network worked fine, we're just out of range; the device must keep
trying the strongest network it knows the password for and never give
up, like a smartphone."

**Root cause of the "struggle" (on glass, boot 2026-09-11 22:11):** NM
autoconnected to the saved profile **`49 Grafton Street`** (a real known
network, but far out of range: scan signal **10/100**, ch36
`18:e8:29:6e:4c:4c`) and retried it for ~3 min while **`The Lab`** sat at
signal 85. wpa_supplicant receipts:
`CTRL-EVENT-ASSOC-REJECT bssid=18:e8:29:6e:4c:4c status_code=16`
(repeated; driver log `aisFsmSteps: Failed to connect 49 Grafton Street
more than 5 times ... scan again`, `... blacklist 1`). NM:
`Activation: failed for connection '49 Grafton Street'`, then
`(wifi) association took too long` / `asking for new secrets` ×3, and
only at **22:14:18** `Associated with 40:f2:01:55:d9:49` (The Lab).
`seen-bssids` had the far BSSID recorded against the profile
(`c8a225e4…=18:E8:29:6E:4C:4C`), so NM kept treating it as a candidate.
Reproduced live: `nmcli --wait 50 con up '49 Grafton Street'` → timeout
with the same `ASSOC-REJECT 16` loop.

**Why NM does this (docs, 2026-09-11):** `connection(5)` — "If multiple
profiles are ready to autoconnect … the one with the better
connection.autoconnect-priority is chosen. If the priorities are equal,
then the most recently connected profile is activated." It does **not**
rank by signal, and "Autoconnect … never replaces or competes with an
already active profile." Default `autoconnect-retries` is 4 (`-1` →
global 4).

**Fix, two parts (both in `services/wifi.nix`, commit `36bbe59`):**
1. **Never give up.** `connection.autoconnect-retries = 0` on the two
   home profiles + global `[main] autoconnect-retries-default = 0`
   (`NetworkManager.conf(5)`; covers user-added profiles too,
   `49 Grafton Street` stays `-1`). Verified on device:
   `NetworkManager --print-config` → `autoconnect-retries-default=0`.
2. **Prefer the strongest known network.** New device loop
   `services/scripts/wifi-smart` (sh), unit `gemini-wifi-smart`,
   options `services.geminiWifi.smartAutoconnect.{enable,pollSeconds,margin,cooldownSeconds}`
   (default on, NM mode only). Every 30 s it rescans, finds the strongest
   visible SSID that has an `autoconnect=yes` profile, connects if
   disconnected, and roams to it if it beats the current network by
   `margin` (default 20 % signal, hysteresis). Nothing known in range →
   keeps scanning. `wifi-smart once` is the diagnostic one-shot; it only
   activates visible known networks, so it cannot wedge.

**Validation on glass before deploy** (script copied to /tmp, real wlan0):
- default margin: stays on `The Lab` (87 % vs `The Lab 2.4GHz` 100 %;
  13 < 20 → hysteresis held);
- `GEMINI_WIFI_MARGIN=10 wifi-smart once` → `roaming 'The Lab' (87%) ->
  'The Lab 2.4GHz' (100%)`, and NM moved to it;
- `nmcli device disconnect wlan0` then `once` → `not connected —
  activating strongest known 'The Lab 2.4GHz' (100%)`;
- device left back on `The Lab` (5 GHz) before the deploy.

**Deploy (rule 0):** clean toplevel `2h0qb9lip72wj084y178yyli9d3bakal…`
(gen **53**, 22:25), `bin/deploy.sh deploy`, rootfs/boot.img untouched
(NO flash — profile-only switch, para untouched). On glass the unit
started (`the following new units were started: gemini-wifi-smart.service`)
and, because the NM restart left wlan0 disconnected, immediately
activated the strongest known network (`The Lab 2.4GHz`, 100 %); it has
been quiet/stable since. `wifi-smart` logs only actions, so a healthy
link produces no journal noise.

**Doc correction [2026-09-11]:** the old `services/wifi.nix` comment
"NM autoconnects to the strongest saved network at boot (autoconnect
default)" was wrong (priority → recency, no preemption) — corrected in
place. Band note: the loop ranks raw signal, so at close range it now
prefers the 2.4 GHz profile (100 %) over 5 GHz (≈87 %); tune
`smartAutoconnect.margin` to change roam eagerness, or add a band bonus
if 5 GHz throughput is preferred.

No pins changed (rule 9); no kernel/delta change.

## 2026-09-12 — gemshell: status-bar window controls, real `xdg_toplevel.close`, settings render path, Fn stuck-mod

Fourth glass report. Builds green (x86_64 nested `0yi8y633…`; aarch64
`b44hsvc9ggb1dydkbziplipf2dvns62f-gemshell-0.1.0`); **on-glass deploy
owed** (nothing flashed this session).

**"All apps launch maximized and I can't shrink or close them, probably
because they don't respond to touch at all; settings is still slow (~2 s
to close a button press)."** and **"the CVBN keys change
volume/brightness without holding Fn."**

Touch was ruled in/out first, in the x86_64 **nested** loop
(`bin/gemshell-nested.sh` equivalent, with `WAYLAND_DEBUG=1`): a trivial
GTK4 test app **did** receive the forwarded `wl_touch` and fire
`clicked`, so touch-to-widget delivery was fine. The real bugs were:

1. **Close did nothing.** `Compositor::close_window()` removed the
   server-side window and dropped the `xdg_toplevel` resource; it never
   sent the **`xdg_toplevel.close` event**, so the app kept running with
   no window. New `request_close()` sends `t.close()` and lets the
   client's `XdgToplevel::Destroy` dispatch remove the frame (popups
   still drop directly). Nested receipt: a minimal GTK4 app logged
   `CLOSE_REQUEST_RECEIVED` + `APP_SHUTDOWN`. `Delete` (XF86_Tools) and
   the SSD titlebar ✕ route through it too.
2. **No reachable restore/minimize for a maximized CSD app.** Added
   **status-bar window controls** for the focused window, left of the
   clock: **minimize · restore/maximize · close**
   (`ui::draw_window_controls` + `ui::window_control_zones` at cx
   112/172/232, hit-tested in `handle_tap`; `focused_window()`).
3. **Settings latency.** The modal settings card is a full-screen
   **opaque** egui `CentralPanel` (all four scene corners sample the
   panel BG `18,21,26`), yet `render_frame` still re-rendered the whole
   hidden scene under it. It now runs the egui panel **first**, skips the
   scene while `settings_open`, clears the panel primitives on the Close
   frame, and the loop re-arms `dirty` only via `settings_open &&
   shell.wants_repaint()` instead of a fixed 60 Hz clock. (Nested frame
   cost was <5 ms; the A53 measurement is still owed.)
4. **Stuck Fn.** Fn = `KEY_RIGHTALT` (level-3 Mod5). `process_key` now
   reconciles: on any non-RALT key, if xkb still holds Mod5 but
   `EVIOCGKEY(96)` says RALT is up, force the RALT key-up first, so plain
   `c`/`v`/`b`/`n` resolve to letters. Root cause unproven (an RALT-up
   dropped while a frame renders is the hypothesis); this is a
   self-healing net, evdev-only.

`docs/gemshell.md` (new “2026-09-12 third glass-report batch” section +
checklist items) and the gemshell row of `AGENTS.md` updated. Next:
deploy, then confirm all four on glass (esp. the Fn regression and the
settings tap latency).

## 2026-09-11 (c) — gemshell: no app padding, fill work area, drag by client title bar, app-level scaling

Third report from the glass, same day. Deployed (gen51) and verified
with a real GTK4 app (gnome-calculator) on the device.

**"Still padding on apps; they need to be draggable by their own title
bars; scaling should apply to apps, not just gemshell."**

1. **Padding.** New toplevels now open **maximized to the work area**
   (`Compositor::work_area`, below the status bar / above the taskbar)
   instead of a fixed inset 1000×700 box; transient dialogs (a toplevel
   that sends `xdg_toplevel.set_parent`) open as a centered floating box
   instead. `Window::buffer_rect` no longer letterboxes a near-matching
   buffer: GTK CSD surfaces reserve a transparent shadow margin, so a
   "maximized" window's buffer is smaller than the configure we sent
   (measured 2062×939 vs the configured 2160×948), and centring it left a
   visible strip of window background around every app. It now stretches
   to fill when the aspect mismatch is under ~13% (imperceptible) and
   letterboxes only for larger mismatches. Device receipt:
   gnome-calculator's light background spans the **full 2160 px width**
   (was 49..2111) and the window-bg colour appears **0** times.
2. **Drag by the client's own title bar.** With CSD the app's header bar
   is the drag handle, but `xdg_toplevel.move` was turned into a bare
   `Gesture::Move`, which stopped forwarding touch to the client (motion
   and up included) — so the client's own gesture never completed. Now
   `client_move = (win, finger, dx, dy)` moves the window while the touch
   is **still forwarded**; the first move un-maximizes to a floating size
   (78%×82% of the work area) so the drag is visible.
3. **Scaling now applies to apps.** `wl_output.scale` is
   `ceil(ui_scale)` (integer — 100%→1, 150%→2 supersampled, 200%→2 1:1),
   broadcast to every bound output on a scale change along with a fresh
   configure for every toplevel. Before this only gemshell's own chrome
   scaled; app buffers were upscaled.

Also in this batch: `set_brightness` runs **inline** on the UI thread
(it is a devmem + sysfs write; the worker may be mid-`nmcli`) and the data
worker drains pending mutations between each slow provider call, so a
mutation waits at most one provider call rather than a whole ~1–2 s
snapshot (the "brightness slider extremely unresponsive / taps from 30 s
ago" report). `spawn_cmd` logs the spawn outcome + pid.

Verified on device: SSD titlebar pixels 0, window-bg pixels 0, app spans
the full width; gnome-calculator runs. Installed service active, transient
stopped, idle CPU ~0.2–1.2 %.

## 2026-09-11 (b) — gemshell: apps launch, settings responsive, single chrome, UI scale (+ Fn keys verified)

Second glass-report batch, same day. Six user-visible fixes + one
feature; all deployed and verified on the unit (gens 46–48).

**Apps did not launch at all (gen46).** GTK4's
`gdk/wayland/gdkdisplay-wayland.c` refuses the whole display unless the
compositor exposes **`wl_data_device_manager`** ("The Wayland compositor
does not provide one or more of the required interfaces, not using
Wayland display"); the app then exits with "Failed to open display".
Added the global + minimal dispatch for `wl_data_device_manager` /
`wl_data_source` / `wl_data_device` (clipboard is a no-op for now).
Receipt on glass: gnome-calculator RUNNING (the
`MESA-EGL: failed to get driver name for fd -1` warnings are libmutter
preferring the render node; harmless).

**Settings froze for seconds and handled taps 30 s+ late (gen46).** The
panel synchronously ran a ~1–2 s `nmcli`/`bluetoothctl`/`wpctl` snapshot
**on the compositor thread** on open and every 3 s, and every slider step
queued another. New `compositor::data::CachedData`: reads come from an
in-memory snapshot (instant), mutations run on a worker, refreshes are
coalesced (`refresh_pending`), mutations are coalesced **by kind,
last-wins**, and pending mutations run *before* a snapshot, so a fast
devmem brightness write is never stuck behind a slow `nmcli`. Brightness/
volume/mute do not request a snapshot. Poll 3 s → 8 s. Device timings:
`nmcli ... device wifi list` (without `--rescan no`) was the worst at
~5.1 s; `data.wifi()` runs three nmcli calls ≈750 ms, so a snapshot is
≈1–2 s — all now off the UI thread.

**Two sets of window chrome / two close buttons (gen48).** GTK4 does NOT
speak `zxdg_decoration_manager_v1`; `gdk_wayland_display_prefers_ssd()`
only consults the **KDE** `org_kde_kwin_server_decoration_manager`
(`gtk/gtkwindow.c:3998` → `gtk_window_should_use_csd()`), which gemshell
does not advertise, so GTK always draws its own header bar — while
`Window.csd` defaulted false so gemshell drew the SSD titlebar too. Fix:
regular toplevels default to **CSD**; `draw_window`, `content_rect`/
`local_coords` and the titlebar hit-test branch on it. `xdg-decoration`
stays advertised for Qt/wlroots clients, answered client-side; an
explicit `set_mode(ServerSide)` gets `csd = false` + the SSD titlebar.
`xdg_toplevel.move` (CSD header drag) moves the window with the finger
down. Receipt: the (33,36,41) SSD titlebar is gone from the device
screenshot; one white libadwaita header bar with its controls remains.

**Variable UI scale (gen48).** Settings > Display: 100% / 150% / 200%.
Layout is in logical units (`Compositor::{lw,lh,ui_scale}` = `W/ui_scale`
× `H/ui_scale`); `Renderer::ui_scale` maps logical across the physical
viewport in the vertex shader. Glyphs are rasterized at `26*SUP`
(`SUP=2`) with metrics `/SUP`, and egui's `pixels_per_point = PPP ×
ui_scale`, so text is crisp at 150/200% (and downscaled at 100%). Touch is
converted physical → logical at ingest. Persists to
`$HOME/.config/gemshell/scale`. Device receipt: at 200% the launcher
tile-bg bbox is 1520×240+320+280 vs the 100% 3-tile row — tiles double.

**Fn brightness/volume keys: verified working, no compositor bug.** The
`gemini` layout maps `XF86MonBrightnessDown/Up` to Fn+B / Fn+N and
`XF86Audio{Lower,Raise}Volume` to Fn+C / Fn+V (level 3, Mod5 = Fn =
KEY_RIGHTALT); `handle_key` already acted on them. On-device receipt via
`bin/kb-inject.c` (built on the device with `nix-shell -p gcc`): Fn+C ×5
→ volume 0.88→0.38, Fn+V ×2 → 0.58, Fn+B ×5 → brightness 2306→0, Fn+N ×3
→ 290. The earlier "keys do nothing" was the old build / the blank UI.

Also: `bin/gemshell-dev.sh run` now forwards its extra-env/settle args
(it silently ignored them, so the first on-device launcher screenshot had
no `GEMSHELL_SCREENSHOT`); `spawn_cmd` logs the spawn outcome and pid so a
failing Exec is visible in the journal.

Deploys: gen46 `f6cs9r7isk7n…`, gen48 `5i6scaxd3126…`; gemshell changed
only, kernel/mesa untouched, no boot.img flash, para unchanged. Left
safe: installed `gemini-gemshell.service` active, transient stopped, UI
scale reset to 100%, idle CPU 0.2 %.

## 2026-09-11 — gemshell glass-report fixes (TEXT root cause, SVG icons, launcher, ~60%→0.2% idle CPU, brightness/audio) + gen44

User report from the glass (against the pre-egui build from the device
clone at `2cd9b6f`, 2026-09-10; several issues persisted at HEAD): most
app icons missing and no text; impossible to reveal new apps or close
the app drawer; tapping icons did nothing; gemshell burning ~60 % of a
core; the settings app had no brightness or speaker/headphone
selection; Fn brightness/volume dead. All six addressed; deployed
(gen44) and verified on glass. (The entry below is headed 2026-09-12
but its commits are 2026-09-11 15:48 — that header date is a day ahead
of git.)

**1. No text — the real root cause (a HEAD bug).** `common/font.rs`
advanced the pen with `h_advance_unscaled(id) * px`. ab_glyph's
unscaled advance is in FONT UNITS (≈600 for DejaVu), so ×26 gave
**~16 000 px per glyph**: `Op::Text` rendered only its first character
and `Op::TextCentered` (launcher title/labels, Close, battery %) put
the pen ~10 000 px off-screen — the whole chrome and every app label
was invisible. The 2026-09-12 fix had corrected the raster *origin* but
not the *advance*. Fix: `face.as_scaled(px).h_advance(id)`
(`ab_glyph::ScaleFont`). Nested screenshot after the fix: title 238 px,
Close 240 px, label row 661 px of text ink (was 0).

**2. Missing icons — SVG support.** Adwaita/Pop/COSMIC ship app icons
as SVG only (Adwaita 50 has no PNG app icons at all); `icons.rs` only
decoded PNG, so most of the 168 `.desktop` apps fell back to a blank
letter tile. Added an SVG name index at startup + lazy per-icon decode
via `resvg` (`default-features = false`, no text engine; `raster-images`
off, so SVGs with embedded bitmaps skip just that node —
`resvg::image: Images decoding was disabled` in the journal). Device
logs "20 PNG + 514 SVG icons indexed"; on-glass launcher screenshot:
8480 unique colours, icons for GNOME/COSMIC apps present.

**3. Launcher unusable.** Scroll clamp was `clamp(-600, 0)` while
`tile_y = START_Y - scroll` and a swipe-up drove scroll *positive*, so
it was clamped straight back to 0; the `*= 0.8` decay then snapped the
grid to the top on release, so no row past ~3 was reachable. Clamp is
now `[0, content_h - H]`, the decay is gone, and `launcher_scroll` is
kept out of `animating()` (a scrolled list must not hold the frame
clock at 60 Hz). Taps inside the LauncherScroll gesture were dropped in
`touch_up`; they now hit-test (icons launch), and a Close button
(top-right) or a tap outside a tile closes the drawer.

**4. Idle CPU ~60 % → ~0.2 %.** The main loop re-rendered every 16 ms
forever (`timeout = 0 if dirty else 16`, and dirty was re-set on every
frame completion). Now it renders only when dirty: timeout 0 when a
frame is due, the remaining 16 ms while a frame is in flight, and a 1 s
idle tick so the status channel (which cannot wake `poll`) is still
drained. `run_egui`'s `dirty` is intentionally cleared by the frame
completion (egui repaints on input, not at 60 Hz). Device measured
1 tick / 6 s = 0.2 % of one core, both with the panel closed and open.

**5. Settings: brightness + speaker/headphone.** New **Display** tab
with a brightness slider (`set_brightness`). The Audio sink parser only
read the `Sinks:` block, so `gemini_speakers` (the L/R-correcting
virtual sink, listed by `wpctl status` under **Filters** tagged
`[Audio/Sink]`) never appeared; it now parses both blocks, resolves
`node.description` + the default through `wpctl inspect`, and selects
by numeric id. The device offers "Built-in Speakers" and
"Headphones / Jack"; `gemini-speakerd` flips the amp, so picking the
sink is enough (verified with `wpctl set-default 34` on the device).

**6. Fn brightness/volume.** No compositor bug: the `gemini` layout
puts `XF86MonBrightnessDown/Up` on AB05/AB06 and `XF86Audio*` on
AB03/AB04 at level 3 (Mod5), and `handle_key` already matches them
(`xkbcli how-to-type` confirms the mapping). The dead keys were a
symptom of the old build / the missing text.

Also: `util::env_flag` so `GEMSHELL_*`=0/empty/false means OFF
(`GEMSHELL_OPEN_SETTINGS=0` used to open the panel); new
`GEMSHELL_OPEN_LAUNCHER` for launcher screenshots;
`bin/gemshell-dev.sh run` now forwards its env/settle args (the `run`
case ignored them, so the first on-device launcher screenshot silently
got no `GEMSHELL_SCREENSHOT`).

Verification: `bin/gemshell-host-check.sh` 30 tests pass; x86_64 nested
screenshots; on-device launcher screenshot pulled from the transient
unit (title/Close/labels + 8480-colour icons); idle CPU 0.2 %. Deployed
host → device: toplevel
`zamkh57b2p9w93nqsmzgqc7x01xallq5-nixos-system-gemini-26.11pre-git`
(gen44, `system-44-link`), gemshell
`lf1zy4xsz1p19nmyxagi3bs097p523h7-gemshell-0.1.0`; kernel/mesa unchanged,
no boot.img flash. Left safe: para unchanged, installed service active.
Next: human look at the glass (icons/text, launcher scroll + close, Fn
brightness/volume, settings Display/Audio), RSS measurement, and the
touch direction/calibration already owed.

## 2026-09-12 — gemshell rework: egui UI (hardware-tessellated), `gemdata` abstraction, shared gemcli impl, mirror + blank-text fixes

User report from the glass: gemshell is left-right mirrored (top/bottom
correct), no text appears, and it needs a real UI library plus a data
abstraction (trait + real + dummy impls). All four addressed; builds
verified on the host, on-glass pass owed.

**1. Left-right mirror — FIXED (root-caused, not calibrated).**
`COPY_CS` (the present() compute copy) sampled the scene FBO with
`texelFetch`, which indexes the texture's BOTTOM-UP storage directly.
The intended 90° rotation was therefore really a transpose — a rotation
composed with a horizontal reflection, i.e. exactly a left-right mirror.
Fix: flip the scene Y before the fetch
(`t = ivec2(s.x, srcSize.y - 1 - s.y)`). The mapping is now
orientation-preserving and matches `touch_to_scene` rotation 90
(`(ny*scene_w, (1-nx)*scene_h)`), so presentation and touch finally
agree. Direction (90 vs 270) is still the `GEMSHELL_ROTATE` knob.
(`src/compositor/render.rs`.)

**2. No text — FIXED (root-caused).** `common/font.rs` rasterized glyphs
with a double origin subtraction: `OutlinedGlyph::draw` already yields
raster-local pixel coordinates rel. to `px_bounds.min`, but the code
subtracted `bounds.min` again, so every glyph whose min.y is above the
baseline (all of them) fell outside its own raster and the atlas was
all zeros — hence no text anywhere (chrome included). Fix: use the draw
coordinates directly (`ab_glyph` receipt: outlined.rs `draw`).

**3. Real UI library (egui) — DONE, hardware accelerated.** The separate
`gemsettings` wl_shm client (hand-drawn CPU UI) is REMOVED. The settings
panel is now an in-process egui overlay (`src/shell.rs`): egui does font
loading/shaping/layout/scrolling/focus/text editing and emits GPU-ready
triangle meshes; `Renderer::draw_egui` uploads the atlas + textures and
draws indexed meshes with a new GLES program (`EGUI_VERT`/`EGUI_FRAG`),
clip rects → `glScissor`, egui's premultiplied blend. Nothing is CPU
rasterized; the scene itself was already GPU. UI runs at PPP=2.0
(1080×540 points). Fonts: `GEMSHELL_FONT` (DejaVu) registered first,
egui bundled fonts as fallback. Open from the status-bar gear / wifi /
bt / speaker zones; Esc/Close dismisses. `GEMSHELL_OPEN_SETTINGS=1`
opens at startup (used by the nested/dev scripts).

**4. Data abstraction (`gemdata`) — DONE.** New workspace crates:
`crates/gemdata` (`DataProvider` trait + data types; no UI/GL/wayland),
`crates/gemdata-device` (real impl: nmcli/bluetoothctl/wpctl/sysfs, each
behind a pure unit-tested parser), `crates/gemdata-dummy` (in-memory
fake selected by `GEMSHELL_NESTED`). The compositor status poller now
reads `DataProvider::status()`; the egui panel is handed `&dyn
DataProvider` and never shells out itself.

**5. gemcli folded in — ONE implementation.** ALL gemcli device modules
(a72, backlight, battery, boot, charger, devmem, error, gpio, gpu,
guard, i2c, power, profile, session, sleep, speaker, status, sysfs,
util, wdt) moved from `pkgs/gemcli/src/` to
`pkgs/gemshell/crates/gemdata-device/src/`. `gemcli` is now a thin clap
frontend over them (`pkgs/gemshell/crates/gemcli/`), still packaged by
`pkgs/gemcli.nix` via `buildAndTestSubdir = crates/gemcli`; `pkgs/gemcli/`
is deleted. gemshell depends on `gemdata-device`, so the same functions
are available to it too. No logic duplicated.

**Build/receipts (host).** `cargo check --workspace` clean; unit tests:
26 in `gemdata-device` (the moved gemcli tests + new parser tests), 3 in
`gemdata-dummy`. `nix build .#packages.x86_64-linux.gemshell` green
(includes egui 0.29 + `epaint_default_fonts`; the nix `src` now filters
the host `target/` tree). Nested run under the host KWin Wayland with
`GEMSHELL_OPEN_SETTINGS=1 GEMSHELL_SCREENSHOT=…` wrote a composited
scene PNG with the panel + text visible (this is how the blank-text and
mirror fixes were checked off-device; the mirror itself only manifests
in the device compute present, so it is reasoned + touch-consistency
checked, not screenshot-verified).

**Found on the way:** `render::build_program` ignored its `vert`
argument and always compiled the global `VERT` — latent because only one
program existed; the new egui program exposed it (link error "fragment
input vColor has no matching output"). Fixed.

**On-glass follow-up (same day, after `bin/deploy.sh deploy` → gens
39–42).** Three more fixes, all verified on the glass:

- **Rotation 180° out.** `GEMSHELL_ROTATE`/`GEMSHELL_TOUCH_ROTATE =
  90` rendered the scene 180° out. The 270 branch is the 180°
  counterpart and the exact inverse of the touch-270 mapping, so the
  defaults moved to **270** together (confirmed correct by the user).
- **Input freeze.** The evdev nodes were opened without `O_NONBLOCK`, so
  the drain loop's follow-up `read()` blocked and the whole compositor
  froze on the first input event (verified live: main thread blocked in
  `evdev_read` on fd 3, `syscall=63`). Added `O_NONBLOCK`.
- **Touch completely dead — THE root cause.** `ABS_MT_SLOT` was defined
  as **57**, but 57 is `ABS_MT_TRACKING_ID` (SLOT is 0x2f = **47**).
  Because the SLOT arm is matched first, every tracking-id event was
  swallowed as a slot change, so a finger-down was never registered.
  Fixed the constant (0x2f). Also corrected the protocol-B **order**:
  `nvt36xxx_report` sends `TRACKING_ID` before `POSITION_X/Y`, so the
  old code emitted the down with the previous contact's coordinates;
  `read_touch` now records per-slot state and emits at `SYN_REPORT`.
  Proof via the repo injector on glass:
  `tapxy 1035 2115` → journal `touch-trail: down id=0 scene=(44,1036)`.

**Touch test aid (`GEMSHELL_TOUCH_TRAIL=1`, set in
`services/gemshell.nix`):** every finger draws a coloured trail on the
scene (`ui::draw_touch_trail`), normal gestures are bypassed, and a
3-finger touch clears the canvas — so touch is verifiable by drawing.
Remove the env to restore normal gestures. `bin/touch-inject.c` gained a
multi-point stroke mode (`tapxy X Y [X2 Y2 …]`) for scripted strokes.

**Not touched / owed:** nothing flashed (deploys only). On-glass still
owed: the egui settings panel over touch + the hardware keyboard, and a
`bin/gemshell-dev.sh` run. Historical session-log entries above still
cite `pkgs/gemcli/src/…` — those paths now live under
`pkgs/gemshell/crates/gemdata-device/src/`.

## 2026-09-11 — gemshell ON GLASS: compositor renders; gemsettings; nested x86_64; landscape

The gemshell compositor's first cut built but crash-looped on the PDA.
This session found and fixed the whole chain, got real pixels on glass,
implemented the `gemsettings` client, added a nested x86_64 dev loop,
and fixed the orientation (a landscape product on a portrait fb).

**Headline bugs (all in the first cut, all now fixed — details in
`gemshell:` commits `efff9ad`, `21a89c5`, `fcf7117` + docs/gemshell.md):**

- **EGL enums were hallucinated** (`EGL_NONE = 0` instead of `0x3038`,
  `EGL_RENDERABLE_TYPE = 0x3095/0x3040`, width/height, extensions, the
  dma_buf attrs `0x32D5..` vs `0x3270..`). With `0 != EGL_NONE`
  `eglChooseConfig` returned `EGL_BAD_ATTRIBUTE (0x3004)` + 0 configs —
  chased on glass as an ICD/`EGL_VENDOR_PATH` problem. Audited every
  constant against libglvnd 1.7.0 headers.
- **`EGLConfig` was `c_void` not `*mut c_void`** → `eglChooseConfig`
  wrote 8-byte handles into 1-byte slots (stack corruption).
- **GL constants also wrong**: `GL_COMPUTE_SHADER 0x8DA2` (0x91B9),
  `GL_RGBA8 0x8D53` (0x8058), framebuffer barrier `0x20` (0x400),
  `GL_BLEND 0x0BE0` (=GL_BLEND_DST; glEnable silently no-op, no alpha).
- **`glShaderSource` given a string, not an array-of-pointers** (3
  sites) — Mesa read the shader text as a pointer (SEGV).
- **`GEMFB_IOC_EXPORT` returns the dma-buf fd as the ioctl result**; the
  code read the ignored arg and treated the positive return as an error.
- **`aPos`/`aUv` looked up with `glGetUniformLocation`** (→ -1) so no
  geometry drew; the frame was only the clear colour.
- `wl_keyboard.key` must carry the raw evdev scancode (the old `+8`
  double-offset every client); `EVIOCGABS` direction/type/nr were wrong;
  `xdg_surface.configure` was never sent (no standard client could map).
- Launcher: honour `XDG_DATA_DIRS`/`$XDG_DATA_HOME` (NixOS has no
  `/usr/share/applications` → 0 apps) and set `HOME` for the service.

**On glass (gen, transient unit via `bin/gemshell-dev.sh`):** the
compositor boots, `GL: OpenGL ES 3.1 Mesa 26.2.2`, imports the LK fb
(`GEMFB_IOC_EXPORT`), builds the scene FBO + present target, launches its
socket and renders. `GEMSHELL_SCREENSHOT` (scene-FBO PNG) confirms the
full UI: status bar, taskbar, windows, text, icons.

**Orientation:** the product is landscape but the LK fb is PORTRAIT
1080x2160 (gemwl uses `WL_OUTPUT_TRANSFORM_90`; `geminipda-drm`
advertises `LEFT_UP`=90 for the same reason). The scene is now
2160x1080 and `present()` counter-rotates 90 into the fb; touch is
un-rotated with the matching inverse. `GEMSHELL_ROTATE` /
`GEMSHELL_TOUCH_ROTATE` allow on-glass calibration. **Owed: a human
look at the glass to confirm the rotation direction** (the same
transform as gemwl should be right, but it is unverified).

**`gemsettings`** was a `fn main() {}` stub; it is now a real wl_shm
client (Wi-Fi via nmcli, Bluetooth via bluetoothctl, Audio via wpctl;
wl_touch + xkb password entry), 1600x940 landscape.

**Nested dev loop on x86_64** (`bin/gemshell-nested.sh`, `GEMSHELL_NESTED=1`):
runs gemshell as a Wayland client under the workstation compositor
(headless EGL/GBM on `/dev/dri/renderD128`, wl_shm present of the scene
FBO, host pointer/keyboard/touch forwarded). Verified on zen3
(radeonsi): compositor + gemsettings render, 59 apps / 92 icons. This
makes UI iteration seconds instead of a device flash. Flake exposes
`packages.x86_64-linux.gemshell`.

**Caution noted:** CPU `mmap`+read of the LK fb dma-buf
(`GEMSHELL_SCREENSHOT_FB`) HUNG the unit until the WDT reset it
(2026-09-11); `geminipda-fb.c`'s `gemfb_mmap` now bounds the remap to
the VMA length, and the path stays unused until re-verified on a
throwaway boot.

Next: on-glass visual/human confirmation of orientation + touch
calibration, then the docs/gemshell.md on-glass checklist.

## 2026-09-11 — gemshell: the Rust compositor reaches BUILD-LEVEL (aarch64 build green; session wiring in; on-glass owed)

The gemshell compositor (this repo's custom Rust desktop —
docs/gemshell.md) went from "source exists, doesn't compile" to
**compiling + linking for aarch64 in the nix build**, and got wired
into the system as the fourth desktop mode.

- **Compiles:** the whole crate (wayland-server 0.31 / xdg-shell / wl_shm
  + EGL/GBM renderer + evdev input + shell UI + the gemsettings
  client) now passes `cargo check` (host, x86_64) and the real
  **`nix build .#packages.aarch64-linux.gemshell`** (remote builder,
  ~6 min): store path `js6rq8h7v1xfg61q1fhyzg4sadqixshm-gemshell-0.1.0`
  (bin `gemshell` 3.0 MiB + `gemsettings` 579 KiB; NEEDED =
  wayland-server/xkbcommon/gbm/EGL/GLESv2; RUNPATH pinned per the
  gemdemo receipt). The wayland 0.31 API rework (the 210-error first
  compile) is done: `Resource::Request`/`DataInit` dispatch, WEnum
  wrappers, `configure(width,height,states)` arg order, `keys` arrays
  as `Vec<u8>`, per-client inner locks (the 17 `Dispatch` bound errors
  came from the wayland object's `D: Resource` generic, not the
  handler).
- **Build gotchas** (all found only by the aarch64 build — the host
  check never links; receipts in docs/gemshell.md "Build gotchas"):
  (1) libglvnd's `libEGL.so.1` doesn't export `eglGetPlatformDisplayEXT`/
  `eglCreatePlatformSurface` (readelf-verified) → resolved via
  `eglGetProcAddress` (EGL 1.5); (2) the store's **xkbcommon 1.13.1
  aarch64 exports only the V_0.5.0-era state API** — `xkb_state_*_mods*`/
  `group_get_index` are all gone → wl modmap masks now built per slot
  with `xkb_state_mod_index_is_active`; (3) `c_char` is `u8` on aarch64
  (the FFI said `i8`); (4) the native link set is declared in
  `pkgs/gemshell/build.rs` (wayland-server/client, xkbcommon, gbm, EGL,
  GLESv2, png, z).
- **Session wiring (all build-level):**
  - `pkgs/gemshell.nix` — the package (rustPlatform, Cargo.lock pinned,
    RUSTFLAGS `-L ${wayland}/lib`, patchelf RUNPATH).
  - `services/gemshell.nix` — `services.gemshellDesktop` module: the
    compositor as a SYSTEM service (User=cjdell, XDG_RUNTIME_DIR=/run/
    gemshell) gated by `ConditionPathExists=/run/gemini-console` (the
    SAME sentinel as console mode), + a `gemini-gemshell-panfrost-load`
    unit (blacklist→load after `gemini-gpu-poweron`, same shape as
    gnome.nix), force-disables the nested gemwl/phosh/LXQt stack +
    standard pipewire, adds gemshell+gemdemo to the system profile.
  - `services/desktop-select.nix` + `gemini-desktop-apply` +
    `pkgs/gemcli/src/session.rs` — **`gemshell` is now a fifth mode**
    (gemcli session list/set; the apply script creates the sentinel,
    NO tty1 getty — the compositor owns the panel; GDM skipped).
  - `config/gemini.nix` — module imported + `services.gemshellDesktop.
    enable = true` (co-install model: inert unless the marker says
    gemshell); cjdell gains the `input` + `bluetooth` groups (evdev
    nodes + bluetoothctl for the compositor).
  - `flake.nix` — `packages.aarch64-linux.gemshell` (+ the let-binding
    sharing the flake's `mesa` binding so the ICD/kmsro pair is the
    same store path as the system's).
- **Verified:** `nix eval .#nixosConfigurations.gemini.config.systemd.
  services."gemini-gemshell".wantedBy` → `["multi-user.target"]`,
  top-level config eval green; gemcli's session-mode unit test updated
  (five modes). **NOT on glass** — the bring-up checklist (docs/
  gemshell.md) is the next session: flash a toplevel with the marker
  set to gemshell (or `gemcli session set gemshell --reboot` from the
  device after a deploy), eyes on the panel (rule 5: flicker → stop →
  TWRP), then touch/keyboard/launcher/gemsettings/gemdemo.
- No flashes this session (host-side build + config work only; device
  untouched — glass state unchanged, whatever the previous session left
  it as).

## 2026-09-11 — README/AGENTS restructure (human README, operational detail to AGENTS)

Docs-only. The README had grown into an agent/ops dump. Split it:

- **README.md** is now a human-facing overview: what the repo is, then
  **"What works today"** as the lead section (boot/GNOME/Wi-Fi/BT/A2DP/
  audio/power/touch/keyboard/wine/DOSBox/graphics/recovery), the device
  + partition layout, boot chain, a condensed layout + device-services
  table, a documentation index, versions, a short build/flash quickstart
  and the R1–R3 constraints.
- **AGENTS.md** gained the moved operational detail: build model +
  commands, the kernel published-base + in-repo-delta model, the manual
  flash procedure + safety model, the on-device
  `nixos-rebuild switch --flake .` loop, pins maintenance, plus the
  formerly README-only Mesa ICD-wiring and rootfs-integration receipts.
- **Correction [added 2026-09-11]:** README R3 still claimed "no DRM —
  the desktop is gemwl"; superseded by the 2026-09-10 `geminipda-drm`
  KMS driver + GNOME. R3 now says so (gemwl stays the rollback).
- Pointer fix: `docs/boot-process.md` §8 now points the kernel-source
  model at AGENTS (the row also still said "borrowed #329", retired
  2026-09-08).
- No code, no build, no flash; device untouched.

## 2026-09-11 — systemd `suspend` disabled; GNOME auto-suspend pinned off (deployed gen 22)

User asked whether `systemctl suspend` is the same thing as `gemcli
sleep`, and reported that **suspend currently just locks the system up**.
Answer: they are different layers — `systemctl suspend` is kernel
`s2idle` (needs a wake source, which this unit lacks), while `gemcli
sleep` is the userspace light sleep (kernel stays up). Decision: disable
systemd suspend and keep `gemcli`/`gemini-sleepd` as the only sleep
path.

- **`config/gemini.nix`** — added `systemd.suppressedSystemUnits =
  [ sleep.target suspend.target hibernate.target hybrid-sleep.target
  suspend-then-hibernate.target systemd-{suspend,hibernate,hybrid-sleep,
  suspend-then-hibernate}.service ]`. Receipt: this nixpkgs pin
  (`dc5d91f84032`) has **no `systemd.mask` option**; NixOS generates
  `/etc/systemd/system` itself and the systemd package ships no
  `/usr/lib/systemd/system` fallback (`readlink result/etc/systemd/
  system/system-suspend.target` → the systemd package's `example/` tree),
  so a suppressed unit genuinely no longer exists. Every initiate path
  now fails fast with "Unit … not found" instead of hanging.
- **`services/gnome.nix`** — locked three dconf keys under
  `org.gnome.settings-daemon.plugins.power`:
  `sleep-inactive-ac-type = nothing`,
  `sleep-inactive-battery-type = nothing`, `power-button-action =
  nothing`. Rationale: `HandleSuspendKey=ignore` only stops the KEY_SLEEP
  evdev event; GNOME's power plugin calls logind's `Suspend()` D-Bus
  method directly, which bypasses it.
- **Docs:** `docs/power-sleep.md` gained a TL;DR bullet + a
  "`systemctl suspend` vs `gemcli sleep`" section with the change list
  and the planned (post-deep-sleep) unification path.
- **Build receipt (rule 0):** eval clean (`nix eval
  .#nixosConfigurations.gemini.config.systemd.suppressedSystemUnits`);
  toplevel built on the remote Pi builder →
  `/nix/store/3wk9yg4dly49cpwqvpkldp0cvqpyv2np-nixos-system-gemini-26.11pre-git`.
  Verified in the built system: no `suspend/hibernate/hybrid-sleep/
  sleep.target` symlinks in `/etc/systemd/system` (only our
  `gemini-sleepd.service` + NixOS's `sleep-actions.service`, which is
  `WantedBy=sleep.target` and so can never start now); the user dconf DB
  (`hzlaznis5…`) contains the three power keys and the lockfile
  (`pmzs0y2d…`) locks them.
- **Deployed (profile switch, no flash): gen 22.** The device is on
  the LAN, not the USB gadget, so `GEMINI_DEV_IP=192.168.49.166
  bin/deploy.sh deploy` → active toplevel
  `3wk9yg4dly49cpwqvpkldp0cvqpyv2np-nixos-system-gemini-26.11pre-git`.
  **Verified on glass after the switch:** `systemctl cat suspend.target`
  → "No files found for suspend.target."; `list-unit-files` has no
  sleep/suspend/hibernate targets (only `gemini-sleepd.service`
  enabled+active, plus the now-unreachable `sleep-actions.service` and
  `systemd-hibernate-clear.service`); as `cjdell`, `gsettings get
  org.gnome.settings-daemon.plugins.power
  {sleep-inactive-ac-type,sleep-inactive-battery-type,power-button-action}`
  = `'nothing'` with `writable: false` (locked). Restarted the running
  session's `org.gnome.SettingsDaemon.Power.target` (the `.service` is
  RefuseManualStart) to clear any already-armed idle timer — service
  active, live value `'nothing'`. No deliberate `systemctl suspend`
  test (that IS the lockup vector). No kernel/boot.img change; `para`
  untouched.
- **Pending follow-up:** a reboot should be harmless, but the change is
  already live; `systemd.suppressedSystemUnits` takes full effect from
  the next boot too (nothing to re-verify).

## 2026-09-11 — DOSBox-X installed with a Gemini keyboard fix (UK table + Fn mapper; build-level)

User asked why DOSBox-X's key mappings are wrong and the Fn keys are
completely missing, then to set it up on the device with a solution.

- **Diagnosis.** DOSBox-X's SDL2 input path keys off
  `SDL_KeyboardEvent.keysym.scancode` (physical position), not the
  level-resolved `keysym.sym` (`src/gui/sdl_mapper.cpp`
  `CKeyBindGroup::CreateEventBind/CheckEvent`; `MakeDefaultBind()` maps
  `SDL_SCANCODE_*` → PC key; `useScanCode()` is `false` for SDL2, so the
  `usescancodes=` knob is a no-op). It therefore ignores the host `gemini`
  XKB layout and uses its own `[dos] keyboardlayout` (default US) — that
  is the wrong `£`/`@`/`;` etc. And Fn is XKB **level 3** only (kernel
  `KEY_RIGHTALT` → `ISO_Level3_Shift`), so Fn+1..0 is the same scancode
  as plain `1` with RALT held: there is no Fn/F1/XF86 scancode for it to
  translate, hence "completely missing". Same class of bug as mutter's
  `<Mod5>XF86…` keysyms (`services/gnome.nix:278‑311`).
- **Added.** `pkgs/dosbox-x-gemini.nix` + `pkgs/dosbox-x-gemini.sh`
  (wrapper: forces `-set "dos keyboardlayout=uk"` and seeds a complete
  mapper into `~/.config/dosbox-x/mapper-dosbox-x.map`),
  `config/dosbox-x/mapper-dosbox-x.map` (generated) and
  `bin/gen-dosbox-x-mapper.sh` (regenerator from the pinned dosbox-x
  `DefaultKeys[]` + SDL scancode enum); wired into
  `config/gemini.nix` systemPackages. Doc: `docs/desktop-plumbing.md`
  §DOSBox-X.
- **Receipts.** dosbox-x pinned `2026.08.02` (nixpkgs `dc5d91f84032`),
  aarch64 output `0qcvjf0xh6z2r83kyl7hcky5jga92zsb` **substituted from
  cache.nixos.org** (rule 9). Mapper validated by running the x86_64
  binary under `SDL_VIDEODRIVER=dummy`: strace shows the file opened,
  and the log shows `DOS keyboard layout loaded with main language code
  UK for layout uk`. Full aarch64 toplevel built via `bin/deploy.sh
  build` (20 s, remote Pi builder):
  `69ixww27wwj5qcc2arpq9y1qc5hbsjf1-nixos-system-gemini-26.11pre-git`,
  `sw/bin/dosbox-x` → `67x55vma46qkgw2qqm8m9p8m35yqcpd9-dosbox-x-gemini`,
  `.desktop` wired. **Deployed to the device (LAN 192.168.49.166):**
  `bin/deploy.sh deploy` → **gen 19** (`system-19-link` →
  `zk6jnhxia5rl4h3xmc0cbc8rix44i73p-nixos-system-gemini-26.11pre-git`,
  config revision `48d64b9`; the earlier gen 18 was the dirty-tree build of
  the same content). On-glass headless run as `cjdell` logs `DOS keyboard
  layout loaded with main language code UK for layout uk` and seeds
  `~/.config/dosbox-x/mapper-dosbox-x.map` (0644, 10 `mod2` Fn binds).
  `systemctl is-system-running` → running, 0 failed units. No
  kernel/boot.img change (profile-only switch; para untouched).
- **Gotchas found.** (1) The mapper section must be `[SDL2]`, NOT `[sdl]`
  (`SDL_STRING` = `"SDL2"`, `include/shell.h:27`) — a wrong section name
  makes `MAPPER_LoadBinds()` silently drop every line and use the
  built-in defaults; caught only by actually loading the file. (2) The
  seeded copy must be `chmod 644` (the store file is 0444) or the mapper
  GUI cannot save. (3) Fn is also the only Alt, so Fn+1 reaches the guest
  as Alt+F1 — deliberate trade. Media keys are not DOS scancodes and are
  not covered.
- **Pending.** ⬜ the final interactive on-device click-through: launch
  DOSBox-X from the GNOME app grid and type `:` (Fn+O or Fn+`) and `\`
  (Fn+3) at a DOS prompt. The overlay itself was verified against the
  real binary on x86_64 (Fn+1..3/5/6 → `| # \ < >`), and the device run
  confirms the UK table + mapper seed/auto-upgrade.
- **Second round (user follow-up: "no way to type `:` and `\`").** The
  gen-19 overlay bound Fn+1..0 to F1..F10 and so STOLE the number row's
  level-3 symbols (`\` = Fn+3 → F3) — exactly the user's complaint. Fix:
  Fn+number = symbol, F1..F10 = **Shift+Fn+number** (XKB level 4).
  Also discovered with `-keydbg`: the default `key_ralt` bind turns Fn
  into guest right-Alt, and a held guest Alt makes the guest layout skip
  its normal/shift planes (`layout_key` uses AltGr planes only; FreeDOS
  UK `UK.KEY` has almost none) — so `key_ralt` is now dropped and Fn is a
  pure mapper `mod2`. The Gemini's separate physical Alt (`KEY_LEFTALT`,
  DTS `MATRIX_KEY(4,1,KEY_LEFTALT)`) keeps working via `key_lalt`.
  Wrapper now auto-upgrades a stale mapper (marker `key_semicolon "key 52
  mod2"`), backing up the old file. Commits `c740786` + wrapper; gen 21.

## 2026-09-11 — Headphones selection still played the speakers: gemini-speakerd's wpctl parse was dead (fixed + on glass, gen 17)

User report: "when i select headphones i still get sound coming through
the internal speakers" (device now reachable on the LAN at
192.168.49.166).

- **Root cause.** `speaker::default_sink()` (`pkgs/gemcli/src/speaker.rs`)
  matched only `node.name = ` at the start of the trimmed line, but
  `wpctl inspect @DEFAULT_SINK@` marks the default node's properties with
  a leading `* ` (`  * node.name = "..."`). The parse therefore returned
  `None` on every poll, `sync_amp()` bailed out, and the amp pads were
  never driven. On this rootfs the default sink had been forced to
  `gemini_speakers` at boot, so the amp stayed ON when the user picked
  Headphones. The 2026-09-10q "amp coupling verified on glass" receipt
  exercised `audio-output headphone` — the CLI's *direct* `speaker off` —
  not the watcher, hence the gap.
- **Secondary defects found while fixing it.** (a) `/etc/gemini` did not
  exist on the fresh post-repartition rootfs, so `audio-output`'s
  `echo > /etc/gemini/audio-output-mode` silently failed and no mode ever
  persisted; (b) a choice made only in GNOME was never written to that
  file, so a reboot reverted to the built-in "speaker" default.
- **Fixes.** `parse_sink_name()` strips `*`/spaces before matching (+ 4
  regression tests; gemcli suite now 16 passed); `gemini-speakerd`
  persists the observed sink (`mode_for_sink`, only on change) and is
  ordered `after gemini-audio-defaults.service` so the boot-time sink
  write settles first; `services/audio.nix` adds the tmpfiles rule
  `d /etc/gemini 0755 root root -`; `services/scripts/audio-output`
  `mkdir -p`s the state dir. `bin/device-ssh.sh`/`bin/deploy.sh` also
  gained a `GEMINI_DEV_IP` override (LAN address support) and deploy now
  copies to `ssh://root@$dev` — without the explicit root user a LAN
  address logs in as `cjdell`, an untrusted nix user, and the daemon
  rejects every unsigned locally-built path (`require-sigs`).
- **Toplevels.** `c7g2n7z…` (wpctl parse fix; shipped as device
  system-16) and final `hra1d2ajgkblw9ql7hf8m9nkiyp3akhg` (mode
  persistence, system-17). Rootfs only — no boot.img/kernel touched.
- **On glass (gen 17).** `journalctl -u gemini-speakerd`:
  `default sink alsa_output.platform-sound.stereo-fallback -> amps OFF`
  and `default sink gemini_speakers -> amps ON`; `speaker status` reads
  `dout=0` for Headphones / `dout=1` for Built-in Speakers following
  `wpctl set-default`; `/etc/gemini/audio-output-mode` rewrites
  `headphone`/`speaker` within ~1 s; `audio-output status` agrees.
  Device left on **Headphones (amp OFF)**.
- **Still owed.** L/R correction by ear (the 2026-09-10q virtual sink is
  unchanged); a real reboot to confirm the persisted mode; jack playback.
- **Docs.** `docs/desktop-plumbing.md` §Speakers carries the correction
  (the 2026-09-10q claim was about the CLI, not the watcher);
  `docs/gemcli.md` gained the on-glass receipt + test-count update;
  `AGENTS.md` speakers row annotated.

## 2026-09-11 — Fn transport keys (Q/W/E) + British/UK regional settings (config-only)

User request: Fn+Q = play/pause, Fn+W = previous track, Fn+E = next
track, and the OS fully British English/UK.

- **Fn transport keys.** The Fn layer symbols already existed in
  `config/xkb/symbols/gemini`: `<AD01>q`/`<AD02>w`/`<AD03>e` carry
  XF86AudioPlay / XF86AudioPrev / XF86AudioNext at level 3. As with the
  2026-09-10p volume keys, a mutter *keysym* accelerator would resolve
  to the standard consumer keycodes at level 0 and never match the Fn
  event, so `services/gnome.nix` now binds the raw xkb keycodes as
  gsd-media-keys schema defaults (still user-remappable):
  `play-static += '<Mod5>0x18'` (Q), `previous-static += '<Mod5>0x19'`
  (W), `next-static += '<Mod5>0x1a'` (E). Q/W/E are evdev 16/17/18 →
  xkb keycode = evdev + 8. `play-static` (not `pause-static`) is the
  MPRIS play/pause toggle.
- **UK regional settings.** `config/gemini.nix` gained
  `time.timeZone = "Europe/London"`, `i18n.defaultLocale =
  "en_GB.UTF-8"` and all nine `LC_*` categories pinned to en_GB via
  `i18n.extraLocaleSettings`. `services/gnome.nix` additionally sets
  and locks `org.gnome.system.locale region = 'en_GB.UTF-8'` (GNOME
  Settings → Region & Language → Formats; full-locale value, upstream
  default `en_US.UTF-8`) so apps that only read that key are UK too.
  Timezone is not a GSettings key: timedated serves `/etc/localtime`.

Verification (on glass, after deploy):
- **Deployed gen 14 then gen 15** (`bin/deploy.sh deploy` under
  `run-job`; rootfs-only, no boot.img/kernel touched). On the device:
  `timedatectl` → `Time zone: Europe/London (BST, +0100)`;
  `/etc/locale.conf` + `locale` → every category `en_GB.UTF-8`;
  `gsettings get org.gnome.system.locale region` → `'en_GB.UTF-8'`
  (`gsettings writable` → `false`, i.e. locked);
  `gsettings get org.gnome.settings-daemon.plugins.media-keys
  play-static` → `['XF86AudioPlay', '<Ctrl>XF86AudioPlay',
  '<Mod5>0x18']`, previous/next likewise.
- Pre-deploy: the media-keys override was compiled against the actual
  gnome-settings-daemon 50.1 gschema with `glib-compile-schemas`
  (rc 0).
- **Gotcha found on glass (gen 14 → fixed in gen 15):** the GNOME
  locale key was first written to dconf path
  `org/gnome/system/locale`, but `org.gnome.system.locale` declares
  `path="/system/locale/"`, so gsettings reads `/system/locale/region`.
  The wrong path left the value in an orphan node (`dconf dump /`
  showed it, `gsettings get` returned `''`). Fixed to `system/locale`
  + lock `/system/locale/region` (services/gnome.nix).
- Docs: `docs/desktop-plumbing.md` §Volume (transport bindings) +
  new §Regional, now marked deployed; verification checklist updated.

On-glass Fn+Q/W/E: ⬜ still owed (needs an MPRIS player) at first

---

**Follow-up (same day, after "the keys do nothing" was reported):**

Verified with `bin/kb-inject.c` (built on device; header build recipe
was broken and is fixed) against Chromium's MPRIS player:
- `fn+q` → PlaybackStatus `Playing` → `Paused` → `Playing` (toggle).
- `fn+e` / `fn+w` → track changed (title "Another Day In Paradise" →
  "You Don't Talk the Way You Used To").

**Root cause of the initial failure — session-variable re-login
gotcha (the important finding):** `extraGSettingsOverrides` are
delivered through `NIX_GSETTINGS_OVERRIDES_DIR`, an
`environment.sessionVariables` value captured by the GNOME session at
**login**. `bin/deploy.sh` (nixos-rebuild switch) rewrites
`/etc/set-environment` but does NOT restart the running user session,
so the live `systemd --user` / `gsd-media-keys` / `gnome-shell` kept
the OLD overrides path (`h068b11…`, 0 transport keys) while
`/etc/set-environment` pointed at the new `spd89psb…` (3 transport
keys). Restarting `gsd-media-keys` alone did not help — it inherits
the stale env from `systemd --user`. Fix applied live without reboot:

    su - cjdell -c "XDG_RUNTIME_DIR=/run/user/1000 \
      systemctl --user set-environment NIX_GSETTINGS_OVERRIDES_DIR=<new>; \
      systemctl --user restart org.gnome.SettingsDaemon.MediaKeys.target"

(`…MediaKeys.service` is `RefuseManualStart/Stop=yes` → restart the
**.target**; a plain `--user kill` is a clean exit and will NOT
respawn.) A logout/reboot applies `/etc/set-environment` permanently,
so no code change was needed — but future sessions should expect any
`environment.sessionVariables` change (and the gsd `-static` key
overrides, volume included) to require a re-login or the set-env trick.

Also fixed in passing: `bin/kb-inject.c`'s build comment (`$( )` shell
substitution doesn't interpolate in Nix; `runCommand` needs
`nativeBuildInputs = [ gcc ]`), and the set-env + gsd-restart sequence
is promoted to **`bin/gnome-session-env-refresh.sh`** (rule 6) — it
reads the expected dir from `/etc/set-environment`, updates the user
manager, and restarts `org.gnome.SettingsDaemon.MediaKeys.target`
(`--all` = every gsd target). Documented in
`docs/desktop-plumbing.md` §Volume.

Next: none required. Device left on **gen 15**
(`/nix/store/fi43wgl2y866r2cxa8rp1s3wxv8m1l4r-nixos-system-gemini-26.11pre-git`),
GNOME session auto-reactivated by the switch; boot.img/TWRP untouched.

## 2026-09-11 — wine + `nix-shell -p` restored on the device (post-repartition fallout)

Two device-side breakages reported after the 2026-09-10 one-way
repartition. Both are fallout from the wiped `/root` — the ad-hoc wow64
deploy and the `/root/gemini-nixos` clone were never re-created:

- **`wine` → `Permission denied`** for the desktop user `cjdell` (as
  root: `ENOENT`). The closure wrapper (`pkgs/wine-cli.nix`, gen36)
  exec'd `/root/wine-x86/wine-wow`: `/root` is `0700`, so `cjdell`
  cannot traverse it, and the wow64 stack had never been re-deployed
  after the repartition (the committed deploy script shipped the 1.8 GB
  64-bit-only `wine64` stack with a `/root` launcher, not the wow64
  launcher the wrapper targets).
- **`nix-shell -p` → `error: file 'nixpkgs' was not found in the Nix
  search path`**. `NIX_PATH` points at
  `/nix/var/nix/profiles/per-user/root/channels/nixpkgs`, which
  `device-rebuild.sh channels` creates; the clone that verb reads the
  flake rev from was gone.

Fixes:

- **nixpkgs channel**: `bash bin/device-repo.sh seed` (device clone @
  `2cd9b6f`) then `device-rebuild.sh channels` → `<nixpkgs>` →
  `/nix/store/byjzdjpvrh042l85fmnvrd3c9hlqaygf-source` (rev
  `dc5d91f84032`). `nix-shell -p hello` verified on the device.
- **wine** (commit `70d0c16`): `pkgs/wine-x86.nix` exposes `wineWow64`
  (`wineWow64Packages.full`, the 32- **and** 64-bit stack the
  `wine`/`wine64` wrappers already targeted); `bin/wine-x86-deploy.sh`
  rewritten to deploy wow64+box64+mesa+grim and install a world-readable
  launcher at `/var/lib/wine-x86/wine-wow` (per-user prefix
  `$HOME/.wine-x86`, user-targeted `init`/`run`); `pkgs/wine-cli.nix`
  wrapper path moved `/root/wine-x86` → `/var/lib/wine-x86`.
- **System gen 13** deployed (`/nix/store/ygrdq1cja80d99dxfax5nnqs25y3sjn8-nixos-system-gemini-26.11pre-git`,
  `system-13-link`) via `bin/deploy.sh deploy` (job `sys-deploy`, 60 s).
  Stack copy = job `wine-deploy` (117 s, ~1 GB); prefix init = job
  `wine-init` (272 s; `wineboot` exits nonzero but `drive_c` +
  `system.reg` are created).
- **Verified as `cjdell`**: `wine --version` → `wine-11.0`;
  `wine cmd /c echo HELLO-WINE` → `HELLO-WINE` (rc 0). box64 prints
  non-fatal `Error initializing native lib…` lines when it cannot
  dlopen an aarch64 build of a wrapped library; it falls back to
  emulation (cosmetic).

No flash; `boot`/`para` untouched; glass never at risk. Next: on-glass
GUI run of a real PE from the GNOME session (the launcher inherits the
session `WAYLAND_DISPLAY` — the old gemwl hardcode is gone).

## 2026-09-11 — GEMINI: EXODUS revived as a first-class demo + GPU stress test

**Deployed to the device as generation 12 (no flash, no boot.img
change); the on-glass pass is done — with a platform-level performance
finding, below.** The 0.2.0 “GEMINI: EXODUS” demoscene (the engine that
held 60 fps on glass, gen33) had been deleted by the 0.3.0 purge (`6f34d24`)
down to the gemdemo triangle. It was recovered byte-for-byte from git
`32ba356` and revived as a first-class package — `pkgs/gemini-exodus`,
v1.0.0 **“Director's Cut”** — then genuinely upgraded (not just restored):

- `src/stress.rs` (new; also the crate's **lib target**, so its tests run
  on a host with no device GL/ALSA link): `--stress 0..3` load model
  (star/warp/particle/nebula multipliers + a debris field) that only
  ever adds more of the *same* single-pass layers; benchmark capture with
  per-chapter frametime and nearest-rank **p1**/worst. Fixing my first
  p1 cut (it returned the single worst frame as “p1” — the `ceil(n*0.99)-1`
  indexing is now tested). Hard `PART_CAP < SPRITE_QUAD_CAP` guard so a
  MAX-stress burst cannot overflow the sprite `DynVbo` mid-benchmark.
- `src/main.rs`: `--bench SECS [--chapter-secs S] [--json]`, `--stats`
  frametime overlay (60/30 fps reference lines + 30-bucket history +
  GPU/A72/stress readout; drawn with the existing font, suppressed during
  `--bench` unless asked), `--no-a72`, and the **chapter-jump race fix**
  (0.2.0 `swap()`-ed the `jump` atomic in the render thread even when
  audio owned the clock — `[`/`]` could be eaten/raced; now exactly one
  clock consumer). A72 render-thread pinning (cpu.rs) retained.
- `src/show.rs`: the **GEMINI constellation** motif (Castor & Pollux
  stick figure, dotted additive star-quads; S3 VOID + S6 ORIGIN) and the
  **twin-sun finale** (S5).
- `src/synth.rs`: third-below harmony lead in the warp theme;
  low-octave counter-melody + timpani under the anthem finale (existing
  voices only — no new DSP). The synth mix test still passes.
- `src/gfx.rs`: a `rock` debris sprite + stress debris layer; sprite VBO
  sized for the star cap (`STAR_CAP`). Rendering stays **direct,
  single-pass — no FBO/bloom/SSAA**, the 0.1.0 multipass lesson.
- `pkgs/gemini-exodus.nix` (same alsa-lib/libglvnd/wayland recipe + rpath
  `patchelf` as gemdemo); in the rootfs closure via
  `services/gemini-pda.nix`; `packages.aarch64-linux.gemini-exodus` in the
  flake. `bin/gemini-exodus-host-check.sh` = the seconds-long x86_64
  type-check loop (pinned rev, shared CARGO_TARGET_DIR with gemdemo).

Verification (rules 0/3):
- host — `cargo check --release` clean; **5 lib tests + 4 bin tests
  pass** (incl. the synth assertion: audible, `peak ≤ 1.001`,
  `rms < 0.45`, the 0.1.0 over-limited guard).
- aarch64 — `nix build .#packages.aarch64-linux.gemini-exodus` on the
  remote builder OK → `/nix/store/khfnmqyqbrdwr62nlli8spf1phxyd96p-gemini-exodus-1.0.0`
  (aarch64 ELF, 4.98 MB binary; RUNPATH pins libEGL/wayland/libxkbcommon/
  alsa). Nix flake + `nixosConfigurations.gemini` toplevel evaluate.
- **Deployed to gen 12** (`jgbz9491…`) via `bin/deploy.sh deploy`; the
  device binary is exactly the built path
  `/nix/store/khfnmqyqbrdwr62nlli8spf1phxyd96p-gemini-exodus-1.0.0`
  (`/run/current-system/sw/bin/gemini-exodus`). No flash, no reboot,
  `para`/`boot` untouched, glass never at risk (rule 5).

### On-glass performance finding (2026-09-11, GNOME, 2160×1080)

Per-chapter steady (12 s each, `--silent`), avg/p1 fps: ASCENT
30.3/18.6, WARP 28.5/19.2, VOID 26.0/17.6, RENDEZVOUS 35.6/25.5, PLANET
26.5/17.3, ORIGIN 26.2/14.6. GPU `Mali-T880 MC4 (Panfrost)`; A72 pin
confirmed (`cpu: render thread pinned to A72 cluster (cpu8/cpu9)`).

- **The renderer is NOT the bottleneck.** Under identical load the
  trivial `gemdemo` triangle fullscreen = ~40 fps; the full EXODUS scene
  = 35.6 fps (27.8 ms vs 25 ms at stress 0) — the whole show costs only
  **~3 ms more than a flat triangle**.
- The wall is the **platform present path**: `geminipda-drm`'s generic
  `drm_fb_blit` XRGB8888→ARGB8888 **full-panel shadow blit** (a
  fullscreen frame damages the whole panel every frame), running in the
  DRM commit worker — `top` showed `kworker/u20:4+events_unbound` pegged
  at **95 %** of one CPU core during the run. The driver also has no
  vblank.
- **`--stress` scales as designed:** S4 stress 0 → 27.8 ms, stress 3 →
  37.8 ms; S2 WARP stress 3 → 45.0 ms. MAX still runs (~22–26 fps),
  i.e. the stress test does load the GPU on top of the present ceiling.
- The 2026-09-10k receipt (gemdemo **74 fps** after the alpha-loop fix)
  proves the same path exceeds 60 when the machine is idle; this session
  had Chrome (2 procs, ~700 MB) + GNOME loaded.
- **Measurement caveat:** a 6 s sweep with 1 s chapter jumps is not a
  fair number (jumps + warmup dominate — it read 29 fps). Use steady
  per-chapter runs (as above).

**Paths to a clean 60 fps (not yet pursued):** (a) the platform fix from
the 2026-09-10 COSMIC handover — a driver-local single-pass shadow blit
(skip the redundant XRGB→ARGB conversion when the source alpha is already
`0xff`) + vblank timing; needs kernel-delta + boot.img + flash. (b) Run
on **`gemwl`** (GPU-direct LK fb, no shadow plane) — session switch, no
kernel change. (c) Run with the machine idle. New helper
`bin/exodus-on-glass.sh` runs the app in the right session env from the
host.

Docs: new `docs/gemini-exodus.md` (design, chapters, bench usage, A72,
receipts, status); pointers added in `docs/gemdemo.md` (version history),
`README.md` (table), `AGENTS.md` (“Where things live”).

Device left: **gen 12** (`jgbz9491…`) on GNOME, `gemini-exodus` on PATH,
all benchmark windows exited; no flash, no reboot, `para`/`boot`
untouched. Reproduce with `bin/exodus-on-glass.sh [-- ARGS]`.

**User verdict (2026-09-11):** reviewed on glass — *needs more work*
(visual polish); NOT signed off. On-glass performance path still open
(above). Frame dumps taken for review: `--dump 200` at S1/S3/S5
(2160×1080 PPM, ~6.7 MB each).

Next: pick a 60 fps path — (a) implement the driver shadow-blit fast path
(+ vblank) as a kernel delta and flash it, or (b) validate the `gemwl`
session route without a kernel change; then re-run the stress matrix and
record the numbers in `docs/gemini-exodus.md`. Optionally a launcher
entry / gemcli verb.

## 2026-09-11 — niri added as a fourth co-installed desktop session

**Deployed to the device as generation 11 (`343g0r3v…`, commit
`5e43187`); no reflash, no boot.img change.** Added niri 26.04
(scrollable-tiling Wayland compositor) as another GDM Wayland session
alongside GNOME + COSMIC, selectable with `gemcli session set niri`
(`gnome|cosmic|niri|console`).

What changed:
- `config/gemini.nix`: `programs.niri.enable = true`.
- `services/desktop-select.nix`: mode enum + docs gain `niri`; the module
  now `mkForce`s `services.displayManager.defaultSession` back to `null`.
  **Reason:** `programs.niri` sets it to `"niri"` with `mkDefault`, and
  GDM's preStart (which runs after the boot applier) would otherwise
  overwrite the marker's session on every display-manager start.
- `services/scripts/gemini-desktop-apply`: `gnome|cosmic|niri)` case arm.
- `pkgs/gemcli/src/{session.rs,main.rs}`: 4 modes; `session list` shows
  niri; clap value_parser updated. `cargo test --offline` 12/12 (incl.
  `modes_are_the_four_known`); `session list` / invalid-mode rejection
  checked by hand.
- `docs/desktop-selection.md` + README + AGENTS.md updated.

Rule 9 verification at the flake pin `dc5d91f84032` (26.11pre1068949):
- `programs.niri` module present (`nixos/modules/programs/wayland/niri.nix`);
  niri package = `niri-26.04`, `providedSessions=["niri"]`.
- aarch64 `niri-26.04` (`jcqnp2ma…`) **CACHED** on cache.nixos.org
  (`nix path-info --store https://cache.nixos.org`, 2026-09-11).
- eval: `programs.niri.enable = true`, merged sessionPackages
  `["gnome","cosmic","niri"]`, `displayManager.defaultSession = null`.

Deploy (2026-09-11): `bin/deploy.sh deploy` built + pinned the native
aarch64 toplevel and switched the device profile + activated — **gen 11**,
system `343g0r3v3xlsr4yya6k2i8w5607y9nsi-nixos-system-gemini-26.11pre-git`
(commit `5e43187`). The activation explicitly reported `NOT restarting
the following changed units: display-manager.service`, so the live COSMIC
session was not disturbed; **0 failed units** after the switch. On-device
checks: `gemcli session list` shows all four modes; `/run/current-system/
sw/share/wayland-sessions/` carries `niri.desktop`; marker still `cosmic`,
AccountsService `cosmic (wayland)` (live session unchanged).

Not done: on-glass boot of niri — the live marker is still `cosmic`.
Switch with `gemcli session set niri --reboot` (checklist in
`docs/desktop-selection.md`). Nothing flashed (deploy = profile switch).

## 2026-09-10w — COSMIC vs GNOME re-measured: `NoSupportedPlaneFormat` is a false lead, and the "COSMIC ~20 fps" was the already-fixed dual-Mesa bug

User asked (a) whether the `NoSupportedPlaneFormat` noise is easy/worth
fixing and (b) whether COSMIC can be made faster easily. Measured on
glass with `gemdemo` 0.3.0 (fullscreen 2160×1080, Panfrost, one Mesa
26.2.2) across a real COSMIC boot and a real GNOME boot on the current
kernel. No flash; no boot.img change.

**Correction (rule 2).** 2026-09-10v's "COSMIC is still ~17–23 fps" and
the handover's "next lead: smithay format/render-selection" are retired.
That figure was the dual-Mesa CPU-composition defect fixed by gen10 —
this boot had **0** `import for wrong devices` and **0** `swiotlb buffer
is full`, and COSMIC is still ~27 fps, so the import failures are not the
cause either. The format warnings are **benign**:
`NoSupportedPlaneFormat` is smithay's per-candidate `warn!` inside
`GbmBufferedSurface::new`'s fallback loop (`backend/drm/surface/gbm.rs`
71–108), emitted for each candidate that fails before one succeeds.
cosmic-comp's list is `[Fourcc::Abgr2101010, Argb2101010, Abgr8888,
Argb8888]` (`src/backend/kms/device.rs:779`). Our plane advertises only
XRGB8888 (`drm_fb_build_fourcc_list` strips alpha from ARGB8888), so the
first three (AB30/AR30/AB24) fail and **AR24 succeeds via
`get_opaque(AR24) == XR24`** with `use_opaque=true` — the intended
no-alpha path. On-glass receipt: `/sys/kernel/debug/dri/0/framebuffer` →
`format=XR24`, 1080×2160. "Fixing" the warnings would mean advertising
10-bit / padded-24-bit ABGR formats the 8-bit panel can't use, adding a
down-convert to the commit-worker blit — not worth it. Doc noise only.

**Measured (same kernel, same `gemdemo` fullscreen, fresh boots).**

| compositor | sustained | early/burst | hot tasks |
|---|---|---|---|
| COSMIC (`cosmic-comp`) | **24–31 fps** (≈27) | ~28 | cosmic-comp ~16 %, DRM commit kworker ~63 % |
| GNOME (`gnome-shell`) | ~35–47 fps | 65–84 (first ~6 s, then decays) | gnome-shell ~70 %, kworker ~74 % |

So COSMIC is **~1.5–2× slower, not 3–4×**; neither is CPU-bound. A
*windowed* gemdemo under COSMIC was **slower** (~20 fps), i.e. the cost
is per-frame / per-full-screen work in cosmic's DRM submit path, not
just the client rect.

**Root cause (hypothesis, with receipts).** The hot kernel task under
both is `geminipda_drm_primary_plane_helper_atomic_update` →
`drm_fb_blit` (the required XRGB→ARGB shadow blit, 2026-09-10k).
cosmic-comp's `redraw()` does `elem.sync.wait()` then `queue_frame`, so
GPU render and the CPU blit serialise; mutter overlaps / damage-limits
them. The driver also has **no vblank** (`geminipda-drm.c` is a
`DRM_GEM_SHMEM_DRIVER_OPS` module and never calls `drm_vblank_init` or
`drm_crtc_handle_vblank`), which additionally misprices cosmic's frame
pacing (`next_presentation_time`). Both are platform-level, not COSMIC
settings.

**Cheap COSMIC knobs (present, likely no-ops here).**
`COSMIC_DISABLE_DIRECT_SCANOUT`, `COSMIC_DISABLE_OVERLAY_SCANOUT`,
`COSMIC_DISABLE_SYNCOBJ` (`src/backend/kms/{mod,surface/mod}.rs`).
Syncobj is already effectively off — `geminipda-drm` sets no
`DRIVER_SYNCOBJ`, so smithay's `supports_fencing=false` and CPU-waits.
There are no overlay planes to scan out either. Not A/B'd on glass this
session.

**Real levers (kernel delta; would also help GNOME).** (1) cheaper
shadow blit — driver-local single-pass blit (cached shadow → OR
`0xff000000` → `writel` to the WC scanout) instead of the generic
helper's per-row line buffer (2026-09-10k already flagged ~2×); (2)
overlap render/blit — advertise `DRIVER_SYNCOBJ` + honour `IN_FENCE_FD`
so compositors submit a fence instead of CPU-waiting; (3) add vblank
timing (`drm_vblank_init` + a refresh-rate timer →
`drm_crtc_handle_vblank`) so pacing has real timestamps. None is a
config toggle; each needs build + deploy + reboot and an A/B against the
commit-worker CPU.

**Gotcha found.** `bin/device-reboot.sh` is an **unsynced** WDT EXRST:
a `gemcli session set <mode>` immediately before it is **lost** (the
marker reverts to the on-disk value). A "cosmic" reboot therefore came
up GNOME and I briefly mis-read GNOME's fps curve as COSMIC's. Run
`sync` after writing the marker before the hard reset. Separately,
`gemcli session set <mode> --apply && systemctl restart display-manager`
lands on the **GDM greeter** when a session is still active; a clean
reboot autologins fine (autologin confirmed in `/etc/gdm/custom.conf`).

**State left.** Device on **COSMIC** (marker `cosmic`, AccountsService
`cosmic (wayland)`, `cosmic-comp` running); no flash, no boot.img change,
`para`/`boot` untouched; glass never at risk (rule 5). Also annotated the
corrected claims in 2026-09-10v and
`docs/handover-2026-09-10-gnome-perf-touch.md`.

**Next.** If pursued, do lever (1) first (self-contained, lowest risk);
A/B with gemdemo fps + commit-worker CPU before/after. Levers (2)/(3)
are larger and should be separate sessions.

## 2026-09-10v — ONE MESA: 25.0.7 fork retired, delta rebased to nixpkgs 26.2.2 (dual-vendor fix, on glass gen10)

User asked whether COSMIC was really hardware-accelerated (it felt slow).
It is (Panfrost, `Mesa / Mali-T880 MC4`, gemdemo GLES 3.1), but the
investigation found a **real platform defect** and fixed it.

**Root cause (measured).** With `hardware.graphics` on (nixpkgs default —
the config comment claimed off) glvnd got TWO mesa ICDs: the system
vendor (`/run/opengl-driver/share/glvnd/egl_vendor.d/50_mesa.json` →
mesa 26.2.2) and the fork's `/etc/glvnd/egl_vendor.d/50_mesa.json` →
mesa-geminipda 25.0.7. `/proc/<cosmic-comp>/maps` had **both**
`libgallium-25.0.7.so` and `libgallium-26.2.2.so`; the journal showed
`Failed to render texture … import for wrong devices DrmNode { ty: Render }`
→ CPU composition. GNOME reached 73–77 fps on one ICD; COSMIC ~17–23.

**Fix (platform improvement, not a COSMIC bodge):**
- Rebased the delta to the pinned nixpkgs mesa **26.2.2**. The fork's
dma-buf caps hunk is **obsolete**: 26.x's generic
`u_init_pipe_screen_caps()` derives `caps->dmabuf` from the kernel's
`DRM_CAP_PRIME` (receipt: `src/gallium/auxiliary/util/u_screen.c`). The
only surviving functional hunk is the T880 polygon-list whole-BO memset.
- New `patches/mesa-panfrost-polygon-list-26.2.2.patch` (38 lines,
sha256 `b2cc4dbd…`), `patch -p1 --dry-run` clean.
- `pkgs/mesa-geminipda.nix` rewritten as a **thin `.override`** of
`pkgs.mesa` (no bespoke build): lean panfrost, no vulkan, empty
`spirv2dxil` output dropped, `mesonAutoFeatures="auto"` +
disable gallium-va/teflon/intel-rt/vulkan-layers (nixpkgs forces
`auto_features=enabled`, which fails for a panfrost-only driver set),
and `libgbm-external=false` so libgbm is bundled (nested stack keeps its
single-mesa contract).
- `config/gemini.nix`: `hardware.graphics.package = mesaGeminipda`; the
`/etc/glvnd` fork manifest removed; stale "stays off" comment corrected;
`mesaGeminipda` dropped from systemPackages.
- Comments updated in `services/{lxqt,phosh}.nix`, `pkgs/gemwl.nix`.

**Build receipts (rule 0).** Patched mesa drv built on the aarch64
builder (three iterations: gallium-va auto-feature failure → removed;
rusticl-disable broke the `opencl` output fixup → kept enabled;
`spirv2dxil` empty output → filtered from `outputs`). System closure now
has ONE mesa `3pfldrz9n0vwb9kn7ps32w9lv4k4109d-mesa-26.2.2` +
the separate `xv6s7zkv…-mesa-libgbm-26.1.3` (thin, no gallium) — **no
25.0.7**. Feature commit `7a10ad9`; toplevel
`i3zgpzsiym5v6ibzylqnafka610lks6m-nixos-system-gemini-26.11pre-git`;
deployed as **gen10** `vc23yky1n472xb1hm78c3clp1srjqnfp-nixos-system-gemini-26.11pre-git`.

**On-glass verification (gen10).**
- `gemcli session set cosmic --reboot` → cosmic-comp[1514] loads only
`3pfldrz9…-mesa-26.2.2` (`libEGL_mesa` + `libgallium-26.2.2`) +
`mesa-libgbm-26.1.3`; **0** occurrences of "import for wrong devices" /
"Failed to render texture" in the boot journal (previously recurring).
- `gemcli session set gnome --reboot` → gnome-shell loads only the same
26.2.2; gemdemo **77 / 73 / 53 fps** (matches the documented ~74);
0 failed units → **no GNOME regression**.
- Device left on `cosmic` (marker), GNOME/KMS boot.img unchanged.

**Remaining (COSMIC perf, nice-to-have, follow-up).** COSMIC is still
~17–23 fps. Not an acceleration/Mesa-version problem: the compositor logs
`Preferred format AB30/AR30/AB24 not available: NoSupportedPlaneFormat`
because `geminipda-drm` deliberately strips alpha
(`drm_fb_build_fourcc_list`; driver comment lines ~47/~234), and it is a
single-plane shadow framebuffer. Next lead: smithay's format/render-selection
path (and possibly advertising ARGB8888) — investigate separately without
regressing the verified GNOME path.

> **[corrected 2026-09-10w]** The "~17–23 fps" here was the dual-Mesa
> CPU-composition defect fixed by gen10, not a smithay-vs-driver format
> problem; `NoSupportedPlaneFormat` is a benign per-candidate probe
> message (AR24 succeeds via its opaque XR24 variant). See 2026-09-10w
> for corrected measurements (~27 fps COSMIC vs ~35–47 fps GNOME) and
> the real platform levers.

Also committed this session: docs/library-deltas.md mesa update,
handover-2026-09-10-gnome-perf-touch.md Issue-1 update.

## 2026-09-10u — desktop/session selector DEPLOYED to glass (device gen9; GNOME session preserved)

Deployed the selector from `2026-09-10t` (`bash bin/deploy.sh deploy`;
profile switch, **no flash**, no boot.img change).

- **Pre-flight blocker resolved:** the first attempt failed at the GC-pin
  step with `No space left on device` — the host root fs
  (`/dev/nvme0n1p3`, 246 G) was 100 % full (`/nix/store` 220 G, 69
  `gemini-nixos-toplevel-*` GC pins). `nix store gc` (dry-run: 7664
  dead paths) freed **78.3 GiB**; the pins (deployed gens) were left
  intact. Deploy then succeeded.
- **Version line (rule 0):** device generation `system-9-link` =
  `/nix/store/zqkz1vh274mbwjg6rsd86bja1wlg9sij-nixos-system-gemini-26.11pre-git`
  (was `system-8-link`). Deploy log: `logs/jobs/deploy-desktop/`.
- **On-device verification (over g_ether):** `gemcli session status` →
  marker `(unset — compile-time default applies)`, console sentinel
  `absent`, **AccountsService session `gnome (wayland)`** (the new
  `gemini-desktop-apply.service` ran), modes `gnome, cosmic, console`;
  `gemini-desktop-apply.service` + `display-manager.service` both
  `active`. `display-manager` was **not restarted** by the switch, so
  the live GNOME session was preserved. `/run/current-system/sw/share/
  wayland-sessions/` carries `cosmic.desktop` (GNOME's session is
  registered through GDM's own data dirs).
- **Not yet done:** the actual COSMIC boot (`gemcli session set cosmic
  --reboot`) — needs eyes-on-glass (rule 5); return command is
  `gemcli session set gnome --reboot`. The `console` mode is likewise
  one marker change away.

Next: run the COSMIC on-glass checklist in `docs/desktop-selection.md`
(steps 3–7), then log the outcome here.

## 2026-09-10t — DESKTOP/SESSION SELECTOR: COSMIC co-installed with GNOME + console mode; `gemcli session` (build-level; nothing flashed)

User ask: try the COSMIC desktop without breaking GNOME — can they
co-exist, can `gemcli` switch / set the startup default, and can there
be a third no-desktop (framebuffer console) mode. Answer + implementation:
**`docs/desktop-selection.md`**.

**Yes, they co-exist.** GNOME and COSMIC are ordinary GDM Wayland
*sessions*: `services.desktopManager.gnome.enable` and
`services.desktopManager.cosmic.enable` are independent and each append
their session to `services.displayManager.sessionPackages` (eval:
`sessionNames = ["gnome","cosmic"]`; portal `configPackages` merges to
`gnome-session + xdg-desktop-portal-cosmic`). Only one compositor owns
the `geminipda-drm` KMS CRTC at a time — a runtime fact, not a build
conflict.

**Runtime selection (new):**
- `services/desktop-select.nix` (`services.geminiDesktop.*`): a oneshot
  `gemini-desktop-apply.service` runs `Before=display-manager.service`
  and resolves `/var/lib/gemini/desktop` (persistent marker).
- `gnome`/`cosmic` → the applier sets the AccountsService session for
  `cjdell` (`SetSession` + `SetSessionType=wayland` via `busctl` — the
  exact mechanism nixpkgs' `set-session.py` uses for
  `displayManager.defaultSession`) and removes the console sentinel.
- `console` → creates `/run/gemini-console`, which suppresses GDM via
  `ConditionPathExists=!/run/gemini-console` (added by the module), and
  starts `getty@tty1` (autologin cjdell) so the fbcon console stays.
- `services/gnome.nix` no longer sets `displayManager.defaultSession`
  (it was emitted as a GDM preStart `set-session` call that would
  overwrite the marker every start); eval now reports `null`.
- `pkgs/gemcli/src/session.rs` (new): `gemcli session
  status|list|set <gnome|cosmic|console> [--apply|--reboot]|apply`. 12th
  unit test (`modes_are_the_three_known`); `cargo test` green.

**COSMIC build facts (rule 9, 2026-09-10):** pinned nixpkgs
`dc5d91f84032` has the `cosmic` module + cosmic-session 1.6.0
(`providedSessions=["cosmic"]`); aarch64 `cosmic-session`,
`cosmic-comp`, `cosmic-panel`, `cosmic-settings`,
`xdg-desktop-portal-cosmic` all **CACHED** on cache.nixos.org (checked
with `nix path-info --store https://cache.nixos.org`). No big local
compiles added.

**Build receipt (rule 0 — feature commit `b318569`):** flake eval green;
aarch64 toplevel built via `sudo nix build --store local --option
builders @/etc/nix/machines --fallback
.#packages.aarch64-linux.toplevel` →
`/nix/store/shsjkxqa3x9nidbbbpmsdyv4lvvpwn1m-nixos-system-gemini-26.11pre-git`
(the 192.168.49.191 builder; a clean-tree rebuild). Inspected the
generated units:
`display-manager.service` carries `ConditionPathExists=!/run/gemini-console`;
`gemini-desktop-apply.service` runs the packaged script with
`GEMINI_DESKTOP_DEFAULT=gnome` / `GEMINI_DESKTOP_USER=cjdell`; both
`gemini-desktop-apply` and `gemcli` are on `sw/bin`.

**Nothing flashed / deployed** — a plain `bin/deploy.sh` profile switch
would install this (no boot.img change needed: the KMS driver is
unchanged). On-glass COSMIC is **unverified**; the checklist (session
switch each way, journal + panel eyes-on per rule 5) is in
`docs/desktop-selection.md`. GNOME + the console path ride the already
verified GNOME/KMS stack; COSMIC's `cosmic-comp`/smithay capability
against `geminipda-drm` is the open question.

Files: `services/desktop-select.nix`,
`services/scripts/gemini-desktop-apply`, `pkgs/gemcli/src/session.rs`,
`pkgs/gemcli/src/main.rs`, `config/gemini.nix`, `services/gnome.nix`,
`docs/desktop-selection.md`, README + AGENTS rows.

Next: `bin/deploy.sh` (device on the g_ether link) then step through
the checklist; if COSMIC fails to bring up the output, capture the
journal before touching `geminipda-drm` (shared with verified GNOME).

## 2026-09-10s — GNOME on-screen keyboard permanently suppressed (mutter `touch_mode` root cause; shell extension + locked dconf)

User ask: stop GNOME ever showing the OSK — "this device has a real
keyboard". The a11y toggle was **already off** (checked cjdell's dconf,
not root's), so the OSK was the **automatic (touch-mode) path**.

**Root cause (source-level, GNOME 50.4).** gnome-shell creates the OSK
when `a11y(screen-keyboard-enabled) || (seat.touch_mode &&
lastDeviceIsTouchscreen)` (`js/ui/keyboard.js`, `_syncEnabled()`), and
mutter sets `touch_mode = !has_pointer` for a seat with a touchscreen and
no tablet-mode switch (`src/backends/native/meta-seat-impl.c`,
`update_touch_mode()`). The Gemini has a touchscreen + a keyboard + **no
pointer** ⇒ `touch_mode=true`. A keypress does not clear `_lastDevice`
(keyboard devices are ignored), so touching/focusing a text field popped
the OSK. Pre-fix the shell log even showed it: `maybeHandleEvent`
(`keyboard.js:1159`) threw on a null actor, which only runs when the OSK
object exists.

**Fix (committed `03b5379`; deployed as gen8 the same session):**

- new `pkgs/gnome-extension-no-osk/` — a GNOME 45+ ESM Shell extension
  forcing `KeyboardManager._lastDeviceIsTouchscreen() = false` (private
  method; re-check on a gnome-shell upgrade). `nix-build` validated.
- `services/gnome.nix` — install it in `environment.systemPackages`,
  and in the system dconf DB set + **lock**
  `org.gnome.shell enabled-extensions=[no-osk@gemini-nixos]` and
  `org.gnome.desktop.a11y.applications screen-keyboard-enabled=false`.
  `nix eval .#nixosConfigurations.gemini.config.programs.dconf…` evaluates
  and shows both settings + all three locks.

**Evidence (on the LIVE shell, GNOME Shell 50.4).** Installed the
extension by hand under `~/.local/share/gnome-shell/extensions/` and ran a
temporary diagnostic that set `_lastDevice` to a fake touchscreen device
and called the real `_syncEnabled()`:

| no-osk ext | `seat.touch_mode` | OSK object |
|---|---|---|
| off | `true` | **CREATED** |
| on | `true` | **not-created** |

The diagnostic was then removed; the hand-installed extension was left
enabled, so the device was immediately OSK-free. **Device state:** the
GNOME session was restarted via `systemctl restart display-manager`
twice during the A/B and once more after the deploy (autologin restored
it each time, ~4–6 s); `para`/`boot` untouched; no boot.img/reflash.

**Deployed + verified (same session).** `bin/deploy.sh deploy` (57 s)
installed the extension as a SYSTEM extension and switched the device to
gen8 `mf8baq82aw90djz5ppwq4fvack2kmbsh-nixos-system-gemini-26.11pre-git`.
The hand-installed user copy was then removed and `display-manager`
restarted; `gnome-extensions info` reports the extension from
`/run/current-system/sw/share/gnome-shell/extensions/no-osk@gemini-nixos`,
Enabled: **Yes**, State: **ACTIVE**, and both
`org.gnome.shell enabled-extensions` and
`org.gnome.desktop.a11y.applications screen-keyboard-enabled` read the
intended values with `gsettings writable` = **false** (the dconf locks
hold — so the system extension is the sole source and cannot be turned
off). **Doc:** `docs/desktop-plumbing.md` §"On-screen keyboard".

## 2026-09-10r — A2DP stutter + after-playback crackle: ROOT CAUSE = the STP PSM (fixed in the delta, verified on glass)

User report: A2DP headphones stutter **predictably** even at idle, plus a
regular crackle **after playback** ("ring buffer not purged"). Both
**root-caused and fixed** — kernel delta + a rootfs deploy, no boot.img
change. New doc **`docs/bluetooth-a2dp.md`** carries the full receipts.

**Symptom, at the transport level.** `dmesg` showed periodic
`[STP] MTKSTP_SYNC: go to MTKSTP_RESYNC2, buff = 7f` →
`mtkstp_process_packet: expected_rxseq = X, parser.seq = Y` →
`stp_do_tx_timeout` (`Resend STP packet`), with HCI-STP TX stalls of exactly
**1.052 s**. `btmon` + the ACL sizes showed the A2DP transport stuck at
**one 577-byte frame per 80 ms (~57 kbps)** while SBC needed ~265 kbps — a
~2× source underrun, i.e. the predictable stutter. `MTKSTP_TX_TIMEOUT` is
180 ms (`stp_core.h:112`); sweeping `btif_wak_hb_ms` (0–100) and the idle
time did **not** change the ~1.5 s resync period, and the resyncs happened
with **no audio traffic at all** → not load-driven, not the WAK heartbeat.

**Root cause — the STP power-save mode (PSM).** Its sleep action is
`mt_combo_plt_enter_deep_idle(COMBO_IF_BTIF)`, whose Android combo-stub
backend is **not wired on this port**: it logs `NULL function pointer` and
returns −1, so the chip never enters deep idle. The PSM therefore saved no
power, but it still gated STP TX past `MTKSTP_TX_TIMEOUT`, injecting the
`0x7f` resync. Decisive A/B via the debug proc
(`echo "0 0" > /proc/driver/wmt_dbg` — commands are hex `<id> <arg>`, id
`0x0` = PSM ctrl):

| PSM | `RESYNC2` | `stp_do_tx_timeout` | `deep idle fail` | per 20 s idle |
|---|---|---|---|---|
| on (default) | 12 | 41 | 198 | |
| **off** | **0** | **0** | **0** | |

With PSM off during playback: 0/0, and the ACL stream became
**237 frames / ~6 s at 818–887 B** ≈ **~265 kbps** — the correct SBC rate.

**Fix (kernel delta, `common_main/core/wmt_lib.c`)**: `gPsEnable` defaults
to 0 and **`wmt_lib_ps_ctrl()` always disables** (never sets `gPsEnable=1`),
because both `mt6630_sw_init()` and `mtk_wcn_wmt_func_off(BT)` (`wmt_exp.c`)
ask to re-enable it — a one-shot init disable would not survive a BT
off/on. `wmt_lib_ps_enable()` is documented as intentionally off. (PSM
starts disabled anyway: `stp_psm_init()` ends with `_stp_psm_disable()`.)

**Deployed + verified on glass 2026-09-10.** Toplevel
`94bw47dh6bhr1rk4jafva9nimz3avwjb` built (`deploy build`, 386 s) and
deployed (`deploy deploy`, gen7); a WDT EXRST reboot loaded the new
`mtk_wcn`. Module hot-reload is NOT viable — after `rmmod`/`modprobe` the
CONSYS stayed `POWER_OFF` and wlan0/hci0 never came back; reboot required.
After the reboot, with the PSM default (no proc write): **idle 12 s = 0
resyncs / 0 timeouts / 0 deep-idle-fails**; **30 s playback = 0/0**;
**25 s idle after stop = 0 A2DP frames** (sink `state: "suspended"`, so the
after-playback crackle source is gone too); **wlan0 stayed associated on
5 GHz and pinged**; MDR-ZX330BT reconnected and is the default sink.

**Not a regression / no power cost:** the PSM's deep-idle backend is absent
(`NULL function pointer`), so disabling it saves nothing; Wi-Fi is
unaffected. Re-enable only if a real MT6630 deep-idle backend is written.

**Kernel-delta hygiene updated:** the delta now diverges from the fork in
**five** files (three `geminipda-drm` files, the DT DRM node, and
`wmt_lib.c`); `bin/sync-kernel-delta.sh` will list `wmt_lib.c` and abort.

## 2026-09-10q — Power modes (GNOME performance → A72) + sleep turns the A72 off + speaker L/R fix & amp toggle

Four user asks this session: (1) silver-button sleep must also power
down the A72 cores, (2) a **performance** power mode visible in GNOME
that activates the A72s, (3) the built-in speakers are **L/R swapped**
(jack is fine), (4) a way to toggle the internal speaker amp from GNOME
for headphone-only use. All four are implemented at build level; the
system closure builds green and gemcli's 11 unit tests pass.
**[updated same session] It WAS then flashed (deployed via
`bin/deploy.sh`, gens 4→6) and verified on glass — see "On glass"
below; two bugs found on the device were fixed and redeployed.**

**Receipts (build-only):** `nix build
.#nixosConfigurations.gemini.config.system.build.toplevel` →
`/nix/store/7chz6w54s5szba798g28a8vcglwclkx0-nixos-system-gemini-26.11pre-git`
(rc=0, patched PPD built with `doCheck=false -Dtests=false`); gemcli
built + test suite `ok. 11 passed; 0 failed` (added the A72-list and the
PPD state.ini parser tests). No boot.img/kernel change — rootfs only.

### 1+2. Power modes: PPD placeholder patch + `gemini-power-profile` → A72

Design + receipts: **new `docs/power-modes.md`**. Summary:
- power-profiles-daemon's generic **placeholder** driver advertises only
  power-saver + balanced (GNOME hides performance). New one-line patch
  `patches/power-profiles-daemon-placeholder-performance.patch` adds
  `PPD_PROFILE_PERFORMANCE`. PPD's own test suite asserts the unpatched
  placeholder (e.g. `test_amd_pstate_error`), so the patched package
  builds with checks off (`-Dtests=false`); the daemon is exercised on
  glass.
- New `services/power-profiles.nix`: PPD (patched) +
  `gemini-power-profile.service` = `gemcli profile watch`. New
  `pkgs/gemcli/src/profile.rs`: reads PPD's `state.ini` on the poll
  path (never spawns the Python `powerprofilesctl` in the loop) and maps
  **performance ⇒ `a72 up both`**, balanced/power-saver ⇒ `a72 down`.
  Sleep-aware (does nothing while `sleep::sleeping()`), with a one-time
  20 s per-boot settle (`/run` flag) so a persisted performance profile
  cannot bring the cluster up during boot (P2 rule).
- `config/gemini.nix` imports the new module; `gemcli profile
  watch|status|set` added to the clap tree.

### 1. Sleep now powers the A72 cluster down (and restores it)

`pkgs/gemcli/src/sleep.rs`: `sleep on` records whether the A72 was up
and runs `a72::down("both")` (the WDT-guarded secure cl2-down, DA9214
rail drop) after the A53 offlines; `sleep off` brings it back after the
A53s. Default cold-boot state leaves it down, so this is a no-op unless
performance (or a manual `a72 up`) is active. `docs/power-sleep.md`
updated (§What the light sleep does, item 3).

### 3+4. Speakers: L/R-correcting virtual sink + amp follows the output

Details: `docs/desktop-plumbing.md` §Speakers. Summary:
- New `services/pipewire/60-gemini-speakers.conf` — a filter-chain
  `Audio/Sink` **`gemini_speakers`** ("Built-in Speakers") that crosses
  the channel pair (two `copy` nodes, swapped input/output arrays); its
  playback stream is pinned to the hardware sink (`target.object`,
  `node.passive`, `node.dont-fallback`, `node.link-group`) so it cannot
  loop back into itself.
- `services/pipewire/50-gemini-alsa-s16.conf` now also renames the
  hardware sink **description** to "Headphones / Jack"; its `node.name`
  stays the ALSA/ACP-generated `alsa_output.platform-sound.stereo-fallback`
  (WirePlumber monitor rules only document `node.description`).
- New `gemini-speakerd.service` (`gemcli speaker watch`, `speaker.rs`):
  follows the PipeWire default sink (`wpctl inspect @DEFAULT_SINK@`) and
  drives the amp pads 243/244 — default `gemini_speakers` ⇒ amps ON,
  anything else ⇒ OFF. Selecting an output in GNOME's Sound menu / Quick
  Settings is therefore the amp toggle.
- `services/scripts/audio-output` now also sets the PipeWire default
  sink (new `sync-default` verb, retried at boot by
  `gemini-audio-defaults`), so the CLI and GNOME agree; persists in
  WirePlumber state.
- **Pad reads are now side-effect-free**: the gpio chardev v1 API can
  only read by requesting a pad as *input*, which releases the amp's
  output drive — so `speaker status`/`selfcheck`/`amps_on` read the
  pinctrl DOUT register via /dev/mem (spkamp's method). `gpio.rs` lost
  the unsafe `read_levels`/`get_values` path.

### On glass (deployed gens 4–6, 2026-09-10q) — with the bugs it found

Flashed via `bin/deploy.sh deploy <toplevel>` (no boot.img needed).
Verification and the three device bugs it exposed:

- ✅ **PPD advertises performance** — `powerprofilesctl list` shows it;
  the GNOME Power Mode selector has all three; profile persists in
  `state.ini`.
- ✅ **performance ⇒ A72 up, balanced ⇒ A72 down**, driven through PPD
  exactly as GNOME does: balanced→0-7 in ~2.5 s; performance→0-9 in
  ~2–4 s (single `cl2-up` attempt).
- ✅ **Speaker amp follows the default sink**: `audio-output headphone` →
  `dout=0`; `audio-output speaker` → `dout=1`. Virtual sink
  `gemini_speakers` + "Headphones / Jack" present in `wpctl status`.
- ✅ **Sleep round-trips the A72**: `sleep on` powered the cluster down
  (while the A53s were still online) and `sleep off` restored it — full
  log in `/tmp/sleeptest.log`, device stayed reachable, 0 failed units.
- ✅ **Settle change-bypass**: with no settle flag, picking balanced
  applied in 3 s (not 20 s).
- ✅ Final state: profile=performance, cpu 0-9, all five gemini units
  active, 0 failed, running gen6 `489anpkzw2z7bpf50yyrc9qnnb6xhqlc`.

**Bugs found on glass and fixed this session (all redeployed):**
1. **Ordering cycle → unit skipped at boot.** `gemini-power-profile`
   was `after power-profiles-daemon.service`, but upstream PPD is
   `After=multi-user.target` + `WantedBy=graphical.target`, so systemd
   deleted the job ("Job gemini-power-profile.service/start deleted to
   break ordering cycle"). Fixed: `wants` only, no ordering
   (`services/power-profiles.nix`).
2. **90 s settle = GNOME says performance, cores off.** Shortened to
   20 s and a profile change during the settle now applies immediately
   (`pkgs/gemcli/src/profile.rs`).
3. **`sleep on` hung the device** (first test): the A72 teardown ran
   *after* the A53 offlines, and the watcher could race the bring-up.
   Fixed: `state=sleeping` is written FIRST (watcher stands down before
   any teardown), the A72 is powered down **before** the A53 offlines
   (the verified watcher order), and all A72 ops take an flock
   (`/run/gemini-a72.lock`) so two secure ops can never overlap
   (`pkgs/gemcli/src/sleep.rs`, `a72.rs`, `profile.rs`). The device
   needed a manual reboot after this one; the redeployed round-trip
   then passed (above).

### On-glass work still owed

1. Confirm the L/R correction by ear: play a left/right test tone to
   the **Built-in Speakers** sink; select **Headphones / Jack** and
   confirm the speakers go silent while the jack still plays.
2. Reboot with performance selected and confirm the 20 s settle + a
   clean boot (the settle path was only exercised by removing the /run
   flag, not by a real reboot).
3. Use the silver button (not the CLI) for one sleep/wake with the A72
   up.
4. Log a version line per flash (rule 0) — the gen4–6 toplevels are
   pinned by `bin/gc-pin.sh` (roots `gemini-nixos-toplevel-20260910-*`).

Device left: **performance, cpu 0-9, gen6, 0 failed units, reachable
over g_ether.**

## 2026-09-10p — GNOME: Settings→Sound device list + Fn volume/brightness keys wired

User report: GNOME Settings' Sound section showed **no devices**, and
the Fn+C/V (volume down/up) and Fn+B/N (brightness down/up) keys did
nothing. Both were config gaps, not driver work; fixed in
`services/gnome.nix` and verified **on glass** the same session.

### 1. Audio — one system PipeWire session, redirected into the GNOME session

`services/audio.nix` runs the ONE PipeWire/WirePlumber/pipewire-pulse
session system-wide with `XDG_RUNTIME_DIR=/run/gemwl-audio` (the
gemwl/phosh/LXQt-era design — the desktop was a system service with no
logind session). GNOME under GDM is a real logind session with
`XDG_RUNTIME_DIR=/run/user/1000`, so every GNOME audio client
(Settings→Sound, gsd-media-keys' Gvc for the volume keys, gnome-shell's
OSD) looked in `/run/user/1000/pulse/native` → empty device list.

Fix: `services/gnome.nix` `environment.sessionVariables` point the
session at the existing system session (no second PipeWire — the MT6351
S16 path is solved once):

- `PULSE_SERVER=unix:/run/gemwl-audio/pulse/native`
- `PIPEWIRE_RUNTIME_DIR=/run/gemwl-audio`

Verified: pulse socket `srwxrwxrwx`; `pw-cli ls Client` shows "GNOME
Shell Volume Control", "GNOME Volume Control Media Keys"
(gsd-media-keys) and Blueman; the gsd volume keys drive the sink.

### 2. Fn media keys — mutter resolves a keysym at LEVEL 0, so bind keycodes

The Fn layer is xkb level 3 (`ISO_Level3_Shift` on RALT = Mod5):
Fn+C/V/T = XF86AudioLower/Raise/Mute, Fn+B/N =
XF86MonBrightnessDown/Up (`config/xkb/symbols/gemini`). mutter matches
on (keycode, mask) and does **not** mask Mod5, so a naive
`<Mod5>XF86AudioLowerVolume` *looked* right — but mutter resolves a
**keysym** accelerator to the **lowest** level that yields it
(`add_keysym_keycodes_from_layout()` stops at the first match), and the
compiled keymap carries those XF86 keysyms at level 0 on the standard
evdev consumer keycodes (`<VOL->`=0x7a, `<MUTE>`=0x79, `<I232/233>`).
So it bound to (0x7a, Mod5) and never matched Fn+C (0x36, Mod5).
On-glass A/B: `gsettings set … "['<Mod5>0x39']"` + injected Fn+N raised
the backlight; the keysym form did nothing.

Fix: bind the Fn layer's xkb keycodes (evdev code + 8) as schema
DEFAULTS via `services.desktopManager.gnome.extraGSettingsOverrides`,
with `extraGSettingsOverridePackages = [ pkgs.gnome-settings-daemon ]`
so gsd's media-keys schema is in the override set:

- brightness up/down = `<Mod5>0x39` / `<Mod5>0x38` (N/B)
- volume up/down     = `<Mod5>0x37` / `<Mod5>0x36` (V/C)
- mute (Fn+T)        = `<Mod5>0x1c`

Deployed + cold-booted; verified on glass: sink 0.39→0.33 (Fn+C),
→0.44 (Fn+V), `[MUTED]` toggle (Fn+T), backlight 25↔37 (Fn+B/N).
Full rationale + mutter/kernel receipts: `docs/desktop-plumbing.md`
§Volume.

### New test tool

`bin/kb-inject.c` — writes raw EV_KEY chords (`fn+c`, `fn+v`, …) to the
keyboard's evdev node so the bindings can be exercised over ssh with no
human at the matrix. Build aarch64 on the remote builder and `nix copy`
it to the device (recipe in the file header). Worth recording: in v6.6
`input_inject_event()` calls `input_handle_event()` and dispatches to
all handles, so writing to the evdev node *does* reach
libinput/mutter (older-kernel lore says otherwise); the first Fn test
failure was purely the wrong (keysym) binding, not injection.

### Also

- `bin/deploy.sh` / `bin/wine-x86-deploy.sh`: `nix copy` now runs with
  `NIX_SSHOPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"`.
  The 2026-09-10n repartition gave the device a new SSH host key and
  `nix copy` (unlike `device-ssh.sh`) does not override known_hosts, so
  every deploy aborted with "REMOTE HOST IDENTIFICATION HAS CHANGED".
- `docs/desktop-plumbing.md` §Volume/backlight/checklist updated; the
  old "physical volume keys are NOT wired (phoc+phosh)" text is gone.

### Version line / device state

- toplevel `qng3px7fjq5mbrxm8j979a2iiqyq9xwj-nixos-system-gemini-26.11pre-git`
  (gen built from the keysym → keycode correction; the first deploy this
  session `h068b11d…-gnome-gsettings-overrides` was the keysym form).
- No kernel/boot.img flash: rootfs-only generation; boot.img stays the
  2026-09-10 `geminipda-drm` build.
- Device left on the new generation, `systemctl --failed` empty, para
  cleared (normal NixOS boot); the temporary dconf overrides used for
  the A/B were reset.

## 2026-09-10o — fresh rootfs had NO WiFi: image embedded uid 1000 (make_ext4fs shim fixed)

Follow-up to 2026-09-10n (clean install). After the fresh image booted,
GNOME showed no WiFi — and the `logrotate` failures recorded in 09-10n
were the SAME root cause, not unrelated.

### Root cause (in the IMAGE, not the NM config)

- `nmcli device` → `wlan0:wifi:unmanaged`; NM journal:
  `plugin: skip invalid file …/libnm-device-plugin-wifi.so: file has
  invalid owner (should be root)`. NM refuses to load a plugin not owned
  by root, so the WiFi device type never registers.
- `ls -ln` showed the **whole store owned `1000:100`** (logrotate's check
  is the identical "file owner is wrong" rule).
- `debugfs -R 'stat <path>' system.img` → `User: 1000 Group: 100`: the
  ownership is baked into the ext4 image, not a boot-time artifact.
- Cause: the R13 shim `pkgs/make-ext4fs-shim.nix` builds the image with
  `mke2fs -d DIR`, and **`mke2fs` copies the source uid/gid verbatim**.
  In a Nix build sandbox the tree is owned by the build user (uid 1000 on
  the aarch64 builder), so every image from the shim had uid-1000 store
  files. (The pre-repartition install had been repaired in place by a
  `nix copy`/rebuild that rewrote ownership; a clean image reintroduced it.)

### Fix (image build — the durable one)

`pkgs/make-ext4fs-shim.nix` now re-execs itself once under `fakeroot`
(guarded by `_MAKE_EXT4FS_IN_FAKEROOT`) and runs `chown -R 0:0 "$dir"`
before `mke2fs`, so mke2fs writes **root-owned** inodes (nixpkgs'
`make-ext4-fs` does the same). Verified standalone on a uid-1000 tree:
debugfs shows `User: 0 Group: 0` (was 1000/100) and the outer `faketime`
mtime (`0xfffff1f1`) is preserved, so fakeroot appends to `LD_PRELOAD`
and image determinism is unchanged. Inputs: `fakeroot` added.

### Runtime repair of the running device (no reflash needed)

```
mount -o remount,rw /nix/store
chown -R root:root /nix/store /nix/var   # ~40 s
systemctl restart NetworkManager
```
→ `wlan0:wifi:connected:The Lab`, `nmcli device wifi list` populated,
`logrotate-checkconf.service` active. (The final `remount,ro` is refused
while the fs is busy; harmless — the boot mount unit restores ro.)

### On-glass validation (same day)

Rebuilt `.#packages.aarch64-linux.default` from the fixed shim, then
reflashed (`bin/repartition-nixos.sh apply --yes` + `boot`) and cold-booted:

- store is **natively root-owned** (wifi plugin `0:0`) — **NO runtime
  chown needed**;
- NetworkManager journal has **no `plugin: skip … invalid owner`** lines;
- `wlan0:wifi:connected:The Lab`; profiles `The Lab` +
  `The Lab 2.4GHz` present; `display-manager.service` active,
  gnome-shell running, 0 restarts;
- `systemctl --failed` is **empty** — the `logrotate` pair from 09-10n
  now passes;
- `/` on p27 grew to 50.3 G free; rule-5 display gate still clean
  (0 hits for mediatek-drm/mtk-mmsys/tps65132/phy-mtk).
- new `system.img` sha256 `db1d85e2…` (root-owned); `boot.img` unchanged
  (`0b176d93…`). The device is left running NixOS with para cleared.

### Receipts

- NM journal: `file has invalid owner (should be root)` (wifi +
  bluetooth + adsl plugins).
- `debugfs` image inode: `User: 1000 Group: 100` before, `0:0` after.
- Host reproduction: `mke2fs -d <uid-1000 tree>` → debugfs `User: 1000`;
  under `faketime … fakeroot` + `chown -R 0:0` → `User: 0`.
- Fix commit: `pkgs/make-ext4fs-shim.nix` (2026-09-10).

## 2026-09-10n — ONE-WAY REPARTITION: TWRP + NixOS only (58 GiB p27 `linux`), clean install on glass

User request: repartition so **TWRP + NixOS are the only bootable
systems**, NixOS owns all flash not needed by TWRP/the boot partitions,
and **no existing user data is needed (clean install)**. Done, flashed
and verified on glass.

### What changed (GPT — verified against `stock-dump/repartition-20260910/`)

- p27 `system` (2.5 GiB) + p28 `cache` + p29 Debian `linux` (27.7 GiB)
  + p30 `boot2` + p31 `boot3` + p32 `userdata` (27.3 GiB) → ONE partition
  **p27 `linux`, start 458752 (0xE000000 = 224 MiB), size 121651167 =
  58.006 GiB**.
- `flashinfo` preserved (was p33) and renumbered **p28**
  (122109919..122142686 = up to last-lba); p1..p26 keep their **exact**
  offset/GUID/name (p22 `boot` at 362496, p26 `keystore` ends 458751).
- Result: **p1 `recovery` (TWRP) + p22 `boot` (NixOS) + p27 `linux`
  (rootfs) are the only system partitions.**

### Tooling

- **New `bin/repartition-nixos.sh`** (`plan|backup|apply --yes|verify|boot`):
  builds/【uses】sgdisk-verified GPT blobs, **byte-verifies** the GPT
  read-back *before* the destructive write, streams the rootfs to the
  **raw disk offset** (the ~8 GB image does not fit TWRP's ~1.9 GiB
  `/tmp`), leaves para sticky (TWRP) and makes `boot` explicit.
  `converge_twrp` is state-aware (Linux→para+WDT EXRST, Android→
  `boot-switch.sh twrp`, POC→power key).
- `bin/flash-nixos.sh`: `rootfs` now **streams to `by-name/linux`** (was
  `userdata`); the `debian` verb + `twrp_para` helper removed; the build
  comment fixed to `.#packages.aarch64-linux.default`.
- `bin/boot-switch.sh`: `debian` verb removed; `android` is now just
  "clear para → boot NixOS".
- `devices/planet-geminipda/default.nix`:
  `system_partition_destination = "linux"`.

### Receipts (2026-09-10)

- `boot.img` sha256 `0b176d93…` (9986048 B); `system.img` sha256
  `ccd15c49…` (7940786782 B, ext4 `NIXOS_SYSTEM`, mke2fs geometry).
- New GPT primary sha256 `cbdd72fc…`, backup `ce27b641…`; pre-repartition
  GPT `675a455c…`/`a12d752c…`; + `recovery para proinfo nvram lk lk2
  boot` pulls — all in `stock-dump/repartition-20260910/` (gitignored).
- `apply`: GPT read-back **byte-identical**; rootfs **full 7.94 GB
  read-back md5 identical** (`cf8ebc65d932b703869845c1b1b12c1c`);
  `boot.img` exact-length md5 identical (`f6881750…`).
- `boot`: NixOS came up on **/dev/mmcblk0p27** (by-label
  `NIXOS_SYSTEM`), fs auto-grew to **58.0 GiB** (57G size / 50G free),
  GNOME up (`display-manager.service` active, gnome-shell present),
  p28 = `flashinfo`, p30–p33 gone. **Rule-5 gate clean**: dmesg shows
  only `geminipda-drm` + panfrost — no mediatek-drm/mtk-mmsys/phy-mtk/
  tps65132.
- Pre-existing + unrelated: `logrotate.service` +
  `logrotate-checkconf.service` fail ("Ignoring …-logrotate.conf
  because the file owner is wrong") — present before this work.
  **[corrected 2026-09-10o — NOT unrelated: same image-uid-1000 cause
  as the missing WiFi; fixed with the make_ext4fs shim.]**

### Follow-ups

- `gemini-boot-debian` (CLI + unit) is now a dead marker (the initrd
  falls back to NixOS) — remove from `services/gemini-pda.nix` on the
  next config pass.
- Docs updated: `docs/repartition-android-space.md` §12, README, AGENTS,
  `docs/disaster-recovery/{README,inventory}.md`.
- Device left running NixOS (para cleared = boots p27); no reflash
  needed for normal iteration.

## 2026-09-10m — keyboard REALLY fixed (gemini was not in the xkb *registry*) + touch 180

Follow-up to 2026-09-10l, which was necessary but not sufficient.

### Keyboard — root cause: libgnome-desktop's XkbInfo could not find "gemini"

Setting the dconf source to `[('xkb','gemini')]` and fixing the model
did **not** change the keymap.  The missing piece:

- GNOME Shell's `KeyboardManager` does not compile whatever the source
  says.  It validates the source id through **libgnome-desktop's
  `XkbInfo`** (`js/misc/keyboardManager.js`: `_xkbInfo.get_layout_info(id)`;
  `getXkbInfo()` = `new GnomeDesktop.XkbInfo()`), and if the lookup
  fails it silently uses `DEFAULT_LAYOUT = 'us'` — no warning, no journal
  entry.
- `XkbInfo` enumerates layouts with **libxkbregistry** (`rxkb`), which
  reads only `rules/evdev.xml` from the xkb tree
  (gnome-desktop 44.5 gnome-xkb-info.c:
  `rxkb_context_new (RXKB_CONTEXT_NO_FLAGS)` + `rxkb_context_parse(ctx, "evdev")`).
  `rxkb` does **not** read `XKB_CONFIG_EXTRA_PATH`, and "gemini" was in
  no registry.  So the lookup always failed.
- Receipts (before the fix): `xkbcli list | grep gemini` empty; a small
  Wayland client (`bin/wl-keymap-dump.c`) dumping the keymap mutter sent
  clients showed a plain **two-level US** map (`<AE01> = [1, !]`, no
  level3, `RALT` not ISO_Level3_Shift).

Fix (all in `services/gnome.nix` + a new package):

- **`pkgs/gemini-xkeyboard-config.nix`** — a copy of the
  `xkeyboard-config` tree (from `share/X11/xkb`; `etc/X11/xkb` is a
  symlink, so `cp -rL`) with (a) `symbols/gemini` installed and (b) a
  `<layout>` entry for gemini inserted into `rules/evdev.xml`.  Sanity
  asserts in the derivation.
- `XKB_CONFIG_ROOT = "${geminiXkeyboardConfig}/etc/X11/xkb"` in
  `environment.sessionVariables` (replaces `XKB_CONFIG_EXTRA_PATH` for
  GNOME).  Both `xkbcommon` and `rxkb` honour it, so mutter and
  gnome-desktop see the same tree.
- The 2026-09-10l dconf system-db lock (`sources=[('xkb','gemini')]`)
  stays — it is what *selects* the layout.

Verification (host + glass):

- `XKB_CONFIG_ROOT=$(out)/etc/X11/xkb xkbcli list` → `- layout: 'gemini'`.
- `xkbcli compile-keymap --layout gemini --model pc105 --rules evdev`
  → `<AE01> = [ 1, !, |, F1 ]`, `<AE03> = [ 3, £, \, F3 ]` (4 levels).
- On glass, after deploy+reboot: gnome-shell env has the new
  `XKB_CONFIG_ROOT`, `gsettings …sources = [('xkb','gemini')]` (locked),
  and **the keymap mutter hands clients is the gemini layout**:
  `<AE01> symbols[1] = [0x31, 0x21, 0x7c, 0xffbe]` (1 ! | F1),
  `<AE03> = [0x33, 0xa3, 0x5c, 0xffc0]` (3 £ \ F3),
  `<RALT> = [0xfe03]` (ISO_Level3_Shift = the Fn key driving level3).
  symbols[2] is the appended locale (us) group, as GNOME does; group 1
  (index 0) is gemini.

Tooling: added **`bin/wl-keymap-dump.c`** (build with
`gcc $(pkg-config --cflags --libs wayland-client)`) — this is what made
"the compositor still ships US" provable instead of guesswork.

### Touch — raw portrait alone was 180 off; sensor is mounted 180° to the panel

The 2026-09-10l change (drop the DT pre-rotation so the driver reports
raw portrait + let mutter apply the panel-orientation transform) was the
right direction, but on glass touch then landed **rotated 180**.

Interpretation: mutter *is* applying the panel-orientation 90°
rotation, and the display is correct, so the panel-orientation property
is right for the output.  A 180° input error with a correct output means
the **touch sensor is mounted 180° relative to the LCD panel** (they are
independent parts).

Fix: `touchscreen-inverted-x;` **+** `touchscreen-inverted-y;` in the
DTS node — in the kernel helper that is `x = max_x - x; y = max_y - y`,
a pure 180° rotation.  A 180 commutes with mutter's 90° rotation, so it
cancels the error regardless of whether mutter picked T90 or T270.

Build/identity:

- boot.img (touch 180): `/nix/store/p6zpsb9byh5nad8ablflywgcjrriip9y-mobile-nixos_planet-geminipda_boot.img`
  sha256 `0b176d934de6e97bda3b9089b9dda30ed105f0a0ca3748497e7ef725e1e7c0c6`,
  DTB carries `touchscreen-size-x/y` + `touchscreen-inverted-x/y`.
- toplevel (keyboard): `f5lpk3cnnj3hpifpnvm0kvgadmz4z5lq-nixos-system-gemini-26.11pre-git`,
  built with `z56mjiwsgsllvn1ybznl230rqyhvpxak-gemini-xkeyboard-config-2026-09-10`.
- Both deployed; `gemdemo` still **74 fps** (2026-09-10k perf fix intact),
  0 failed units.
- **CONFIRMED on glass by the user 2026-09-10m:** touch top-left lands
  top-left, and the Fn layer / UK symbols type correctly.  All three
  GNOME issues from the handover (perf, keyboard, touch) are closed.

## 2026-09-10l — GNOME keyboard fixed (gemini layout was never selected) + touch handed to mutter's panel-orientation transform

Two follow-ups from the user after the kworker fix: GNOME's keymap was
wrong / Fn produced nothing, and touch was still rotated. Both traced to
the same root: things the nested gemwl era set up are simply not what
GNOME/mutter uses.

### Keyboard — mutter ignores XKB_DEFAULT_LAYOUT; the layout comes from gsettings

> **[corrected 2026-09-10m] This was NECESSARY BUT NOT SUFFICIENT.**
> Setting the source to `[('xkb','gemini')]` and fixing the model did not
> change the compiled keymap: GNOME Shell validates the source id against
> the **xkb registry** (libgnome-desktop XkbInfo) and silently falls back
> to 'us' when it is absent. The real fix (registering gemini in
> `rules/evdev.xml` + `XKB_CONFIG_ROOT`) is in 2026-09-10m. The claims
> below about gsettings/lock are accurate; the conclusion “keymap fixed”
> was wrong on glass.

Recipe (services/gnome.nix):
- The session set `XKB_DEFAULT_LAYOUT=gemini` (and even an invalid
  `XKB_DEFAULT_MODEL=gemini`), but mutter builds its keymap from the
  gsettings `org.gnome.desktop.input-sources sources` list and ignores
  the env. On glass it was `[('xkb', 'us')]` — hence US symbols and a
  dead Fn/level3 layer (no F1–F12, no @ on Fn+K, no £). Receipts: mutter
  50.4 `meta-keymap-native.c` hardcodes rules=evdev/model=pc105 and
  `meta-keymap-description.c` uses the rules from the keymap description.
- A stale per-user dconf value outranks a plain system default, so the
  fix installs the source in a system dconf db **and locks it**:
  `programs.dconf.profiles.user.databases = [{ settings = {"org/gnome/desktop/input-sources".sources = mkArray [mkTuple ["xkb" "gemini"]];}; locks = ["/org/gnome/desktop/input-sources/sources"]; }]`.
- `XKB_DEFAULT_MODEL` corrected `gemini` -> `pc105` (the symbols file is
  a `partial alphanumeric_keys` overlay, not a model).
- Verified on glass after reboot: `gsettings get …sources` =
  `[('xkb', 'gemini')]`, `gsettings writable …sources` = **false** (lock
  took effect), and the generated system db reads back
  `[('xkb', 'gemini')]`. `xkbcli compile-keymap --layout gemini --model
  pc105 --rules evdev` succeeds. No xkb errors in the journal.

### Touch — the kernel now reports raw portrait; mutter rotates it

- Root cause: the DT pre-rotated the sensor to landscape
  (`touchscreen-inverted-x` + `touchscreen-swapped-x-y`), but mutter
  **also** applies the panel-orientation transform to absolute input:
  `meta_monitor_manager_get_monitor_matrix()` computes the matrix
  "corrected for LCD panel-orientation" (`meta-monitor-manager.c`), and
  for `panel_orientation = Left Side Up` (connector prop value 2,
  confirmed with modetest) `meta-kms-connector.c` maps it to
  `TRANSFORM_90`, whose matrix is `{0,-1,1,1,0,0}` = `(x'=1-y, y'=x)`.
  Pre-rotating as well double-rotates (the user's "rotated by 90").
- Fix: removed `touchscreen-inverted-x` and `touchscreen-swapped-x-y`
  from the DTS node (kept `touchscreen-size-x/y`), so the driver reports
  the sensor's native frame. On glass the device now advertises
  `ABS_MT_POSITION_X 0..1079`, `Y 0..2159` (portrait), where before it was
  2159x1079.
- The former transform was calibrated for the gemwl/nested stack, which
  consumes the LK framebuffer directly and has no panel-orientation
  handling; those desktops are force-disabled under GNOME.
- **Pending on-glass confirmation** of the rotation direction: if it is
  still off, the offset is a rotation and can be applied either with a
  `LIBINPUT_CALIBRATION_MATRIX` udev rule (fast, no flash) or by adding
  the matching inverted-x/inverted-y pair to the DT. [awaiting user]

### Build/identity

- boot.img (DT change): `/nix/store/81grjjhzwdhybjr7xivha1gf7y7saigv-mobile-nixos_planet-geminipda_boot.img`
  sha256 `c5befa54e905b62f61c18c4492c033f816b9c8d4c735b472f80d24015d071ea9`
  (flashed to p22, then `boot-nixos`).
- toplevel (keyboard + module): `hd8kf8yhx52157hmfw779ra0cwlkrs15-nixos-system-gemini-26.11pre-git`
  (deployed). 0 failed units after reboot.
- No kernel .c logic changed this session (the touch driver change is a
  comment; the previous entry's alpha-loop removal is already on glass).

## 2026-09-10k — FOUND IT: the ~99 % kworker behind the sluggish GNOME is geminipda-drm's per-pixel alpha loop; removed (it was redundant — the XRGB→ARGB blit already writes 0xff)

User added the decisive clue to the 2026-09-10j handover: **a kworker at
~99 % CPU while scrolling the UI**. Live recon (gen
`3w3xh4hwr52ynd50mn645nl8paiqpwi2-nixos-system-gemini-26.11pre-git`,
kernel `ribq69k94rz3nl88p5vzmjd70d8h2z89-linux-6.6.0`, boot.img
`1d2f350a…`) turned that into a proven, reproducible root cause. No
blind guessing — this is the `file:line` + backtrace receipt.

**Reproduction (no rebuild).** Ran `gemdemo` 0.3.0 (spinning triangle,
`OpenGL ES 3.1 Mesa 26.2.2`, renderer `Mali-T880 (Panfrost)`) as a client
of the running GNOME session:
`runuser -u cjdell -- env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus <gemdemo-0.3.0>/bin/gemdemo`.
Result: gemdemo logged **`6 fps`** and `top -H` showed
**`kworker/u20:1+events_unbound` at 96–100 %**. The Mali *did* draw
(renderer = Panfrost), so this was **not** a renderer fallback — the
display-commit path was the bottleneck.

**Which kworker / where.** While gemdemo ran:
`echo l > /proc/sysrq-trigger` → `dmesg` backtrace (PID 56,
`kworker/u20:1+events_unbound`):

```
geminipda_drm_primary_plane_helper_atomic_update+0x1f8/0x228 [geminipda_drm]
drm_atomic_helper_commit_planes+0xf8/0x2e4 [drm_kms_helper]
drm_atomic_helper_commit_tail+0x54/0xb4 [drm_kms_helper]
commit_tail+0x180/0x20c [drm_kms_helper]
commit_work+0x14/0x2c [drm_kms_helper]
process_one_work+0x13c/0x278
worker_thread+0x344/0x470
kthread+0x110/0x114
```

The DRM atomic-commit work (system_unbound_wq) is running our shadow-plane
`atomic_update`. The sample sits at `0x1f8/0x228` (~90 % through the
function) — the **inlined per-pixel alpha loop**.

**Root cause.** `geminipda_drm_force_alpha()` did
`writeb(0xff, row + x*4 + 3)` for every pixel: 1080 × 2160 = **2.33 M
barriered byte stores per full-screen update** (arm64 `writeb()` carries a
`__iowmb()`/`dmb`), i.e. 2.33 M memory barriers in the commit worker every
frame. That is why the worker never leaves the CPU and the compositor is
throttled to ~6 fps. (A plain `modetest -v` page-flip hammer on a CPU
RAM buffer showed the same worker at ~30 % — the GPU dma-buf sync added by
GNOME/gemdemo accounts for the rest.)

**…and the loop is redundant.** In the v6.6 DRM core,
`drm_fb_build_fourcc_list()` replaces native alpha formats with non-alpha
ones (`drm_fb_nonalpha_fourcc()`: ARGB8888 → XRGB8888, documented as
"primary planes usually don't support alpha"). On-glass proof:
`cat /sys/kernel/debug/dri/0/framebuffer` → gnome-shell's fb is
`format=XR24 little-endian` (XRGB8888), `pitch[0]=4352`. Because
`sdev->format` (the scanout) is ARGB8888, `drm_fb_blit()` takes its
**XRGB8888 → ARGB8888 conversion path**
(`drm_fb_xrgb8888_to_argb8888_line()`, `pix |= GENMASK(31,24)`), which
writes the `0xff` alpha byte the OVL needs *during the copy*. The separate
pass only re-wrote bytes that were already correct.

**Fix (this entry).** Deleted `geminipda_drm_force_alpha()` and its call
from `geminipda_drm_primary_plane_helper_atomic_update` in
`devices/planet-geminipda/kernel/delta/drivers/gpu/drm/tiny/geminipda-drm.c`;
rewrote the file-header bullet + an inline comment explaining that the
blit's XRGB→ARGB conversion is the single source of the alpha byte, so it
must not be reintroduced. No functional change. The kernel delta now
diverges from fork rev `06fd13e11` by this one file (noted in the kernel
derivation header).

**Build/flash/verify results (this entry).**

- **Artifacts:** boot.img
  `/nix/store/zvr9lrjvidbdri6ziq0044xgmkaihb3d-mobile-nixos_planet-geminipda_boot.img`
  sha256 `d649896e3e762efbc062d0a82a5163f65d2ff720cdb937e6c95918e22ee03846`
  (9,986,048 B); toplevel
  `/nix/store/1m7v0g75nyq6i31q5vl5qx4n6zsr50fg-nixos-system-gemini-26.11pre-git`.
  New module
  `0c83e11a82bd15a25e2e6a1614322f9c44577d6bb00cef05699dd6dc8a7e235d`
  (404,832 B, down from 408,672 B).
- **No boot.img flash needed (verified, not assumed):** the initrd has
  only 35 entries and does **not** contain `geminipda-drm.ko`, and the
  rebuilt `Image.gz` is byte-identical to the old one except 70 bytes of
  embedded `/nix/store/…-linux-6.6.0` path strings (the `linux_banner`
  is identical). A driver-module change therefore lives only in the
  rootfs system closure — `bin/deploy.sh deploy` + reboot suffices. The
  boot.img was still built (rule 0 identity) but was **not** flashed.
- **A/B via hot-swap (before deploy):** `rmmod geminipda_drm; insmod
  /tmp/geminipda-drm-new.ko` on the running system (display-manager
  stopped) — `gemdemo` went **6 fps → 73 fps** and the commit worker
  from ~99 % to ~65 %.
- **Cold-boot proof:** rebooted with `bin/device-reboot.sh`; the deployed
  system closure loads module `0c83e11a…` (sha compared), `gemdemo`
  reports **64–65 fps**, commit worker ~40–77 % (vs 6 fps / 99 % before).
- **Remaining cost is legitimate:** sysrq-l backtraces now show
  `drm_fb_xrgb8888_to_argb8888_line` → `__drm_fb_xfrm_toio` →
  `drm_fb_blit` → `atomic_update` — i.e. the XRGB→ARGB conversion that
  writes the alpha byte, which is required. Follow-up idea (not done): a
  driver-local single-pass blit (read the cached shadow buffer, OR
  `0xff000000`, `writel` to the WC scanout) would skip the generic
  helper's intermediate `stmp` line-buffer copy; possibly ~2× less
  commit CPU. Not needed for responsiveness.
- **Mesa-mix hypothesis demoted:** since responsiveness is restored by
  the kworker fix alone, the two-Mesa-versions concern from 2026-09-10j
  is unlikely to be the bottleneck — confirm `GL_RENDERER`/maps before
  attempting the Mesa 26 rebase.

**Tooling fix (2026-09-10k).** While touching the delta, found that the
built delta **diverges from fork rev `06fd13e11` in four files**: the
DRM/KMS work was added directly to the delta — delta-only
`drivers/gpu/drm/tiny/{geminipda-drm.c,Kconfig,Makefile}`, plus a
modified `arch/arm64/boot/dts/mediatek/mt6797-gemini-pda.dts`
(`planet,geminipda-drm` node). `bin/sync-kernel-delta.sh` did
`rm -rf delta` first, so a routine sync would have **silently deleted
the DRM driver and the DTS node — breaking GNOME**. Added a pre-flight
divergence guard: the script now lists the diverged files and aborts
unless `FORCE=1`; also updated its stale default rev `188aade69` →
`06fd13e112b23c5b2b4a9310af633cd27c14c57f`. Receipt: running it now
prints exactly those four files and exits without touching the delta.

## 2026-09-10j — GNOME on glass: user reports sluggish UI + 90°-rotated touch; recon + handover written

User confirmed GNOME runs with the **correct orientation**, but reported
(1) the UI is **very sluggish, feels like software rendering** and (2) the
**touchscreen is rotated 90°** (left edge activates the right of the UI).
No code changed this entry — live recon + a handover so the next session
starts with a diagnosis.

**Recon (live device, gen `3w3xh4w…`):**
- **Sluggishness — the gnome-shell process has TWO Mesa versions mapped:**
  `libgallium-26.2.2.so` ×6 (`mesa-26.2.2`, what mutter/gnome-shell 50.4
  link) **and** `libgallium-25.0.7.so` ×4 (the `mesa-geminipda` fork, via
  `/etc/glvnd/egl_vendor.d/50_mesa.json` →
  `…-mesa-geminipda-25.0.7/lib/libEGL_mesa.so.0`), plus split
  `mesa-libgbm-26.1.3`. Root cause hypothesis: the mixed GL/GBM stack
  degrades the kmsro path. **panfrost is idle** meanwhile:
  `runtime_status=suspended`, `active_time=78059 ms` vs
  `suspended_time=667252 ms` of 12 min — the UI is not being drawn on the
  Mali. (kmsro itself is proven good: `kmscube` with the fork env on card0
  → `Mali-T880 (Panfrost)`.) No llvmpipe/softpipe string in the journal,
  so the fallback is silent.
- **Mesa fork patch audited** (`patches/mesa-panfrost-geminipda-25.0.7.patch`,
  272 lines/5 files): only **two functional hunks** — `pan_screen.c`
  `caps->dmabuf = IMPORT|EXPORT` (needed for GBM/dma-buf) and
  `pan_cmdstream.c` **whole-polygon-list CPU memset each batch** (a real
  **T880 tiler workaround**; `PAN_NO_POLYLIST_MEMSET=1` restores upstream
  as an oracle). Everything else is env-gated debug (`PAN_TILERDBG`,
  `PAN_TILER_MASK`, `PAN_DUMP_POLYLIST`, `PAN_POLYLIST_FRESH`,
  `PAN_FLUSH_POLYLIST`, `PAN_MESA_TILER_HEAP_CPU`). So the fix is to make
  ONE Mesa carry these (rebase to the pinned mesa 26.2.2), not to drop the
  fork.
- **Touch:** device = `Novatek NT36772 Touchscreen` (`event0`); kernel
  driver `novatek-nt36xxx.c` documents native **portrait 1080×2160** and a
  DT-driven transform to landscape **`X'=y, Y'=1080-x`** (tuned for gemwl).
  Board DTS sets `touchscreen-inverted-x` + `touchscreen-swapped-x-y`.
  Under GNOME the DRM mode is portrait with `panel orientation = Left Side
  Up` and mutter rotates the output; grepping mutter 50.4's
  `meta-input-*.c`/`meta-seat-impl.c` finds **no** panel-orientation input
  handling, so the touch mapping must come from the device space and the
  gemwl-era transform is now off by the panel orientation.

**Handover written:** `docs/handover-2026-09-10-gnome-perf-touch.md` —
root-cause hypotheses, the single-Mesa fix (rebase the fork onto mesa
26.2.2 and make every process resolve to it; quick experiment = force the
fork as the only GL), the touch calibration plan (measure with
`libinput`/`evtest`, iterate live via a `LIBINPUT_CALIBRATION_MATRIX` udev
rule, then bake the winner into the `cap_touch@62` DT props and reflash),
the tooling/env patterns that worked, and a definition-of-done. Device left
untouched.

**Next:** follow the handover; confirm the in-session `GL_RENDERER` first
(it should be Panfrost, not llvmpipe) — that one measurement decides
whether the sluggishness is the Mesa mix or something in mutter's selection.

## 2026-09-10i — ON GLASS: GNOME is the default desktop, GPU-accelerated (kmsro → panfrost); KMS boot.img flashed and verified

User: "proceed with the recommended path, test on the device." DONE — the
kmsro path from 2026-09-10h was implemented, flashed and verified on
hardware.

**Version line (golden rule 0).**
- Kernel: `/nix/store/ribq69k94rz3nl88p5vzmjd70d8h2z89-linux-6.6.0` —
  published v6.6 base + delta (fork rev `06fd13e11`) + new
  `drivers/gpu/drm/tiny/geminipda-drm.c`; `DRM_GEMINIPDA=m`,
  `DRM_KMS_HELPER=m`.
- boot.img: sha256 `1d2f350a794f55cc828132c42a0696952207a04ae9ba2a8ef81fbc11a0a7be82`
  (9,986,048 B); embeds `Image.gz` + the DTB with `planet,geminipda-drm`
  (verified by extracting both and `cmp`-ing against the new kernel).
- Toplevels: gen65 `8wj3h24m…` (KMS kernel, nested desktop still default),
  then **gen66 `3w3xh4hwr52ynd50mn645nl8paiqpwi2-nixos-system-gemini-26.11pre-git`
  = GNOME default**. Mesa fork `mfqyzn3rz6i2w5hliz5w8jr5vylk8h3m-mesa-geminipda-25.0.7`.
  GNOME/mutter/gnome-shell **50.4**.

**Sequence.** Built toplevel+bootimg (cached+remote builder) → deploy
`bin/deploy.sh deploy` (gen65) → `bin/flash-nixos.sh boot <img>` (converged
to TWRP, **backed the old boot up** to
`stock-dump/boot-20260910-134318.img`, flashed `boot`, stayed para =
`boot-recovery`) → `bin/flash-nixos.sh boot-nixos` (para clear + reboot) →
new kernel came up on g_ether in ~1 min.

**On-glass results (all confirmed):**
- `/dev/dri/card0` = geminipda-drm, `/dev/dri/card1` = panfrost,
  `/dev/dri/renderD128`. Driver bound: `[drm] Initialized geminipda-drm
  … on minor 0`; `card0-DSI-1` present.
- `drm_info`: connector DSI **`Status: connected`**, mode
  **`1080×2160@60.00 preferred driver`**, immutable
  **`panel orientation … = Left Side Up`**, dumb buffers + PRIME +
  modifiers supported. debugfs: `crtc[33]: crtc-0 active=1`,
  `mode: "1080x2160": 60 …`, `connector[35]: DSI-1 crtc=crtc-0`,
  `crtc-pos=1080x2160+0+0`.
- **kmsro pairing (the make-or-break):** `kmscube` (fork mesa env:
  `LD_LIBRARY_PATH`, `LIBGL_DRIVERS_PATH`, `GBM_BACKENDS_PATH`) on card0 →
  `OpenGL ES 3.1 Mesa 25.0.7` / **`renderer: "Mali-T880 (Panfrost)"`**.
  GPU acceleration proven, no software fallback.
- **GNOME session:** `services.gnomeDesktop.enable = true` deployed (gen66).
  Journal: `Added device '/dev/dri/card0' (geminipda-drm) using atomic
  mode setting`; `Created gbm renderer for '/dev/dri/card0'`;
  `GPU /dev/dri/card0 selected primary from builtin panel presence`;
  gnome-shell holds **both** `/dev/dri/card0` and `/dev/dri/renderD128`
  open (the kmsro pair). `Failed to open gpu '/dev/dri/card1': No suitable
  mode setting backend found` is correct/harmless (panfrost has no
  `DRIVER_MODESET`). No llvmpipe/swrast/software-rendering warnings.
- **Unattended boot:** `systemctl reboot` → device back in ~48 s with
  `display-manager` **active** and gnome-shell auto-logged in as `cjdell`.
  `systemctl --failed` = **0 units**. gemwl/phosh-nested/lxqt-nested all
  inactive (force-disabled, as designed).
- GNOME apps present on PATH (gnome-calculator/calendar/maps/clocks/
  weather/contacts/papers/loupe/gnome-text-editor).

**Correction carried forward:** 2026-09-10h's "software-rendered" finding
was wrong; the live device confirms the opposite (see the doc).
`services.gnome.nix` `softwareRendering` stays **false**.

**Caveats / follow-ups (none block daily use):**
- No X server runs (Wayland-native). Mutter advertises
  `Using public X11 display :0`; Xwayland starts only on demand, so no
  X11 process is active. Compiling mutter with `xwaylandSupport = false`
  is possible (overlay; rebuilds gnome-shell) if the letter of "no
  Xwayland" is required — not needed for function.
- Geolocation portal gap: `No entry for geolocation` — Maps/Weather cannot
  place you until a geoclue location agent exists (gnome-shell normally
  provides one; something in this image does not register it).
- No camera (`Failed to start camera monitor`) — hardware, Snapshot can't
  work. Benign noise: "Failed to obtain high priority context",
  `g_close(fd:0) EBADF`, dbus "Ignoring duplicate name".
- Minor driver cleanups for next kernel build (cosmetic, not urgent):
  use `fb->format` instead of `sdev->format` in the plane's
  `atomic_update` (commented as intentional); consider returning the
  `drm_fbdev_generic_setup` value; the `struct copy` from `DRM_MODE_INIT`
  is fine as written.
- Delta/fork divergence (unchanged from 2026-09-10g): the delta is the
  source of truth; the legacy fork is still at `06fd13e11`.
- DR ledger: new boot backup `stock-dump/boot-20260910-134318.img`
  (pre-GNOME boot) — inventory note pending.

**Next:** user to confirm the visual result (orientation/colours/fonts).
If good, GNOME is the everyday desktop; the nested gemwl/phosh/LXQt stack
remains as a buildable fallback (set `services.gnomeDesktop.enable = false`
and re-deploy, no reflash needed since the kernel still carries
`FB_GEMINIPDA`).

## 2026-09-10h — CORRECTION: GNOME on the Gemini PDA IS GPU-accelerated (Mesa kmsro); no software rendering

User pushed back on 2026-09-10g's conclusion that GNOME would be
software-rendered: "software rendering is not acceptable. what are our
options?" Re-investigating from source showed the conclusion was WRONG,
and the `softwareRendering = true` default it produced (which sets
`LIBGL_ALWAYS_SOFTWARE=1`) would have actively defeated the correct path.

**Corrected mechanism (all source receipts re-verified 2026-09-10):**

1. **Panfrost owns a card node.** `ls -l /dev/dri` on the live PDA shows
   `card0` (226,0) AND `renderD128` (226,128), both panfrost. Panfrost is
   `DRIVER_RENDER | DRIVER_GEM | DRIVER_SYNCOBJ` (no `DRIVER_MODESET`),
   but the kernel's `drm_dev_register()` (`drivers/gpu/drm/drm_drv.c`)
   still allocates + registers the PRIMARY minor for a non-accelerator
   DRM device. So mutter (which enumerates only `/dev/dri/card*` in
   default mode, `meta-backend-native.c` `init_gpus()`) DOES see panfrost.
   My earlier "panfrost is invisible to mutter" was false.
2. **kmsro is already in our mesa fork.** `pkgs/mesa-geminipda.nix`
   auto-enables kmsro whenever panfrost is enabled (mesa `meson.build`:
   `with_gallium_kmsro = system_has_kms_drm and gallium_drivers.contains
   (true)`), and the built `libgallium-25.0.7.so` exports
   `kmsro_drm_screen_create`, `panfrost_drm_screen_create_renderonly` and
   `pipe_kmsro_create_screen` (grep of the store path, 2026-09-10). kmsro
   exists exactly for this: Mali is 3D-only, so Mesa pairs it with a
   display controller.
3. **Mesa's pipe-loader falls back to kmsro for unknown KMS names.**
   `pipe_loader_drm_probe_fd_nodup()` calls `get_driver_descriptor(name)`,
   and on failure `get_driver_descriptor("kmsro")` ("kmsro supports lots
   of drivers, try as a fallback") — `src/gallium/auxiliary/pipe-loader/
   pipe_loader_drm.c`; reached by `dri2_init_screen()`
   (`src/gallium/frontends/dri/dri2.c`).
4. **kmsro pairs platform display devices with platform render devices.**
   `kmsro_drm_screen_create()` →
   `pipe_loader_get_compatible_render_capable_device_fds()` pairs any
   PLATFORM display-only device with a platform render driver
   (`loader_open_render_node_platform_devices([panfrost, panthor, …])`).
   Our `geminipda-drm` is a DT platform device. Mesa EGL also calls it
   directly: `dri_query_compatible_render_only_device_fd()`
   (`platform_drm.c:get_fd_render_gpu_drm()`).
5. **Net:** EGL on `geminipda-drm` is a panfrost-backed renderonly screen,
   `GL_RENDERER = Mali-T880 (Panfrost)`. Mutter's `meta-render-device.c`
   marks a device hw unless `GL_RENDERER` starts with
   llvmpipe/softpipe/swrast, and `choose_primary_gpu_unchecked()` prefers
   a GPU with a connected built-in panel AND hw rendering → picks
   `geminipda-drm` as primary and renders via panfrost. **GPU-accelerated
   GNOME, no nesting, no bespoke compositor.**

**Changes made to match the correction:**
- `services/gnome.nix`: `softwareRendering` **default changed true →
  false**; the option is now documented as a DEBUG fallback only, and the
  description carries the kmsro mechanism. (Left the option itself so a
  session can still be forced up if the pairing fails on glass.)
- `docs/gnome-feasibility.md`: the "software-rendered by construction"
  section replaced with the kmsro finding + receipts; the stale
  "no such card / panfrost is a render-only node" text and the old
  "render split is the main unknown" plan bullet corrected; on-glass
  checklist gained **step 2b** (verify `GL_RENDERER` == Mali-T880, not
  swrast) and a new **"Options if the kmsro pairing does not hold"**
  section (pairing-inputs fix → explicit kmsro name/alias in the fork →
  mutter secondary-GPU path → real mediatek-drm → llvmpipe debug).
- Kernel artifacts from 2026-09-10g are unchanged and still valid
  (driver/boot.img build fine; the KMS device is the prerequisite for
  either path).

**Remaining honest risk:** the kmsro runtime sharing
(`panfrost_create_kms_dumb_buffer_for_resource` allocating a dumb buffer
on the shmem `geminipda-drm` card and importing it into panfrost) is
source-plausible but unproven on glass. That is exactly what checklist
step 2b checks. No software work is needed before flashing the already
built boot.img.

## 2026-09-10g — STANDARDS-COMPLIANT KMS DEVICE: `geminipda-drm` driver + NixOS GNOME desktop (built, eval-verified; not flashed)

User directive (following 2026-09-10f): "make the kernel stack appear to
userspace as a typical Linux machine (or as close as possible) … Then get
GNOME running." Approved the DRM/KMS route, so this session IMPLEMENTED it
(the previous entry only documented it).

**Kernel: `geminipda-drm`, a DRM/KMS driver for the LK framebuffer.** New
delta `devices/planet-geminipda/kernel/delta/drivers/gpu/drm/tiny/`:
`geminipda-drm.c` + `Kconfig` (full base tiny Kconfig + `DRM_GEMINIPDA`)
+ `Makefile`. Modelled on upstream `simpledrm` (v6.6 source read for the
exact APIs: shadow planes, `drm_fb_blit(&dst,&pitch,fmt,src,fb,clip)`,
`drm_fb_clip_offset`, `drm_connector_set_panel_orientation`). Design:
- binds new DT node `planet,geminipda-drm` (added to the board DTS next to
  `planet,geminipda-fb`); geometry from `/chosen` `atag,videolfb` with the
  same parser as geminipda-fb.c;
- one CRTC + one **shadow primary plane** (GEM buffer blitted into the LK
  scanout memory each atomic update — the "render on panfrost, copy into
  the OVL region" model, in-kernel; no CPU render, no panel re-init);
- **DSI** connector (mutter's `meta_output_info_is_builtin()` treats DSI
  as a built-in panel) + fixed mode 1080x2160, stride 4352, ARGB8888;
- standard **panel orientation** property, default **Left Side Up = 90**,
  module param `panel_orientation` (0..3). Receipt: mutter 50.4
  `meta-kms-connector.c` maps LEFT_UP→MTK_MONITOR_TRANSFORM_90 and
  `calculate_view_transform()` renders the rotation in the compositor
  because our CRTC advertises no HW rotation — i.e. the same transform
  gemwl uses with `-t 90`, but standard. This AVOIDS needing kernel
  rotation.
- after each blit, force the alpha byte to `0xff` (the `ARGB8888 renders
  BLACK` receipt in `pkgs/gemwl/gemwl.c`).

Config pipeline: `bin/prune-kernel-config.sh` section 4 now keeps
`DRM_GEMINIPDA|KMS_HELPER` and new section 9b emits
`CONFIG_DRM_GEMINIPDA=m` + `CONFIG_DRM_KMS_HELPER=m` (DRM itself is `=m`,
so the driver is a module; udev autoloads it from the DT modalias).
Regenerated `kernel/config`. Rule-5 gate in `kernel/default.nix` still
passes (no mediatek-drm/mtk-mmsys/DSI-PHY). `FB_GEMINIPDA` stays enabled,
so the SAME kernel still supports gemwl/phosh/LXQt — one desktop stack at
a time (rollback preserved).

**Userspace: `services/gnome.nix`** (`services.gnomeDesktop.enable`,
**default false**). Uses the ORDINARY NixOS modules —
`services.desktopManager.gnome` + `services.displayManager.gdm` +
autologin — not a bespoke session (this is the point of the KMS work).
Force-disables gemwl/phosh/LXQt (mutually exclusive), keeps the repo's
custom PipeWire (`services.pipewire.enable = lib.mkForce false`; GNOME's
pipewire definition collided with nixpkgs alsa.nix and would have started
a second `pipewire.service`), and loads panfrost after
`gemini-gpu-poweron` (gemwl used to own that step). Imported from
`config/gemini.nix`.

**Verification done here (no hardware):**
- kernel derivation evaluates: `
  nix eval .#nixosConfigurations.gemini.config.mobile.boot.stage-1.kernel.package.drvPath`
  → `/nix/store/nc7mzs5j2ncr036y02jiywm11np1yr02-linux-6.6.0.drv`
  (rule-5 assert passes).
- full kernel **build succeeded** on the remote aarch64 builder
  (192.168.49.191) via `bin/run-job.sh`:
  `/nix/store/ribq69k94rz3nl88p5vzmjd70d8h2z89-linux-6.6.0` (403 s).
  Verified in the output: `lib/modules/6.6.0/kernel/drivers/gpu/drm/tiny/
  geminipda-drm.ko` (modinfo: `of:N*T*Cplanet,geminipda-drm`,
  parm `panel_orientation`; depends `drm_kms_helper,drm,drm_shmem_helper`,
  all present + in `modules.dep`); `dtbs/mediatek/mt6797-gemini-pda.dtb`
  carries `planet,geminipda-drm` (and the old `planet,geminipda-fb`).
  First build attempt failed on two v6.6 API details, both fixed:
  `drm_atomic_get_new/old_plane_state` + `drm_plane_helper_atomic_check`
  needed `drm_atomic.h`/`drm_plane_helper.h`; and `.remove` must return
  `int` in v6.6 (not `void`).
- **boot.img built**: `
  /nix/store/j7z4v7lcy36c9c7znm9fw7qx6g6wgcqp-mobile-nixos_planet-geminipda_boot.img`
  (9,986,048 B / 9.52 MiB; kernel 7.66 MiB + ramdisk 1.86 MiB; fits the
  16 MiB partition). sha256 `1d2f350a794f55cc828132c42a0696952207a04ae9ba2a8ef81fbc11a0a7be82`.
  cmdline carries the mandatory `bootopt=64S3,32N2,64N2` + the verified
  console params (dumped with `bin/dump-bootimg-header.sh`). No
  `geminipda-drm.panel_orientation=` override → default 90 (left-up).
  **Not flashed.**
- system config with GNOME enabled evaluates green:
  `nixos-system-gemini-26.11pre-git.drv` (temporary `default = true`
  eval, then restored); the renamed option warning fixed to
  `services.desktopManager.gnome.enable`.
- GNOME app suite entry from 2026-09-10f unchanged (eval-green).

**NOT done / important caveats:**
- **Nothing flashed.** `services.gnomeDesktop.enable` stays false until the
  KMS boot.img is on the device (two-step; on-glass checklist in
  `docs/gnome-feasibility.md`). Booting this rootfs on a kernel without
  `geminipda-drm` gives no session.
- **Delta workflow divergence:** the driver + Kconfig/Makefile were added
  DIRECTLY to the tracked delta (now the source of truth per
  `bin/sync-kernel-delta.sh`'s own header). The legacy fork is still at
  `06fd13e11`, so `base+delta != rev` and a future `sync-kernel-delta.sh`
  would need the fork fast-forwarded (or the rev treated as the last
  synced point). The script already documents the delta as authoritative.
- Xwayland: nixpkgs' mutter is built with it; it is lazy (no X client →
  not started). A `-Dxwayland=false` mutter build would remove it if
  wanted. Documented in `services/gnome.nix`.

**Next:** (1) `bin/flash-nixos.sh boot` with the new boot.img (sha256
`1d2f350a…`, 9.52 MiB), keeping para = `boot-recovery`; (2) confirm
`/dev/dri/card0` + `card0-DSI-1` + panel orientation on glass;
(3) set `services.gnomeDesktop.enable = true` and deploy the rootfs;
(4) verify rotation (try `panel_orientation=3` if wrong), colors (alpha),
touch, audio. **Expectation to check first on glass:** mutter 50.4
`meta-backend-native.c` enumerates only `/dev/dri/card*` in default mode,
so panfrost (renderD128, render-only) is invisible to it; the only card is
the shmem `geminipda-drm`, so GNOME will select it via the software
fallback and render with llvmpipe (`LIBGL_ALWAYS_SOFTWARE=1` set by the
module). I.e. GNOME = correct but software-rendered; GPU acceleration
stays with gemwl/phosh.

## 2026-09-10f — VANILLA GNOME request: documented as a KMS project (not a config flip); GNOME app suite installed

Task: user asked to make vanilla GNOME the default desktop (Phosh is
WIP) and install the supporting GNOME apps (calc/maps/calendar/…) — with
the explicit constraint "No X11 or Xwayland. Phosh is essentially built
on GNOME so it should be possible".

**Finding (read-only research; NOTHING flashed/deployed).** Vanilla
GNOME cannot run on this device's current stack, and it is not a Phosh-
style missing-protocol problem:

- The pinned nixpkgs' `mutter`/`gnome-shell` are **50.4** (receipt:
  `nix eval .#nixosConfigurations.gemini.pkgs.{mutter,gnome-shell}.version`).
  Extracted the mutter source the same way
  (`nix build .#…pkgs.mutter.src` → `mutter-50.4.tar.xz`) and read it:
  `src/backends/` has **no `x11/` directory**, and there are **no**
  `META_TYPE_BACKEND_X11` / `BACKEND_X11_NESTED` / `x11_nested` matches
  anywhere in `src/`. `src/core/meta-context-main.c` implements only the
  native (KMS) backend and `--headless`; there is no `--nested`. (GNOME 49
  disabled X11 by default; GNOME 50 removed it — Phoronix/heise.)
- `src/backends/native/` is all `meta-crtc-kms.c` / `meta-gpu-kms.c` /
  `meta-drm-buffer-*.c` — the native backend needs a `/dev/dri/cardN`
  KMS pipeline + GBM. This device has none: `geminipda-fb.c` exposes the
  LK framebuffer as fbdev `/dev/fb0` (+ `/dev/gemfb` dma-buf) and
  panfrost is **render-only** (`/dev/dri/renderD128`).
- "Phosh is built on GNOME" is true of the *toolkit/apps*, not the
  session: Phosh is a GTK4/libadwaita shell **client** over its own
  wlroots compositor (`phoc`), so it nests in gemwl. GNOME Shell **is**
  the compositor (mutter); it has no Wayland-client backend, so it can
  never be a gemwl client — that is the whole difference.

**Shipped (safe part):** new `services/gnome-apps.nix`
(`services.geminiGnomeApps.enable`, default true) → system-profile GNOME
apps (gnome-calculator/calendar/maps/clocks/weather/contacts/characters/
text-editor/system-monitor/disk-utility/connections/usage/baobab/seahorse/
file-roller/papers/loupe/snapshot/screenshot/console) plus
adwaita-icon-theme, gsettings-desktop-schemas, gnome-online-accounts and
`services.geoclue2`. They are ordinary Wayland clients and show up in
Phosh's or LXQt's app grid. Imported from `config/gemini.nix`.
**Eval-green**: `nix eval .#nixosConfigurations.gemini.config.system.build.toplevel.drvPath`
→ `/nix/store/60x682qn2g9926nr7hscy4pr7q86ib5f-nixos-system-gemini-26.11pre-git.drv`
(2026-09-10; new files `git add`-ed so the flake sees them). No desktop
default was changed, no kernel/boot change, nothing flashed.

**Documented the real path:** `docs/gnome-feasibility.md` — expose the LK
framebuffer as a DRM/KMS device. The in-tree `simpledrm` (v6.6) is the
template: it binds a `simple-framebuffer` platform device, uses
`DRM_GEM_SHADOW_PLANE_HELPER_FUNCS` and blits a GEM buffer into the fixed
firmware region (`screen_base`) every atomic update — exactly the "render
on panfrost, copy into the LK OVL region" model, in-kernel. Plan: enable
`DRM_KMS_HELPER`/`DRM_SIMPLEDRM`/`DRM_FBDEV_EMULATION`, register the
region from the existing `geminipda_fb_get_geometry()` parser, then a
hand-rolled GNOME session (logind seat + gnome-session/gsd/portals — the
current desktops are logind-less systemd system services, so GNOME's
session model does not drop in). Risks: rule 5 (display path — on-glass
only, rollback kept); the render-only panfrost + KMS `simpledrm` split is
the main unknown (mutter primary-GPU selection). Retiring gemwl is the
end state. **Deliberately NOT started** without explicit go-ahead, per
rule 5.

Next: (a) decide whether to start the KMS/GNOME project; (b) if Phosh's
WIP status is the real blocker, consider flipping the default to LXQt in
the meantime (one-line option change); (c) the app-service gaps (geoclue
has no shell-side location agent; GOA needs a session manager) are noted
in the doc.

## 2026-09-10e — REBOOT/POWEROFF FIXED: mt6797-power delta driver on glass (reboot self-boots, poweroff turns the unit off); one boot-loop gotcha

Task: implement the 2026-09-10d design and make `systemctl reboot` /
`systemctl poweroff` actually work.

**What shipped.** New in-repo kernel delta
`drivers/power/reset/mt6797-power.c` (+ Kconfig/Makefile, DTS node
`power@10007000`, `CONFIG_MTK6797_POWER=y` in the lean config):

- **restart** (priority 200, above the mainline `mtk_wdt` handler's 128):
  replays LK's `mtk_wdt_reset(1)` verbatim — WDT_RESTART reload, keyed
  hw-reboot mode (`KEY|EXTEN|AUTO_RESTART`), `WDT_SWRST=0x1209`.
- **poweroff** (`register_platform_power_off`): MT6351 RTC space over the
  pwrap regmap — clear RTC_AL_SEC 2-sec bits, unlock RTC_PROT (0x586a,
  0x9136), write RTC_BBPU = `KEY|AUTO|PWREN` = 0x4309, WRTGR trigger; if
  still alive after 1 s (USB holds the rails) fall back to LK
  off-mode-charging (WDT reset mode 0).

Fork rev `06fd13e11`; `devices/planet-geminipda/kernel/default.nix`
header updated; delta re-synced and byte-verified.

**First cut boot-looped (the important receipt).** The driver used
`devm_platform_ioremap_resource()` on `0x10007000`. That calls
`devm_request_mem_region()`, i.e. it *claims* the TOPRGU block. Mainline
`mtk_wdt` binds the `watchdog@10007000` node through the
`mediatek,mt6589-wdt` compatible and also maps that block exclusively, and
`mt6797_power_driver_init` is linked **before** `mtk_wdt_driver_init`
(System.map: `ffff80008103f534` vs `ffff80008103faf8`). So `mtk_wdt` failed
`-EBUSY`, the LK-armed watchdog was never kicked, and the SoC reset ~20 s
into every boot. Symptom on the host: USB cycling `0e8d:2000` (preloader)
→ `0525:a4a2` (RNDIS) → reset, every ~29 s; mtkclient could not hold a
preloader session (`Status: Handshake failed, retrying...`).
**Fix:** map the shared block with `devm_ioremap()` (no region claim); both
drivers map it, only `mtk_wdt` claims it. Rebuilt + re-flashed.

**Recovery from the loop.** `para` had been cleared by `boot-nixos`, so
there was no software path. The user force-powered-off and held the
volume-up side button to hold a stable preloader; a persistent
`sudo bash bin/run-mtk.sh w para stock-dump/para-boot-recovery.bin`
(patched mtkclient; `Wrote … to sector 32832 with sector count 1024`)
restored the TWRP-sticky marker, `run-mtk.sh reset` booted TWRP
(`18d1:4ee2`), then `bin/flash-nixos.sh boot` re-flashed.

**Also fixed:** `bin/prune-kernel-config.sh` was dropping `CONFIG_BT` on
every regeneration (BT is not in the drop family list now), which would
have clobbered the Bluetooth bring-up the next time the lean config was
regenerated. Confirmed the only config delta is `CONFIG_MTK6797_POWER=y`.

**Verified on glass (2026-09-10).** boot.img
`2fbca31446cf1f70e1b37a8a109c3737e59f8adec7fbdea2d08b47c6a6c3c1f8`
(`/nix/store/cln7rip7khayq5jwabgf7ilindhbrib7-mobile-nixos_planet-geminipda_boot.img`):

- `mt6797-power 10007000.power: MT6351 RTC_BBPU readback 0x000d` +
  `MT6797 restart + MT6351 poweroff handlers registered`.
- `mtk-wdt 10007000.watchdog: Watchdog enabled (timeout=31 sec, nowayout=0)`
  → the region conflict is gone; device booted and stayed up (no loop).
- `systemctl reboot` → new boot_id (`70dd8a9d…` → `dff7d973…`) in ~40 s.
- `systemctl poweroff` → USB went silent (no preloader/RNDIS, no loop, no
  limbo); the unit is off. Screen state not observed (user may confirm).
  **[corrected 2026-09-29 — see the 2026-09-29 entry at the top: this was
  NOT a true off. "USB went silent" also describes a halted AP, and a
  2026-09-29 measurement found the unit alive (100 mA @ 5 V).]**
- The pre-existing `dev_addr_check` wlan0 warning in dmesg is unrelated
  (Wi-Fi MAC address, NetworkManager).

**Version line (rule 0).** kernel fork rev `06fd13e11`; boot.img sha256
`2fbca314…`; the broken first-cut boot.img backup is
`stock-dump/boot-20260910-115503.img` (do not reflash it). Mesa/wlroots/
gemwl pins unchanged. Device left with `para` cleared (normal boot) and
powered off after the poweroff test.

**Gotchas for next time.** (1) TOPRGU is shared with `mtk_wdt` — never
claim `0x10007000`. (2) A hung/looping boot has no software path back:
the preloader window is the way in, and a stable one needs a held side
button (the loop's own windows are too short for mtkclient's DA
handshake). (3) `mtkclient` writes `hwparam.json` into the repo root —
delete it before committing (gitignored? — no, it is not; removed here).

**Follow-ups.** `bin/device-reboot.sh` still arms WDT by hand (works;
flip to plain `systemctl reboot` over ssh once convenient).
`machine_emergency_restart` (panic path) still bypasses the restart-handler
chain — see `docs/power-states.md` §6 item 5. On-glass test plan §7
items 3 (off-mode-charging display) and 4 (battery-guard CRIT) not fully
exercised.

## 2026-09-10d — POWER STATES RESEARCH: why `systemctl reboot` limboes, why poweroff is impossible, and the source-verified fix (no flash, no kernel change)

Symptom report: `systemctl reboot` → black-screen limbo (PMIC on, power key
dead; recovery = 10 s power+side hold). `systemctl poweroff` impossible.
Battery risk while "off-ish" (≈1.6 W drain per docs/power-sleep.md).

Findings (all source-verified; full write-up: **docs/power-states.md**):
- Limbo = arm64 6.6 `machine_restart()` with no restart handler:
  `smp_send_stop()` → `do_kernel_restart()` (empty chain) →
  `printk("Reboot failed -- System halted")` → `while(1)`
  (arch/arm64/kernel/process.c:126, v6.6 tag). PMIC stays on, LK never
  re-runs (panel uninitialised), power key routed to the dead AP.
- Poweroff refused earlier: `do_reboot()` gates poweroff(2) on
  `kernel_can_power_off()` (kernel/reboot.c v6.6) — no handler → -EINVAL.
- Fix exists and is verifiable from source:
  - **reboot** = TOPRGU WDT SWRST sequence — LK's `mtk_wdt_reset(1)`
    verbatim (gemini-lk lk/platform/mt6797/mtk_wdt.c:34; RESTART key
    0x1971 @+0x08, SWRST key 0x1209 @+0x14, MODE KEY|EXTEN|AUTO_RESTART;
    AUTO_RESTART = bypass-power-key → self-boot; LK re-inits panel =
    rule-5 safe). Vendor 3.18 wdt_arch_reset identical (mt6797/mtk_wdt.c:335).
  - **poweroff** = MT6351 `RTC_BBPU` = 0x4309 (KEY|AUTO|PWREN) over the
    pwrap regmap (RTC space 0x4000+: 0x4018 clear 2SEC, 0x4036 unlock
    0x586a/0x9136 + 0x403c WRTGR triggers) — LK rtc_bbpu_power_down
    (mt_rtc.c:109) and vendor mt_power_off (mtk_rtc_common.c:397,
    pm_power_off hook at mt_pm_init.c:620). Charger present → vendor
    fallback: WDT SWRST mode 0 → LK off-mode charging.
  - pwrap regmap (mtk-pmic-wrap.c in our delta, max_register 0xffff) can
    address the RTC space — LK proves the interface; kernel has so far
    only touched PMIC main space.
- Design: new delta driver `drivers/power/reset/mt6797-power.c` (Kconfig
  MTK6797_POWER=y) + DTS node (reg 0x10007000 + phandle to pwrap);
  register_restart_handler (SWRST) + register_platform_power_off (BBPU +
  chrdet fallback). Test plan with 10 s-combo/WDT-escape protocol in
  power-states.md §7 (para=boot-recovery until verified).
- Uncertainties logged: SWRST-from-kernel + pwrap→RTC-space writes are new
  paths (identical to LK/vendor but never run from our kernel); post-BBPU
  on-USB behaviour unverified (vendor fallback copied); panic /
  machine_emergency_restart path NOT covered by the handler chain — 10 s
  combo + userspace WDT remain the panic recovery; WDT LENGTH encoding
  discrepancy (LK ×2048 vs field-verified (SECS<<5)|0x8) irrelevant to the
  SWRST design.

Next: implement the driver in the kernel delta, build (aarch64, ~6 min),
flash boot.img under para=boot-recovery, run the §7 test sequence
(reboot / poweroff-on-battery / poweroff-on-USB / battery-guard end-to-end).
Device left as found: gen62 running, para unchanged (NixOS default).

## 2026-09-10c — TOUCH: real multitouch wl_touch device (no cursor) for the phosh/LXQt desktops

Task: "the touchscreen behaves as a traditional input device moving a
visible onscreen cursor; make it a real multitouch device in userspace
so modern GNOME apps get proper gestures like pinch."

**Root cause (not the kernel).** The NT36772 kernel driver
(`devices/planet-geminipda/kernel/delta/drivers/input/touchscreen/
novatek-nt36xxx.c`) was already a correct 10-point Protocol-B
direct device (verified in source: input_mt slots, ABS 2160x1080
landscape, `input_mt_set_slots(10)`). The cursor came from **gemwl**:
its touch handlers emulated the first finger as an absolute pointer
(warp `wlr_cursor` + synthetic `BTN_LEFT` press/release), and its seat
only advertised POINTER|KEYBOARD — so the nested compositor never
created a touch device and every app saw a mouse.

**Change** (commit **39ee053**, `pkgs/gemwl/gemwl.c`): seat advertises
`WL_SEAT_CAPABILITY_TOUCH` when a touch device is attached; touch
handlers forward EVERY finger via `wlr_seat_touch_notify_down/motion/
up` + `_frame` (normalized 0..1 → output layout box →
`wlr_scene_node_at` → surface-local — same transform as the pointer
path; nested toplevel is full-screen at scale 1 so the wl_touch
"relative to the down surface" contract holds); a `client_has_touch()`
guard drops a down until the nested client called
`wl_seat.get_touch()` (avoids per-down wlroots error spam pre-session);
`GEMWL_TOUCH_POINTER_EMU=1` restores the legacy first-finger-as-pointer
behaviour for A/B (and suppresses the TOUCH capability in that mode).
Why it works: wlroots' wayland backend — in BOTH lines we run (0.19.3
for phoc, 0.18.2 for labwc) — synthesizes a `wlr_touch` from the outer
seat when the TOUCH cap is present (`backend/wayland/seat.c`
`seat_handle_capabilities` → `init_seat_touch`) and forwards the
`wl_touch` events into it; phoc then runs its own touch stack
(`seat_add_touch` → cursor touch handlers → `wlr_seat_touch_notify_*`
to the phosh apps) plus compositor-side gesture recognizers
(`gesture-zoom.c` pinch, `gesture-swipe.c`) and touch-point overlay
feedback (`touch-point.c`). Source receipts: wlroots 0.18.2/0.19.3
trees + phoc v0.54.0 (`gitlab.gnome.org/World/Phosh/phoc`) read on 2026-09-10.
Docs: `docs/desktop-plumbing.md` §Touch.

**Deployed + verified as far as the journals go** (2026-09-10 ~02:30):
- gemwl built clean (aarch64 builder), pushed to the device clone
  (`bin/device-repo.sh push` → 39ee053), `nixos-rebuild switch --flake .`
  on the PDA (297 s, system `mbzswv53…-nixos-system-gemini-26.11pre-git`;
  the switch also pulled the device out of light sleep — it was asleep
  since 01:23 when the units were "started").
- gemwl log 02:22:02: `gemwl input: touchscreen attached (wl_touch
  forwarding): Novatek NT36772 Touchscreen`.
- phoc (with a transient `G_MESSAGES_DEBUG=all` /run drop-in, since
  removed): `[backend/wayland/seat.c:356] seat 'seat0' offering touch`,
  `New input device: wayland-touch-seat0 touch seat:seat0`,
  `Adding device wayland-touch-seat0 2` — the synthesized touch device
  exists end-to-end.
- **Synthetic-finger test** (`bin/touch-inject.c`, new — writes raw
  Protocol-B events into the NT36772 evdev node): tap → gemwl
  `touch DOWN id=0 (0.50,0.50) -> surface-local (1080,540)` + UP;
  pinch → `touch DOWN id=0 (…830,540)` + `touch DOWN id=1 (…1330,540)`
  + motions + both UPs. So kernel evdev → libinput → gemwl's wl_touch
  forwarding is proven with real (synthesized) events.
- NOT yet verified: the last hop phoc → phosh app (a real finger on
  glass: no cursor, tap/drag, pinch-zoom in a GTK4/WebKit app).
  A wl_touch probe client to close that gap is `bin/touch-probe.c`
  (WIP — blocked on the wayland 1.26 API change, see gotchas below).

**Gotchas (keep for the next input session):**
- **aarch64 `struct input_event` is 24 bytes** (16-byte `timeval`
  first), not the 8 of 32-bit: an 8-byte evdev write is rejected with
  EINVAL (`count < input_event_size()` in evdev_write). The kernel
  replaces the timestamp on injection, so tv=0 is fine.
- `/sys/class/input/inputN/dev` is **empty** on this kernel build —
  find the evdev node via the `eventN` subdirectory of inputN (the
  `device` symlink points at the i2c client, not an event device).
- The device store holds BOTH arches of wayland 1.26.0
  (`vjcw…`=x86_64, `njzs…`=aarch64) — ld says "skipping incompatible".
- **wayland ≥1.23 unified `struct wl_interface`**: generated protocol
  headers declare `extern const struct wl_interface wl_seat_interface`
  — the classic per-interface client listener structs are gone;
  touch-probe.c needs that migration before it compiles.
- Light sleep (silver button) stops gemwl/phosh-nested and UNBINDS
  the NT36772 touch driver — after `gemcli sleep off` the touch device
  re-registers and gemwl hot-plugs it (that is the 02:22:02 "Adding"
  above, not a boot). `G_MESSAGES_DEBUG=all` makes phoc log the
  wlroots DEBUG lines (its `wlr_log_init(WLR_DEBUG, log_glib)` bridge
  maps them to glib DEBUG).

**Left behind:** device AWAKE on the new generation, gemwl +
phosh-nested active, touch driver bound, debug drop-in removed,
`/var/tmp/touchinj-src/` + a built `touchinj` in the device store if
the synthetic test is wanted again. Next: user's on-glass finger test
(no cursor on touch, pinch-zoom in a web app); if anything misbehaves,
A/B with a `GEMWL_TOUCH_POINTER_EMU=1` drop-in on gemwl.

## 2026-09-10b — DESKTOP PLUMBING: battery via UPower, Wi-Fi via NetworkManager, backlight access (eval-green + kernel built; NOT yet deployed)

Task: "phosh is not usable at the moment — need backlight/volume
controls, wifi/bluetooth, battery status; do it the most standard way
so all desktop environments just work." Design + receipts:
**docs/desktop-plumbing.md**.

What landed (all DE-agnostic system services, no shell-specific glue):

- **Battery → upower**: the unit has NO fuel-gauge IC, so upower/DEs
  had nothing to show (only a TYPE_USB charger supply existed). The
  kernel fork now registers a `Battery`-type `bq25890-battery-N`
  power_supply beside the charger in the same driver (capacity from
  the VBAT ADC vs a 1S Li-ion OCV table, ≤90 % while pre/fast-charging,
  100 % only at termination; status/voltage/temp/health mirrored; a
  `bq25890_supplies_changed()` helper fans out
  `power_supply_changed()` to both supplies). Fork commit
  **c8f0787d** (2026-09-10), synced with `bin/sync-kernel-delta.sh` →
  delta 517 files, byte-verified `v6.6 + delta == c8f0787d`; kernel
  default.nix header rev updated. Built OK on the aarch64 builder:
  `linux-6.6.0` drv `78z54a5j…` (System.map carries
  `bq25890_battery_supply_get_property`).
- **`services/plumbing.nix`** (new, imported from config/gemini.nix):
  `services.upower` enabled, percent thresholds 15/5/2 % matching the
  guard's 3.64/3.57/3.50 V points, `criticalPowerAction = "Ignore"`
  (only gemini-battery-guard powers the unit off — the voltage-derived
  % must not drive policy), `ignoreLid`, and a `brightnessctl` package.
- **Backlight access**: kernel-side 0666 sysfs attrs are IMPOSSIBLE
  (first attempt failed the build: `VERIFY_OCTAL_PERMISSIONS` rejects
  write bits for group/other on DEVICE_ATTR — that delta change was
  reverted before the successful build). Replaced with a udev RUN rule
  (plumbing.nix) chmodding `/sys/class/backlight/%k/{brightness,bl_power}`
  to 0666 on add — the standard runtime answer for the session-less
  system-service desktop (no logind session → logind SetBrightness and
  udev uaccess never apply).
- **Wi-Fi → NetworkManager** (`services/wifi.nix`,
  `services.geminiWifi.useNetworkManager` default **true**): NM owns
  wlan0 (CONSYS — bring-up units unchanged, NM ordered after +
  Wants=gemini-wifi-internal) and wlan1 (RTL8821CU dongle); usb0
  unmanaged (static g_ether link); home networks ("The Lab", "The Lab
  2.4GHz") as NM profiles via ensureProfiles, same psk as
  etc/wifi/profiles.conf; DNS via the default resolvconf rc-manager
  (no systemd-resolved); `wifi.scanRandMacAddress=false` (gen3 driver
  has no MAC randomization); ModemManager off. Legacy standalone
  wpa_supplicant+dhcpcd+`wifi auto` = `useNetworkManager = false`
  fallback (kept installed). cjdell's `networkmanager` group membership
  + NM's polkit rule make the DE UI work with no logind session.
- **Bluetooth**: already standard (bluetoothd auto-powered + blueman);
  no change.
- **Volume**: state already PipeWire/WirePlumber; physical media keys
  are NOT wired by any standard path under phoc+phosh (phosh has no
  media-key code; gsd can't global-grab on Wayland without gnome-shell
  — same as PinePhone). Documented as a follow-up (actkbd/wevdaemon
  style daemon → `wpctl`), deliberately not part of this layer.

Eval: `nixosConfigurations.gemini` toplevel builds green; verified
config values (upower 15/5/2 + Ignore, NM enabled/unmanaged usb0/rand=
false, modemmanager false, brightnessctl in the closure).

**DEPLOYED: gen62** (`bin/deploy.sh deploy`, 2026-09-10 — device
switched to `k5n57yqb…`, gc-pinned; kernel flash NOT done, see below).
Sleep-path fixes found while checking the device (it was ASLEEP — the
silver button had stopped gemwl; `gemcli sleep off` woke it):
`phosh-nested.service` was missing from gemcli's sleep SERVICES list
(a sleep left phoc + the phosh session running against a stopped gemwl
— the observed "phosh not usable"), and the wifi sleep/wake path was
legacy-only (killed NM's wpa_supplicant + restarted the non-existent
gemini-wifi-auto). gemcli sleep.rs is now NM-aware (link park/raise +
NM autoconnect; legacy path retained when NM is inactive) and stops
phosh-nested. Fork commit c8f0787d + pkgs/gemcli/src/sleep.rs.

On-glass results, gen62 (over g_ether): **NetworkManager works** —
wlan0 managed and already CONNECTED to "The Lab" (autoconnect via the
seeded profile), usb0 unmanaged, both home profiles present; upower
running (D-Bus activated) with the line_power device and
`battery-missing-symbolic` on the DisplayDevice; the backlight class
device exists (`/sys/class/backlight/backlight`, type raw, max 255)
and after `udevadm trigger --action=add …` the attrs are rw-rw-rw- —
unprivileged `su cjdell -c 'brightnessctl -c backlight set 9%'` writes
23/255; gemwl + phosh-nested active. The class-glob udev trigger
(`--subsystem-match=backlight`) does NOT fire the rule; syspath does
(and a reboot will).

**boot.img flashed + battery verified on glass.** `nix build
.#packages.aarch64-linux.bootimg` → `result-boot` (bootopt
`64S3,32N2,64N2` preserved, checked with
`bin/dump-bootimg-header.sh`), flashed from TWRP (`bin/flash-nixos.sh
boot result-boot`; previous image backed up to
`stock-dump/boot-20260910-021726.img`), then `boot-nixos`. Device on
the new kernel now shows:

    bq25890-battery-0 type=Battery status=Charging capacity=90
                      vbat=4044000 temp=380 health=Good
    upower: battery_bq25890_battery_0 "gemini-battery (voltage-derived)"
            state=charging percentage=90% icon=battery-full-charging
    DisplayDevice: 90% charging  (→ phosh top-bar battery icon)

90% is the intended charging clamp (VBAT 4.044 V maps to ~93%, capped
at 90 while pre/fast-charging). Backlight attrs came up `rw-rw-rw-` on
the fresh boot (the udev rule fires naturally on device add).
NetworkManager reconnected wlan0 to "The Lab" unattended; upower, NM,
gemwl, phosh-nested, bluetooth all active. **gen63** deployed
(g53pxmax… — adds the NM-aware sleep + WDT fixes below).

**The boot flash first FAILED and seeded two real fixes.** The WDT
EXRST arm no-opped (documented "reboot trap") and the unit stayed up
2h+; a `systemctl reboot` then froze it at a black screen (that
behaviour is now recorded as an open P1 in docs/phase-2-on-glass.md
§4 — the user confirms it is long-standing). Register read:
`0x10007000 = 0x00000000` (MODE disarmed), `0x10007004 = 0x00000040`
— the A72 bring-up (`cl2-up.sh` → `wdt_disarm`) case of §2b, NOT the
kernel watchdog driver (CONFIG_MEDIATEK_WATCHDOG is also in
config.full-329). Fixes:

- `bin/device-reboot.sh`, the two Linux→reboot hops in
  `bin/flash-nixos.sh` and `gemcli`'s `wdt.rs arm()` now restore
  `MODE = 0x2200005D` before writing `LENGTH`; docs/phase-2-on-glass.md
  §2b §4 updated.
- `bin/device-reboot.sh` success detection was unsound (it pinged the
gadget that was still up during the 2 s WDT window → false "rebooted
✓"); it now waits for the USB gadget to DROP first and requires a
changed `/proc/sys/kernel/random/boot_id`. Verified twice:
"gadget gone after ~12s" → "device back (new boot_id) OK", ~47 s.
- Recovery this time was a manual power-on into TWRP (the user did it);
the A72-cluster-is-up case leaves no software path to TWRP if the para
write is needed — the MODE restore above fixes the reboot half.

Remaining (manual, physical): sleep/wake round trip via the silver
button on the new gemcli (desktop + wifi must both return), and the
phosh on-screen controls (brightness slider, wifi/BT pages, battery
icon) confirmed by eye. Full checklist: docs/desktop-plumbing.md.

Also touched: docs/phosh.md (closed the stale "no upower / no NM"
Known-gaps bullet, marked [corrected 2026-09-10]), README services
row, AGENTS "Where things live". No legacy GeminiPDA doc touched
except the kernel fork repo (commit c8f0787d — the fork's bring-up
branch, which is where kernel edits live per the delta hygiene rule).

## 2026-09-10a — PHOSH IS THE DEFAULT DESKTOP + ON GLASS: module default flip → gen59 crashed (2 bring-up bugs fixed) → gen61 unlocked by the user (0000)

Task: "can we make phosh the default desktop?" → yes, and it now is:
the module defaults flipped (services.phoshDesktop.enable = **true**,
services.lxqtNested.enable = **false** — LXQt becomes the alternative),
committed 3e83f6c; docs (README/AGENTS/phosh.md) updated to match.

Gens: gen58 (start) → gen59 (flip, CRASHED) → gen60 (schema fix,
passcode) → gen61 (PAM fix, UNLOCKED on glass). Versions on glass:
phosh 0.54.0 + phoc 0.54.0 (wlroots 0.19.3, fork-gbm override) + mesa
25.0.7 fork — all pre-pinned; toplevels 152x4bz (g58) / bj6gys6i (g59)
/ 9vd6qqw8 (g60) / 3fik824n (g61). No boot.img/para touch; all
profile switches via deploy.sh. btpreload triage .so shipped for the
crash debug (never wired into any unit; runtime override removed).

**gen59 crash (deterministic, ~6 s after "Enabling shell mode")**:
silent SIGABRT in phosh then SIGBUS in phoc, every run, NRestarts loop.
Triage without touching the image: btpreload LD_PRELOAD (SIGABRT/SIGBUS
handler + backtrace_symbols_fd) → backtrace showed abort ←
g_log_default_handler ← g_log from gio's g_settings_set_property
(gsettings.c:676): g_error "No GSettings schemas are installed on the
system". Root cause: NixOS gsettings packages install schemas under
share/gsettings-schemas/<pkgname>/glib-2.0/schemas (not upstream
share/glib-2.0/schemas), and libexec/phosh is UNWRAPPED (phoc -E), so
the hand-rolled XDG_DATA_DIRS never pointed at the schemas → gio's
default source empty → first g_settings_new aborts. Fix (408071b):
services/phosh.nix carries the gsettings-schemas dirs of phosh,
gnome-shell, squeekboard, gnome-console, gsettings-desktop-schemas and
gnome-settings-daemon on XDG_DATA_DIRS. Runtime-verified (drop-in env
override): "Phosh ready after 3.39s". GPU truth on glass: phoc gles2 →
OpenGL ES 3.1 Mesa 25.0.7, Mali-T880 (Panfrost); EGL
EGL_EXT_image_dma_buf_import present.

**Lockscreen blocked entry (gen60)**: phosh demanded a passcode but
cjdell had none (locked account — users.users.cjdell created without a
password; passwd -S = L). User chose passcode 0000 → set live via
chpasswd + baked the yescrypt hash into users.users.cjdell (408071b).
Then 0000 still failed: phosh PAM-authenticates in-process under the
service name "phosh" and NixOS generates no /etc/pam.d/phosh → pam
fell back to the deny-all "other" file (pam_warn(phosh:auth) spam) →
every code rejected. Fix (7f33bee): security.pam.services.phosh = { }
(default unix rules; pam_unix as euid 1000 → setuid
/run/wrappers/bin/unix_chkpwd → /etc/shadow). gen61 deployed → user
unlocked the phosh lockscreen with 0000 — **phosh is on glass and
usable as the default desktop** (LXQt gen58 remains the rollback
state: bash bin/deploy.sh rollback 3).

State left: gen61 current, phosh-nested active NRestarts=0, LXQt unit
absent, runtime triage overrides removed (only the committed config
remains). Remaining journal noise = documented v1 gaps (no
gnome-session/dconf/upower/NM — non-fatal; phosh.md Known gaps).

Next: cold-boot re-verify (do NOT reboot while the user is on the
device — offered instead); then eyes-on-glass UX pass (top bar, app
grid, gnome-console/firefox GL clients), then the phosh.md checklist
items 3-5, and decide whether the LXQt nested desktop stays packaged
(as alternative) or gets disabled by default permanently.

## 2026-09-09q — DEPLOY ROUND-TRIP: gen58 on the device (content-identical to gen57 — no config change since)

Request: "deploy the current gen to the device and switch." Ran
`bash bin/deploy.sh deploy` under run-job (job rc 0, 2026-09-09
23:30Z). HEAD since the gen57 deploy (22:50) was only the docs commit
15edf0b, but the flake source-tree hash changed → new toplevel, so the
deploy was a real build+ship+switch round trip rather than a no-op:

- built toplevel 152x4bzsqj1xbp6ksckqb432220c3zl7 (native aarch64,
  remote builder; small delta over gen57's 4.43 GB closure — the
expected docs-commit behavior: same content, new path), gc-pin added.
- `nix copy` → device store; profile → `system-58-link`;
  switch-to-configuration clean (polkit restart, /etc setup; no
  config/units actually changed vs gen57).
- Verified: gen58 (current) = 152x4bz…, activation log clean.
- No flash, no para/boot touch; gen57 remains the rollback target
  (`bash bin/deploy.sh rollback`).

Next: nothing pending — the round trip confirmed the deploy loop is
still green after the cjdell desktop rework.

## 2026-09-09p — CJDELL IS THE DEFAULT DESKTOP USER: LXQt + Phosh + audio sessions run as cjdell (not root), passwordless sudo — gens 55-57, deployed + cold-boot verified over ssh

Task: "create a user cjdell that is the default user for desktop
environments on the device, with password-less sudo."

Since the 2026-09-07 LXQt landing the desktop booted as ROOT systemd
system services with HOME=/root (every session env + seed script hard-
coded /root; chrome needed --no-sandbox for euid 0). This session made
cjdell a real account and moved the desktop + audio SESSIONS onto it.

Changes (commits e828e5c, e572f79, d63edbc — all config/scripts, NO
boot.img reflash; device flashed nothing):
- config/gemini.nix: users.users.cjdell (uid 1000 — REPLACES the old
  placeholder `gemini` account, removed at gen55 activation; home
  /home/cjdell; groups wheel/video/audio/networkmanager; operator ssh
  key, same as root). getty autologin -> cjdell. Passwordless sudo =
  the existing security.sudo.wheelNeedsPassword=false (wheel). Root
  keeps the ssh admin path. chrome --no-sandbox comment updated (the
  desktop is no longer root; flag kept — no userns on this kernel).
- services/lxqt.nix + services/phosh.nix: session units get
  User=cjdell, HOME/XDG_* = /home/cjdell, and their OWN cjdell-owned
  RuntimeDirectory (/run/lxqt-session, /run/phosh-session) hosting the
  session bus (was: shared /run/gemwl root bus). Session configs seed
  to /home/cjdell (services/scripts/start-lxqt-nested + config/lxqt/*
  all $HOME-relative now).
- services/audio.nix: pipewire/wireplumber/pipewire-pulse run as cjdell
  (systemd chowns /run/gemwl-audio to User=) so the desktop reaches the
  sound sockets; gemini-audio-defaults stays root (amixer + devmem
  speaker-amp gpio). Only loss vs root: no RT scheduling (no rtkit) —
  PipeWire logs it and runs SCHED_OTHER.
- services/desktop.nix: gemwl STAYS a root service (fbcon unbind via
  /sys/class/vtconsole bind + /dev/gemfb 0600 are root-only). Its
  runtime dir is now 0755 (was 0700) + UMask=0111 -> wayland-0 socket
  0666, so the cjdell sessions can connect. Single-user trusted PDA;
  cjdell has passwordless sudo anyway.

On-glass receipts (2 bugs found + fixed during deploy):
- gen55: lxqt-nested restart-looped "Could not connect to remote
  display: No such file or directory" — libwayland resolves a BARE
  WAYLAND_DISPLAY against XDG_RUNTIME_DIR, and the session's runtime
  dir was no longer gemwl's (/run/gemwl) where wayland-0 lives. Fix
  (e572f79): start-lxqt-nested + prepare-phosh-session ln -s
  $XDG_RUNTIME_DIR/wayland-0 -> /run/gemwl/wayland-0; phosh unit env
  sets WAYLAND_DISPLAY=wayland-0 for phoc.
- gen56: session up (NRestarts=0) but the 1.5x UI scale probe only
  looked at wayland-1..3 — labwc's OWN listening socket now takes
  wayland-0 in the session dir (wlroots auto-socket unlinks + reuses
  the name after the parent link is consumed; the parent connection is
  already established). Fix (d63edbc): probe wayland-0 first. Probing
  gemwl itself is a safe no-op (no wlr-output-management on gemwl).

Verified over ssh on gen57 after a COLD WDT reboot (para untouched):
- gen57 current (3fjm15ims…); gemwl (root) + lxqt-nested + pipewire
  active, NRestarts=0; labwc/lxqt-session/lxqt-panel/pcmanfm-qt/dbus-
  daemon all run as cjdell; /run/lxqt-session + /run/gemwl-audio
  cjdell-owned, /run/gemwl 0755 root with wayland-0 0666.
- `su - cjdell -c 'sudo -n whoami'` -> root (passwordless);
  `ssh cjdell@10.15.19.82` works (same key); tty1 agetty autologins
  cjdell; /home/cjdell/.config/{lxqt,labwc} seeded; wlr-randr on
  labwc shows WL-1 Scale: 1.5.

PENDING (needs eyes): visual glass check of the cjdell LXQt session
(was: root session on gen54). Everything else (units, sockets, perms,
seed, sudo, ssh, audio as cjdell) verified over ssh. If the glass is
wrong: `bash bin/deploy.sh rollback` (gen54 = the old root desktop) or
`systemctl disable gemwl lxqt-nested` for the console.

Next: eyes-on-glass; then decide whether chrome can drop --no-sandbox
(non-root now — needs a userns or SUID test), and whether gemcli's
units flip to a cjdell-user model.

## 2026-09-09o — PHOSH DESKTOP LANDED (the LXQt alternative): phoc 0.54.0 nested inside gemwl hosting the phosh shell — built + readelf-verified host-side, NOT switched (no config/device change on glass)

Task: "try phosh as the desktop environment instead of lxqt; will gemwl
support it with GPU acceleration?"

**Answer (design + receipts in docs/phosh.md):** gemwl cannot host phosh
— it is a minimal xdg-shell KIOSK compositor (no layer-shell /
foreign-toplevel / session-lock / text-input, no phoc-private protocol
that phosh expects). The proven nested pattern (labwc in gemwl) is
reused with phoc (phosh's own compositor):
`WLR_BACKENDS=wayland phoc -v -S -C <ini> --socket phosh -E start-phosh-shell`
nested on gemwl's wayland-0, exec'ing `$out/libexec/phosh` (NOT
bin/phosh-session — that starts its own DRM phoc). GPU acceleration
stays end-to-end fork-mesa: GTK4 (EGL ICD → fork) → phoc (wlroots
0.19 gles2/gbm on renderD128 → fork) → gemwl (GPU blit to LK fb).

**Files:** `services/phosh.nix` (option `services.phoshDesktop.enable`,
default OFF — LXQt stays the default; assert-guarded to require
lxqtNested off), `services/scripts/{prepare-phosh-session,
start-phosh-shell}`, `pkgs/phoc-geminipda.nix`, flake packages
`phoc`/`phosh`/`squeekboard`, docs/phosh.md, config/gemini.nix import.

**Pins:** phosh 0.54.0 + phoc 0.54.0 + squeekboard 1.43.1 from the
pinned nixpkgs (dc5d91f84032). phoc builds against nixpkgs'
wlroots_0_19 (0.19.3) — phoc 0.54 does NOT build against the repo's
wlroots 0.18.2 pin (that stays gemwl's). pkgs/phoc-geminipda.nix
re-runs nixpkgs' phoc expression with its wlroots 0.19.3 rebuilt so its
libgbm is the FORK mesa (single-mesa GPU closure — cross-mesa EGL/gbm
is the AFBC landmine the fork avoids). phoc's own layer-shell
0-dimension revert patch (nixpkgs recipe) composes on top.

**Build receipts (2026-09-09):**
- First rebuild attempt failed: replacing nixpkgs' mesa-libgbm (which
  PROPAGATES libdrm — gbm.nix propagatedBuildInputs=[libdrm]) with the
  fork (propagates nothing) dropped wlroots' meson `libdrm` lookup
  ("Run-time dependency libdrm found: NO"). Fix: add pkgs.libdrm to
  the override buildInputs.
- `nix build .#packages.aarch64-linux.{phoc,phosh}` rc=0 (~40 s: the
  only local compiles are wlroots 0.19.3 + phoc relink on the remote
  builder; phosh's GNOME-sized closure substitutes from cache).
- readelf on the forked wlroots: RUNPATH carries
  …-mesa-geminipda-25.0.7/lib; libgbm.so.1 resolves to the fork's
  libgbm.so.1.0.0 (single-mesa confirmed; stock mesa-libgbm 26.1.3 in
  phoc's closure is from phoc's other deps, not its wlroots).
- `phoc.drvPath` != stock pkgs.phoc.drvPath (override is live).

**Not done (deliberate):** no gnome-session scaffolding (v1 execs the
shell directly — the classic nested-dev flow; expect missing-session
warnings + reduced features), no portals/stevia-ibus/upower/NM
integration, no on-glass run (device untouched this session; next:
switch on a clone + run the checklist in docs/phosh.md).

## 2026-09-09n — FLAKE GAINS `nixosConfigurations.gemini`: the STOCK `nixos-rebuild switch --flake .` loop works on the device (no bash script) — config-only, NOT yet switched on glass

Task: "re-organise the flake so a clone of this repo ON the device can
make on-device config changes with `nixos-rebuild switch --flake .`
(not using a bash script)".

**Why it works without a script now (receipts):**
- The MNX eval (lib/eval-with-configuration.nix → NixOS evalConfig with
  MNX + the FULL nixos module-list) ALREADY produces every piece the
  tool needs: `config.system.build.toplevel` (= mobile.outputs.toplevel
  — modules/outputs.nix default; the drv the device boots),
  `config.system.build.nixos-rebuild` (installer/tools/tools.nix is in
  the module list) and nixos-rebuild in sw/bin by default
  (system.tools.nixos-rebuild.enable = config.nix.enable &&
  !disableInstallerTools; **nix.enable is DEFAULT-TRUE at this pin** —
  nixos/modules/config/nix.nix, verified in dc5d91f84032). The bash
  nixos-rebuild is GONE from this nixpkgs (removed; replaced by the
  Python nixos-rebuild-ng — pkgs/by-name/ni/nixos-rebuild-ng).
- /etc/NIXOS + /nix/var/nix/profiles/system already exist on the
  device (MNX rootfs postBootCommands created them at first boot),
  `/run/current-system/nixos-version` exists (toplevel writes it).
- The ONLY missing piece was the flake output: added
  `nixosConfigurations.gemini = eval.eval` (the RAW evalConfig result —
  the lib.nixosSystem shape; NOT the eval shim wrapper which carries a
  `__please-fail` throw). Host receipt: `nix eval` of
  .#nixosConfigurations.gemini.config.system.build.toplevel.drvPath ==
  .#packages.aarch64-linux.toplevel.drvPath
  (`97gjx75slr…-nixos-system-gemini-26.11pre-git.drv`, both) and
  .config.system.build.nixos-rebuild.drvPath =
  `nixos-rebuild-ng-26.11.drv` (the reexec step will resolve).
- `system.configurationRevision = self.rev or null` wired into the
  eval (flake.nix, inline module): every toplevel records the git rev
  it was built from (self.rev = `<sha>` clean / `<sha>-dirty` modified
  / null on a non-git copy — dirty semantics verified on nix 2.34.8
  with a throwaway repo), shown by `nixos-rebuild list-generations` /
  `nixos-version --configuration-revision`. Host + device builds of
  the SAME commit therefore converge to the same store path.

**nixos-rebuild-ng flow (read from its source at the pin):** reexecs
itself from `.#nixosConfigurations."<hostname>".config.system.build.
{nixos-rebuild,toplevel}` (hostname from `uname -n` = gemini), then
`nix-env -p /nix/var/nix/profiles/system --set <toplevel>` (guarded by
a `test -f nixos-version`) + `<toplevel>/bin/switch-to-configuration
switch` via systemd-run — i.e. the same activation device-rebuild.sh /
deploy.sh do by hand. --install-bootloader stays OFF by default (no
bootloader on this device — nothing changes there).

**Docs touched:** README "On-device build/switch" section (now
nixos-rebuild-primary, device-rebuild.sh = convenience wrapper),
README status + bin table row, AGENTS.md status + rows, config/gemini.nix
comment, bin/device-rebuild.sh header.

**ON GLASS PROOF (same session, after committing df6efc2 + pushing the
clone via bin/device-repo.sh push — device clone was dirty with a stray
chmod +x on bin/device-rebuild.sh; cleared to unblock the push):** all
three steps ran as root from /root/gemini-nixos on the PDA via
device-ssh, under run-job:
- `nixos-rebuild dry-build --flake .` — rc=0 in ~70 s (reexec built
  the config's own nixos-rebuild-ng, full eval; only 3.6 KiB of
  fetches needed).
- `nixos-rebuild build --flake .` — rc=0 in ~38 s; config-glue drvs
  (etc/system-units/dbus units) compiled locally, the rest
  substituted; toplevel `j334wc266x2c4xmq7ndijcnm58bjzqhg`.
- `nixos-rebuild switch --flake .` — rc=0 in ~54 s; standard NixOS
  switch dance ("do not know how to make this configuration
  bootable" warning expected — LK boot.img boots the device, no NixOS
  bootloader). **gen54 current**; gemwl/lxqt-nested/gemini-sleepd/
  bluetooth all still active.
- `nixos-rebuild list-generations` shows gen54 with **Configuration
  Revision = df6efc232c24cd807293f2e1abc67ea83d968b96** (older gens:
  "Unknown" — they predate the wiring); `nixos-version
  --configuration-revision` agrees.
- Host/device convergence receipt: host `nix eval` of the toplevel
  drvPath at the SAME clean commit == the device dry-build's drv
  (f8a75ndy…nixos-system-gemini-26.11pre-git.drv) — identical store
  path, host build == device build. Device nix = 2.34.8 (same dirty-
  suffix semantics verified on the host).

Nothing flashed (profile-only switch; para untouched).

## 2026-09-09m — BLUETOOTH GUI + CLI TOOLS: persistent bluetoothd on the system bus, bluetoothctl + blueman-manager/applet on glass (gens 52-53; config-only, no kernel/boot.img change)

Task: "continue the bluetooth effort; we need gui and cli tools working".
Closed bring-up follow-up #1 (persistent bluetoothd/dbus wiring) and
put a real GUI + CLI stack on glass:

**Config (all in services/bluetooth.nix, imported from config/gemini.nix):**
- `hardware.bluetooth.enable` — the nixpkgs option at this pin. The
  bring-up doc's claim that `services.bluetooth` "does not exist in this
  MNX eval" was HALF-RIGHT: the option was RENAMED to hardware.bluetooth
  before this rev (the services.* alias is gone); the MNX eval DOES
  import the full nixpkgs module list (mobile-nixos lib/release-tools.nix
  evalWith: ../modules/module-list.nix ++ nixos/modules/module-list.nix).
  Enabling it wires bluetoothd (dbus-org.bluez.service alias) + bluez in
  systemPackages (bluetoothctl/btmgmt/hciconfig/hcitool on PATH) + dbus
  policy (bluez joins services.dbus.packages) + /etc/bluetooth/main.conf.
- `gemini-bt-hci.service` — modprobe hci_stp at boot (After=
  gemini-wifi-internal; the module tree ships it in current-system:
  drivers/misc/mediatek-connectivity/drv_bt/hci_stp.ko).
- `systemd.targets.bluetooth.wantedBy = [multi-user.target]` — bluetoothd
  starts at boot.
- **Privacy = off** (main.conf) — WITHOUT it the daemon's AutoEnable
  power-on fails at boot: bluez 5.87 runs mgmt set-privacy during the
  auto-power path and the MT6630 rejects it ("Failed to set privacy:
  Rejected (0x0b)") -> Powered: no after every reboot (interactive
  `bluetoothctl power on` always worked). Verified with a manual
  `bluetoothd -f` test conf first, then baked in. gen53.

**On glass (gen52 vgqzphxbk1qlg5lgfjdsx3pc67wls4r1, gen53
lryfqqadq05zzvmva8bwzb3svqmda53y — config-only deploys via deploy.sh, no
kernel/boot.img change):**
- bluetooth.service ACTIVE, owns org.bluez on the SYSTEM bus (dbus-
  broker). Legacy bring-up test daemons (gemini-bt-bus/gemini-btd
  systemd-run transients on the permissive private bus) STOPPED.
- **Boot path: Powered: yes with NO manual step** (AutoEnable after the
  Privacy=off fix). Alias "gemini". hci0 UP RUNNING, br/edr + le.
- **CLI**: `bluetoothctl show` / `power on` / `scan on` -> Discovery
  started / Discovering: yes, 0 STP timeouts; btmgmt/hciconfig/hcitool
  all on PATH.
- **GUI**: blueman 2.4.6 (cache-healthy at the pin — 254-path closure
  on cache.nixos.org, rule 9). `blueman.desktop` autostart picked up by
  lxqt-session via /run/current-system/sw/etc/xdg/autostart; applet + a
  NEW separate blueman-tray process run in the session. The panel had NO
  tray plugin -> added [statusnotifier] to panel.conf (live + seeded in
  config/lxqt/panel.conf via start-lxqt-nested). Verified objectively:
  StatusNotifierWatcher RegisteredStatusNotifierItems =
  [":1.x/org/blueman/sni"] + IsStatusNotifierHostRegistered=true.
  blueman-manager opens a window on the nested labwc (screenshots at
  /tmp/bt-mgr-final.png + /tmp/bt-glass-labwc.png, pulled to the host;
  the model can't view them but grim file sizes + the dbus SNI registry
  prove render + registration). Left OPEN on glass for the user's first
  look.
- lxqt.nix: blueman joins the session lxqtApps list when
  config.hardware.bluetooth.enable (PATH for the autostart Exec= + XDG
  dirs). dconf warnings in the manager log are cosmetic (no dconf
  daemon in the session).

**Still open (renumbered follow-ups in docs/bluetooth-bringup.md):**
LE event-mask 0x2001 low-bit quirk; RF/pairing test with a real device;
hci_stp lsmod-vanish cosmetic quirk; PAN tethering (kernel lacks
CONFIG_BT_BNEP — bluetoothd logs "kernel lacks bnep-protocol support");
cosmetic bluetoothd-start mgmt failures (clear/add UUID 0x03) on this
controller.

Version lines: gen52 = services/bluetooth.nix first cut (bluez + dbus
wiring + hci loader + blueman), gen53 = + Privacy=off (auto-power boot
fix). Device current gen 53, para untouched (NixOS default), desktop up.

## 2026-09-09l — BLUETOOTH BRING-UP part 2: CONSYS rx-stall ROOT CAUSE + fix, hci0 init to the BR/EDR wall (gens 46-48; module-only, no boot.img reflash)

Task: continue the 2026-09-09k BT work — "fix the CONSYS rx stall, then
hciconfig hci0 up / bluetoothctl scan on glass".

ROOT CAUSE OF THE RX STALL (found + fixed):
- The rx "stall" was the CONSYS MCU's AUTONOMOUS SLEEP, not a vFIFO
  deadlock: with the device idle, a HCI reset's reply sat INSIDE the
  asleep MCU and surfaced only at the next open's func-on WAK pulse —
  measured 17 s late (83 min in one case). The RX vFIFO was empty at
  every register sample while the reply was "missing" (0x11000aac
  WPT=RPT, VALID=0), and a bare WAK pulse with NO traffic released the
  stuck reply instantly.
- FIX 1 (05bbb33d3, mtk_wcn): wake-before-send — mtk_wcn_btif_write
  now pulses the BTIF WAK line (0x1100c064) before EVERY transport
  write. On glass this converted the ~2 s cmd timeouts into ~40-120 ms
  exchanges; the full hciconfig init sequence ran (BD read back, etc.).
- FIX 2 (16e6d817, mtk_wcn): WAK keep-awake heartbeat — the MCU can
  doze again INSIDE the 40-80 ms reply window, so replies landed late/
  out-of-order and the HCI cmd-sync machinery desynced (-EINTR/
  -EREMOTEIO noise, post-timeout replies, cascading bogus failures).
  Now pulse WAK every ~30 ms while BT writes are recent, stop ~200 ms
  after the last one (module params btif_wak_hb_ms / btif_wak_hb_hold).
  Post-fix each command gets exactly ONE reply on the ~80 ms cadence.

LE / FEATURE-OVER-ADVERTISING WALL (next blocker, being worked):
- hci0 init now proceeds past the transport issues but the MT6630
  OVER-ADVERTISES: its feature/supported-command replies claim more
  than the firmware implements and it refuses the corresponding
  commands — LE Set Event Mask (0x2001) -> 0x20 Command Disallowed
  (deterministic, even with the MCU held awake), GET_MWS_TRANSPORT_
  CONFIG (0x140c) refused with the MWS bit set, some LE 5.x commands
  -> 0x01 unknown. Each refusal aborted the whole open and kept BR/EDR
  down too.
- FIX 3 (7dce5d17 -> f6b135a3, bluetooth.ko): HCI_QUIRK_EXT_INIT_BEST_
  EFFORT (renamed from LE_INIT_...) — hci_stp sets it at probe; the
  kernel runs the le_init3 + hci_init4 + le_init4 stages best-effort
  (log + continue) so BR/EDR + basic LE come up. Note: 0x2001 is
  refused even with the MCU held awake (WAK hammering) -> not a sleep
  race; suspected radio-config/BT-firmware-state gating, deferred until
  hci0 is UP and LE can be probed live.

STATE (gens 46-48): hci_stp + bluetooth modules (no boot.img change);
every fix module-only. Kernel fork HEAD f6b135a3 (synced to delta
byte-identical). Latest glass run got through ALL of init1/2/3 and
hci_init4's first 4 extras; refused 0x140c (MWS) -> init4 tolerance
build in flight at session end. 40-80 ms per exchange still (fine);
the 2 s HCI timeout no longer fires.

EXPERIMENTAL OBSERVATIONS worth keeping:
- hci_stp module vanishes from lsmod after a failed open attempt in
  some runs (bluetooth+hci_stp both gone, wifi unaffected) — cause
  not chased; modprobe hci_stp again re-creates hci0. Suspect the
  failed-open path + deep-idle stub WARNs (mtk_wcn_stub_alps.c
  "NULL function pointer" on COMBO_IF_BTIF deep idle, cb never
  registered) — cleanup TODO.
- The eFUSE BD reply 00:00:46:02:79:01 != the unprogrammed default
  00:00:46:66:20:01, so the driver treats it as factory-programmed
  (MediaTek OUI 00:00:46) — plausible real BD; not the blocker.
- Host-side tooling: on-device `nix-shell -p bluez` provides
  hciconfig/hcitool; bluetoothd needs a start (dbus bus present,
  /var/lib/bluetooth absent). New host helper: bin/bt-glass-test.sh
  (up / up-wak / regs / wak / init1).

NEXT ACTION: verify the init4-tolerance build on glass -> hciconfig
hci0 up should COMPLETE (BR/EDR) -> hcitool dev + hcitool scan ->
start bluetoothd (nix-shell -p bluez) -> bluetoothctl power on +
scan on -> then probe LE post-open (hcitool cmd 0x08 ...) to see if
the 0x20 LE refusal is only an open-time artifact.

--- RESOLVED LATER THE SAME DAY (second half of this entry, appended later that evening): ---

All of the above NEXT ACTION landed ON GLASS. Version lines per flash
(module-only deploys via bin/deploy.sh + WDT reboot, gens ~46-50; the
kernel Image/boot.img untouched all day):

1. gen46 05bbb33d3 (wake-before-send)
2. gen47 7dce5d17 -> f6b135a3 (LE-init tolerance -> ext-init best-
   effort incl. hci_init4: the NEXT wall after LE was GET_MWS_TRANSPORT_
   CONFIG 0x140c refused with the MWS feature bit set)
3. gen48/49 16e6d817 -> 440be2a1 (WAK heartbeat: write-triggered 200 ms
   hold -> OPEN-GATED. Why: the first bluetoothd discovery ran the
   transport into "stp_do_tx_timeout: TX retry limit = 10" -> spike
   resync — no host writes for the whole discovery interval -> MCU
   dozed -> scan-disable got no ACK. Heartbeat now runs
   mtk_wcn_btif_open..close (~30 ms pulse train), write-pulse kept for
   the session's first write.)
4. gen50 7af6ee9c (LE local record: btmgmt le on -> "powered br/edr le";
   without it mgmt discovery REJECTED 0x0b because HCI_LE_ENABLED hangs
   off the refused Write LE Host Supported 0x200d).

FINAL GLASS STATE (gen50): hci0 UP RUNNING, BR/EDR + LE both on;
`btmgmt find` finds real LE devices over the air (4A:A8:59:57:1B:65,
41:BE:81:BB:F9:E4 — LE Random, rssi -96..-98, distant beacons);
bluetoothctl power on + scan on work end-to-end on a private system
dbus (org.bluez policy missing from the stock /etc/dbus-1 static tree
-> test bus with permissive policy under systemd-run units); 0 STP
timeouts / 0 resyncs across repeated 40 s discoveries. Left on glass:
hci0 UP (safe — no panel/fastboot involvement).

Still open: (1) persistent bluetoothd/dbus in config/gemini.nix —
services.bluetooth does NOT exist in this MNX eval (see bring-up doc);
(2) LE event-mask 0x2001 low-bit acceptance quirk (kernel d0 05 refused
at open, best-efforted; mgmt path still delivers DeviceFound); (3) RF
test with a nearby device; (4) hci_stp module-vanish-on-failed-open
cleanup TODO.

## 2026-09-09k — BLUETOOTH BRING-UP (MT6630 CONSYS): hci_stp driver ported (3.18 vendor → 6.6), BT radio powers on and answers HCI — blocked on a CONSYS rx-delivery stall (BT-channel frames arrive ~17 s late) so hci0 init can't complete yet (gens 37-42; NO boot.img reflash — module-only changes)

Task: "get bluetooth working". The Gemini's BT = the BT half of the
on-die MT6630 CONSYS combo (same chip Wi-Fi uses; no separate FW/HCI
bus): BT HCI rides the WMT/STP fabric as the BT channel of mtk_wcn.
Ported the vendor 3.18 in-kernel BlueZ driver for this exact device
(drv_bt/, CONFIG_MTK_COMBO_BT_HCI = the halium-defconfig path) to 6.6
as CONFIG_MTK_WCN_BT_HCI=m hci_stp (H4 HCI over STP BT_TASK_INDX).
Full story: docs/bluetooth-bringup.md.

FINDINGS / FIXES (each commit on the fork geminipda-bringup, synced to
the repo delta byte-identical; fork HEAD after session b0c0254e5 + the
resilient-init commits):
1. BT func-on CRASHED the combo chip (PSM wait-timeout assert, whole
   chip reset, wifi died) whenever BT turned on after ~70 s idle: the
   MCU autonomously sleeps even on the DMA transport and the delta's
   BTIF wake was a NO-OP. Fix: pulse the BTIF WAK line
   (ap_wakeup_consys @ 0x1100c064, vendor hal_btif_send_wakeup_signal)
   from mtk_wcn via a self-contained ioremap — deliberately NOT via
   the builtin spike's ops (spike = kernel Image; module-vs-Image ABI
   drift oopsed once: new mtk_wcn + old boot.img's spike -> pc-garbage
   oops at mtk_wcn_btif_wakeup_consys+0x28; lesson: builtin spike
   changes need a boot.img reflash, module changes don't). Also never
   arm CONSYS PSM in the SOC spike sw_init (wmt_ic_soc.c).
2. With the WAK fix: OPID(3) type(0) ok in ~0.12 s — BT func-on
   WORKS, hci0 opens fast, func-off ok, no crashes.
3. Chip answers HCI with CORRECT cmd-completes (saw 04 0e 04 01 03 0c
   00 = reset complete) but BT-channel RX delivery stalls: a reset
   sent at T got its reply delivered T+17 s (during the next open),
   and every kernel HCI request (2 s HCI_CMD_TIMEOUT) dies first:
   "hci0: Opcode 0xc03 failed: -110" / hciconfig "Connection timed
   out". The stall lives in the SHARED CONSYS rx path
   (mtk-consys-spike.c consys_wmtrxd/btif_rx_drain — poll ~50 ms +
   2-5 ms usleep can't stall 17 s alone; suspect wmt_mtx stall or
   vFIFO-full after the earlier assert-reset cycles). UNSOLVED.
4. eFUSE BD read (vendor 01 09 10 00) returns nothing on this unit —
   driver autogens a locally-administered address (or bd_addr param).

Also learned/recorded: the bare devmem 0x10007004=0x48 WDT write does
NOT reboot the unit anymore (cl2-up leaves WDT mode=0); full sequence
does: 0x10007000=0x22000015 + 0x10007004=0x48 + 0x10007008=0x1971
(LK RESTART_KEY 0x1971 — mtk_wdt.c) — used ~8x this session.

Generations: 37 (modules first land) … 42 (last: tracing build). All
module-only; boot.img untouched; device left HEALTHY on wifi ("The
Lab"), ssh over g_ether. Kernel commits: devices/
planet-geminipda/kernel/{config,config.full-329,default.nix} (BT=m
+ MTK_WCN_BT_HCI=m + rev headers); new docs/bluetooth-bringup.md.
Next: fix the CONSYS rx stall, then hciconfig/bluetoothctl on glass.

## 2026-09-09j — 32-BIT WINDOWS ON THE PDA VIA WINE-WOW64: dsd_lm.exe (Doomsday "Lego Mania", Assembly 2003) RUNS — music plays, 32-bit GL stack proven on glass with the glprobe32 probe — but its scene presents BLACK; root-caused to the demo's fixed-function/display-list GL vs the guest GL being panfrost-T880 (env-llvmpipe does NOT stick); wine/wine64 CLIs added (gen36)

Task: "get /root/Downloads/dsd_lm.exe running" (Doomsday's Lego-Mania
PC demo, scene.org — fmod.dll + ijl11.dll + lmania.ogg beside it).

**Static analysis first (rule: never run blind):** dsd_lm.exe is a
**PE32 Intel i386 (32-bit) + UPX-packed** app importing OPENGL32/GLU32
(realtime GL per its readme), fmod, WINMM, GDI32-SwapBuffers
(double-buffer via gdi32, the classic path), USER32 dialogs +
ChangeDisplaySettings. The existing stack (wine64 11.0, 64-bit-only
prefix) canNOT run 32-bit PEs — its lib/wine has only x86_64-unix/-windows,
no i386 payload; prefix syswow64 was empty.

**Stack extension (all hydra-cached at dc5d91f84032, rule 9):**
- nixpkgs **`wineWow64Packages.full`** = wine-wow64 11.0
  (`5qkxzvcr5k4pglxsnzvqz308sn6cs02m`; 706 MB closure, 1072 i386-windows
  PEs + x86_64-unix — the new-WoW64 single-loader build, no i686 linux
  needed). box64's README: "Wine WOW64 build to run x86 Windows programs
  in Box64-only environments… experimental but works in most cases"
  (the known-broken case = wined3d/D3D pointer maps; ours is GL).
- Shipped via `nix copy` (25 s); launcher /root/wine-x86/wine-wow
  (wine64's twin, WINEPREFIX=/root/.wine-wow default) + GC root; wineboot
  -u in the new prefix **populated syswow64** (159 s job). VERIFIED:
  32-bit PEs load; the 64-bit d3d9test cube also spins under wine-wow
  (present path intact).
- glprobe32 (new, pkgs/glprobe32): 32-bit i686-windows PE (mingw32 cross
  — `GL/gl.h` path case matters on Linux) that mirrors a 2003 demo's GL
  dance (2 shared contexts, display list built on ctx B executed on A,
  colored clears, gdi32 SwapBuffers) and glReadPixels its own buffer to
  BMPs. ON GLASS: user saw the flashing colored triangle — **32-bit
  guest GL present under box64+wow64 WORKS and shows color**.

**dsd_lm.exe on glass:** runs (32-bit PE in wow64), fmod music plays via
winmm, window appears (small square, top-left of the 2160×1080 glass —
ChangeDisplaySettings fails on wayland, -2, so no fullscreen), but the
scene is BLACK (white intro frame → black, stays). Trace receipts:
- +wgl: EGL 1.5 init fine, TWO shared contexts, pixel format 174
  (10,10,10,2 + depth/stencil on llvmpipe's config list), MakeCurrent
  on the window OK — but a later 120 s run showed 8302
  win32u_wgl_context_flush (~69/s — the app presents constantly; the
  presented content is black, not a stall). glReadPixels imports
  suggest readback use, no GDI BitBlt (renders GL straight to window).
- **Mesa guest GL is panfrost-T880, NOT llvmpipe** (renderer string
  "Mali-T880 MC4 (Panfrost)", GL 3.1 Mesa 26.2.2) — with
  GALLIUM_DRIVER=llvmpipe AND LIBGL_ALWAYS_SOFTWARE=1 exported! The
  guest mesa's EGL-wayland opens the T880 render node via GBM and loads
  panfrost_dri regardless of those env vars (DRI device loading ignores
  them). **The earlier session's "llvmpipe was the safe path" claim is
  WRONG — every wine GL run (d3d9test 55-60 fps cube incl.) was
  panfrost-T880 through box64 all along** [corrected 2026-09-09; the
  wedge incident stands (it was real) but its attribution to "forced
  guest panfrost" is unsupported — today's long guest-panfrost runs
  (probe minutes, triangle) were stable; re-root-cause needed].
- **Hypothesis for the black scene (NEXT ACTION to test):** dsd_lm is a
  fixed-function GL 1.x engine (glLightfv/glMaterialfv/glFog/glTexGen/
  glPolygonMode/glListBase/glCallLists imports) — panfrost GL 3.1's
  fixed-function + display-list coverage is the weak spot; llvmpipe
  implements it fully. Since the guest CANNOT be env-switched to
  llvmpipe, testing it needs a mesa build with panfrost disabled
  (or the ICD swapped) for the guest — see handover below.

**CLIs (the ask):** wine/wine64 now on the device shell via
pkgs/wine-cli.nix + config/gemini.nix systemPackages (**gen36**,
5fec818): thin wrappers exec'ing /root/wine-x86/wine-wow (the stack
stays a standalone GC root — docs/wine-d3d.md §5). A first attempt to
write wrappers into /root/.nix-profile/bin failed (that profile dir is
uninitialised on this device — vestigial PATH entry) — systemPackages
is the right home. Device left running gen35 at session start; gen36
build/deploy in progress at close.

Files: pkgs/wine-cli.nix, config/gemini.nix (gen36), pkgs/glprobe32.nix
+ pkgs/glprobe32/glprobe32.c, /root/wine-x86/wine-wow (device),
docs/wine-d3d.md §7. Not committed in-repo: the UPX-unpacked exe
(replaced /root/Downloads/dsd_lm.exe; original backed up as
dsd_lm.exe.upx.bak on the device; sha256 feb7bd4c…).

## HANDOVER — suggested actions to finish the dsd_lm.exe work

1. **Test the fixed-function hypothesis:** get the demo onto a true
   software GL. Env forcing FAILED (GALLIUM_DRIVER, LIBGL_ALWAYS_SOFTWARE
   — panfrost_dri loads via GBM regardless). Options: (a) build the
   x86_64 guest mesa (pkgs/wine-x86.nix `mesa`) with panfrost
   disabled/swrast-only and redeploy just that path — cleanest;
   (b) LIBGL_ALWAYS_SOFTWARE plus pointing __EGL_VENDOR_LIBRARY_FILENAMES
   at a swrast-only ICD dir; (c) box64 hiding /dev/dri (no known knob).
   If the demo shows content on llvmpipe → confirmed panfrost GL bug →
   decide: accept llvmpipe (slow) for 32-bit GL guests or fix panfrost.
2. **Re-root-cause the 2026-09-09 wedge** (attribution to forced guest
   panfrost now doubtful — today's guest-panfrost runs were stable):
   reproduce deliberately with fresh prefix + no other GPU users + WDT
   net (para=boot-recovery) when the user approves a risky run.
3. **Make the demo actually visible/useful on glass:** it renders into a
   small window (ChangeDisplaySettings unsupported on winewayland).
   Wine virtual desktop (registry) or gemwl viewport scaling could scale
   it; or accept windowed. Also gemwl LACKS wlr-screencopy (grim fails:
   "compositor doesn't support the screen capture protocol") — add
   wlr_screencopy_manager_v1_create to pkgs/gemwl/gemwl.c for receipts.
4. **Fold the wow64 stack into the tooling:** extend bin/wine-x86-deploy.sh
   (status/deploy/init/run/log/kill) with the wine-wow64 path + wow
   prefix so deploys are scripted (today's device setup was ad-hoc:
   wine-wow launcher, GC root, prefix).
5. **Cleanup:** delete the stale wine-wow prefix/probe dirs when done;
   keep dsd_lm.exe.unpacked receipts; consider UPX-unpacking step inside
   the run flow (wine CAN load UPX'd PEs only if wow64's 32-bit exec
   works for the stub — unpacked on host is deterministic, prefer it).
6. **Log/commit:** session-log entry + docs/wine-d3d.md §7 (32-bit
   wow64 path, glprobe32, panfrost-not-llvmpipe correction) done here;
   close out gen36 verification (wine --version on device) next session.

## 2026-09-09i — GEMDEMO PURGED TO A SINGLE-FILE GLES 3.1 TEMPLATE (0.3.0), DEPLOYED + CONFIRMED ON GLASS (gens 34-35)

Verdict from the 0.2.0 "GEMINI: EXODUS" on-glass work: the demoscene is
useless except as proof of hardware interaction → stripped to the
skeleton. `pkgs/gemdemo/` is now **ONE Rust file** (`src/main.rs`, ~560
lines incl. the receipt comments) that boots a GLES 3.1 EGL context on a
winit Wayland surface (panfrost fork, Mali-T880) and draws one spinning
per-vertex-shaded triangle while cpal plays a 440 Hz sine through the
`gemini16` plug — the OpenGL-app template going forward (copy file +
pkgs/gemdemo.nix). Deleted: gfx/show/synth/font/cpu/glctx/glutil/audio/
shaders modules (~5200 lines; sources + the 0.2.0/0.1.0 docs live in git
history). libc dep dropped (was only the A72-affinity code). Lockfile
regenerated (212 pkgs; winit 0.29.15 / egl 0.2.7 / gl 0.14 / cpal 0.15.3).

The hardware receipts the purge preserved — now the file-header doc + a
compact docs/gemdemo.md: DSA `glCreate*` silent stubs → `glGen*`;
one interleaved VBO per VAO, no instancing / no multi-buffer VAOs / no
`glDrawElements` (panfrost crashes); `wl_egl_window` required for the
Wayland surface; **audio wire state S16_LE @ 44100 Hz** (S32/48k on the
16-bit MT6351 = noise) — `gemini16` by name (hint-gated) + forced I16
config; uniform typos are silent on GLES → assert.

**Deploys:** `bash bin/deploy.sh deploy` ×2 under run-job (each ~67 s,
rc=0):
- **gen34** `ybrd7qcd…` (commit 6f34d24) — first 0.3.0; on-glass smoke
  FAILED at the shader: `shader tri failed:` with an EMPTY log — the
  inlined compile closure passed the shader-TYPE enum straight to
  glShaderSource (dropped the `glCreateShader` call; invalid object
  handle → compile status 0). Lesson: cargo check can't see GL bugs;
  the panic-with-log helper is only as good as the code before it.
- **gen35** `29afwk7d…` (commit 26746d7, the fix) — **ON GLASS +
  CONFIRMED**: identity `2xd3gy5y6…-gemdemo-0.3.0`, windowed under
  labwc (1025×576): `gemdemo 0.3.0 — Mesa / Mali-T880 (Panfrost) —
  OpenGL ES 3.1 Mesa 25.0.7`; `audio 'gemini16' — 44100 Hz, 1 ch, I16`;
  steady 59-60 fps heartbeat over 12 s (no GL errors, no swap fails);
  user confirmed triangle spins + 440 Hz sine audible.

Docs updated: docs/gemdemo.md (rewritten as the template guide),
README.md row, AGENTS.md row, flake.nix + services/gemini-pda.nix
comments (no longer "demoscene"). Device left: **gen35**, para
unchanged (NORMAL/NixOS), gemwl + lxqt-nested up. Next: nothing owed
for the template itself — future GL work starts from src/main.rs.

## 2026-09-09h — GEMDEMO 0.2.0 DEPLOYED AS GEN33 (old 0.1.0 "AETHER" replaced on the device)

`bash bin/deploy.sh deploy` built + switched gen33 (rc=0, ~42 s — the
only closure delta was gemdemo → 0.2.0). Device now serves
`/run/current-system/sw/bin/gemdemo` →
`/nix/store/xxy3r8rld35a5m97wfv3ly366w52ng2i-gemdemo-0.2.0` (the exact
store path measured at ~60 fps on glass; banner "GEMINI: EXODUS
v0.2.0"). Old AETHER 0.1.0 (`k9njxxs…`) is out of the active profile
(still in the gen32 store — rollback-safe via `deploy.sh rollback`).
Smoke-run of the installed copy under the labwc session: planet maps
bake, frame dump OK, A72s up + pinned.

Rollback: `bash bin/deploy.sh rollback` (one gen) restores 0.1.0 if the
fullscreen QA on glass goes wrong.

## 2026-09-09g — GEMDEMO 0.2.0 ON GLASS (windowed under labwc): 60 fps across the show; S5 planet bake fix measured

Deployed the 0.2.0 build to the PDA (nix copy → /nix/store, run as the
labwc session client) and measured:

- **Audio live**: cpal→gemini16 S16 @ 44.1k opened cleanly; per-cb
  peak logs ~0.92 steady (under the 0.97 ceiling — the old over-limit
  slam is gone; master rms healthy). Beat clock drives the show.
- **A72s**: gemini-a72-up unit requested from inside the demo and the
  render thread pinned to cpu8/cpu9 (confirmed in the banner).
- **FPS**: full show windowed at 1025×576 held 58.9–61 fps through
  S0–S4 and S6 (the old 0.1.0 was single digits).
- **S5 PLANET reveal was the one miss**: the per-pixel fbm planet
  (disc ~44k px at the reveal) dragged fps to ~42. Fixed by baking the
  surface: albedo + cloud maps (256×128) generated ONCE at startup in
  Rust per palette (PAL_EARTH/PAL_PC consts in show.rs, shared with
  the bake), spin = equirect UV scroll, lighting stays per-pixel but
  cheap (no fbm/trig-in-noise in the hot path, spec via repeated
  squaring). Before/after on the same run path: 42 → 60 fps at the
  reveal. No per-frame FBO involved — "bake, don't multipass."
- PPM dumps (window readback RGBA→RGB) land on the device at
  /tmp/s5*.ppm (s5c = S5 reveal ~60 fps build). Region stats confirm
  content (planet mid-bright ~YAVG 134 vs sky ~97–102).
- The 2026-09-09 model couldn't VIEW images — a human should eyeball
  the dumps (title centring, ring depth, palette) before fullscreen.

Files: pkgs/gemdemo/src/gfx.rs (planet bake), src/show.rs (PAL consts),
docs/gemdemo.md (updated status + strategy). Session log entry above
holds the full 0.2.0 rewrite record.

## 2026-09-09f — GEMDEMO 0.2.0 "GEMINI: EXODUS": complete rewrite (assembly-style cinematic spacesynth show) — code green, NOT yet on glass

After the 0.1.0 "AETHER" verdict (2026-09-09: single-digit FPS, flawed
visuals, music broken — master rms ~0.88 from the naive limiter, saw-DC
voices), the whole demo was rewritten in this session:

- **Renderer → direct single-framebuffer layering** (gfx.rs): no post
  chain, no SSAA, no raymarch, no FBOs at scale 1.0. Sky gradient (only
  fullscreen pass) → baked procedural sprite layers (soft/star/ring/
  cloud textures: stars, nebula, halos, particles, shock rings) → warp
  streaks (VS-animated, static VBO) → rotating lit fbm planet + banded
  ring arcs (small-region, low noise budget) → vector ship + engine
  trail → text. Keeps the fork receipts: glGen*, one interleaved
  buffer/VAO, expanded triangles, no instancing/DrawElements.
- **Score engine rewritten** (synth.rs): real mix bus with headroom,
  per-part sends to dotted-8 ping-pong delay + Schroeder reverb,
  sidechain pump, soft-knee ceiling + fast-attack/slow-release limiter;
  chime removed. 126 BPM A-minor, 80 bars / 7 chapters (bar counts
  SHARED with the visual director show.rs). Synth sanity is unit-tested
  (no silence, no clip, no over-limit).
- **Director** (show.rs) turns the beat clock into pure-data
  FrameStates per chapter (S0 EARTH liftoff … S5 PLANET COMPUTERS
  reveal … S6 ORIGIN credits); titles in chapters.
- **A72 cores** (cpu.rs): best-effort — asks gemini-a72-up unit
  (guarded: only on systems where the unit exists), bounded 20 s wait,
  pins the render thread (libc added to Cargo.toml).
- glctx/glutil warnings cleaned; dead modules scenes.rs/post.rs/
  raymarch.rs/sensors.rs deleted; new host check loop
  `bin/gemdemo-host-check.sh` (x86_64 cargo check + unit tests via the
  flake's nixpkgs alsa-lib.dev/pkg-config — seconds).

Verified: host `cargo check` clean; `cargo test --release` 4/4 pass;
aarch64 flake build green (native-aarch64 remote builder, ~80 s,
rc=0). Host kwin/XKB run attempt for frame dumps abandoned (winit
XKBNotFound in the agent's session context) — visual QA is owed ON
GLASS via `--dump` (PPM) + `[`/`]` chapter sweep while watching fps.

Files: pkgs/gemdemo.{nix,src/{main,show,gfx,synth,cpu,audio,font,glctx,
glutil,shaders}.rs}, docs/gemdemo.md (rewritten), Cargo.toml 0.2.0,
bin/gemdemo-host-check.sh.

## 2026-09-09e — BOOT-TIME EXT4 AUTO-REPAIR ON GLASS (defense against hard power-off): initrd e2fsck pre-mount, both paths verified, p22 flashed + sha'd

Follow-up to the boot-panic recovery above. The panic's root cause —
torn ext4 orphan chain that the KERNEL's mount-time journal replay
could not recover (oops every boot, pre-userspace) — is now defended
in the initrd itself:

**Change (commit f45befe):** `devices/planet-geminipda/initrd.nix` runs
`e2fsck -y $ROOT` BEFORE the rw mount, both NixOS (p32) + Debian (p29)
branches; static aarch64 e2fsck (pkgsStatic.e2fsprogs 1.47.4, 1.38 MB)
staged into the cpio. Boot.img 9.37 → 9.52 MiB (< 16 MiB cap), header
geometry + cmdline byte-identical (bootopt intact). Plus
`bin/fsck-p32.sh` — scripted TWRP operator fallback (check/repair/
status; unmounts p32 first — the live-mount spurious-drift lesson).

**Why unconditional -y is free:** e2fsck 1.47.4 `check_if_skip` — on a
HEALTHY fs (VALID set, interval=0, max_mnt_count=-1 as on ours) it
skips in ms (verified locally: 2 ms on a 300 MB test fs). Only journal
RECOVER / ERROR_FS / orphan / VALID-cleared force real work. After a
journal replay e2fsck restarts but the restart re-evaluates and skips
(replay cleared RECOVER) — so a dirty boot costs only the (small)
journal replay, never a full 27 GiB scan (confirmed empirically below).

**On-glass verification (new initrd = boot.img
`yx3fr5qm5…`, sha256 19b299c1effdd950a7699222f9336bb133f009bef7dc554d8e9dc6d2c3aab5bf;
flashed to p22 12:45, backup `stock-dump/boot-20260909-124502.img`):**
1. **Clean boot** — TWRP cycle → NORMAL: boots gen32 in ~34 s
   (unchanged; skip path), no failed units, fs clean, no kernel
   recovery line. p22 read-back sha == image sha.
2. **Dirty boot** — WDT EXRST reboot from the running system (unclean
   shutdown, journal dirty, no TWRP in between): dmesg receipt —
   p32 ro-probe 3.22 s → unmount 3.28 s → **[initrd e2fsck replays
   journal]** → kernel rw mount 3.51 s finds CLEAN journal (NO
   "recovery complete" line — the kernel would have replayed if the
   initrd hadn't) → stage-2 remount 3.87 s. Boot still ~34 s total.
   fs state clean, mount count 3→4.

**Result:** the kernel no longer ever replays a possibly-torn journal
(the incident's oops vector). Clean boots pay ~0; dirty boots (any
hard power-off) self-recover to a clean journal before userspace. The
full torn-orphan repair (the incident's case) is the same -y path
(orphan state forces the full pass — minutes, on an already-broken
boot). TWRP + bin/fsck-p32.sh remain the operator escape hatch.

Note for a future serial/console watcher: the initrd prints
`==> e2fsck -y … (auto-repair; skips fast when clean)` + e2fsck
output on the fbcon console during the ~230 ms window — visible live
but not captured in dmesg/journal (userspace console writes).
Device left: running gen32, para cleared (NORMAL), desktop + wifi up.

## 2026-09-09d — BOOT-PANIC RECOVERY (post-wedge): p32 torn-orphan ext4 from the unclean power-off → repeated pre-mount initrd panics; fixed with offline e2fsck -fy in TWRP, no reflash — device back on gen32, clean cold boot

Follow-up to the wine D3D9 session below (device left WEDGED after the
forced guest-panfrost experiment). User power-cycled; NORMAL boot then
**kernel-panicked every attempt** (fbcon bottom-third only, display mostly
black), device ended up in TWRP. Recovery steps + receipts:

**Read-only diagnosis from TWRP (all clean except the rootfs):**
- p22 `boot` exact-length sha256 = `be6f4d2192d8a95ef762cd17b910fa3af98d3fb98f76a91eef3decd4da4a2e51` — **byte-identical to the verified lean 6.6.0 boot.img** (`boot-img-lean-20260908`); kernel image was never the problem.
- Full 34-partition GPT present; battery 3.98→4.06 V charging (DR rule 3 ok).
- pstore/ramoops empty (`ramoops@44410000`, 896 KiB, IS registered in the lean config/DT — but no crash record survived; the pre-mount panics never reached the backend or left no record).
- p32 (NIXOS_SYSTEM) ext4: **journal dirty + ~50-inode torn orphan linked list (822900-822955) + stale free-block/inode counts** (groups 88/94/96/97/100/106). Pass 2/3/4 (directory structure/connectivity/refcounts) all CLEAN — pure post-crash metadata, no content damage.

**Root-cause chain (receipt):** wedge power-off tore the ext4 orphan
chain; every subsequent NixOS boot died at/before the p32 mount
(journal replay → orphan-list error → mount fail → initrd panic) —
proven by: (a) the journal stayed dirty across all panic attempts
(journald never got to write), (b) `journalctl --list-boots` shows NO
boots between the wedged session (-1) and the recovered boot (0) — the
panics left no journal trace because they were pre-mount.

**Fix (TWRP):** `umount /data /sdcard` → `e2fsck -fy /dev/block/mmcblk0p32`
(orphan list FIXED per inode; counts rebuilt) → verify `e2fsck -fn` rc=0,
all 5 passes clean. ~2 min total. **No reflash, no image change.**

**Result:** para cleared (NORMAL) → single clean cold boot — gen32
`0zjh8q82yb2y3qd4c4iyvk5h19v9xz7i` (current), kernel 7.5 s + userspace
36.7 s, `tune2fs` state=clean mount count 1, `systemctl --failed` empty,
gemwl + lxqt-nested NRestarts=0, renderD128 + card0 up, wifi active,
battery charging 4.14 V. Also serves as the long-owed **cold-boot
confirmation of the dual-boot initrd gen-lookup landing on the newest
generation (gen32): PASS** (first cold boot since gen9 — gens 10-32
were all live deploy.sh switches).

Notes/learned: (1) new receipt — unclean power-off of a wedged box can
tear the ext4 orphan chain so the NEXT boot's journal replay/mount
fails repeatedly with no journal trace; TWRP + offline `e2fsck -fy` is
the fix; the fs was otherwise structurally sound (nix store + profiles
untouched). (2) If a boot-time panic recurs, check pstore from the next
successful boot (`/sys/fs/pstore/dmesg-ramoops-*`) — the region exists
but was empty this time (observation, unverified why).

Device left: **AWAKE on gen32, para cleared (NORMAL/NixOS default),
desktop + wifi up, battery charging.** Pending from the wine session:
re-run `bin/wine-x86-deploy.sh run d3d9test.exe` on the llvmpipe path +
grim screenshot receipt; guest-panfrost stays BANNED without
para=boot-recovery + a clean prefix (per the incident LEARNED note).


## 2026-09-09c — Windows D3D9 on the PDA: wine64+box64 deployed, first D3D9 frame rendered; guest-panfrost run wedged the device (power cycle owed)

Task: "add wine + an x86→arm translator and test a simple Windows D3D9 app".

**Stack (all hydra-cached at the flake nixpkgs rev dc5d91f84032 — verified with `nix path-info --store https://cache.nixos.org`, rule 9):**
- box64 0.4.4 (aarch64 x86-64 translator; 5 paths/81 MB)
- wine64 11.0 (x86_64-linux; 342 paths/1.8 GB) — the closure carries glvnd (libEGL dispatch) but NO GL impl
- mesa 26.2.2 (x86_64-linux; 273 MB) — the GL impl; eglPlatforms x11+wayland; wired in via `__EGL_VENDOR_LIBRARY_FILENAMES` (the env var glvnd 1.7.0 actually implements — verified by grepping the lib; `__EGL_VENDOR_LIBRARY_FILE` does NOT exist)
- d3d9test 1.0 — self-written x86_64-windows PE (pkgs/d3d9test/): rotating vertex-coloured cube + GDI FPS overlay; fixed FVF 0x0042 (D3DFVF_XYZ|D3DFVF_DIFFUSE, mingw-w64 ground truth)
- grim 1.5.0 (screenshots)

**Why box64+wine64, not FEX+i686-wine:** fex-emu is not in this nixpkgs; `pkgsCross` has no i686-linux (i686 *is* a valid import system but its wine NAR is 404 on cache.nixos.org → source compile); wine64+box64 is the only all-cached path. box86 NAR also missing at this pin (not shipped — the test PE is 64-bit).

**Build traps found (all in docs/wine-d3d.md §2/§3):**
- winegcc inside the nixpkgs wine64 package is configured NATIVE (`-dumpmachine` → x86_64-unknown-linux-gnu) — emits an ELF + sh wrapper, not a PE. The PE is built with `pkgsCross.mingwW64` (x86_64-w64-mingw32-g++ 15.3.0 + mingw-w64 14.0.0 headers, needs `allowUnsupportedSystem = true`).
- Wine's headers are partial classic-era D3D9 (no flexible-FVF macros, no DEFAULT_SWISS, D3DCAPS9 in d3d9caps.h, D3DMATRIX = anonymous union, C++ mode for COM classes).
- **gemwl had to gain viewporter**: winewayland refuses to init without wp_viewporter. 2 lines in pkgs/gemwl/gemwl.c (wlr_viewporter_create; wlroots 0.18 handles viewport resources internally — no per-window plumbing). Shipped as **gen32** (deploy.sh; system-32-… on device at session end).

**Deployment:** bin/wine-x86-deploy.sh (status/deploy/init/run/log/shot/kill) — `nix copy` of the 5 paths to the device store + /root/wine-x86 launcher + GC roots (host+device). Not a flake systemPackage (1.8 GB would bloat every deploy delta).

**On-glass results:**
- `wine cmd /c echo` → WINE-HELLO-WORLD (stack proven).
- d3d9test: **FIRST FRAME PRESENTED — D3D9 rendering under wine64+box64**, steady **55–60 FPS** on the llvmpipe path (guest mesa swrast — stable for 10+ min).
- Receipts in device:/root/wine-x86/logs/app.log + the WINEDEBUG=+d3d9 log (7346+ DrawIndexedPrimitive calls).

**BUG 1 (wine, deferred):** GetAdapterDisplayMode/GetDeviceCaps page-fault (garbage-pointer read at 0x9000E000D030F) under box64 on wine 11's wayland driver — gemwl has no xdg_output so wine's screen struct is partly uninitialised. Worked around in d3d9test (identity query gated behind D3D9TEST_IDENTITY, off by default). Adapter string is wine's fallback "NVIDIA GeForce 6800" (GL_RENDERER unavailable via D3D9).

**INCIDENT (unresolved, power cycle owed):** to find out whether the T880 could render the guest, I forced `GALLIUM_DRIVER=panfrost MESA_LOADER_DEBUG=all` on the app. Within ~20 s the **device wedged: kernel alive (ping 0.25 ms, g_ether up, ARP normal) but userspace dead** (sshd banner timeout, TCP:22 connect timeout, flat net counters). Controlled pan_js (kernel panfrost job thread) CPU-time A/B with the llvmpipe app showed +1 jiffie over 27 s — i.e. **the stable path was llvmpipe (CPU), not the T880**; the forced-panfrost run is the suspect (guest x86_64 panfrost 26.2.2 + box64 ioctl path + T880 already held by native gemwl). No software reset available (WDT-EXRST needs an ssh shell; preloader needs buttons). **LEARNED: never force guest panfrost on this stack until root-caused; llvmpipe is the safe renderer; guest-panfrost is a separate, risky experiment (fresh prefix, no other GPU users, WDT safety net via para=boot-recovery first).**

Device left: **WEDGED (userspace dead) — needs physical power cycle.** After power-on: `bash bin/device-ssh.sh 'echo ok'` (auto net-up), verify gen32 + desktop, re-run `bin/wine-x86-deploy.sh run d3d9test.exe` (llvmpipe path), take the grim screenshot receipt, then leave para as-is (normal/NixOS default; system is verified) — or para=boot-recovery if more risky work is planned.

Files: pkgs/wine-x86.nix, pkgs/d3d9test.nix, pkgs/d3d9test/d3d9test.cpp, bin/wine-x86-deploy.sh, docs/wine-d3d.md, pkgs/gemwl/gemwl.c (viewporter).

## 2026-09-09b (gemdemo audio FIXED-ENOUGH, MUSIC ON GLASS — "awful mix" = the follow-up) — S32 noise trap + engine DC bugs, all committed

Audible music now plays on the device (S16 @ 44.1 k via gemini16). Three
separate root causes were stacked; the last two made the music NOTHING
(audibly) and were found with a host-side render harness
(`/tmp/harness`: `rustc -O harness.rs` beside synth.rs — synth.rs is
pure math, no device needed):

1. **S32-on-wire noise trap** (fix: `hint { show on }` on gemini16 in
   services/pipewire/asound.conf; live via /root/.asoundrc stopgap — /etc
   is read-only): cpal 0.15 enumerates ALSA devices by name hints, so the
   custom plug was invisible and the app fell back to `default`, whose
   F32-in → S32-on-wire plays as noise on the 16-bit-only MT6351.
   (aplay -v A/B: FLOAT→S32 = noise, S16 = clean.)
2. **cpal stream → S16** (audio.rs): after 1, also force an I16 @ 44.1 k
   config when offered (was defaulting to F32; the I16 branch had bugs —
   mono-collapse, wrong frame math — now fixed interleaved-stereo).
3. **saw generator DC bug** (synth.rs, THE silence): `2.0*(p-p.fract())-1.0`
   with p∈[0,1) is ALWAYS -1 (p.fract()==p) — every saw voice (pad, arp/
   pluck, bass osc) output constant -1 DC → tanh/limiter → flat +0.9 DC
   (inaudible). Saw = `2.0*p-1.0` in all three sites.
4. **step decode bug** (synth.rs): `s = bar % STEPS_PER_BAR` froze each
   bar to its first step's arrangement (pad re-triggered every bar, arp
   patterns scrambled). Now `s = step % STEPS_PER_BAR`; `next_step` init
   0 so step 0 plays at t=0.

Harness evidence (8 s RMS/zc per 0.5 s): before fixes — rms 0.27 → 0.90
flat, zc→0 (DC clamp) by t=1 s. After — rms 0.27→0.88 climbing as the
arrangement builds, zc 1.4 k→24 k Hz (real drums/hats/noise). Startup
chime (0.6 s, 880→660→440 Hz) added as an audibility aid — still in
(remove when polishing).

USER-HEARD: 3-note chime then... silence (pre-fix). After the fixes:
"I hear sound, but it's awful" — the mix is over-limited (master rms
~0.88 sustained; master_gain/limiter tuning + the chime removal are the
"fix later"). NOT yet deployed to the running system (device gen31 still
ships the silent build; deploy gen32 when the mix is fixed).

Device state: /root/.asoundrc stopgap present (re-add after root home
wipes); no stray gemdemo processes.

## 2026-09-09a (gemdemo audio: "no music, buzz at open + snap at close" ROOT-CAUSED + FIXED) — S32-on-wire noise trap; gemini16 now discoverable

User report (running gemdemo in the desktop session): no music — only a
brief buzz at start and a snap at exit. Root cause found + fixed at the
alsaLib config level, NOT in the synth (the engine render math is
sound; the stream ran the whole time with no errors):

- **Diagnosis**: `audio.rs` opens `gemini16` (the S16_LE-pinning plug,
  services/pipewire/asound.conf) in front of the 16-bit-only MT6351
  AFE — but cpal 0.15 enumerates ALSA devices via name HINTS, and a
  custom plug is invisible unless it carries `hint { show on }`. Every
  run printed device 'default' → fromenv → sysdefault → hw:0.
- **Live-verified with aplay -v** (2026-09-09): S16_LE in via `default`
  → S16 on the wire (clean). **FLOAT_LE in (the demo's F32 stream) →
  alsa's plug converts to S32_LE on the wire** — and the driver never
  programs the data-width register (mt6797-dai-adda.c), so S32 plays
  as square-wave/white noise (the pre-existing S16/S24/S32 receipt in
  asound.conf's header). The "buzz" was the S32 noise; the "snap" the
  stream open/close against a wrong-width codec state.
- **Fix** (repo, in `services/pipewire/asound.conf`): add `hint { show
  on; description "Gemini PDA MT6351 S16 output" }` to `pcm.gemini16`
  so alsa-lib advertises it; `aplay -L` now lists gemini16 and the demo
  opens device 'gemini16' — F32-in is converted to S16 by the plug's
  pinned slave (the same conversion pipewire's WirePlumber path does),
  clean by construction.
- **Live on the device now**: /etc is NixOS read-only, so the fix was
  applied via `/root/.asoundrc` (root-run sessions read it) — the demo
  prints `audio device 'gemini16' — 44100 Hz, 2 ch, F32`. The next
  system switch carries it in /etc/asound.conf (config change staged in
  services/pipewire/asound.conf). NOTE: `/root/.asoundrc` is a stopgap;
  re-add it if a rebuild wipes root's home.
- audio.rs header + gemdemo.md updated with the receipt. Also confirms
  the demo's audio path = F32 engine → alsa plug → S16 codec is the
  DESIGNED one (pipewire does the same); no engine change needed.

Next: user ears-check (desktop terminal: `gemdemo --windowed --ssaa 1`),
 then switch the system to carry the asound.conf hint in /etc.

## 2026-09-08 (gemdemo on glass — gen31 `cb3b5iv1q6pq8g` builds, NOT deployed) — "AETHER" demoscene/GPU stress test BUILT + RUNNING on the device; every blocker between "green build" and "on glass" closed

`gemdemo` (docs/gemdemo.md) now runs on the live LXQt/labwc session and
directly on the gemwl compositor. Verified via the new `--dump` stage
readback: real scene content through the whole chain (scene FBO → bloom/
feedback post → window), zero panfrost kernel faults, no crash over
60 s+, audio up (pulse/PipeWire, "44100 Hz 2ch F32"). A full receipt +
the complete bug list is in docs/gemdemo.md; highlights:

- Derivation `pkgs/gemdemo.nix` now pins wayland + libxkbcommon on the
  RUNPATH (winit 0.29 dlopens them — the generic rpath fixup only pulled
  the -dev outputs → `NoWaylandLib`).
- glctx: wrap the wl_surface in `wl_egl_window_create` (EGL needs the
  size; bare surface → EGL_BAD_NATIVE_WINDOW); dropped the bogus
  EGL_RENDER_BUFFER/EGL_RGB_BUFFER config key/value pair (BAD_ATTRIBUTE).
- glutil: ALL object creation switched from the 4.5-core DSA `glCreate*`
  to `glGen*` — the DSA entry points are silent "unsupported function"
  stubs on the GLES 3.1 context (ghost objects; mesa: `glBufferData(no
  buffer bound)`).
- Scenes: NO instancing and NO multi-buffer VAOs — the fork's panfrost
  u_vbuf segfaults on instancing and multi-buffer VAOs storm the GPU
  (`panfrost: js fault JOB_BUS_FAULT` + sched timeouts). All draws are
  single interleaved buffers (wlroots-proven pattern). NO DrawElements
  (index-minmax scan crashes). Tunnel verts expanded.
- font.rs: atlas 16×4 → 16×6 (table outgrew it — OOB bake panic) and
  the Writer no longer stores a `*const Font` into a SceneSet that gets
  MOVED (dangling pointer SIGSEGV).
- Shader fixes: `vec5` → pos+uv split, `pal()` duplicated into the
  vortex VS, composite `u_bloom` sampler/float redeclaration renamed
  `u_bloomamt`, dead `u_res` dropped (link-time optimized out →
  uniform() panic).
- Post FBO 1×1 bug: `Post::new()` allocates 1×1 and the init
  `state.resize()` no-ops when sizes already match — fullscreen windows
  that never send a Resized event left the whole post chain at 1×1
  (resolve magnified one texel → flat colour). Force `post.resize`
  after init.
- parse_args rewrite (had never run with flags): flag-only args spun at
  100% CPU, value flags were left at the value position → both fixed.
- Dump tooling: `--dump <frame#> <path>` stage PPMs + `--solid R,G,B`
  target self-test. Readback had to be GL_RGBA (GLES3 rejects
  GL_RGB/UNSIGNED_BYTE for RGBA8 fbos — silent error, zero-filled
  buffers, phantom "everything black"; scene was fine all along).
- Perf truth: fullscreen ssaa2 (4320×2160 scene) ~1–2 fps on the T880;
  ssaa1 ~4 fps; windowed ssaa1 ~15 fps. Stress protocol follows.

To run on the device now (store path in the log tail below):
`XDG_RUNTIME_DIR=/run/gemwl WAYLAND_DISPLAY=wayland-1 gemdemo
--windowed --ssaa 1 --silent` from the LXQt session (add `--section N`
0..6 to jump in). `services/gemini-pda.nix` systemPackages now carries
`gemdemo` — gen31 `cb3b5iv1q6pq8g` (toplevel) built 2026-09-08 but NOT
switched (session was read-only on the live system); deploy when wanted
(`bash bin/deploy.sh build && bash bin/deploy.sh deploy`).

## 2026-09-08 (browsers GL fix, gen28 `1nkzm5nih…`) — REAL CHROME/FIREFOX GL PATH: fork mesa gained the wayland EGL platform; Firefox no-WebGL + Chrome-won't-start root-caused and fixed at build level; wlegltst proves ES 3.1 / Mali-T880 / Panfrost through the nested stack

Follow-up to the browsers-install entry below. User glass test:
"no WebGL in Firefox (not even software); this DID work in Debian.
Chrome won't even start."

Root causes (each verified on device):
1. **Fork mesa had NO wayland EGL platform** (`-Dplatforms=` empty,
surfaceless-only via -Degl-native-platform). Browser GL needs
EGL_PLATFORM_WAYLAND against the nested compositor — with none,
Firefox could not create ANY GL context ("not even software": no
wayland platform AND no swrast/llvmpipe in the fork = nothing to fall
back to). Debian's fork had the wayland platform (es2gears_wayland +
hardware WebRender worked there — legacy GeminiPDA session-log
2026-09-04); the NixOS port had dropped it. `wlegltst.c` (wayland-EGL
smoke client in pkgs/gemwl/) existed but could never pass.
2. **GBM_BACKENDS_PATH not exported**: the fork libgbm has NO baked
backend path (strings-verified) and honors only that env var
(Debian start-lxqt-nested.sh exported it) — browser glxtest/GPU probe
needs dri_gbm.so (which the fork DOES ship at lib/gbm).
3. **Chrome refused to start**: "Running as root without --no-sandbox
is not supported" (zygote_host_impl_linux.cc:102) — the desktop is a
root systemd session; userns/SUID sandbox can't drop root. Fixed with
--no-sandbox in the wrapper (trusted single-user PDA; comment in
config/gemini.nix).

Changes (all in-tree, this session):
- pkgs/mesa-geminipda.nix: `-Dplatforms=wayland` (surfaceless
  preserved via -Degl-native-platform=surfaceless) + wayland build deps.
  Build-discovery gotcha worth recording: wayland's + wayland-scanner's
  .pc live ONLY in their -dev outputs and wayland-protocols' in
  share/pkgconfig, but the nixpkgs pkg-config wrapper role vars don't
  surface all of them to mesa 25's build-time dependency() lookups
  (meson.build:2054 wayland-scanner, :2061 wayland-protocols) — seeded
  env.PKG_CONFIG_PATH + PKG_CONFIG_PATH_FOR_BUILD with the three dirs.
- services/lxqt.nix: GBM_BACKENDS_PATH=${mesaGeminipda}/lib/gbm in the
  session env (port of the Debian env var; mesaGeminipda already in
  scope there).
- config/gemini.nix: google-chrome overridden with commandLineArgs =
  "--no-sandbox" (+ rationale comment).

Version lines (rule 0): mesa-geminipda 25.0.7 now
`mfqyzn3rz6i2w5hliz5w8jr5vylk8h3m` (wayland platform; libEGL_mesa
DT_NEEDED libwayland-client verified on device); relinked
wlroots/labwc/gemwl against it; toplevel gen28
`1nkzm5nih5r39q7wzjf2qv07rbr2r9mi` deployed 2026-09-08 (~78 s
delta — mesa was pre-built, only the relinked drvs + toplevel
shipped). gemwl + lxqt-nested active after activate (no mesa-rebuild
regression). Kernel #329 + boot.img unchanged.

Verification on glass (probe, before user eyes-on): fork's own
wlegltst against the LIVE session prints: wl_drm present (v2),
linux_dmabuf present (v4), eglGetPlatformDisplay(wayland): ok,
EGL 1.5, **GL: OpenGL ES 3.1 Mesa 25.0.7 | Mali-T880 (Panfrost)**, 150+
swaps — i.e. the full client-GL chain (fork EGL wayland -> nested
labwc wl_drm/dmabuf -> panfrost) works end to end. Firefox needs
exactly this chain. Firefox also mapped a window under the new env
(labwc journal: identifier=firefox).

**USER-VERIFIED ON GLASS 2026-09-08: "it works great"** — Firefox
(WebGL) and Chrome both running from the LXQt desktop after the gen28
deploy; entry closed. (Chrome's chrome://gpu mode — ANGLE-on-panfrost
vs SwiftShader — not reported; launcher tuning can follow if a future
session wants it.)


Asked "can we get real Google Chrome on the device" — research + install
session. Findings (web-verified 2026-09-08): Google's official Linux arm64
stable deb exists (dl.google.com …/google-chrome-stable_current_arm64.deb,
133 MB — download page doesn't link it yet, but the URL + apt repo are
live; Widevine + Google sync included, per omgubuntu 2026-07). nixpkgs
removed the old google-chrome path but it lives on at
`pkgs/by-name/go/google-chrome` with **aarch64-linux in platforms** and the
arm64 deb hash at the repo's pinned rev `dc5d91f84032` (v152.0.7977.82).
Firefox 155.0.1 also aarch64-cached at the pin.

GL reasoning (the interesting part): the client GL wiring ALREADY existed
— config/gemini.nix installs the fork's glvnd ICD manifest at
/etc/glvnd/egl_vendor.d/50_mesa.json, and nixpkgs' firefox wrapper ships
libglvnd on LD_LIBRARY_PATH (`withGlvnd` defaults on for Linux), so
Firefox's dlopen of libEGL.so.1 dispatches to mesa-geminipda → panfrost
renderD128 — the same chain Debian's Firefox used for WebGL. The repo was
just missing its first third-party GL client. Chrome's nixpkgs wrapper
only adds ozone/wayland auto-flags when `NIXOS_OZONE_WL` is set (added to
the lxqt-nested env); its default ANGLE path will try Vulkan (absent on
Midgard) then GL/SwiftShader — empirical on glass.

Changes (commit: this session): `config/gemini.nix` systemPackages += [
`pkgs.google-chrome` `pkgs.firefox` ] (comment carries the rationale);
`services/lxqt.nix` unit path += both (bare-name launch in the session
terminal) + `NIXOS_OZONE_WL=1` env; launchers
`config/lxqt/Desktop/{google-chrome,firefox}.desktop` (seed dir — fresh
installs get them via sessionConfig; live device copied now).

Version lines (rule 0): google-chrome 152.0.7977.82
`p20940mi4ir8fk54gi341q72jfajcn2p` (built locally on the Pi — unfree =
never on cache.nixos.org, but the drv is only unpack+patchelf, minutes),
firefox 155.0.1 `d2p0bvy7ap8jqbgsr7drxzyjjh9ckai6` (cache-substituted),
toplevel gen27 `r3hj9x7bcba35qgf8rbhyvw1imd9a7yv`; kernel #329 + boot.img
unchanged. Deploy via `bin/deploy.sh deploy` under run-job: 157 s total
(build → nix copy delta → profile switch + activate); post-activation
gemwl + lxqt-nested active, launcher icons on /root/Desktop.

Next: eyes-on-glass — launch both from the LXQt desktop/menu, read
`chrome://gpu` (panfrost vs SwiftShader?) and open a WebGL page in Firefox
(expectation: works, Debian parity). Log the result here with a [verified
2026-09-08/09] note; if Chrome lands on SwiftShader, try `--use-angle=gl`
(routes ANGLE through the fork EGL).
 (v2, gens 25–26): instant backlight-first sleep/wake (~1-2 s each way), wifi chip teardown removed from the button path (~29 s stall → iface down + daemon kill), press debounce + queue drain in the sleepd daemon — full cycle re-verified on glass incl. wifi re-association

User report: the silver button was "very sluggish" — press once =
nothing, press a few more times = the backlight flickered on/off at
~1 s intervals. Root causes found in the sleepd journal (two bugs):

1. **~29 s stall in the sleep path when wifi was up**: the sleepd
   journal showed `stopping … gemini-wifi-auto` at :44 then ASLEEP
   only at :13 — the gap is `wifi-internal stop` = `echo off >
   /sys/kernel/debug/wcn/pwr` blocking ~29 s in the kernel when the
   chip is fully associated (the WMT whole-chip teardown). The
   backlight (the only visible effect) came LAST in the v1 sequence,
   so the press appeared dead for ~30 s → the user pressed again…
2. **Queued presses cascaded**: each press toggled only after the
   previous ~1-30 s toggle finished, so a flurry of presses produced
   rapid sleep/wake/sleep at ~1 s cadence (the flicker) — and the
   rapid stop/start cycling tripped gemwl's start rate limit
   (`start-limit-hit` → failed, needed reset-failed).

Also learned: the gemini-wifi-* units are RemainAfterExit oneshots
with no ExecStop — `systemctl stop` on them does NOT kill the
wpa_supplicant they spawned (wifi survives a unit stop; only the
`echo off` chip teardown actually stopped it, which is why wlan0
disappeared in the v1 tests).

**v2 fix (pkgs/gemcli/src/sleep.rs, gens 25-26):**

- `sleep on` reordered: backlight off FIRST (instant visible
  acknowledgement), then inputs unbind, cpus 1-7 offline, services
  stop, wifi fast-down (`ip link set wlan0 down` + pkill wpa_supplicant
  + the iface's dhcpcd — the CONSYS chip STAYS powered; the ~29 s
  `echo off` teardown is gone from the button path). Total ~1-2 s
  worst case, backlight in the first ~50 ms.
- `sleep off` reordered the same way (backlight on first); wake
  re-associates by RESTARTING gemini-wifi-auto.service (a plain
  `start` was a no-op — the RemainAfterExit unit stayed "active"
  through sleep). `systemctl start` in the wake path now reset-failed
  first (gemwl start-limit recovery).
- `sleep key` (the daemon): 1 s press debounce (one physical press =
  one toggle even if the polled driver double-reports) + drain of any
  events queued while a toggle ran — mashing can no longer cascade.

On-glass re-verification (gen26): `time gemcli sleep on` = 2.1 s
(backlight off in the first ms; wifi down, wpa dead while asleep),
`time gemcli sleep off` = 1.1 s; wifi re-associated + DHCP
(192.168.49.166) after wake; desktop/audio services all back; asleep
soak ichgr 150 mA (best yet — v1's wifi unit-stop left the radio
alive). Version lines: gemcli 0.1.0; toplevel
`m96fkx88…-nixos-system-gemini` (gc-pinned toplevel-20260909-sleepd).
Kernel untouched. Device left: AWAKE, gen26, desktop + wifi up,
backlight 10 %.

Next: user physical press test on the new daemon (the fix is verified
via ssh toggles; the debounce/drain live path needs a real finger).

## 2026-09-08 (7th) — POWER-SAVING INVESTIGATION + SILVER-BUTTON SLEEP/WAKE ON GLASS (gens 22–24): `gemcli sleep on|off|status|key` light clamshell sleep (backlight off, A53 cpus 1-7 offline, keyboard+touch unbound, services stopped) + `gemini-sleepd.service` KEY_SLEEP daemon — two full sleep/wake round-trips verified; deep-sleep (s2idle) documented as kernel follow-up

Asked-for: investigate power savings (backlight, cores incl. the A53s,
anything else) so the device draws as little battery current as
possible, kill keyboard input while the closed clamshell presses the
keys, and wire the silver side button (mt6351-keys KEY_SLEEP) as
sleep/wake. Outcome: an awake LIGHT sleep is implemented, verified on
glass and now owned by the silver button; TRUE deep sleep needs a
suspend wake source (PMIC/pwrap INT kernel work — no flash happened,
no kernel change). Version lines (rule 0): gemcli 0.1.0 (same crate
version; new subcommands). Gens 22→23→24
`girqg7wp8` → `ip3432lw` → `zbyzpwc` (current). Kernel unchanged 6.6.0
lean; boot partition untouched (pure package/system deploys via
bin/deploy.sh). Device left: AWAKE, gen24, desktop+wifi+audio up,
backlight 10 %, battery fast-charging ~4.08 V, para clear.

**Investigation receipts (docs/power-sleep.md).** The unit still has NO
suspend/resume path: `/sys/power/state` = freeze/mem(s2idle) exists and
CONFIG_SUSPEND=y, but nothing can WAKE s2idle — the side keys are
PMIC-debounced bits in TOPSTATUS 0x220 POLLED by mt6351-keys over
pwrap (no IRQ route in mainline; vendor 3.18 wakes via the PMIC INT →
pwrap EINT status), and the kernel boots clk_ignore_unused /
pd_ignore_unused / regulator_ignore_unused. Power ladder on glass
(USB 500 mA input, ICHGR charge-current proxy, 50 mA ADC steps):
backlight 100 % → off recovers ≥150 mA@5 V (at 100 % the battery
discharges even at full input — vbat 4084→3984); desktop/gemwl idle,
pipewire, CONSYS wifi, and A53 cpus 1-7 each measure ≤50 mA (at/below
ADC resolution). Awake floor with everything off ≈ 400 mA@4 V ≈ 1.6 W
(LCD TDDI panel logic stays on — fbcon kernel can't blank the panel,
rule 5; no cpufreq driver for MT6797; no A53-cluster power-down path).

**Implementation (docs/gemcli.md §sleep + pkgs/gemcli/src/sleep.rs).**
`gemcli sleep on`: stop the heavyweight services that were running
(gemwl/lxqt-nested, pipewire/wireplumber/pipewire-pulse,
gemini-wifi-internal/auto), power the CONSYS chip down via
`wifi-internal stop` (the oneshot units have no ExecStop), offline A53
cpus 1-7 (cpu0 stays), backlight off (bl_power=4, brightness kept),
unbind the clamshell input drivers (matrix-keypad platform `keyboard`
+ novatek-nt36xxx i2c `4-0062`) so the closed lid's key presses make
no input, and record everything in /run/gemcli-sleep.state. `off`
reverses (rebind → backlight → cores → services async). `key` scans
/sys/class/input for the mt6351-keys evdev node (not a hardcoded
eventN), watches for KEY_SLEEP value==1 and toggles — it backs the new
enabled `gemini-sleepd.service` (Restart=always; sshd + battery-guard +
sleepd itself are never stopped). SIGPIPE reset to SIG_DFL in main()
so `gemcli … | head` dies quietly instead of panicking (Broken pipe,
seen during testing).

**On-glass verification.** Full round-trip ×2 via ssh (no button
needed): sleep → cpu online=0, bl_power=4, kbd/touch driver dirs
empty, all 7 units inactive, wlan0 gone (CONSYS powered down), state
file correct, rc=0. Wake → cpus 0-7, bl_power=0, inputs rebound,
services active, wlan0 re-associated (auto still activating a few
seconds), state cleared. **Bug found + fixed during testing:** the
wifi chip power-down was skipped because `systemctl stop --no-block`
raced the `is-active` check (wlan0 stayed up through sleep) — capture
active-ness BEFORE stopping. The "wifi-internal: pwr-off failed
(modules stay loaded)" verdict during sleep is the known whole-chip-
reset no-op (legacy receipt); wlan0 disappearing confirms the teardown.

**Not done this session (next steps):** (1) the physical silver-button
press test is the user's (daemon verified watching event3; toggle logic
verified via ssh commands — a real press is the last check); (2) deep
sleep = kernel follow-up: wire the PMIC HOMEKEY/PWRKEY INT → pwrap
INT_EN → wake-capable IRQ so mt6351-keys can wake s2idle, then probe
s2idle entry (WDT-escaped) and drop the clk/pd/regulator_ignore_unused
flags for the suspend path — recipes in docs/power-sleep.md §Deep
sleep. Host gc-pin: toplevel-20260909-sleepd.

## 2026-09-08 (6th) — GEMCLI ON GLASS (gens 16–21): deployed via deploy.sh, selfcheck ALL PASS, battery/backlight/charger parity byte-identical, a72 up/down round-trip verified — gpio v1 ioctl bug found (v6.6 renumbering) + host disk-full incident

The (5th) entry's next step: get gemcli onto the device. Version
lines (rule 0): gemcli 0.1.0 everywhere; gens 16–21 in order
`ac5hn0p3` → `78lq8ki` → `yqzfd51` → `838j92y` → `ias4zzi` →
`243v8bs` (current). Kernel unchanged (6.6.0 lean, boot partition
touched by nothing — config/package deploys only). Device left:
gen21 current, para clear (NixOS p32 default), A72 cores back offline
(0-7), backlight 10 %, battery fast-charging ~4.06 V.

**Host disk-full incident** (first deploy failed): root fs `/` (which
holds /nix) was 100 % — the gc-pin symlink after the toplevel build
failed ENOSPC. Freed ~7 G of leftover kernel A/B scratch in /tmp
(kfull/kclean/kbase/kernsrc/kobj + kernel dumps from the
borrow-retirement session; nothing referenced them) and re-ran. Host
/nix is 212 G/246 G — a `nix-collect-garbage` is owed soon (the
pinned-deploy gcroots make it safe; deferred to keep this session
focused).

**Deploy mechanism** worked as designed the rest of the way:
run-job + `bash bin/deploy.sh deploy` (~50 s each: build cached,
delta nix copy, profile switch + activate — no flash, no reboot
needed for the new systemPackages entry to land).

**On-glass results**: `gemcli selfcheck` = ALL PASS (8/8: devmem SPM/
WDT reads, bq25890 psy, raw BQ25896 i2c read, backlight 9 %, cpu map
0-7, para present, gpio pads 243/244, gpu regs). Byte-identical
parity: battstat vs `gemcli battery status`, bq25896-raw.sh vs
`gemcli charger raw`, `backlight get`; write round-trip
`set 20` → 20 everywhere → restored 10. `gemcli a72 up both`: cpu8
cold on attempt 1 (DA9214 bus i2c-2, SPM pre-seq, sramldo, WDT-armed
PSCI) + cpu9 warm → 0-9; `a72 down both`: cpu9 per-core, cpu8
last-A72 secure teardown (ISO bit1 re-asserted, PWR_CON bit0 clear),
DA9214 BUCKB rail dropped → 0-7, cold-boot state. rc 0 both ways.

**THE bug**: gpio probes failed EINVAL while the C gpioout succeeded
on the same pads. Bisected with an aarch64 strace (shipped via the
repo's `nix copy --to ssh://10.15.19.82` mechanism from the pinned
rev): strace decoded the C call as GPIO_GET_LINEHANDLE_IOCTL but left
the Rust one raw — and the v6.6 UAPI header shows the v1 ioctl
numbers were REORGANISED after the pre-5.x kernels:
`GPIO_GET_LINEHANDLE_IOCTL` is nr 0x03 (nr 0x02 is now
GPIO_GET_LINEINFO_IOCTL). My nr-0x02 request hit the lineinfo ioctl
with a linehandle struct → EINVAL. Fixed in pkgs/gemcli/src/gpio.rs
(nr 0x03) + a regression test pinning the exact _IOC literals
(8 tests green). Also: chip resolution now scans
GPIO_GET_CHIPINFO_IOCTL ngpio per chip (this kernel: gpiochip0
pinctrl_paris 262 lines + gpiochip1 aw9523b 16) instead of assuming
chip0. Cosmetic: cl2-up warm log now says OK/FAILED (the "rc=1" form
was a success-boolean that read backwards).

Units still ExecStart the scripts — the flips (backlight-default →
wdt/boot/a72 hand-runs → gpu-poweron → battery-guard LAST) are the
remaining step per docs/gemcli.md; nothing in this session flipped
one. strace 7.2 left in the device store (debug tool, harmless).

Next: host nix-collect-garbage (gcroots are in place), then the
lowest-risk unit flip (gemini-backlight-default → `gemcli backlight
set 10`) with a WDT-reboot A/B.

## 2026-09-08 (5th) — GEMCLI LANDED (Rust device-control CLI): clap-based tool with script-parity ports of backlight/battery/charger/power/guard/a72/wdt/boot/gpu/speaker; native aarch64 build rc=0 — nothing flashed, no unit flipped

Asked-for consolidation: one native Rust binary ON the device for the
functions the bring-up shell scripts handle (services/scripts/*), built
in-repo as `pkgs/gemcli.nix` + `pkgs/gemcli/` (the pkgs/* vendored-source
pattern, like gemwl/speaker-amp). Deps are clap 4.5 (derive — the asked-
for "nice CLI parser crate") + libc 0.2 only; access model = /dev/mem mmap
(devmem-equivalent), i2c-dev ioctls incl. DT-base adapter resolution +
`-f` force semantics, gpio chardev v1 for the speaker pads, sysfs for psy/
backlight/cpu. Docs: docs/gemcli.md (map, exit codes, corrections,
parity + flip recipe).

**Scope**: `backlight` (sysfs-first, DISP_PWM0 devmem fallback + clock
gates), `battery status` (battstat exit codes 0/2/3/4/5), `charger raw`
(BQ25896 conversion-trigger + register decode), `power`
(status/watch/charge/dim-to-charge), `guard run` (the safety daemon —
same env knobs, same /run/battery-guard/state + history CSV),
`a72 up/down` (cl2-up/down: DA9214 BUCKB via i2c, SPM pre-sequence,
sramldo SMC, WDT-armed PSCI hotplug/teardown), `gpu poweron/status`
(full MTCMOS vendor sequence), `wdt-reboot`, `boot` (para marker:
recovery/debian/nixos + --no-reboot), `speaker`, `status` aggregate,
`selfcheck` (read-only on-glass parity harness), `version` (rule-0
banner). wifi/wifi-internal + audio-output/defaults deliberately NOT
ported yet (daemon/card orchestrators, not register control) — phase 2.

**Corrections over the scripts (all `[corrected 2026-09-08]`, noted in
module headers + docs/gemcli.md)**: (1) cl2-up.sh always exited 0
(trailing `log` echo rc) even after GAVE UP — gemcli returns the real
outcome; (2) gemini-boot-recovery wrote a SHORT 15-byte para record (no
conv=sync) — gemcli always writes the full padded 32-byte command like
gemini-boot-debian; (3) battery-guard's bash rotation wrote a 7-column
header over 8-field rows — gemcli always writes the 8-column header.

**Build receipt** (build-level only, nothing on the device): run-job
`gemcli-build`, rc=0 in 86 s — rustc/cargo 1.97.1 + llvm substituted
from cache.nixos.org at the pinned rev dc5d91f84032, compiled on the
192.168.49.191 aarch64 builder, out
`ldl1j87ks6s09qzzvbhvhkzqrb00fv53-gemcli-0.1.0` (bin 1.34 MB,
stripped, opt-level s). 6 hardware-free unit tests (civil-time,
cpu-range parse, para decode, charger ichgr parse, CON1 duty->pct) ran
green in-sandbox via buildRustPackage's default checkPhase. Local
`cargo check`/`test` clean on the host too.

**Wiring**: flake package `.#packages.aarch64-linux.gemcli` + eval
verified; services/gemini-pda.nix adds gemcli to
environment.systemPackages NEXT TO the scripts (busybox + i2c-tools
stay for console hand use); README layout table + phase-3 services
table + AGENTS.md where-things-live row; this log. The systemd units
still ExecStart the scripts — flipping is an on-glass job:
`gemcli selfcheck` first, then the lowest-risk-first flip order in
docs/gemcli.md (backlight-default -> wdt/boot/a72 hand-runs ->
gpu-poweron -> battery-guard LAST -> a72-up).

Next: deploy a gen with gemcli in the closure (bin/deploy.sh), run
`gemcli version` + `gemcli selfcheck` on glass, log the version line,
then start the parity diffs.

## 2026-09-08 (4th) — repin closure built + deployed: gen15 (nixpkgs dc5d91f84032) live, desktop healthy — 11-min build vs multi-hour

Executed the (3rd) entry's next step: build the repinned toplevel,
deploy to the device, health-check. Kernel UNCHANGED (6.6.0
#1-mobile-nixos — boot partition untouched, no flash; config-only
deploy like gens 11-14). Eyes-on-glass still owed (see next).

**Build** (run-job `repin-build`, rc=0 in **679 s**): toplevel
`1ia0xwzq8smb88jxk7ig3hfaym7jfsn4-nixos-system-gemini-26.11pre-git`,
gc-pinned `gemini-nixos-toplevel-20260908-1537`. The Qt6/LXQt closure
came from cache.nixos.org (qtbase-6.11.2 `mpgq1xlh…`, lxqt-session
2.4.0 `5ijcyvm3…`, lxqt-panel 2.4.1, pcmanfm-qt 2.4.1, systemd-261.2
`awq6qdiy…`, pipewire 1.6.8 — store hashes EXACTLY match the narinfo-
verified cached paths from the (3rd) entry; no local build logs).
Custom drvs compiled on the Pi as expected: mesa-geminipda 25.0.7
`yb4jq7sx…`, wlroots-geminipda 0.18.2 ×2, labwc-geminipda 0.8.3,
gemwl 1.0 `l53xj1w0n…`. For contrast the same toplevel under the old
pin compiled the whole Qt6/LXQt closure = hours.

**Deploy** (run-job `repin-deploy`, rc=0 in 209 s): `nix copy` of the
new closure over g_ether + `nix-env -p` set + `switch-to-configuration`
switch → device profile **system-15-link**;
`/run/current-system` = the new toplevel. Version float vs old pin
(12th-gen docs): qtbase 6.11.1→6.11.2, lxqt-panel 2.4.0→2.4.1,
systemd 261→261.2, pipewire 1.6.7→1.6.8, ffmpeg 8.x→9.0.1 default —
the documented R2 trade-off; verified pins (kernel/mesa/wlroots/labwc/
gemwl) unchanged.

**Post-switch health (over ssh, no reboot)**: gemwl + lxqt-nested
active, NRestarts=0 both; `systemctl --failed` empty; /run/gemwl
sockets (bus, wayland-0) present; gemwl running from the new store
path. Old generations stay selectable for rollback (profile list).

**Next / owed**: eyes-on-glass DONE (user, gen15 — desktop renders,
no flicker/uninitialised-LCD, core-rule-5 clearance). Still owed: one
cold WDT reboot to confirm the dual-boot initrd gen-lookup lands on
gen15; if green, this is the new cache-healthy baseline (golden rule 9).

## 2026-09-08 (3rd) — NIXPKGS REPIN to the hydra-built channel rev dc5d91f84032 (26.11pre1068949): Qt6/LXQt now substitutes; old-rev workaround overlays pruned

Executed the long-deferred handover/3a decision (repin nixpkgs to a
hydra-built rev). Device UNTOUCHED (still gen14 on p32, para cleared,
Debian p29 intact) — repo-side change only, no build/deploy run.

**What and why.** The old pin came from MNX's npins
(`nixos-26.11pre1031299.0bb7ec54c848`, a releases.nixos.org channel
snapshot); its qtbase 404'd on cache.nixos.org for x86_64 AND aarch64
(base closure only — glibc 200, qtbase/systemd/lxqt 404), so every
Qt6/LXQt compile ran on the 8-core Pi builder. Fix = pin nixpkgs to
the newest rev hydra built the FULL closure for: the nixos-unstable
CHANNEL snapshot behind `channels.nixos.org/nixos-unstable` =
**dc5d91f840324650bac8c379428c7037a416959a** (`26.11pre1068949`, cut
2026-09-07). Raw master commits newer than the channel cut only get
per-commit trunk-combined coverage — exactly the slow situation.

**Receipts (verified 2026-09-08, `nix path-info --store
https://cache.nixos.org` on aarch64 outPaths eval'd at the new rev):**
qtbase/qtwayland/qtsvg/lxqt-session/lxqt-panel/pcmanfm-qt/qterminal/
pavucontrol-qt/qpwgraph/papirus-icon-theme/systemd/nix/pipewire/
alsa-utils/openblas/ffmpeg-headless — narinfos ALL 200. Old rev
contrast: qtbase-6.11.1/lxqt-session-2.4.0/systemd-261 404, glibc 200.
(First probe used curl and wrongly showed all-404 — the sandbox proxy
mangles curl; nix's own HTTP client is the reliable check.)

**flake.nix:** nixpkgs pinned via `builtins.fetchTree` (tarball,
narHash `sha256-VaWGJ6+cIYN2erfSecbRV+4ljI185Ty2wUrXyvQbgOw=`,
`nix flake prefetch`-verified) and handed to the MNX eval shim through
its `pkgs` argument (shim forbids system+pkgs together; system comes
from `pkgs.stdenv.hostPlatform`, module pkgs re-import the same source
via `pkgs.path`). Bump recipe in the flake comment + README Pins:
take the rev behind `channels.nixos.org/nixos-unstable/git-revision`.

**config/gemini.nix:** pruned the old-rev/cross-era workaround
overlays (each forced non-hydra drv hashes down its subtree =
cache misses): systemd `withLibBPF=false`, ffmpeg(-headless)
`withCudaLLVM=false`, openblas `dynamicArch=false`, and the
libfm/libfm-extra/menu-cache autoreconf AM_GLIB_GNU_GETTEXT fix.
Kept: the make_ext4fs shim (R13 — deliberate rootfs-geometry fix).
Rationale: hydra built the un-overridden aarch64 defaults in this
channel (that's why they're cached), so the old bugs are gone (or
were cross-only). If a real build re-hits one, re-add it with a date.

**Measured (dry-run, root `--store local`, new rev):** toplevel
aarch64 — 379 derivations will be built → **242** (after the overlay
prune), 1447 paths (2.7 GiB) will be fetched. The remaining 242 are
NixOS per-config glue (unit-*/etc-*/udev/system-path — never cached)
+ the custom drvs (wlroots-geminipda ×2, labwc-geminipda, mesa fork,
kernel, gemini-firmware/…). No qt/lxqt/systemd/ffmpeg/openblas/libfm
compiles left. Eval green (MNX 2c132754 + device config compatible).

**Cost/risk:** the whole closure re-hashes → one full rebuild + one
full `nix copy` to the device; LXQt/Qt float slightly newer (R2
trade-off — the verified kernel/mesa/wlroots/labwc/gemwl pins are
independent of nixpkgs).

**Next:** DONE — build + deploy + health-check recorded in the
2026-09-08 (4th) entry below (gen15, 1ia0xwzq8s…).

## 2026-09-08 (2nd) — INTERNAL WI-FI (MT6630 CONSYS) WORKING ON NIXOS: wlan0 up, "The Lab" connected, internet ~4.5 ms — the wifi half of handover-2026-09-08-wifi-keyboard is DONE (gens 11-14)

Executed `docs/handover-2026-09-08-wifi-keyboard.md` §2 (wifi
workstream; keyboard §3 was the first session of the day — gen10). All
three boot units now pass: nvram → internal → auto. Kernel unchanged
`6.6.0 #1-mobile-nixos`; no flash, four config-only deploys via
`bin/deploy.sh`: gen11 `ggb0ni0n…`, gen12 `hgakgh7jw…`, gen13
`2qlbx6cb…`, gen14 `zf71qrycr…` (current).

Root causes fixed (all confirmed on glass):
- **R15 (wifi.nix nvram unit, handover §2a):** the unit's ExecStart was a
  multi-line `/bin/sh -c '…'` Nix string — systemd parsed the literal
  newlines as unit directives → bad-setting on EVERY boot since c6afc5c
  (verified: journal "Invalid section header '[ -e
  /etc/wifi/profiles.conf …'"). /data/nvram/APCFG/APRDEB/WIFI and
  /etc/wifi/profiles.conf were never installed → wlan_gen3 probe died on
  nvram_read. Fix: `wifiStateInstall` = pkgs.writeShellScript (unit file
  stays single-line; embeds the firmware + profiles seed store paths).
- **/lib/firmware (wifi.nix, handover §2b):** wlan_gen3's kalFirmwareOpen
  walks a HARDCODED path list (/storage/sdcard0, /vendor/firmware,
  /lib/firmware) — kernel file-open, not request_firmware; none existed
  on NixOS. Fix: `systemd.tmpfiles.rules = [ "L+ /lib/firmware - - - -
  /run/current-system/firmware" ]` + wifi-internal
  `After=systemd-tmpfiles-setup.service`. Verified: dmesg "[wlan]MAC
  address: 00:09:34:5a:af:c1" (factory NVRAM record), "wlanProbe ok",
  "FW OWN" — the factory MAC + TX cal load.
- **NEW NixOS-specific root cause (services/scripts/wifi, gens 12-14):**
  with the two fixes in, wlan0 came up and ASSOCIATED (iw link: "The
  Lab", RSSI -42) but `wifi auto`/`wpa_cli` always failed
  ("Failed to connect to non-global ctrl_ifname … Invalid argument") and
  the auto-connect never got a lease. The wifi CLI was ported verbatim
  from Debian (ctrl_interface=/var/run/wpa_supplicant), but this nixpkgs
  wpa_supplicant is built with the **unprivileged-daemon.patch**, whose
  wpa_cli HARDCODES ctrl dir `/run/wpa_supplicant/control` and client
  dir `/run/wpa_supplicant/client` — and refuses to run without the
  latter (strace: it only `access()`es …/client, never even calls
  socket()). Debug trail: python dgram probes + a copied aarch64 strace
  7.2 (Pi → host → device nix copy) → the patched source in the pinned
  nixpkgs (`pkgs/os-specific/linux/wpa_supplicant/
  unprivileged-daemon.patch`). Fix: write_wpa_conf emits
  `ctrl_interface=/run/wpa_supplicant/control`; wpa_ensure mkdirs
  `…/control` + `…/client`, kills stale daemons via the pid file, drops
  the ctrl dirs, and verifies wpa_cli connectivity 1 s after start (fail
  loudly instead of polling empty for 45 s).
- Verified on glass (gen14, units restarted by the switch in boot
  order): all three units active/Result=success; `wpa_cli -i wlan0
  status`: ssid=The Lab, freq 5180 (5 GHz), key_mgmt=WPA2-PSK,
  **wpa_state=COMPLETED**, ip_address=192.168.49.166, addr
  00:09:34:5a:af:c1; `ping -I wlan0 1.1.1.1` ~4.5 ms (2/2). One cosmetic
  journal note: dhcpcd's "Failed to set DNS configuration … resolve1 …
  unknown unit" (no systemd-resolved; resolv.conf is system-managed —
  harmless).

Next: cold-boot persistence check owed (the deploy-switch restart
exercises the same unit chain; a real power cycle confirms tmpfiles
creates /lib/firmware + unit ordering — do it when the user next
reboots), then the handover's remaining keyboard on-glass typing check
(Fn+K @, shift+3 £) + a `wifi auto` connect test at boot.

## 2026-09-08 — DESKTOP KEYBOARD MAPPINGS + SHELL fixed on glass (gen10): symbols/gemini shipped into the closure; gemwl + labwc now compile layout "gemini"; explicit bash login shell + SHELL for the session

Worked `docs/handover-2026-09-08-wifi-keyboard.md` §3 (keyboard xkb
missing from the closure — the Fn layer / UK layout NEVER worked in
NixOS) + the user's follow-up ask (default shell = bash, not sh). Both
fixed, deployed as **gen10** `y7v5z1sgwlq32812fpvspd5vs7338qh7`
(kernel unchanged `6.6.0 #1-mobile-nixos`, lean self-built) via
`bin/deploy.sh deploy`; no flash, no TWRP. Glass-visible effects land at
the next compositor start / terminal open.

- **xkb layout shipped (root cause from the handover, verified on
glass gen9 first):** gemwl logged `xkbcommon: ERROR: [XKB-338] Couldn't
find file "symbols/gemini"` every boot and fell back to the default US
keymap; labwc logged "Found layout English (US)". Consequence on glass:
Fn (`KEY_RIGHTALT` → `<RALT>`) behaved as Alt (no level3 layer) and
shift+3 gave `#` not `£`. New in-repo artifacts:
  - `config/xkb/symbols/gemini` — vendored byte-identical from
    GeminiPDA `build/rootfs-files/xkb/symbols/gemini` (sha256
    `f56fbab8…`, 5,937 B; provenance README `config/xkb/README.md`;
    same re-copy rule as `config/keymaps/`).
  - `pkgs/gemini-xkb.nix` — packages it as an xkbcommon include dir
    (`$out/symbols/gemini`); exposed as flake package `gemini-xkb`.
  - `services/desktop.nix` (gemwl unit) + `services/lxqt.nix`
    (lxqt-nested unit): `XKB_CONFIG_EXTRA_PATH=${gemini-xkb}`;
    lxqt-nested additionally `XKB_DEFAULT_LAYOUT=gemini` (labwc 0.8.3
    builds its keymap from that env — `src/input/keyboard.c`
    `set_layout`; gemwl hardcodes layout "gemini", gemwl.c:818).
- **Shell:** `users.defaultUserShell = "${pkgs.bashInteractive}/bin/bash"`
  in `config/gemini.nix` (root + gemini now point at the store bash in
  `/etc/passwd`, previously the inherited `/run/current-system/sw/...`
  default — already bash, now pinned), and the lxqt-nested unit exports
  `SHELL=` to the same binary so terminal apps (qterminal — a systemd
  system service has no SHELL env and qterminal falls back to /bin/sh)
  spawn bash.
- **Verified on glass (journal + unit env, gen10):** gemwl restarted
  clean (NRestarts=0) and logs `keyboard keymap: model=pc105
  layout=gemini variant=(default)` ×2 with NO XKB-338 after the
  restart; labwc logs "Found layout **Gemini English (UK)**";
  `systemctl show lxqt-nested -p Environment` carries
  `XKB_CONFIG_EXTRA_PATH=/nix/store/w802hc2bbx…-gemini-xkb-2026-09-08`,
  `XKB_DEFAULT_LAYOUT=gemini`, `SHELL=/nix/store/s6hkkyiy…-bash-
  interactive-5.3p9/bin/bash`; `/etc/passwd` root+gemini = the store
  bash. **User on-glass typing check still owed:** Fn+K → `@`, Fn+L →
  `;`, shift+3 → `£`, shift+' → `~`, shift+. → `?` (qterminal), and
  `echo $0` → bash (was sh before this gen in the desktop terminal).
- **Still open from the handover (not this session's ask):** wifi §2a
  (nvram unit malformed — single-line ExecStart) + §2b (`/lib/firmware`
  symlink) — wifi-internal still fails at boot on gen10; console Fn
  check on the fbcon VT (console.keyMap gemini-uk.map was already wired
  in-repo; on-glass typing of the VT layer untested this session).

## 2026-09-08 (afternoon) — LEAN KERNEL ON GLASS (A/B run done): self-built 6.6.0 boots + desktop verified; boot-log display quirk observed; wifi/keyboard handover written

Executed `docs/handover-2026-09-08-kernel-on-glass.md` — the first boot of the
self-built lean kernel (no #329 borrow). Outcome: **PASS on all §6 criteria**
except wifi-internal — which this session re-diagnosed as rootfs-packaging
gaps, not the deep CONSYS issue previously assumed (see below). Version lines
+ receipts:

- boot.img `/nix/store/b0a7lvxxbq13ryfzh8i1267rzyda7wcs-…_boot.img` —
  8.93 MiB (9,367,552 B), sha256 `be6f4d2192d8a95ef762cd17b910fa3af98d3fb98f76a91eef3decd4da4a2e51`,
  gc-pinned `boot-img-lean-20260908`; cmdline carries
  `bootopt=64S3,32N2,64N2`; header geom (kernel 0x40200000, ramdisk
  0x45000000, tags 0x44000000, pagesize 2048) verified pre-flash via
  `bin/dump-bootimg-header.sh`; **flash bytes verified post-flash from
  TWRP**: image-sized prefix of p22 `boot` sha256 == local image
  (whole-partition sha differs only by leftover bytes of the old
  14.7 MiB #329 image past the 8.93 MiB end — benign).
- generation **gen9** `r2mr8hf2l3k7l3029yhb8dgc27i7m84g-nixos-system-
gemini-26.11pre1031299.0bb7ec54c848` — gc-pinned
  `toplevel-20260908-1342`; deployed via `bin/deploy.sh` (5-path delta:
  `linux-6.6.0` + `linux-6.6.0-modules` + etc + toplevel) while gen8 ran,
  then flashed + rebooted to land kernel+gen together (§3 pairing).
- On glass: `uname -r` = **6.6.0 #1-mobile-nixos** (banner not #329);
  `/run/booted-system` kernel-modules = 6.6.0; `modprobe sramldo-smc`
  OK; panfrost at 17.3 s → renderD128 + card0; gemwl + labwc +
  lxqt-session/panel + pcmanfm running, **NRestarts=0**; battery-guard
  active (charging 4.06 V); boot 7.4 s kernel + 56.3 s userspace.
- Failed unit (systemctl --failed): only `gemini-wifi-internal`;
  additionally `gemini-wifi-nvram` is **bad-setting** (malformed unit —
  never ran on any boot; root-caused below). Old #329 `boot`
  auto-backed-up to `stock-dump/boot-20260908-134552.img` during the
  flash.

**fbcon boot-log display quirk (OBSERVED, unverified regression):**
operator noted the kernel boot logs appear **only in the bottom third of
an otherwise-healthy landscape display** (desktop full-screen and proper
→ panel/rule-5 clean). Evidence it may be *inherited*, not a lean
regression: cmdline is byte-identical to the #329 image
(`fbcon=rotate:3 fbcon=font:TER16x32` in both), fb driver + fbcon config
options identical between lean and full-329 configs (only FB_EFI/
FB_CORE/FB_DEVICE/FB_MODE_HELPERS pruned — `/dev/fb*` now absent, fbcon
unaffected). fbcon took over at 0.29 s at 135×33 (full landscape width in
fbcon's rotated accounting; TER16x32 font), then gemwl released it at
19.7 s. LK fb = 1080×2160 portrait buffer, OVL-scanned to landscape;
fbcon's software-rotation glyph grid lands only partially in the visible
window — the desktop (gemwl) renders correctly because it writes pixels
with full knowledge of the OVL layout. Operator: "I think it started with
the new kernel but I'm not completely sure" — NOT confirmed either way;
re-check against a #329 boot when convenient. Cosmetic only (fbcon
console window pre-gemwl). Follow-up if wanted: compare a #329/gen8 boot
visually, or probe alternate rotate values on a bench boot.

**Wifi/keyboard ROOT-CAUSE SPOTS (both "never worked in NixOS" items
re-diagnosed — they are ROOTFS-PACKAGING gaps, not the deep CONSYS
chip issue the 2026-09-07 log assumed):**

- `gemini-wifi-nvram.service` has been **malformed since the original
  port** (c6afc5c): its multi-line `''/bin/sh -c '…' ''` ExecStart lands
  in the unit file with real newlines/indent → systemd
  "Unbalanced quoting"/"Invalid section header" → **bad-setting, never
  ran on ANY boot** (verified: systemd-analyze verify fails on ALL
  stored gens 2/7/8/9; journal receipts Sep 07 15:41 + every boot).
  Consequence: `/data/nvram/APCFG/APRDEB/WIFI` (factory MAC+TX cal)
  and `/etc/wifi/profiles.conf` were never installed (`/etc/wifi` does
  not even exist on glass).
- wlan_gen3 probe fails on **two missing files**, per this boot's dmesg:
  (1) `nvram_read: failed to open!!` / `glLoadNvram fail` ← the dead
  nvram unit above; (2) `kalFirmwareOpen: Open FW image
  WIFI_RAM_CODE_6797 failed` at all three HARDCODED paths
  `/storage/sdcard0`, `/vendor/firmware`, `/lib/firmware` — none exist
  on NixOS (firmware lives in the nix store; the firmware_class param
  path serves request_firmware — the WMT/ROMv3 leg loaded fine via it
  ("live client re-synced to the patched full-mode MCU") — but
  wlan_gen3's kalFirmwareOpen uses its own hardcoded list, not
  request_firmware). The legacy Debian rootfs satisfied it by
  installing blobs into `/lib/firmware/` (GeminiPDA
  `build/rootfs-files/wifi-consys/install-wifi-consys.sh`). Fix
  direction: symlink `/lib/firmware` → the firmware dir (e.g.
  `/run/current-system/firmware`) via systemd-tmpfiles/activation +
  repair the nvram unit ExecStart (single-line or a script).
  **The CONSYS MCU link itself is healthy on this kernel** (resync OK,
  func-on leg 0) — the "deep CONSYS issue" framing from 2026-09-07
  needs re-testing after the two packaging fixes; the 30 s wlan0
  timeout is downstream (wlan_gen3 probe).
- Keyboard mappings never worked because **the Gemini xkb layout is
  not in the NixOS closure**: gemwl hardcodes layout "gemini"
  (`pkgs/gemwl/gemwl.c`, GEMWL_XKB_LAYOUT override) but xkbcommon
  fails every boot: `[XKB-338] Couldn't find file "symbols/gemini"`
  (include paths = the stock xkeyboard-config-2.47 + `/root/.config/xkb`,
  `/root/.xkb`, `/etc/xkb` — all absent). gemwl then falls back to the
  default keymap → wrong UK keysyms / no Fn (level3) layer. The legacy
  Debian rootfs shipped `symbols/gemini` (GeminiPDA
  `build/rootfs-files/xkb/symbols/gemini`, deploy-xkb-gemini.sh) — no
  NixOS equivalent exists yet. Kernel side is FINE: the matrix device
  is event2 "keyboard" (114-key bitmap), NT36772 touch = event0,
  mt6351-keys = event3, USB mouse = event1. Console keymap
  (`console.keyMap = gemini-uk.map`) is set but only covers the VT
  console, not the gemwl/LXQt desktop path.

Device left: **gen9 on p32, para cleared, NixOS default, booted on the
lean 6.6.0 kernel**, desktop up, wifi-internal failed + wifi-nvram
bad-setting (both root-caused above), Debian p29 untouched. Next:
mediatek wifi + keyboard mappings workstream — see
`docs/handover-2026-09-08-wifi-keyboard.md`.

## 2026-09-08 — Kernel SELF-CONTAINED + LEAN: published-base + delta-tree source model; borrow retired; kernel now builds in-nix (aarch64) in ~6 min

**Goal reached:** the rootfs no longer borrows kernel #329 artifacts from
GeminiPDA — the kernel builds in-repo from the published Linux **v6.6**
base + a tracked file-tree delta, and the config is now a pruned
**device-minimal** config. First lean self-built kernel build:

    linux-6.6.0            /nix/store/cgi059k6lsg14vgd5jl288kjxjp9c5w2-linux-6.6.0
    Image.gz 8,016,646 B   sha256 ace1a67874347d1257f2d4aa28a2d25378f87a6fb117db5af097f6a30ae0addf
    DTB (mt6797-gemini-pda.dtb) sha256 462e7140d6f1a819acf9a06543b1758b9269c7d89bc912f6edc29ff65b2b22b1
                           ^ byte-IDENTICAL to the borrowed #329 DTB (cmp, 2026-09-08)
    release 6.6.0 (moddir 6.6.0); 388 modules (was 1165); sramldo-smc.ko vermagic 6.6.0

Source model (kernel/default.nix, docs/library-deltas.md):
- **base** = kernel.org linux-6.6.tar.gz fetched by hash (sha256-PIj/…);
  byte-identical to `git archive v6.6` of the fork (ffc253263a…). Same
  commit pinned as a git submodule at `kernel/base` (read-only pointer,
  `git submodule update --init --depth 1 kernel/base` to fetch).
- **delta** = `devices/planet-geminipda/kernel/delta/` — the 512 plain
  files (457 A + 55 M, 0 D/R) the bring-up line changes over v6.6;
  copy-replace is exact: v6.6+delta == geminipda-bringup@188aade69
  (the #329 tree) — verified byte-for-byte. NO patch files (repo rule:
  agents edit source). Regenerate with `bin/sync-kernel-delta.sh`
  (replaces bin/snapshot-kernel.sh; no more 225 MB tarball).
- **config** = lean (default): `bin/prune-kernel-config.sh` from
  `config.full-329` (the exact #329 config, kept for A/B). Prune drops
  hardware that can never exist: 51 foreign ARCH_* (only ARCH_MEDIATEK),
  ACPI/EFI/XEN/KVM/PCI/ATA/SATA/NVMe/UFS, media/DVB, BT/NFC/CAN/
  802.15.4, vendor HID/touch/DRM (panfrost-only chain kept), foreign
  SoC clk/pinctrl/gpio/mfd/regulator/phy/rtc/leds/nvmem/iio/etc,
  crypto accelerators, DEBUG_INFO+lockdep. Result: 4,213 → 2,876
  textual → 1,655 enabled after the builder's olddefconfig cascade
  (=y 3,083→1,400, =m 1,021→~250). All 79 keep-symbols verified
  present post-normalization. Rule-5 gate now also asserts the config
  (eval-time, regex-free line scan — builtins.match on the 288 KB file
  stack-overflows the evaluator, found 2026-09-08).

Nix-build fixes discovered (all in kernel/default.nix; the delta stays
byte-identical to the fork):
- mediatek-connectivity Makefiles emit RELATIVE -I$(src)… — fine for
  the fork's in-tree builds, broken under the mobile-nixos O= build
  (wmt_core.c lost osal_typedef.h). postPatch anchors them on
  $(srctree).
- CONFIG_EXTRA_FIRMWARE_DIR in both configs pointed at an absolute
  GeminiPDA host path; now "firmware" (relative → $(srctree)/firmware)
  with the ROMv3 blobs staged into the tree at src-assembly from
  pkgs/gemini-firmware/ (tracked in-repo).
- gpio-aw9523b (keyboard expander) uses gpio_chip.irq, which needs
  CONFIG_GPIOLIB_IRQCHIP — a promptless select-only bool the #329
  config got from other (now-pruned) gpio drivers. postPatch adds the
  select to the fork Kconfig entry.
- sandbox quirk: `mkdir $out/firmware` AFTER the tar+cp steps got
  EACCES on the aarch64 builder; creating it FIRST works.

Timing: lean kernel builds in ~6 min total on the 192.168.49.191
builder (the full-config build had not finished drivers at the ~8.5 min
mark when it failed). Payload 13.47 → 8.02 MB (Image.gz); boot.img
headroom ~1.3 MiB → ~6.5 MiB. Module tree 388 .ko (~288 MB unstripped;
strip/DEBUG_INFO follow-up considered).

NOT flashed. The kernel is unverified on glass (as is any rebuild):
next step = the on-glass A/B run documented in
`docs/handover-2026-09-08-kernel-on-glass.md` (build boot.img + new
nixos gen, flash both in lock-step — §3 pairing rule, para=boot-
recovery discipline, rule-5 eyes check); keep kernel/borrowed +
config.full-329 until then (rollback). Also
still owed: docs sweep (README “Kernel phase”, AGENTS M5/M1 rows,
library-deltas kernel entry, .gitignore snapshot comments) — partially
done this session.

## 2026-09-08 — Handover completed: native-aarch64 MERGED into main (native build model canonical); LXQt desktop first on glass (gen8); two repo bugs fixed on the way

Handover (`docs/handover-2026-09-07-lxqt-native.md`) closed out + the
native branch became canonical — full story below; the worktree
`/home/cjdell/Projects/gemini-nixos-native` was removed once merged
(the handover job log was preserved under `logs/jobs/`).

Native toplevel build finished rc=0 (47790 s ≈ 13.3 h on the
192.168.49.191 builder): `b8hyqdvz…-nixos-system-gemini-26.11pre…`
(the deterministic drv `5br2r178…` from the handover). PINNED twice per
the handover/user requirement — verified with `nix-store -q --roots`
and `gc-pin.sh list`:
- per-user: `/nix/var/nix/gcroots/per-user/cjdell/gemini-nixos-toplevel-20260907-native`
- root-level: `/nix/var/nix/gcroots/gemini-lxqt-native-20260907`

Merge (commit 88a2699): `native-aarch64` folded into `main` — flake
`buildSystem = x86_64-linux → aarch64-linux` (devShells stay x86_64),
`pkgs/speaker-amp.nix` cc via `stdenv.cc.targetPrefix`. Follow-ups in
commit 7a5d6e8: `bin/deploy.sh` build verb = the proven native command
(`sudo nix build --store local .#packages.aarch64-linux.toplevel
--option builders @/etc/nix/machines --fallback`; daemon still has no
`builders =` line); flake.nix/README/AGENTS/feasibility R7 corrected
(cross toplevel ABANDONED — no longer the described model). Flake
outputs are now `packages.aarch64-linux.*` only.

**Desktop on glass — the actual test.** Deployed the native closure to
the device (deploy.sh deploy PATH; `nix copy` 2.1 GiB closure took
160 s over g_ether) and exercised the LXQt session live. Generation
history + version lines (rule 0; kernel #329 + dual-boot boot.img on
p22 UNCHANGED throughout — profile switches only):
- gen6 `b8hyqdvz…` (native toplevel): lxqt-nested.service crash-
  looped — `sessionConfig` in services/lxqt.nix did multi-source `cp
  ${file} ${file} $out/dir`, keeping the STORE basenames
  (`<hash>-lxqt.conf` …), so start-lxqt-nested's seed failed
  (`cannot stat …/lxqt/lxqt.conf`). Fixed: cp each file to its exact
  target name (flat layout $out/lxqt/{lxqt.conf,session.conf},
  $out/labwc/{rc.xml,autostart}, $out/themerc) — commit 7a5d6e8.
- gen7 `svwxm2bz…`: seeding OK, labwc died at startup — "Skipping gbm
  allocator: disabled at compile-time / unable to create allocator".
  wlroots 0.18.2's meson `allocators` option defaults to `['auto']`
  and `mesonAutoFeatures=disabled` resolves it to `[]`. Fixed:
  `-Dallocators=gbm` on the withDrmBackend (labwc) variant
  (pkgs/wlroots-geminipda.nix; gemwl's trimmed build unchanged) —
  commit 9a83d46.
- gen8 `3bqy4v4…` (current): **desktop up**. Rebuilds were fast
  (31 s / 28 s — config+wlroots delta only; the 13 h cost was the one-
  off full closure). Verified after a cold WDT-EXRST reboot
  (device-reboot.sh): booted gen8, gemwl + lxqt-nested active,
  NRestarts=0; session = labwc (nested, wayland-1) + lxqt-session +
  lxqt-panel + pcmanfm-qt --desktop + lxqt-policykit-agent +
  lxqt-notificationd + qterminal (autostart); layer-shell surfaces
  mapped; EGL 1.5 on Mali-T880 (Panfrost) Mesa 25.0.7 fork; gemwl
  compositing continuously (noafbc readback path). Root configs seeded
  to /root/.config/{lxqt,labwc} + LXQt runtime confs (panel.conf etc.).
  No crashes/OOM in the boot journal. Only unit failing = the known
  gemini-wifi-internal (CONSYS, pre-existing). **Eyes on glass
  (2026-09-08, user): the desktop renders correctly — panel/desktop/
  windows visible, no flicker/uninitialised-LCD** (core rule 5
  clearance).

**Still owed / next**: (1) The handover's 3a decision: repin nixpkgs
to a hydra-built unstable rev so future native builds are mostly
cache substitutes (current pin 26.11pre1031299 is 404 on cache for
both platforms — that's why Qt/LXQt compiled). Deferred deliberately
(would re-hash the whole verified closure). (2) Cosmetic: labwc built
without libsfdo → titlebar icon falls back to menu button; 1.5x output
scale is best-effort via the start script's wlr-randr probe. Device
left safe: gen8 NixOS on p32, para cleared, Debian p29 intact.


## 2026-09-07 (night) — LXQt desktop ported in-tree (committed); cross toplevel abandoned at nixpkgs-cross walls; native-aarch64 branch + Pi-builder build running (handover: docs/handover-2026-09-07-lxqt-native.md)

User goal: LXQt running like on the GeminiPDA Debian (labwc-nested).
Repo-side port done + committed on `main` 7a5bf23 (device untouched,
still gen5 `c10qkjdw`); the on-glass/closure build went the native
route. **Check-up doc for tomorrow: `docs/handover-2026-09-07-lxqt-native.md`.**

Done / landed (commit 7a5bf23):
- The black screen from the previous session was root-caused: gemwl ran
  fine but its startup client tinytest-anim SEGV'd ~1 s after map
  (dangling listener vtable — libwayland stores the pointer, the
  function-scope compound literals died before the async release/done
  event → wl_closure_invoke crash). Fixed in pkgs/gemwl/tinytest-anim.c
  + tinytest.c (file-scope statics).
- **LXQt nested desktop in-tree**: pkgs/labwc-geminipda.nix (labwc 0.8.3
  pinned, on wlroots 0.18.2), services/lxqt.nix (lxqt-nested.service:
  labwc -S lxqt-session hosting nixpkgs lxqt 2.4/Qt 6.11; XDG_CONFIG_
  DIRS = lxqt etc/xdg autostart assembly, Papirus icon theme,
  QT_PLUGIN_PATH, HOME=/root env), config/lxqt/ (session configs +
  vendored Gemini openbox themerc, seeded by services/scripts/
  start-lxqt-nested), gemwl now runs with no -s client. desktop.nix/
  config/gemini.nix/README/AGENTS/feasibility updated.
- pkgs/wlroots-geminipda.nix gained withDrmBackend (labwc 0.8.3 needs
  wlr_drm_lease_v1 headers; runs nested, never opens a DRM device).
- Built clean (aarch64): labwc-geminipda (a189p0f7…), gemwl (w70sh6in…).
- Overlays (committed, main): openblas 0.3.33 cross DYNAMIC_ARCH
  ARMV9SME missing-file fix (single ARMV8 when host isAarch64);
  libfm/libfm-extra/menu-cache autoreconf AM_GLIB_GNU_GETTEXT fix
  (native glib.dev/gettext/intltool).
- Host disk cleaned (~14 G on `/`; no GC run — roots intact).

Cross toplevel build (main) ABANDONED after: shiboken6/pyside6 (KF6
python bindings — nixpkgs: "cross is currently very broken") fixed by a
kguiaddons hasPythonBindings=false overrideScope overlay (evaluated
clean, then LOST when config/gemini.nix was git-checkout-ed during the
shutdown — re-derive if cross resumes); final wall Qt6CoreTools missing
for the whole lxqt scope under cross (prototype whole-scope
CMAKE_PREFIX_PATH override recursed — unresolved). See the handover for
the full blocker chain.

Native-aarch64 route (user decision):
- Branch `native-aarch64` + worktree /home/cjdell/Projects/
  gemini-nixos-native (flake: buildSystem = aarch64-linux, native eval;
  devShell stays x86_64). Native toplevel drv 5br2r178… (deterministic).
- speaker-amp.nix cc fix (stdenv.cc.targetPrefix) — commit a689b96.
- Distributed build running: host root `--store local` + `--option
  builders @/etc/nix/machines` → Pi 192.168.49.191 (8-core NixOS)
  compiles, host substitutes from cache.nixos.org (Pi's own cache link
  drops large NARs — HTTP 206 — so Pi-local builds were abandoned).
- **Cache truth (asked):** our nixpkgs pin 26.11pre1031299.0bb7ec54c848
  is NOT on hydra's cache (qtbase narinfo 404 for x86_64-native AND
  aarch64-native) → Qt6/LXQt outputs compile once per platform; 1315
  paths still came from cache.nixos.org during the native build. Fix =
  repin nixpkgs to a hydra-built rev (decision pending; native-aarch64
  is the model that benefits).
- Device state unchanged (gen5 on p32, para cleared, Debian p29 intact;
  nothing flashed).

Next (tomorrow, per handover): poll build-native-host from the native
worktree; on rc=0 PIN the toplevel (per-user gc-pin + root-level
`/nix/var/nix/gcroots/gemini-lxqt-native-20260907` root, verify with
nix-store -q --roots); record the version line; then decide nixpkgs
repin vs deploy-native-closure vs reconcile branches.

## 2026-09-07 — PHASE-2 MILESTONE: FIRST NIXOS BOOT ON GLASS (ssh to a NixOS shell over g_ether); the bootopt discovery; full saga + TODO in docs/phase-2-on-glass.md

The moment of truth happened and mostly worked. **NixOS boots and runs on
the hardware** (p32 userdata; hostname gemini; kernel #329; sshd at
10.15.19.82; g_ether; store re-hydrated; gemini-gpu-poweron + battery-
guard active). Debian (p29) intact throughout. The boot image needed
ONE fix before it would boot at all — see the bootopt discovery below.
Full knowledge capture + the open TODO list: `docs/phase-2-on-glass.md`.

Versions flashed this session (rule 0 lines):
- p22 boot.img sha `3965955f91162467d17e8659c103ac67ee4c79a4950bed38ea3cd456ffc364fb`
  (dual-boot initrd, #329 payload `3a2a7f3a…822`, bootopt cmdline).
- p32 system.img sha `091707d7…` (gen `yl6hkkih…` embedded; flash md5-
  verified + first-MiB readback verified). [corrected: the earlier prep
  entry's `dcfv0nsj` gen came from a separate path-info eval — the
  image's own registration says `yl6hkkih`]
- Debian p29 untouched; para cleared at milestone end (NixOS default).

What happened / what was learned (receipts point at phase-2-on-glass.md):

1. **WDT-EXRST silent no-op from a mid-session A72 bring-up** (the
   cl2-up wdt_disarm trap): the first converge-to-TWRP reboot never
   fired (device uptime 13.9 h unchanged; WDT_MODE 0x10007000 = 0). Fix
   discovered + used: `devmem 0x10007000 32 0x2200005D` (key|0x5D)
   restores LK's mode → `0x48` fires → EXRST. (§2b in the doc.)
2. **THE bootopt discovery**: our boot.img (kernel field sha-identical
   to the working Debian image, geometry identical, ramdisk recipe-
   equivalent) hung on the LK logo ~15 s → WDT boot loop, no kernel
   text, empty pstore (death precedes ramoops/fb). Bisected to the
   HEADER: re-packing our kernel+ramdisk with the old image's header
   (pack-boot-img-custom-ramdisk.py --reference) booted Debian through
   OUR initrd's debian branch. Root cause: LK's
   `platform_parse_bootopt(boot_hdr->cmdline)` (load_image.c:839)
   needs `bootopt=64S3,32N2,64N2 log_buf_len=4M` in the field — the
   field is inert for the KERNEL (CMDLINE_FORCE) but LK reads it first
   ([corrected] boot-process.md §4). Fixed in config/gemini.nix
   kernelParams. (§2a.)
3. **Loop recovery proven ~5×**: para restore via the preloader window
   (`run-mtk.sh w para stock-dump/para-boot-recovery.bin` — the loop
   provides the power-cycles) → TWRP in ~25 s. Also confirmed the
   drills-doc FAC_RESET note is not needed when the preloader path is
   available.
4. **run-mtk.sh hardened**: a second python3.14 mtkclient store path
   broke the python3.13 deps scan (Cryptodome vanishing) — pick_pkg()
   now selects a candidate whose deps resolve. (§2d.)
5. **flash-nixos.sh fixed**: 30 s adb timeout killed the 1.5 GiB rootfs
   push → adb_push (900 s) + wc -c verify; TWRP busybox stat has no -c.
   (§2e.)
6. **Boot attempts + recoveries**: boot-nixos #1 (14:50) looped (pre-
   bootopt image); control tests with the Debian backup boot.img proved
   flash/para/eMMC flows; the P3 header test proved the bootopt cause.
7. **First NixOS boot** (fixed image, ~15:27): fbcon log on the LCD ✓,
   initrd markers ✓, switch_root to gen `yl6hkkih` ✓, store rehydrated,
   systemd up, sshd answering. On-glass checks pass: uname #329,
   hostname gemini, 3.6 GiB RAM, gpu-poweron + battery-guard active.
8. **Not-quite-working (→ TODO in the doc)**: growfs (`/` 3.1 GiB —
   udev by-label coldplug race + resize2fs EINVAL at group #25, kernel
   ext4_resize_fs -22); vconsole (setfont TER16x32 not a kbd font);
   gemini-audio-defaults (status 127); gemini-wifi-internal (mtk_wcn
   modprobe); gemini-a72-up failed at boot (should be opt-in like
   Debian's handoff); nixos-rebuild round-trip untested (phase-2
   criterion second half); Debian-branch re-verify on the final image
   pending.

Device left: NixOS running on p32 (milestone state), para cleared,
Debian p29 intact/bootable, A72s offline, battery charging. Nothing
flashed since the successful boot. Commits pending (14 modified + 2
intent-to-add — see git status). Next: the phase-2-on-glass.md TODO
(P0 growfs first).

## 2026-09-07 — p32 DUAL-BOOT DECIDED + IMPLEMENTED (repo-side): NixOS rootfs → Android userdata, Debian stays on p29; images rebuilt + verified; flash plan updated (awaiting user go-ahead)

User decision this session: "override Android" = take the p32 route of
`docs/repartition-android-space.md`. All §10 decisions made (dated in the
doc): **a)** default OS on para-clear = NixOS; **b)** marker = para
offset-0 command field (byte-exact `cmp` in the initrd); **c)** kernel
stays borrowed #329 (Debian keeps booting the same boot.img); **d)** no
p32 ciphertext backup (`--backup-rootfs` dropped). §9 change list landed:

- **`devices/planet-geminipda/initrd.nix` — dual-boot initrd**: reads the
  32-byte para command (p2 of the largest mmcblk, sysfs size read,
  byte-exact `cmp` vs `boot-debian\0`+20 zeros — cmp-on-files because
  ash vars can't hold NULs); zeros/unknown → NixOS default, marker →
  Debian branch replicating `GeminiPDA/build/initramfs-6.6/init` verbatim
  (A72 opt-in + fstab `/` fix + `switch_root /sbin/init`); mode target
  missing → fall back to the OTHER kind's rootfs → shell only if none.
  Fixed during review: `/tmp` did not exist in the initrd staging dirs
  (would have silently ignored the marker); `${…}` inside the nix `''`
  string is interpolated — replaced with `$var` concatenation +
  `$(basename …)`; added applets dd/cmp/chmod/basename.
- **`devices/planet-geminipda/default.nix`**:
  `system_partition_destination = "userdata"` (p32) + comment.
- **`bin/flash-nixos.sh`**: `rootfs` → `by-name/userdata` (p32, ≥20 GiB
  sanity still passes at 27.3 GiB; prompt "Type 'wipe android'");
  `--backup-rootfs` REMOVED (§10d); NEW `debian` verb (running Linux:
  ssh para-write + WDT EXRST self-boot with read-back verify; TWRP:
  adb). New `twrp_para` helper writes any 32-byte marker.
- **`bin/boot-switch.sh`**: NEW `debian` verb (para=boot-debian + reboot
  from TWRP; no adb wait — Debian has no adbd).
- **NixOS side**: NEW `services/scripts/gemini-boot-debian` (mirror of
  gemini-boot-recovery; 32-byte `conv=sync,fsync` write + read-back
  verify) + hand-started `gemini-boot-debian.service` in
  `services/gemini-pda.nix`. NOTE: new untracked files must be
  `git add -N`ed before building — the flake source export only carries
  tracked paths (hit + fixed this session; the packaged utils lacked the
  script until `git add -N services/scripts/gemini-boot-debian`).
- **Docs**: repartition doc → ✅ decided/implemented (§10 + impl record),
  README (intro, layout rows, Flashing steps, unit table, "do not flash"
  text), AGENTS cheat-sheet boot targets + flash pipeline + where-things-
  live rows, boot-process §3/§5/§6/§7 (selector implemented; fallback =
  other-kind rootfs; gemini-boot-debian exists; size correction — the
  earlier 15,433,728 B / 14,108,276 B figures were an older gzip
  encoding; current sha-verified build is 14,815,232 B / 13,489,966 B
  payload), feasibility §9 phase-2 row annotation, session-log entry.

Images rebuilt + verified (NO FLASH — device untouched, still Debian on
p29, para cleared):

- **`result/boot.img`** 14,815,232 B — sha256
  `016c232351bd5de18c1d855cf9c6c2804ceaf532ad6d96d4a65d96d09152dd47`;
  dual-boot initrd verified inside: /init carries the para selector
  (mkdir /dev/pts /newroot /tmp, dd+cmp marker check, debian branch with
  A72/fstab handoff, NixOS gen lookup); **native busybox `sh -n` clean**;
  the four marker writers (boot-switch debian, flash-nixos twrp_para +
  ssh inline, gemini-boot-debian) all produce byte-identical 32-byte
  commands == the initrd's `cmp` reference (tested on host). Kernel
  field unchanged: payload sha `3a2a7f3a…822` (#329, verified).
- **`result/system.img`** 1,640,378,368 B — sha256
  `091707d716767b31835b821ac6f723b7fb5c4fb8b8058775903b163134b3c056`;
  generation `dcfv0nsj…-nixos-system-gemini-…` — now carries
  `gemini-boot-debian.service` + the packaged CLI
  (`vgrm0ygh…-gemini-pda-utils/bin/gemini-boot-debian`, /bin/sh
  shebang kept under R10). ext4 label NIXOS_SYSTEM re-verified.

**Flash plan (supersedes the earlier p29 entry's next-action; run only
on the user's word):** `flash-nixos.sh status` → `boot` (dual-boot
boot.img → p22; current #329 Debian boot.img auto-backed-up to
stock-dump/) → `rootfs --yes` (system.img → p32 userdata, Android FDE
gone) → `boot-nixos` (para-clear → NixOS p32 first boot) → on-glass
checks (ssh `uname -r`, generation `dcfv0nsj`, growfs). Debian stays
untouched on p29 and boots any time via `boot-debian` (host
`flash-nixos.sh debian` / `boot-switch.sh debian`, or on-device
`gemini-boot-debian`). Note: once the dual-boot boot.img is in p22,
Debian boots ONLY through its initrd's debian branch — rollback of
`boot` = `boot-switch.sh restore` (auto-backup). Nothing flashed yet.
Versions: kernel #329 (payload sha `3a2a7f3a…822`); boot.img sha
`016c2323…`; system.img sha `091707d7…`; generation `dcfv0nsj`; Mesa
25.0.7 fork; wlroots 0.18.2; gemwl 1.0; Mobile NixOS `2c132754`;
nixpkgs `nixos-26.11pre1031299.0bb7ec54c848`.

## 2026-09-07 — PHASE-2 PREP: flash images built + verified (nothing flashed); device state recorded; waiting for the go-ahead

Host-side readiness for the first real NixOS rootfs flash. **No flash,
no write to the device** — all checks read-only.

- **Images rebuilt fresh** (`bash bin/run-job.sh start build-images --
  nix build .#packages.x86_64-linux.default`, rc=0, 21 s — heavy deps
  cached from the 2026-09-07 toplevel rebuild). `result/` now carries:
  - `boot.img` 14,815,232 B — sha256
    `7f346637d69f74744861a993da7aab27ec56900c347f6d587ed62a618a943329`;
    fits p22 (16 MiB) with 1.87 MiB headroom. Kernel field verified:
    `kernel/borrowed/Image.gz` (sha `3f8761a4…`, 13,466,943 B) + 23,023 B
    appended DTB = 13,489,966 B payload, sha `3a2a7f3a…822` — the exact
    documented verified #329 payload (boot-process.md §2); decompressed
    sha `96d0cbbb…`. Ramdisk = minimal initrd, gzip cpio with `/init`
    (1,321,716 B, sha `52c7d580…`). NOTE: old build's 14,108,276 B
    kernel field was a different gzip encoding — identity verified via
    the decompressed sha, so the new image is the same #329 kernel.
  - `system.img` 1,640,366,080 B (1.53 GiB) — sha256
    `cf13e8bc45e3f2b21f2f405bbd213f25ea72cec6db6229b69a6148ecb0ef0952`;
    ext4 label `NIXOS_SYSTEM`; generation inside the image =
    `/nix/store/xy5m38g0…-nixos-system-gemini-26.11pre1031299.0bb7ec54c848`
    == current `.#toplevel` (carries the 2026-09-07 outstanding.md
    fixes: ssh key, hostname, keymap, logind, DRM order, backlight, R10).
- **Device state recorded (live over g_ether, kernel
  `6.6.0-00048-g188aade698dd` = #329)**: eMMC = mmcblk0 58.2 GiB,
  boot0/boot1 **4 MiB each** (live measure — resolves the 2-vs-4 MiB
  open question in inventory.md: DA-log figure confirmed, legacy 2 MiB
  superseded; inventory annotation updated); para (p2) = all zeros
  (cleared → NORMAL boot); p29 = Debian 14 G / 28 G used (54 %);
  battery bq25890 voltage_now = 3.884 V (≥ 3.8 V precondition OK).
- **Tooling re-verified**: devshell closure built (adb, mtkclient store
  pkg fetched); ssh key `~/.ssh/id_ed25519_gemini` present; patched
  mtkclient at `/usr/local/lib/mtkclient-patched`; `flash-nixos.sh
  status` → `device state : linux`, both artifacts present.
- **Flash-safety machinery re-read**: `boot-switch.sh flash` backs up
  the current `boot` to `stock-dump/boot-<ts>.img` before writing;
  para backed up once per session only if `stock-dump/para.bin` is
  absent (it exists — 08-30 readback never clobbered); `restore`
  returns the latest backup. `flash-nixos.sh` leaves para =
  boot-recovery (TWRP sticky) until `boot-nixos` is run.

Versions (targets for the next session's version lines): kernel #329
(borrowed, decompressed sha `96d0cbbb…`); boot.img sha
`7f346637…`; system.img sha `cf13e8bc…`; generation `xy5m38g0`;
Mesa 25.0.7 fork; wlroots 0.18.2; gemwl 1.0; Mobile NixOS `2c132754`;
nixpkgs `nixos-26.11pre1031299.0bb7ec54c848`.

Next action (awaiting user go-ahead): `bash bin/flash-nixos.sh
status` → `boot` → (decide: `--backup-rootfs` of Debian p29 first?)
→ `rootfs --yes` → verify serial/fbcon in TWRP-sticky state →
`boot-nixos` → on-glass checks (ssh `uname -r`, generation, growfs,
§13 items). Optional-but-recommended DR before the flash: gather.md
step 2 (preloader dump via DA session, device off) — step 1 now done
(live 4 MiB boot areas).

## 2026-09-07 — GOLDEN-REPO PIVOT: gemini-nixos declared the primary repo for the whole Gemini PDA project; DR playbook ported here from the sibling (no revert of GeminiPDA)

User decision this session: gemini-nixos will **eventually completely
replace GeminiPDA** — new content goes in THIS repo, GeminiPDA is the
legacy source being folded in (not reverted, not edited for new work).
What was done:

- **AGENTS.md rewritten as the golden charter**: new "Repo status:
  GOLDEN" header + transitional rule (a topic's truth is wherever its
  latest content is; port pointers decay), a **migration plan M1–M7**
  (receipt docs / DR knowledge / stock-dump blobs / recovery tooling /
  kernel+LK trees / services / session history), and all existing rules
  0–8b preserved with authority references changed from
  "sibling is the authority" to "this repo is golden; legacy paths are
  transitional".
- **DR playbook ported here**: `docs/disaster-recovery/{README,
  inventory,gather,drills}.md` — inventory now carries a copy-status
  column (here vs legacy-pending M3) and the bulk-migration rsync
  command; tooling references point at this repo's `bin/`.
- **Recovery tooling ported (M4)**: `bin/run-mtk.sh` (patched-mtkclient
  launcher; version-agnostic store lookup + clear errors when the
  devshell closure / patched copy is missing) and `bin/usb-watch.sh`,
  both from the legacy `build/` originals; `mtkclient` (nixpkgs
  2.1.4.1 — verified present in this repo's nixpkgs pin) added to the
  flake devshell so the store pkg + Loader DAs exist for the launcher.
- **Boot-critical blobs copied (M3 partial)**: `stock-dump/` here now
  holds lk/para/para-boot-recovery/recovery/twrp-noswipe/boot/boot2/
  boot3/logo/proinfo/nvram + gpt txt + 2 representative boot images
  (130 MB); every sha256 re-verified OK against the ledger. Bulk
  (android images, firmware zip, ~85 boot backups) stays in
  `GeminiPDA/stock-dump/` until the documented rsync.
- **Pivot notes added** (dated, non-destructive) to the docs that still
  asserted sibling authority: README (golden banner + local DR row),
  boot-process, repartition-android-space, library-deltas,
  mobile-nixos-port-feasibility (marked historical), outstanding.md.
- Legacy sibling edits from earlier this session (DR folder,
  flashing.md pointer, hardware.md [open question], its session-log
  entry) are **left in place** per "no revert" — they are now legacy
  copies; this repo is the golden ledger.

Versions (nothing flashed): unchanged — kernel #329
`6.6.0-00048-g188aade698dd` (borrowed); Mesa 25.0.7 fork; wlroots 0.18.2;
gemwl 1.0; Mobile NixOS `2c132754`; nixpkgs `nixos-26.11pre1031299.0bb7ec54c848`.

Next action: when the device is next on the bench (phase 2 window), run
`docs/disaster-recovery/gather.md` steps 1–8 BEFORE any flash work —
preloader dump + raw GPT + BROM-entry check are one-time, device-healthy
tasks; then continue M1 (port the receipt docs) whenever a doc session
allows.

## 2026-09-07 — A72 cluster power-DOWN brought over (cl2-down.sh, verbatim; build-level)

## 2026-09-07 — A72 cluster power-DOWN brought over (cl2-down.sh, verbatim; build-level)

Ported the sibling's proven A72 power-down path (GeminiPDA @ 738d19f,
2026-09-07) into this repo's script set:

- **NEW `services/scripts/cl2-down.sh`** — byte-identical copy of the
  sibling's `build/a72-bringup/cl2-down.sh` (md5
  `3b70536b79cee74ee156762e09a6a002`), exec bit set. What it does
  (receipts live in the sibling's session-log/hardware.md): per-core
  PSCI offline is safe (cpu9 while cpu8 up; "psci: CPU9 killed (polled
  0 ms)"); the LAST-A72 branch runs the secure power_off_cl3 teardown
  inside the controller's AFFINITY_INFO SMC (UNBOUNDED waits — WDT 20 s
  armed is the recovery), then — only after B_EXT_BUCK_ISO re-assert
  (0x10006290 bit1) + 0x10006218 bit0 clear are confirmed — drops the
  external DA9214 BUCKB rail (vendor cpu_power_off_buck). Post-down
  state = cold-boot state; re-enable = the existing `cl2-up.sh`
  (verified ×2 cycles on the sibling unit).
- **`services/scripts/cl2-up.sh`** — already carried the sibling's
  bus-wait hardening (diff vs 738d19f: empty).
- **Packaging**: no Nix changes needed — `gemini-utils.nix` copies the
  whole `scripts/` dir, so `cl2-down.sh` now ships in `gemini-pda-utils`
  (on-device PATH via `environment.systemPackages`) and gets the R10
  store-bash shebang rewrite automatically. It is a hand-run CLI only
  (`cl2-down.sh [cpu9|cpu8|both]`), deliberately NOT a systemd unit —
  the down is on-demand and must not race `gemini-a72-up` at boot
  (documented in `services/gemini-pda.nix`). Uses only busybox devmem /
  i2c-tools i2cset / coreutils+gnused+util-linux — all already in the
  service/system PATHs or the base closure.
- **Docs**: README unit/CLI table row, `services/gemini-pda.nix` +
  `gemini-utils.nix` headers, feasibility doc R5 addendum + phase-3
  row (2026-09-07 fragment). No stale "never offline" language existed
  in this repo (sibling corrected its own hardware.md).

Versions: unchanged — kernel still the borrowed #329 (pin
`733c0c7ea74195bd30734f599f37e69febfd38e0`); nothing flashed (still
build-level; device untouched). Kernel tree unchanged in the sibling
commit too.

Next action: unchanged — phase 2 on-glass verification; when the NixOS
rootfs is live, `cl2-down.sh both` then `cl2-up.sh` is the on-device
round-trip to prove the pair on this stack.

## 2026-09-07 — boot-process explainer doc (from the Q&A session; repo-only)

Saved the bootstrapping Q&A (kernel identity / cmdline / rootfs selection /
initramfs builds) as **NEW `docs/boot-process.md`** — a plain-language
explainer layered over the receipt docs. Contents: the one NORMAL boot
slot (only `boot` p22 / `recovery` p1 are loadable by LK), boot.img =
kernel+DTB+ramdisk, the shared borrowed #329 kernel (byte-identity
verified: kernel payload sha256 `3a2a7f3a…822` matches between the
sibling's new_kali_boot.img and `kernel/borrowed/`), the ramdisk `/init`
as the rootfs selector (content markers; NixOS store-only vs Debian
`/etc/os-release`), the four cmdline locations + `CMDLINE_FORCE` (both
OSes boot the identical forced cmdline today; per-OS cmdlines only with
per-OS kernels), the para-marker selector table, the two initramfs builds
vs the one proposed dual-boot initrd, and the size constraints shaping it
all. README layout table got a row for the doc. Nothing flashed; no code
changes.

Next action: unchanged — §10 decisions of `docs/repartition-android-space.md`,
then the §9 implementation.

## 2026-09-07 — Android-space repurpose + dual-boot investigation (repo-only; no device interaction, nothing flashed)

Investigated (per the user, no device changes): can the NixOS rootfs go
where Android currently is, keeping the GeminiPDA Debian rootfs (p29) for
testing, ideally bootable without reflashing `boot`? Also: the 16 MiB
`boot` size-constraint risks + workarounds. Outcome = a proposal doc, no
code changes:

- **NEW `docs/repartition-android-space.md`** — full write-up with
  receipts: partition map + LK boot truth (`boot`/`recovery` are the only
  partitions LK loads — `mt_boot.c:1484/1527`; boot2/boot3 never), the
  p32 `userdata` (27.33 GiB) recommendation over p27/GPT surgery, the
  para-command dual-boot selector (`boot-recovery` reserved for LK→TWRP,
  `boot-debian` → p29, zeros → p32 NixOS default), the Debian handoff to
  replicate from the GeminiPDA initramfs (fstab `/` fix, A72 opt-in
  enforcement), boot-budget measurements (boot.img 14.72 MiB of 16 MiB =
  1.28 MiB headroom; kernel gz 13.45 MiB → 34.3 MiB decompressed; LK is
  zlib/gzip-only; RD_* all =y for future initrd formats), the
  shared-kernel constraint (§8), and the repo-side change list (§9) +
  open decisions (§10).
- **`docs/mobile-nixos-port-feasibility.md`**: two dated [superseded
  2026-09-07] annotations — §7 Non-risks "Android partition layout" row
  and §8 decision 4 (Android p27/p32 fate) — pointing at the new doc
  (both previously assumed p29-only repurpose with Android untouched).
- README not touched (nothing flashed/decided yet; its p29-target text
  stays until §10 decisions land).

Evidence gathered this session (facts for the record, all verified
read-only): current on-device `boot` backup `stock-dump/boot-20260907-
013541.img` carries kernel #328 (00047-g3b3a2b6 — the pre-#329 boot),
#329 is what the device runs now (sibling log 2026-09-07); the repo's
borrowed `kernel/borrowed/Image.gz` = the same #329 payload
(00048-g188aade698dd, version string verified); #329 `.config` has
`CONFIG_CMDLINE_FORCE=y` (boot.img cmdline inert for both OSes); store
build artifacts measured: boot.img 15,433,728 B (kernel 14,108,276 B +
ramdisk 1,321,716 B, 2048 pages) + system.img 1,849,479,168 B; para env
window @ 0x20000 (env.h:36-44) — offset-0 command writes never touch it.

Next action: user decides §10 (default OS, marker location, kernel
phase), then implement §9 (initrd dual-boot branch, flash/boot-switch
re-target to p32, boot-debian verb/unit) — still nothing flashed until
the phase-2 on-glass cycle.

## 2026-09-07 — outstanding.md worked: SSH/logind/keymap/DRM-race/udev/backlight/NAT + R10 shebang fix (build-level; nothing flashed)

Worked the `outstanding.md` rootfs-viability list (§13 order). **No
flash** — device still on the GeminiPDA Debian rootfs / kernel #329; it
was reachable over g_ether for read-only captures + one host-NAT test.

Fixes landed (all build-level, toplevel rebuilt + closure-inspected):

- **§2 SSH root key + §11 hostname** (`config/gemini.nix`):
  `users.users.root.openssh.authorizedKeys.keys` = the
  `id_ed25519_gemini.pub` key; `networking.hostName = "gemini"`.
  Closure: `/etc/ssh/authorized_keys.d/root` carries the key;
  `/etc/hostname` = `gemini`. Toplevel now `nixos-system-gemini-…`.
- **§3 logind side-key policy** (`config/gemini.nix`):
  `services.logind.settings.Login.{HandleSuspendKey,HandleHibernateKey,
  HandlePowerKey} = "ignore"` — CORRECTED the audit's fix sketch:
  `services.logind.extraConfig` is **removed** in this nixpkgs pin
  (module now exposes `settings.Login`). Closure `logind.conf` has the
  `[Login]` section.
- **§4 keymap** (vendor + config): `config/keymaps/gemini-uk.map`
  copied verbatim from GeminiPDA `build/rootfs-files/keyboard/` (+ a
  provenance README); `console.keyMap = ./keymaps/gemini-uk.map`.
  Closure `/etc/vconsole.conf` = `KEYMAP=<store path>`;
  `loadkeys --validate` passes.
- **§5 DRM/panfrost race** (`services/gemini-pda.nix`):
  `boot.kernelModules` += `drm drm_shmem_helper gpu-sched panfrost`
  (+ `mt6351-keys` for §11 determinism). Closure
  `/etc/modules-load.d/nixos.conf` lists them; all four .ko verified
  present in the borrowed #329 module tree.
- **§6 udev USB host-PM rule** (`services/gemini-pda.nix`):
  `services.udev.extraRules` with the B-19 three lines; closure
  `99-local.rules` carries them. Device-only rule captured verbatim
  from the live Debian rootfs first.
- **§7 backlight-default** (`services/gemini-pda.nix`):
  `gemini-backlight-default` oneshot unit (10 %, after udevd); in the
  closure + `multi-user.target.wants`.
- **§8 host NAT** (`bin/usb-tether-nat.sh`, NEW): port of the sibling
  `build/usb-tether-nat.sh` — auto-detected upstream iface + tool
  checks. **Verified live** this session: device `ping -c1 1.1.1.1`
  succeeds (~3 ms) with the NAT rule up.
- **§10 wdt/boot-recovery units** (`services/gemini-pda.nix`):
  `gemini-wdt-reboot.service` + `gemini-boot-recovery.service` as
  hand-started oneshots (no `wantedBy`); both in the closure. AGENTS/
  README unit claims now accurate. boot-recovery comment clarified:
  plain reboot powers off → next power-on (sticky para) lands in TWRP.
- **§11 minors**: hostname + mt6351-keys pin done (above);
  renderD129→renderD128 comments fixed in `services/desktop.nix`;
  serial-getty + wifi-DNS remain on-glass checks.
- **§9 GPU warmup**: DECIDED deferred to the first gemwl boot on glass
  (#329 banding question needs glass); risk + both fix options
  documented in the `services/desktop.nix` header.

**R10 — new port delta found while working the list** (feasibility doc
§7 R10, `services/gemini-utils.nix`): the verbatim Debian scripts shebang
`#!/bin/bash`, but a NixOS rootfs has NO `/bin/bash` (stage-2 only makes
`/bin/sh` via `environment.binsh`) and systemd ExecStart execs scripts
straight (kernel resolves `#!`) → every bash unit (gpu-poweron, a72-up/
cl2-up, battery-guard, audio-defaults, backlight) would have failed on
glass with status=203/EXEC. Fix: package-time shebang rewrite to the
store bash. Verified: packaged scripts now `#!<store>/bin/bash`;
`#!/bin/sh` scripts untouched.

Closure inspected: `/nix/store/pscdi0fn9lan4rcnh2c4g9ksvhd3kh58-
nixos-system-gemini-26.11pre1031299.0bb7ec54c848` (== current
`.#packages.x86_64-linux.toplevel`). Rebuilt under `bin/run-job.sh`
(toplevel-rebuild, rc=0). No image was built/flashed — the boot/rootfs
artifacts are unchanged by this session (config/closure only).

Device-only files captured from the live Debian rootfs (pre-flash
insurance, per outstanding.md §0/§12): the udev rule, logind drop-in,
`/etc/modules-load.d/99-gpu.conf`, `backlight-default.service`, root
`authorized_keys` — all match the inline copies in outstanding.md.

Next action (unchanged): phase 2 — on-glass verification. When the
device is next on the bench: `bin/flash-nixos.sh status` → `boot` →
`rootfs --yes` → verify serial/fbcon → `boot-nixos`; then the on-glass
checks per outstanding.md items (ssh `uname -r`, silver button, keymap
Fn combos, `ls /dev/dri`, backlight get, dongle plug, §9 banding watch).
Log the outcome here with image hashes.

## 2026-09-07 — AGENTS.md + flash/recovery tooling imported (build-level; nothing flashed)

What happened (repo-only; no device interaction — unit untouched, still
running the GeminiPDA Debian rootfs on kernel #329):

- **`AGENTS.md` created** at the repo root, adapted from the sibling's
  AGENTS.md (which was read in full). Brought over, re-contextualised for
  the Mobile NixOS port: version/commit hygiene (rule 0), record-before-
  forget + date + receipts, the LCD-panel safety rule (applies to the
  kernel derivation when the in-repo build is finally used — must stay
  the fbcon/EXCLUDE_DISPLAY build), scripts-over-ad-hoc (rule 6),
  devshell-only CLIs (rule 7 — bare host PATH verified to lack
  python3/adb/make, 2026-09-07), run-job detached runner (rule 8), the
  "where things live"/"when to update what" tables, a device-operations
  cheat sheet, and session start/end discipline. New meta-rule for this
  repo: **the sibling GeminiPDA project is the knowledge authority** —
  receipts live there; this repo records port decisions/deltas.
- **Flash/recovery tooling added under `bin/`**, ported from the
  sibling (GeminiPDA @ abc0afb, 2026-09-07):
  - `bin/net-up.sh`, `bin/device-ssh.sh`, `bin/device-reboot.sh` —
    g_ether host-side link/ssh/WDT-EXRST reboot (near-verbatim ports;
    device 10.15.19.82, key ~/.ssh/id_ed25519_gemini).
  - `bin/boot-switch.sh` — adb/TWRP boot-target state machine
    (status/twrp/android/flash/restore). Deltas vs the sibling: adb/lsusb
    resolved via the flake devshell (self re-exec with a
    `GEMINI_DEVSH_REEXEC` guard); dropped the Gemian `linux` command —
    on this project Linux = the NixOS boot.img in `boot` itself, booted
    by `android` (para-clear + reboot); `xxd` replaced with coreutils
    `od` in `status`.
  - `bin/flash-nixos.sh` — NEW orchestration for THIS repo's artifacts:
    converges to TWRP from ANY device state (running Linux → para write
    over ssh + WDT EXRST self-boot; Android → adb hop; POC/offline →
    prompts), then flashes `boot.img` → `boot` and/or `system.img` → p29
    (`linux`, by-name, sanity-checked ≥20 GiB via /proc/partitions since
    TWRP has no blockdev). Safe default: leaves para = boot-recovery
    (TWRP sticky) — never boots an unverified image unattended. Optional
    `--backup-rootfs` (adb exec-out dd, slow → run-job). Rootfs flash
    prompts unless `--yes`/`wipe p29`.
  - `bin/run-job.sh` — verbatim port (usage strings `bin/`-ified);
    smoke-tested 2026-09-07 (sleep job, rc=0).
- **`flake.nix` devShell** extended: python3+git now also
  `android-tools` (adb 36.0.1) + `usbutils` (lsusb) — the bare host
  PATH has no adb (rule 7). Verified `nix develop` resolves both.
- **`.gitignore`**: `logs/` (run-job state) + `stock-dump/` (device
  partition backups — nvram/IMEI private, never commit).
- **`README.md`**: layout table rows for the new `bin/` scripts;
  "Flashing" section rewritten around `bin/flash-nixos.sh` /
  `bin/boot-switch.sh` with the safety model; "do not flash yet" warning
  retained (still build-level only).

Versions (nothing flashed this session — recorded for the record):
kernel #329 `6.6.0-00048-g188aade698dd` (borrowed, pin
`733c0c7ea74195bd30734f599f37e69febfd38e0`); Mesa 25.0.7 fork; wlroots
0.18.2; gemwl 1.0; Mobile NixOS `2c132754`; nixpkgs
`nixos-26.11pre1031299.0bb7ec54c848`.

Next action: phase 2 — on-glass verification. When the device is next on
the bench: `bin/flash-nixos.sh status` → `boot` → `rootfs --yes` → verify
serial/fbcon → `boot-nixos`; log the outcome here with image hashes.

## 2026-09-07 (evening) — OUTSTANDING-ISSUES SWEEP + DEPLOY MECHANISM: gens 2-5 on glass, every documented failure root-caused; R12/R13/R14 + initrd multi-boot bug + panfrost ordering fixed; rootfs grown to 27.3 GiB; first-ever multi-boot NixOS cycle

Worked the phase-2-on-glass.md TODO from a LIVE gen1 NixOS. Outcome:
**NixOS now boots reliably on every power-on** (a latent initrd bug had
silently made every post-first boot fall back to Debian), rootfs is
27.3 GiB, GPU/audio/vconsole/backlight/battery all green on a clean
boot, and a workstation-style build/switch/deploy loop exists.

Root causes found + fixed (all verified on glass, gens 2-5):
- **R12 — systemd 261 removed the unit `Path=` key.** Every service
  PATH via `serviceConfig.Path = lib.makeBinPath [...]` was ignored
  (journal: `Unknown key 'Path'`): audio (amixer), wifi (modprobe),
  GPU (busybox) all status=127. Migrated 11 uses to the nixpkgs module
  option `path = [ pkgs... ]`. New finding → feasibility doc R12.
- **R13 — make_ext4fs image geometry can't grow past 2x.** Kernel
  online-resize EINVAL at 819200 blocks/25 groups (next sparse_super
  backup group's reserved-GDT entries missing from the resize inode);
  reproduced on the host kernel with the real image. mke2fs geometries
  grow cleanly → `pkgs/make-ext4fs-shim.nix` (make_ext4fs CLI on
  mke2fs) via a `lib.mkAfter` overlay (plain overlay defs lost to mnx's
  overlay list). New system.img = flex_bg/64bit/metadata_csum, grows
  1.5G→27G. Current install grown OFFLINE from TWRP
  (`bin/flash-nixos.sh grow-rootfs` NEW verb: static musl aarch64
  e2fsprogs, e2fsck + resize2fs; fs now 7,164,155 blocks = 27.3 GiB,
  e2fsck -fn clean).
- **R14 — service Type/RemainAfterExit under `unitConfig` ([Unit]) is
  ignored** → oneshots ran Type=simple and deactivated on exit; moved
  to serviceConfig. New finding → feasibility doc R14.
- **panfrost boot ordering**: probed at modules-load (16 s) before GPU
  power-on → "gpu soft reset timed out" -110 → no /dev/dri ever.
  Blacklisted at boot + `services/scripts/panfrost-load.sh` retry
  (rmmod+reprobe until renderD128, up to 60 s) as gemwl ExecStartPre.
  gemwl now runs on glass (renderD128). Desktop = first real on-glass
  GPU chain on NixOS.
- **initrd multi-boot bug**: is_nixos used `[ -e profiles/system ]`,
  which fails in the initrd (nix-env profile chain ends in an ABSOLUTE
  /nix/store path; no /nix/store in the initrd namespace). First boot
  only ever worked via nix-path-registration; EVERY later boot fell
  back to Debian. Fixed with readlink-based resolution. Probe mounts
  also now `-o ro,noload` (no 30x journal replay of dirty partitions
  after WDT resets — killed the "orphan cleanup on readonly fs" flood).
- Minor: console.font TER16x32 removed (kernel font, not kbd); a72-up
  opt-in (no wantedBy); boot.growPartition=false (growpart unit was
  failing on the by-label root); wifi `auto` quiet no-op without an
  interface. wifi-internal REMAINS genuinely broken (deep CONSYS issue:
  modules + WMT pwr-on run, wlan0 never appears — "live client resync
  FAIL"/STP-not-ready; needs its own session).

Mechanism (the "workstation" ask): **bin/deploy.sh** — host cross-
builds the toplevel (bounded --max-jobs 8 --cores 8), pins it
(**bin/gc-pin.sh**, per-user gcroots), ships the delta via
`nix copy --to ssh://10.15.19.82`, switches the device system profile
(`nix-env -p /nix/var/nix/profiles/system --set`) + activates.
Generations 2-5 built + deployed this way; old gens stay bootable /
rollback = `deploy.sh rollback`. Host GC hygiene: the operator's
earlier `nix-collect-garbage` swept the whole cross closure (gen3
silently re-cross-compiled ~259 packages) — every deploy is now pinned.

Version lines (rule 0): gens = gen2 `4glxja3x…`, gen3 `81pdlpvx…`,
gen4 `wlyqyp6…`, gen5 `c10qkjdw…` (current);
boot.img p22 now sha `f3050e06…` (fixed initrd; backed up to
stock-dump/); current rootfs fs = 7,164,155 blocks. Kernel unchanged
#329. Deployed through deploy.sh + reboot-verified; device left:
**gen5 booted on p32 (27.3 GiB), para cleared, NixOS default; only
failed unit = gemini-wifi-internal; Debian p29 untouched.**

Next: wifi-internal deep-dive (CONSYS bringup); commit this session's
changes; consider the self-heal profile unit + native on-device
nixos-rebuild plumbing (flake aarch64 outputs) as follow-ups.
## 2026-09-08 (on-device build/switch, gens 29-30) — THE PDA BUILDS + SWITCHES ITSELF: gen30 built AND switched entirely on-device; `nix-shell -p` works against the flake-pinned nixpkgs; device-rebuild.sh + device-repo.sh

Follow-up to the "native on-device nixos-rebuild plumbing" note from
the 2026-09-07 evening sweep. User ask: iterate the config/add programs
on-the-go (no host) and have `nix-shell -p pkg` work on the PDA.

Facts verified on glass gen28 (before any change):
- `/nix/store` is bind-mounted **ro in the MAIN mount namespace** while
  the socket-activated nix-daemon runs in a **private mount namespace
  that sees it rw** (mountinfo: main `ro,…`, daemon ns `rw,…`, different
  mnt ids — the MNX/NixOS read-only-store design). The daemon is the
  only store writer; **root nix clients auto-connect to the daemon when
  the socket exists** (plain `nix-store --add` as root succeeded on
  gen28 — no `store = daemon` line needed).
- The nix module is enabled (nix.conf is generated) but
  `experimental-features` was EMPTY on gen28 → flake builds failed.

Changes (host commit `6b017bd`, deployed as gen29):
- `config/gemini.nix` new "On-device Nix" section:
  `experimental-features = nix-command flakes`; `max-jobs = 2`,
  `cores = 2` (3.6 GiB RAM bound); `sandbox = false` (trusted
  single-user root PDA); daemon build temp on disk via
  `systemd.services.nix-daemon.environment.TMPDIR = /var/tmp` (/tmp is
  a 1.9 GiB tmpfs — a kernel/mesa build needs GBs). Eval receipt:
  putting it under `serviceConfig.environment` failed ("cannot coerce a
  set to a string") — the option is `systemd.services.X.environment`,
  a SIBLING of serviceConfig.
- `nix.nixPath` + nix.conf `nix-path` → the per-user channels dir
  (root's login-shell NIX_PATH + every nix client).
- systemPackages: `git` 2.55.0 + `micro` (device repo + on-the-go
  editing).
- New `bin/device-rebuild.sh` (runs ON the PDA from
  /root/gemini-nixos; verbs status/build/switch PATH/rollback [N]/
  channels/gc; `build` refuses a dirty repo — rule 0) and new
  `bin/device-repo.sh` (host side; seed/push/pull the repo over g_ether
  as a git BUNDLE — no github round-trip, works offline).
- Gotchas hit + fixed during bring-up (all committed):
  `--extra-experimental-features` takes ONE argv token (multi-feature
  values can't survive word-splitting → drop the flag; nix.conf carries
  the features since gen29); bundle-path fetches need an explicit
  refspec (`git fetch bundle main:refs/remotes/host/main` — git won't
  fetch a bundle's implicit HEAD); amending host commits after seeding
  diverges the device clone (reseeded; device had no unique work).

`nix-shell -p` on the device (the ask): `nix-channel` is BROKEN on this
NixOS 26.11 per-user channels layout (EINVAL/"reading symbolic link
…/channels/nixos" against the dangling ~/.nix-defexpr/channels
symlink — nix-channel abandoned). `device-rebuild.sh channels` installs
the channel MANUALLY: the pinned-rev github tarball fetched by nix
itself (`nix-instantiate --eval` of `builtins.fetchTarball`) → symlink
`/nix/var/nix/profiles/per-user/root/channels/nixpkgs` → the SAME
content-addressed store source the flake's fetchTree unpacks to (no
double download) + a GC root. VERIFIED: `nix-shell -p hello --run …`
substitutes + runs on the PDA (note: legacy `-p` builds a stdenv shell
env, so it pulls gcc/binutils from cache each time — cached, but not
free).

On-device build receipts (rule 0):
- gen29 (host-built via deploy.sh): `b54gw7a…` (config change above;
  87 s host build). Device left running it while the repo was seeded.
- **gen30 (DEVICE-built + DEVICE-switched)**: config change made ON the
  device (`8936db5` "add ripgrep" — device git identity mirrored from
  the host), built with `device-rebuild.sh build` → full flake eval on
  the PDA (mnx tarball fetched into the Git cache; config-glue drvs
  compiled locally) → `8vwpdpz…` in ~4.5 min; `device-rebuild.sh
  switch` activated it. Desktop gemwl + lxqt-nested stayed up through
  the switch (NRestarts=0). ripgrep 15.2.0 live. The device commit was
  pulled back to the host (fast-forward) — host main now contains the
  on-the-go work.
- Kernel/boot.img/flash: NONE this session (profile-only switches;
  para untouched).

Device left: gen30 current (`8vwpdpz…`), repo clone /root/gemini-nixos
@ `b58721d` clean, channels pinned to the flake nixpkgs rev
`dc5d91f84032`, desktop up, gens 27-29 selectable for rollback
(`device-rebuild.sh rollback`).

Next: big custom-drv compiles (kernel/mesa) stay on the host/Pi loop
(deploy.sh) — the PDA compiles them only when their sources change
(expect ~30+ min; RAM-bound 2×2 jobs; a zram/swapfile is the open
improvement for desktop-up compiles). Cold-reboot check of gen30 owed.
