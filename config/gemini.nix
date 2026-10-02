# System configuration for the Gemini PDA (stage-2).
#
# Phase 0/1/3 scope: a headless NixOS that boots from the `linux`
# partition and is reachable over the g_ether USB network (10.15.19.82).
# Desktop: vanilla GNOME on the geminipda-drm KMS device is the DEFAULT
# (services/gnome.nix, 2026-09-10); the native Rust gemshell compositor
# is the co-installed alternative selected per boot by the desktop
# marker (`gemcli session set`), and the fbcon console is the last
# fallback. The nested gemwl/LXQt and Phosh desktops, the COSMIC session
# and niri were REMOVED 2026-09-12 — GNOME + gemshell + console are the
# supported set. (gemwl/services/desktop.nix remains in-tree but is
# force-disabled by the GNOME/gemshell modules; it has no session client
# since the nested sessions were removed.)
{ config, lib, pkgs, ... }:

let
  # Mesa 25.0.7 + geminipda panfrost fork (see pkgs/mesa-geminipda.nix
  # for why this is a standalone derivation, not nixpkgs' mesa).
  mesaGeminipda = pkgs.callPackage ../pkgs/mesa-geminipda.nix { };

  # wine/wine64 shell wrappers (thin; the wine-wow64 stack itself is a
  # standalone device GC root — pkgs/wine-cli.nix header).
  wineCli = pkgs.callPackage ../pkgs/wine-cli.nix { };

  # DOSBox-X with the Gemini keyboard fix (UK table + Fn+1..0 -> F1..F10
  # mapper; docs/desktop-plumbing.md §DOSBox-X).
  dosboxXGemini = pkgs.callPackage ../pkgs/dosbox-x-gemini.nix { };
in
{  

  imports = [
    # Phase 3 device services: gpu-poweron, a72-up, battery-guard,
    # backlight/power CLIs, boot-recovery, wdt-reboot.
    ../services/gemini-pda.nix
    # Audio: PipeWire/WirePlumber/pipewire-pulse root system session,
    # ALSA S16 pinning, speaker-amp output mode (ported from the
    # verified GeminiPDA bring-up).
    ../services/audio.nix
    # Wi-Fi: internal MT6630 CONSYS stack (mtk_wcn + wlan_gen3 + WMT
    # pwr-on) + USB RTL8821CU dongle auto-connect + factory NVRAM
    # (ported from the verified GeminiPDA bring-up).
    ../services/wifi.nix
    # Bluetooth: MT6630 CONSYS hci_stp stack as a persistent service —
    # bluetoothd on the system bus (hardware.bluetooth at this pin),
    # hci0 module bring-up, bluez CLIs + blueman GUI (bring-up story:
    # docs/bluetooth-bringup.md).
    ../services/bluetooth.nix
    # Phase 4 (preview): gemwl — the wlroots-0.18 compositor that owns
    # the LK framebuffer (/dev/gemfb, GPU-direct), with tinytest smoke
    # clients + the pinned wlroots 0.18.2 (pkgs/gemwl.nix,
    # pkgs/wlroots-geminipda.nix). Auto-starts at boot; disable with
    # `systemctl disable gemwl` for a console-only boot.
    ../services/desktop.nix
    # DE-agnostic desktop plumbing (2026-09-10): UPower battery/AC
    # status, brightness sysfs access + standard control CLIs
    # (brightnessctl; volume is wpctl via audio.nix) — the services
    # layer so ANY desktop's own controls work (GNOME today).
    # docs/desktop-plumbing.md.
    ../services/plumbing.nix
    # Power modes (2026-09-10): power-profiles-daemon + the GNOME Power
    # Mode selector, bridged to the A72 cluster — "performance" onlines
    # the A72s, balanced/power-saver powers them down. Patches
    # PPD's placeholder driver to advertise performance (GNOME hides it
    # otherwise). docs/power-modes.md.
    ../services/power-profiles.nix
    # GNOME application suite (2026-09-10): calculator/calendar/maps/
    # clocks/weather/contacts + viewer/etc. as Wayland clients on the
    # system profile, so they show up in the app grid of whichever
    # nested shell is enabled. NOT the GNOME session/shell — that still
    # needs a DRM/KMS device (docs/gnome-feasibility.md).
    ../services/gnome-apps.nix
    # Vanilla GNOME desktop on the KMS device (2026-09-10, DEFAULT ON):
    # the standard NixOS GNOME + GDM modules, which need the geminipda-drm
    # kernel driver (/dev/dri/card0). Mutually exclusive with gemwl, which
    # it force-disables. docs/gnome-feasibility.md.
    ../services/gnome.nix
    # Desktop/session selector (2026-09-10; +gemshell 2026-09-11): a
    # persistent marker (/var/lib/gemini/desktop) picks whether the GNOME
    # GDM session auto-logs in, whether the native Rust compositor
    # (gemshell) owns the panel, or whether to stay on the fbcon console.
    # They are ALL co-installed; only one owns the panel per boot.
    # `gemcli session set gnome|gemshell|console`.
    # docs/desktop-selection.md.
    ../services/desktop-select.nix
    # gemshell — the native Rust Wayland compositor as a boot session
    # (co-installed, marker-selected; inert unless the marker says
    # `gemshell`). docs/gemshell.md.
    ../services/gemshell.nix
    # greetd + tuigreet in place of GDM (2026-10-02): a text greeter on
    # tty1 with a session menu, so desktops can be swapped at login.
    # Toggle: services.geminiGreeter.enable below.
    ../services/greeter.nix
  ];

  system.stateVersion = "26.11";

  # ---- Regional settings: British English / UK (2026-09-11) ----------
  # Hardware is the UK silkscreen unit (shift+3 = £, see the console
  # keymap + config/xkb/symbols/gemini). Match the OS to it:
  #   - time.timeZone writes /etc/localtime (Europe/London, GMT/BST with
  #     automatic DST) and is what systemd-timedated — i.e. GNOME's
  #     Settings -> Date & Time, which reads the same data — reports.
  #   - i18n.defaultLocale sets LANG in /etc/locale.conf for every
  #     session/service; en_GB.UTF-8 gives British English, £ and
  #     dd/mm/yyyy formats.
  #   - extraLocaleSettings pins every LC_* category to GB as well, so a
  #     stray inherited LC_ALL/LANG cannot make one category fall back to
  #     en_US/C.  GNOME gets the region from the session locale; the
  #     gnome-shell side also sets org.gnome.system.locale (see
  #     services/gnome.nix) so the Settings -> Region & Language panel
  #     agrees even before a relogin. [added 2026-09-11]
  time.timeZone = "Europe/London";
  i18n.defaultLocale = "en_GB.UTF-8";
  i18n.extraLocaleSettings = {
    LC_ADDRESS = "en_GB.UTF-8";
    LC_IDENTIFICATION = "en_GB.UTF-8";
    LC_MEASUREMENT = "en_GB.UTF-8";
    LC_MONETARY = "en_GB.UTF-8";
    LC_NAME = "en_GB.UTF-8";
    LC_NUMERIC = "en_GB.UTF-8";
    LC_PAPER = "en_GB.UTF-8";
    LC_TELEPHONE = "en_GB.UTF-8";
    LC_TIME = "en_GB.UTF-8";
  };

  # ---- Desktop: vanilla GNOME is the DEFAULT (2026-09-10) ------------
  # The geminipda-drm KMS device (/dev/dri/card0) plus Mesa kmsro make
  # the STANDARD NixOS GNOME session possible; verified on glass
  # 2026-09-10 (kmscube on card0 -> OpenGL ES 3.1, renderer
  # "Mali-T880 (Panfrost)"). services/gnome.nix runs
  # services.desktopManager.gnome + GDM and force-disables the nested
  # gemwl stack, so exactly one desktop owns the panel.
  # Evidence + on-glass receipts: docs/gnome-feasibility.md.
  services.gnomeDesktop.enable = true;

  # Selector fallback when /var/lib/gemini/desktop is absent (fresh
  # install); `gemcli session set` overrides it persistently.
  services.geminiDesktop.enable = true;
  services.geminiDesktop.mode = "gnome";

  # Login: greetd + tuigreet instead of GDM autologin (2026-10-02). The
  # `gnome` marker mode now means "show the greeter"; pick the session
  # with F3. Set to false to return to GDM + autologin.
  # services/greeter.nix.
  services.geminiGreeter.enable = true;

  # The gemshell compositor is CO-INSTALLED (the marker decides per
  # boot); its units are inert unless `gemcli session set gemshell`.
  # 2026-09-11: build-level (on-glass bring-up checklist owed —
  # docs/gemshell.md).
  services.gemshellDesktop.enable = true;

  # ---- Console --------------------------------------------------------
  # The kernel config bakes the console setup
  # (console=tty0 console=ttyS0,921600n1 earlycon fbcon=rotate:3
  #  fbcon=font:TER16x32) via CONFIG_CMDLINE + CMDLINE_FORCE until the
  # docs-R4 A/B switch happens. Keep the NixOS side consistent with it:
  # console.font stays null (the default) so systemd-vconsole-setup does
  # NOT run setfont — TER16x32 is a KERNEL fbcon font name, not a kbd
  # consolefont, so setting it here made setfont exit 66 and the unit
  # fail (phase-2 TODO P2). The kernel font (what the LCD shows) is
  # untouched by vconsole-setup either way. [fixed 2026-09-07]
  # console.font = "TER16x32";

  # Gemini built-in keyboard layout: UK base + the Fn layer as
  # AltGr/Shift combos (outstanding.md item 3). Vendored verbatim from the sibling
  # GeminiPDA project (build/rootfs-files/keyboard/gemini-uk.map, kbd
  # text format, header `keymaps 0-127`; provenance note in
  # config/keymaps/README). NixOS accepts a store path here — the
  # console module writes KEYMAP=<path> to /etc/vconsole.conf and
  # systemd-vconsole-setup loads it with kbd's loadkeys directly (the
  # device-only busybox .bkmap binary from the Debian rootfs is NOT
  # needed; loadkeys --validate passes on the text map).
  console.keyMap = ./keymaps/gemini-uk.map;

  # Same parameters, declared on the NixOS side too. Inert while the
  # kernel enforces CONFIG_CMDLINE; this is the bridge for dropping
  # CMDLINE_FORCE (LK appends the boot.img cmdline to /chosen/bootargs).
  #
  # bootopt/log_buf_len are NOT for the kernel — they are consumed by
  # LK's platform_parse_bootopt (platform/mt6797/load_image.c:839) from
  # the boot.img cmdline FIELD before handoff. Without "bootopt=" the
  # boot hangs on the LK logo (~15 s, LK-WDT loop) before the kernel
  # console ever appears — observed + bisected 2026-09-07 (a boot.img
  # identical except for the field booted fine). Value copied from the
  # verified GeminiPDA image: bootopt=64S3,32N2,64N2 log_buf_len=4M.
  #
  # g_ether.dev_addr/host_addr stay (they are the NIC's link addresses,
  # not RNDIS): the host-side scripts find the interface by the MAC pair
  # and the device keeps 10.15.19.82 on usb0 — see bin/lib/host.sh.
  boot.kernelParams = [
    "bootopt=64S3,32N2,64N2"
    "log_buf_len=4M"
    "console=tty0"
    "console=ttyS0,921600n1"
    "earlycon"
    "maxcpus=8"
    "nokaslr"
    "fbcon=rotate:3"
    "fbcon=font:TER16x32"
    "g_ether.dev_addr=42:00:15:19:82:01"
    "g_ether.host_addr=42:00:15:19:82:00"
    "clk_ignore_unused"
    "pd_ignore_unused"
    "regulator_ignore_unused"
    "consoleblank=0"
  ];

  # ---- Users / access ---------------------------------------------------
  # The bring-up/admin interface is ssh over the USB NIC as
  # root@10.15.19.82. The login key is **committed in this repo** —
  # keys/gemini_ed25519(.pub), see keys/README.md — and declared here, so a
  # from-scratch reflash needs NO manual key provisioning (the old recipe
  # was: log in with the `toor` password and append the pubkey by hand;
  # that is retired). bin/lib/host.sh defaults every host script to the same
  # file, so nothing important lives outside the repo. Security trade:
  # keys/README.md (deliberate, single-user lab device).
  #
  # The legacy inline pubkey below is the Linux workstation's old
  # ~/.ssh/id_ed25519_gemini: kept for the TRANSITION only, because the
  # rootfs currently on the device still trusts it (and the workstation's
  # own key is not in this repo). Drop it once every device in play has
  # been reflashed with this generation — the repo key is the canonical one.
  users.users.root.openssh.authorizedKeys.keyFiles = [
    ../keys/gemini_ed25519.pub
  ];
  users.users.root.openssh.authorizedKeys.keys = [
    "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIJ1AU4h4b3z6GFRHVRgXCJ5UMfJU5F8B7A38u7migkuh gemini-pda-root (device-ssh.sh, legacy — see comment)"
  ];

  # ---- SSH host key: PINNED (from this repo) ---------------------------
  # The device's sshd host key is the SAME committed keypair, installed as
  # a real 0600 file by the activation script below, and sshd is told to
  # use exactly that one key. Why: a generated host key changes on every
  # from-scratch install, which is what makes ssh scream
  # "REMOTE HOST IDENTIFICATION HAS CHANGED" (and leaves stale known_hosts
  # entries) after a reflash. Pinned => the device identity is stable and
  # keys/known_hosts stays valid forever.
  #
  # Notes:
  #  * hostKeys REPLACES NixOS's default [rsa, ed25519] list, so no other
  #    (randomly generated) host key is offered — clients must speak
  #    ed25519, which every OpenSSH since 6.5 does;
  #  * generateHostKeys is left at its default (true) as a FAILSAFE: the
  #    sshd-keygen unit is conditioned on our file being absent/empty, so it
  #    does nothing normally, and would restore a working (if unpinned)
  #    sshd rather than stop starting if activation ever failed;
  #  * the install must be an activation script, not environment.etc: /etc
  #    entries are symlinks into the world-readable store and sshd refuses
  #    host keys that are group/other-readable. Consequence: the private
  #    key IS in the store (documented in keys/README.md).
  services.openssh.hostKeys = [
    { type = "ed25519"; path = "/etc/ssh/ssh_host_ed25519_key"; }
  ];
  system.activationScripts.gemini-ssh-hostkey = {
    deps = [ "etc" ];
    text = ''
      install -d -m 0755 /etc/ssh
      install -m 0600 ${../keys/gemini_ed25519} /etc/ssh/ssh_host_ed25519_key
      install -m 0644 ${../keys/gemini_ed25519.pub} /etc/ssh/ssh_host_ed25519_key.pub
    '';
  };

  # This PDA is the device called `gemini` everywhere else (hostname was
  # NixOS's default `nixos` in the rootfs; the Debian rootfs it replaces
  # uses `gemini`).
  networking.hostName = "gemini";

  # cjdell = the DEFAULT user of the device (2026-09-09; replaces the
  # original placeholder `gemini` account): the console getty autologins
  # as cjdell, and the DESKTOP session (GNOME via GDM; the gemshell
  # compositor system service) runs as cjdell — HOME=/home/cjdell, the
  # audio session (services/audio.nix) under the same user so the
  # desktop can reach its sockets. Passwordless sudo comes from the
  # wheel NOPASSWD rule below — that is what makes "desktop user but
  # full admin" work: the compositor (gemwl) + device services stay
  # root, cjdell's apps sudo for the root-only CLIs (backlight,
  # battery, gemcli, ...).
  users.users.cjdell = {
    isNormalUser = true;
    uid = 1000; # gemini's old uid (the account replaces it; home is new)
    description = "Default Gemini PDA user (desktop + console)";
    # [added 2026-09-10] the desktop lock screen PAM-authenticates the
    # session user — a locked (passwordless) account can never unlock.
    # Passcode chosen by the user: 0000 (same hash the device has since
    # 2026-09-10; yescrypt). Single-user trusted PDA — the ssh key below
    # and passwordless sudo are the real admin paths.
    hashedPassword = "$y$j9T$rN7mlRnmUJwGrMTOei0xE.$yjjac4vlLlZKFC1MHpXmJYFvy80knuKN7q5a3ZEptE1";
    extraGroups = [
      "wheel" # passwordless sudo (security.sudo.wheelNeedsPassword = false)
      "video" # /dev/dri/card0 (uaccess does not cover the systemd-
      # service desktop — there is no logind session to tag)
      "audio" # /dev/snd* — the PipeWire session runs as cjdell too
      "networkmanager"
      # gemshell compositor (system service, User=cjdell): the evdev
      # nodes (/dev/input) + bluetoothctl (2026-09-11).
      "input"
      "bluetooth"
    ];
    # Same operator key as root — the repo key + the legacy workstation key
    # (see the comment at the root account above): `ssh cjdell@10.15.19.82`
    # debugs the desktop session as the session user (root stays the admin
    # path).
    openssh.authorizedKeys.keyFiles = [
      ../keys/gemini_ed25519.pub
    ];
    openssh.authorizedKeys.keys = [
      "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIJ1AU4h4b3z6GFRHVRgXCJ5UMfJU5F8B7A38u7migkuh gemini-pda-root (device-ssh.sh, legacy — see comment)"
    ];
  };
  security.sudo.enable = true;
  security.sudo.wheelNeedsPassword = false; # cjdell (wheel) = passwordless sudo
  services.getty.autologinUser = "cjdell";

  # ---- Shells ----------------------------------------------------------
  # Explicit bash (bashInteractive) as every account's login shell (ssh
  # root, getty autologin gemini). NixOS's inherited default already
  # resolves to /run/current-system/sw/bin/bash (bash-interactive in the
  # system profile — verified on glass gen9), but pin the store path so
  # the choice never depends on profile composition. [2026-09-08]
  users.defaultUserShell = "${pkgs.bashInteractive}/bin/bash";

  services.openssh.enable = true;

  # No suspend/resume path exists on this unit (only the LK-configured WDT
  # EXRST path self-boots; a software reset powers the PDA off). The silver
  # key reports KEY_SLEEP (mt6351-keys) and would make logind attempt a
  # suspend the device cannot resume from; ESC/On reports KEY_ESC (never
  # KEY_POWER), but keep all three inert. Mirrors the verified Debian
  # drop-in /etc/systemd/logind.conf.d/99-gemini-sidekeys.conf.
  services.logind.settings.Login = {
    HandleSuspendKey = "ignore";
    HandleHibernateKey = "ignore";
    HandlePowerKey = "ignore";
  };

  # Suspend is ALSO disabled at the unit level (2026-09-11). The logind
  # key handling above only intercepts the KEY_SLEEP key event; it does
  # NOT stop anything that calls logind's Suspend() D-Bus method directly
  # — notably GNOME's power plugin (auto-suspend on idle / the Power
  # button action) or a bare `systemctl suspend`. On this bring-up unit
  # s2idle cannot resume (no wake source) and a suspend was observed
  # LOCKING THE SYSTEM UP. Sleep/wake is owned by gemini-sleepd +
  # `gemcli sleep` (the light sleep — docs/power-sleep.md).
  #
  # NixOS at this pin has no `systemd.mask` option. `suppressedSystemUnits`
  # drops the generated /etc/systemd/system symlinks for these upstream
  # units; NixOS generates that whole directory itself (the systemd
  # package installs no /usr/lib/systemd/system to fall back to), so the
  # units genuinely no longer exist and every initiate path fails fast
  # with "Unit … not found" instead of entering a suspend that hangs.
  # (Verified: `readlink result/etc/systemd/system/suspend.target` -> the
  # systemd package's example/ tree, i.e. generated, not a builtin.)
  systemd.suppressedSystemUnits = [
    "sleep.target"
    "suspend.target"
    "hibernate.target"
    "hybrid-sleep.target"
    "suspend-then-hibernate.target"
    "systemd-suspend.service"
    "systemd-hibernate.service"
    "systemd-hybrid-sleep.service"
    "systemd-suspend-then-hibernate.service"
  ];

  # No host firewall on the trusted g_ether link. Beyond that, NixOS's
  # default nftables firewall demands a set of NF_TABLES/NETFILTER_XT_*
  # options as =y, which the (verified) bring-up kernel config ships as
  # =m — the kernel builder's config validator rejects that mismatch.
  networking.firewall.enable = false;

  # ---- Root partition growth -------------------------------------------
  # mnx modules/rootfs.nix sets boot.growPartition = mkDefault true (and
  # fileSystems."/".autoResize = true) for its rootfs. autoResize is
  # what we want: systemd-growfs-root grows the fs to the FULL partition
  # (p32 is already 27.3 GiB; the fs inside the flashed image is small).
  # boot.growPartition is NOT: it runs cloud-utils `growpart` on the root
  # DEVICE to enlarge the *partition*, which is meaningless here (the
  # partition is already full-size and the by-label device path is not
  # something growpart can parse) — its unit failed on every boot
  # ("must supply partition-number"). Disable it; growfs-root does the
  # real fs growth. [fixed 2026-09-07]
  boot.growPartition = false;

  # ---- On-device Nix: self-sufficient build/switch + nix-shell -p ------
  # (2026-09-08) Make the PDA a first-class build/switch target: a repo
  # clone at /root/gemini-nixos can iterate config/programs on the go
  # via `bash /root/gemini-nixos/bin/device-rebuild.sh build|switch` and
  # `nix-shell -p <pkg>`. Since 2026-09-09 the flake also exposes
  # `nixosConfigurations.gemini` (flake.nix), so the stock loop works
  # too: `cd /root/gemini-nixos && nixos-rebuild switch --flake .` —
  # nixos-rebuild is in the closure by default (`system.tools.nixos-
  # rebuild.enable` = `config.nix.enable`, default-true at this pin;
  # the bash nixos-rebuild is gone from nixpkgs — this is the Python
  # nixos-rebuild-ng). It needs nothing beyond what the MNX rootfs
  # postBootCommands set up at first boot (/etc/NIXOS + the system
  # profile). Background facts verified on glass gen28
  # (2026-09-08):
  # - /nix/store is bind-mounted ro in the MAIN mount namespace while
  #   the nix-daemon runs in a PRIVATE mount namespace that sees it rw
  #   (mnt ns + mountinfo verified) — the read-only-store design. The
  #   socket-activated nix-daemon is therefore the only store writer,
  #   and root clients AUTO-connect to it when the socket exists (plain
  #   `nix-store --add` as root succeeded on gen28), so no `store =
  #   daemon` line is needed.
  # - The NixOS `nix` module is enabled (nix.conf is generated) but
  #   experimental-features was EMPTY on gen28 → flake builds failed;
  #   enabled below.
  nix.settings = {
    experimental-features = [ "nix-command" "flakes" ];
    # RAM-bound mobile builds (3.6 GiB total, ~1-2 GiB free with a
    # desktop up): bound the concurrent compilers. Normal on-device
    # switches are config-glue + cache.nixos.org substitutions (the
    # pinned nixpkgs rev IS the hydra-built channel snapshot — golden
    # rule 9), so they are quick; the custom drvs (mesa fork, kernel,
    # wlroots/gemwl, firmware, gemcli) only compile when their
    # sources change — long on the A72/A53 mix, prefer the host
    # deploy.sh loop for those.
    max-jobs = 2;
    cores = 2;
    # Trusted single-user root PDA (same trust model as the rest of the
    # device): sandbox buys nothing here and risks lean-mobile-kernel
    # namespace edge cases; store writes are daemon-mediated either way.
    sandbox = false;
  };
  # Big on-device compiles (kernel/mesa when their sources change) need
  # GBs of build-dir space; /tmp is a 1.9 GiB tmpfs (RAM). Point the
  # nix daemon's build temp at the disk rootfs (/var/tmp — 20 GiB free
  # on gen28). [2026-09-08]
  systemd.services.nix-daemon.environment.TMPDIR = "/var/tmp";
  # `nix-shell -p <pkg>` / `<nixpkgs>` resolution. NixOS's default
  # NIX_PATH points at a `channels/nixos` entry that does not exist on
  # this device (falls through to the unpinned flake registry = master
  # nixpkgs — rule-9 violation + package drift). Point it at the
  # per-user channels dir, populated by `device-rebuild.sh channels`
  # with the SAME rev the flake pins (dc5d91f84032 — cache-healthy by
  # construction, package versions match the running system). nixPath
  # drives the login-shell NIX_PATH; belt+braces: the same list as the
  # nix.conf `nix-path` so non-login contexts (device-ssh.sh, the
  # gemshell system-service session) resolve <nixpkgs> too. [2026-09-08]
  nix.nixPath = [
    "nixpkgs=/nix/var/nix/profiles/per-user/root/channels/nixpkgs"
    "nixos-config=/etc/nixos/configuration.nix"
  ];
  nix.settings."nix-path" = [
    "nixpkgs=/nix/var/nix/profiles/per-user/root/channels/nixpkgs"
    "nixos-config=/etc/nixos/configuration.nix"
  ];

  # ---- Overlays (2026-09-08: pruned to the ones still needed) ----------
  # The cross-build workaround overlays (systemd withLibBPF=false,
  # ffmpeg/ffmpeg-headless withCudaLLVM=false, openblas dynamicArch=false,
  # the libfm/libfm-extra/menu-cache autoreconf AM_GLIB_GNU_GETTEXT fix)
  # were written against the OLD npins rev (26.11pre1031299) and the
  # abandoned cross model [2026-09-07]. After the 2026-09-08 nixpkgs
  # repin (flake.nix -> channel snapshot dc5d91f84032) they were REMOVED:
  # hydra built the un-overridden defaults for aarch64-linux in that
  # channel (their narinfos are 200), so each override only forced
  # non-cached drv hashes down its dependency subtree (systemd -> the
  # whole closure, ffmpeg -> pipewire/alsa-plugins, openblas ->
  # numpy/python3, libfm-* -> pcmanfm-qt) and cost cache misses. If a
  # real build of this rev hits one of the old bugs, re-add the specific
  # override (with a date + receipt). The systemd-bpf LSM units
  # (restrict-fs, io_uring restrictions) are irrelevant for a trusted
  # single-user PDA.
  # The Wi-Fi firmware blobs (MediaTek WMT/ROMv3/CONSYS + Realtek
  # rtw88) are vendor-furnished and tracked in pkgs/gemini-firmware/
  # (unfreeRedistributable) — the same blobs the verified device runs.
  nixpkgs.config.allowUnfree = true;

  # List-typed option: definitions from all modules (including Mobile
  # NixOS's own overlays) are concatenated in module order, so a plain
  # definition here appends after theirs.
  # Overlays run in list order (last wins); the mnx base modules also
  # append overlays here, so mkAfter guarantees OUR entries (the shim in
  # particular) come after theirs and actually take effect.
  nixpkgs.overlays = lib.mkAfter [
    (final: prev: {
      # mobile-nixos builds the rootfs image with the Android
      # make_ext4fs tool, whose ext4 geometry the kernel can only
      # online-grow to exactly 2x the image size (then EINVAL — the fs
      # stops at 819200 blocks/25 groups; observed on glass AND
      # reproduced on the host kernel with the real image). Replace it
      # with an mke2fs shim that produces a normal growable ext4
      # (defaults: flex_bg/64bit/metadata_csum) — every mke2fs geometry
      # tested grows 1.5G -> 27G online cleanly. See
      # pkgs/make-ext4fs-shim.nix (R13).
      make_ext4fs = final.callPackage ../pkgs/make-ext4fs-shim.nix { };
    })
  ];

  # ---- Networking: the USB gadget NIC (CDC-ECM) ----------------------
  # The kernel auto-instantiates the legacy g_ether gadget (CONFIG_USB_ETH=y;
  # the MACs come from the g_ether.dev_addr/host_addr params above). No
  # userspace configfs setup is needed, and the driver binds the UDC by
  # itself whenever the cable/role switch presents it — which is exactly why
  # the precomposed gadget is kept instead of a configfs service (a
  # userspace gadget can lose the link if its ordering is wrong, and usb0 is
  # the only debug path into this unit).
  # [2026-09-17] It now enumerates as **CDC-ECM** (0525:a4a1), not RNDIS:
  # RNDIS support is gone from the kernel config (bin/prune-kernel-config.sh
  # section 9c) because only Linux/Windows drive it in-box and macOS — the
  # host this change is for — has no RNDIS driver at all. Linux (cdc_ether)
  # and macOS (com.apple.driver.usb.cdc.ecm) both bind the ECM config
  # natively, and the host scripts find the interface by MAC. Receipts +
  # the Mac-side workflow: docs/usb-network.md.
  # The interface is usb0, on the fixed 10.15.19.0/24 link with the
  # host side at 10.15.19.1 (bin/net-up.sh).
  networking.interfaces.usb0.ipv4.addresses = [
    { address = "10.15.19.82"; prefixLength = 24; }
  ];
  # Default route via the USB host is a FALLBACK only: metric 1000 sits
  # behind NetworkManager's Wi-Fi default route (metric ~600), so the
  # device uses Wi-Fi for internet whenever it is connected, and the USB
  # host only when it is the sole uplink (bin/usb-tether-nat.sh). With
  # no metric this static route (metric 0) won every lookup and all
  # traffic died at 10.15.19.1 when the host was not NATing.
  # [atzero unit, 2026-10-02]
  networking.defaultGateway = {
    address = "10.15.19.1";
    interface = "usb0";
    metric = 1000;
  };
  networking.nameservers = [ "1.1.1.1" ];

  # Static link only — no DHCP client needed.
  networking.dhcpcd.enable = false;
  # Nameservers for the g_ether host link (fed to resolvconf as the
  # "static" interface; NetworkManager's DHCP nameservers merge in
  # front when a wifi network is up — wifi.nix, NM mode).

  # ---- Graphics (phase 4 preview; ONE Mesa since 2026-09-10) ---------
  # pkgs/mesa-geminipda.nix is a thin override of the pinned nixpkgs
  # Mesa 26.2.2 (panfrost + the T880 polygon-list delta), wired as
  # hardware.graphics.package. hardware.graphics then supplies the single
  # glvnd ICD (/run/opengl-driver/share/glvnd/egl_vendor.d/50_mesa.json,
  # absolute library_path baked in) plus the DRI/GBM driver dirs;
  # libglvnd supplies the client libs (libEGL.so.1 / libGLESv2.so.2) that
  # dispatch to it. This replaced the old setup (hardware.graphics
  # unintendedly ON *and* a fork /etc ICD), where glvnd loaded BOTH mesa
  # 26.2.2 and the 25.0.7 fork in one process — a compositor had
  # libgallium-25.0.7 + libgallium-26.2.2 loaded at once and could not
  # import buffers between them ("import for wrong devices"). Receipts:
  # docs/handover-2026-09-10-gnome-perf-touch.md, docs/library-deltas.md.
  hardware.graphics = {
    enable = true;
    package = mesaGeminipda;
  };
  # ---- Browsers (2026-09-08) -------------------------------------------
  # Real Google Chrome (nixpkgs google-chrome 152.0.7977.82, native
  # aarch64 deb — arm64 Linux stable only ships since ~2026-07, and the
  # repo's nixpkgs pin already carries it with aarch64 support; unfree,
  # allowed above) + Firefox 155.0.1. Both go through the SAME client GL
  # path the Debian rootfs verified (Firefox WebGL worked there on the
  # fork mesa): nixpkgs' firefox wrapper ships libglvnd on LD_LIBRARY_PATH
  # (withGlvnd defaults true on Linux), so its dlopen of libEGL.so.1
  # dispatches via the /run/opengl-driver 50_mesa.json ICD above ->
  # mesa-geminipda (26.2.2) -> panfrost renderD128. Two gaps had to
  # close first (both 2026-09-08, on-glass failures): (1) the fork was
  # built with NO wayland EGL platform (-Dplatforms=) so browser GL had
  # no display path at all — pkgs/mesa-geminipda.nix now builds
  # surfaceless,wayland; (2) the fork libgbm needs GBM_BACKENDS_PATH to
  # find dri_gbm.so (browser glxtest GPU probe). Firefox must be
  # DESKTOP-launched (session env).
  environment.systemPackages = [
    pkgs.libglvnd
    # --no-sandbox (2026-09-08, comment updated 2026-09-09): chrome was
    # first needed when the desktop ran as ROOT (system service,
    # HOME=/root) — the zygote refuses euid 0 without it. The desktop
    # now runs as the cjdell user, but the sandbox still needs a userns
    # or SUID helper this lean kernel/stack does not provide, so the
    # flag stays until a sandboxed run is verified on glass. Accepted
    # on this trusted single-user PDA (same trust model as the rest of
    # the cjdell desktop session).
    (pkgs.google-chrome.override { commandLineArgs = "--no-sandbox"; })
    pkgs.firefox
  ] ++ [
    # On-device iteration (2026-09-08): git for the device repo clone at
    # /root/gemini-nixos (bin/device-rebuild.sh + bin/device-repo.sh) and
    # micro as a small terminal editor for on-the-go config tweaks
    # (swap for vim/neovim if preferred).
    pkgs.git
    pkgs.micro
    pkgs.ripgrep  # on-the-go add (gen30 demo, 2026-09-08)
  ] ++ [
    # wine/wine64 on the shell PATH (gen36, 2026-09-09): thin wrappers
    # exec'ing the standalone wine-wow64 launcher /root/wine-x86/wine-wow
    # (the stack itself stays a standalone GC root, NOT a system
    # package — docs/wine-d3d.md §5).
    wineCli.wine
    wineCli.wine64
  ] ++ [
    # DOS emulation (2026-09-11): DOSBox-X with the Gemini keyboard fix —
    # UK keyboard table + an Fn+1..0 -> F1..F10 mapper (the Fn layer is XKB
    # level 3, invisible to DOSBox-X's scancode-based input).  The wrapper
    # keeps the upstream .desktop; see docs/desktop-plumbing.md §DOSBox-X.
    dosboxXGemini
  ];

  # The glvnd ICD now comes from hardware.graphics above (the compiled-in
  # libglvnd scan list includes /run/opengl-driver/share/glvnd/
  # egl_vendor.d). Do NOT add a second /etc/glvnd manifest here: a second
  # mesa vendor is exactly the dual-gallium bug this config fixes.

  # The Mobile NixOS stage-1 is disabled for this device (docs R1: its
  # initrd cannot fit the 16 MiB boot partition). The boot ramdisk is a
  # minimal partition-scanning busybox initrd (devices/planet-geminipda/
  # initrd.nix). Nothing in stage-1 (USB gadget, boot GUI, boot SSH) is
  # available; serial (ttyS0) + fbcon are the bring-up interfaces.
  #
  # Also stop NixOS from building its own initrd into the system closure
  # (we boot from the LK-packed boot.img, never from an NixOS initrd).
  boot.initrd.enable = false;

  # Keep volatile state out of the 27.7 GiB rootfs for now.
  fileSystems = {
    "/tmp" = {
      device = "tmpfs";
      fsType = "tmpfs";
      neededForBoot = true;
    };
  };

  # Skip the documentation HTML build.
  documentation.enable = false;

  # kexec-based stage-0 recovery is not usable on this hardware.
  mobile.quirks.supportsStage-0 = lib.mkForce false;
}
