# Mobile NixOS for the Planet Computers Gemini PDA (MT6797X).
#
# Build model = NATIVE aarch64 (canonical since the native-aarch64 merge
# 2026-09-08; formerly a branch + worktree, now main): every drv is
# system = aarch64-linux — built on the aarch64 remote builder
# (192.168.49.191, /etc/nix/machines ssh://cjdell@…; run as root with
# `--store local` + `--option builders @/etc/nix/machines --fallback`,
# e.g. bin/deploy.sh build) with cache.nixos.org substitution. NOTE:
# nixpkgs is pinned BY THIS FLAKE (below), not by MNX's npins, since
# 2026-09-08 — to the nixos-unstable CHANNEL snapshot dc5d91f84032
# (26.11pre1068949, cut 2026-09-07), whose FULL aarch64 closure hydra
# built and published (qtbase/qtwayland/systemd/pipewire/…
# narinfos all 200, verified 2026-09-08). Under the old npins rev
# (0bb7ec54c848) those compiled outputs were 404 for x86_64 AND aarch64
# (base closure only — glibc 200, qtbase 404), so Qt6 compiled
# from source every time. The only remaining local compiles are the
# custom drvs: patched mesa (nixpkgs 26.2.2 + T880 delta), wlroots/gemwl pins, the
# kernel, gemini-firmware. The CROSS toplevel model (x86_64 host,
# buildSystem=x86_64-linux) is ABANDONED — it hit the nixpkgs cross
# walls (Qt6CoreTools missing for the desktop scope, etc.; see
# docs/handover-2026-09-07-lxqt-native.md). Native aarch64 drvs hash-
# match nothing cross-built (no shared store paths with pre-merge gens).
#
#   nix build .#packages.aarch64-linux.default  # boot+recovery+system img (native aarch64 drvs)
#   nix build .#packages.aarch64-linux.bootimg  # boot.img only
#   nix build .#packages.aarch64-linux.rootfs   # rootfs.img only
#   nix build .#packages.aarch64-linux.initrd   # stage-1 initrd (size measurement, docs R1)
#
# PREFER THE PLATFORM-DISPATCHED ENTRY POINT (rule 10): it runs this same
# native path on Linux and the Apple-`container` VM path on a Mac, with one
# verb surface (docs/building.md):
#   bash bin/build.sh start <TARGET>   # detached; poll with `wait <TARGET>`
# The darwin-only outputs (the devshell a Mac uses, the CA bundle it mounts)
# live in flake-macos.nix and are merged in at the bottom of `outputs` —
# the Linux outputs above are untouched by them.
#
# Flashing is manual (no fastboot on this device): dd the images to the
# `boot` and `linux` partitions via the patched TWRP, or via adb. See
# README.md and docs/mobile-nixos-port-feasibility.md §3.5.
#
# Mobile NixOS is NOT a flake (no flake.nix, even at branch tip), so on
# nix 2.34 it cannot be a flake input (inputs must contain a flake.nix).
# We fetch the pinned commit as a tarball instead — the same mechanism
# Mobile NixOS itself uses to pin nixpkgs (npins, builtins.fetchTarball).
# The full commit SHA in the URL is the pin (deterministic tarball).
# Nixpkgs is NOT taken from Mobile NixOS's npins anymore (its pin,
# nixos-unstable 26.11pre1031299.0bb7ec54c848, is only base-closure-
# cached on hydra — see the header): this flake pins its own rev below
# and hands it to the MNX eval via the eval shim's `pkgs` argument
# (the shim forbids system + pkgs together; system is carried by pkgs).
{
  description = "Mobile NixOS for the Planet Computers Gemini PDA (MT6797X, LK framebuffer, no DRM)";

  outputs = { self, ... }:
    let
      # Native build target = aarch64 (this branch). drvs are
      # system=aarch64-linux: nix routes them to the 192.168.49.191
      # remote builder (machines file) or substitutes from cache.nixos.org.
      buildSystem = "aarch64-linux";

      # Mobile NixOS source tree, pinned to an exact commit (was: the
      # repos/mobile-nixos submodule). Bump the SHA to update.
      # NOTE: do not bind `inputs` in the outputs pattern on nix 2.34 —
      # the pattern's bindings are treated as input declarations, and a
      # bare `inputs` binding spawns a phantom "flake:inputs" registry
      # lookup that fails ("cannot find flake 'flake:inputs'").
      # nix 2.34: builtins.fetchTarball no longer pins content (its old
      # `sha256` file-hash arg is gone); use fetchTree, which pins the
      # NAR hash of the UNPACKED tree (GitHub tarball bytes are not a
      # canonical representation). Re-verify the narHash when bumping
      # the rev:  nix flake prefetch github:mobile-nixos/mobile-nixos/<rev>
      mnx = builtins.fetchTree {
        type = "tarball";
        url =
          "https://github.com/mobile-nixos/mobile-nixos/archive/2c132754323fc1915e8d21dcfc0ef68ab084c6fb.tar.gz";
        narHash = "sha256-CzwmiKxuh1u+H8hDnrVeUl/fL59PvsgLbr2l0FhqWK0=";
      };

      # Nixpkgs pin (2026-09-08 repin — see the header): the current
      # nixos-unstable CHANNEL snapshot, i.e. the newest rev whose FULL
      # closure hydra published (raw master commits newer than the
      # channel cut only get per-commit trunk-combined coverage — the
      # exact slow situation this repin fixes). Bump by taking the rev
      # behind https://channels.nixos.org/nixos-unstable/git-revision,
      # then re-verify the narHash:
      #   nix flake prefetch github:NixOS/nixpkgs/<rev>
      nixpkgs = builtins.fetchTree {
        type = "tarball";
        url =
          "https://github.com/NixOS/nixpkgs/archive/dc5d91f840324650bac8c379428c7037a416959a.tar.gz";
        narHash = "sha256-VaWGJ6+cIYN2erfSecbRV+4ljI185Ty2wUrXyvQbgOw=";
      };

      # Out-of-tree device + system configuration. `nixpkgs.buildPlatform`
      # = aarch64-linux: native eval (build machine == device arch). The
      # MNX eval shim builds pkgs from its own npins unless `pkgs` is
      # passed; pass ours (system comes from pkgs.stdenv.hostPlatform,
      # and the nixpkgs module re-imports the same source via pkgs.path,
      # so module pkgs == eval pkgs == this rev).
      #
      # `system.configurationRevision` records the git rev of THIS flake
      # tree in every toplevel (rule 0 — a switched generation carries an
      # identity): self.rev = the HEAD sha on a clean git checkout,
      # "<sha>-dirty" when the tree is modified (nix 2.34 suffix — see
      # the 2026-09-09 flake restructure), null only when evaluated from
      # a non-git copy. It shows in `nixos-rebuild list-generations` /
      # `nixos-version --configuration-revision` and makes a host build
      # and an on-device build of the SAME commit converge to the same
      # store path (both evals carry the same rev string). Commit before
      # switching so the generation is a clean commit, not "-dirty".
      configurationRevisionModule = {
        system.configurationRevision = self.rev or null;
      };

      eval = import (mnx + "/lib/eval-with-configuration.nix") {
        pkgs = import nixpkgs { system = buildSystem; };
        device = ./devices/planet-geminipda;
        configuration = [
          { nixpkgs.buildPlatform = buildSystem; }
          configurationRevisionModule
          ./config/gemini.nix
        ];
      };

      inherit (eval) outputs;

      # Minimal busybox initrd (docs R1 — the Mobile NixOS stage-1 does
      # not fit the 16 MiB boot partition). Same derivation the device
      # module wires into mobile.outputs.initrd.
      initrd = (import ./devices/planet-geminipda/initrd.nix) {
        inherit (eval) pkgs;
      };

      # Mesa 26.2.2 + the T880 polygon-list delta (thin override of the
      # pinned nixpkgs mesa; also in the system closure via
      # config/gemini.nix; standalone here for size/iteration checks).
      mesa = eval.pkgs.callPackage ./pkgs/mesa-geminipda.nix { };

      # Phase 4 desktop stack (wlroots 0.18.2 + gemwl + tinytest clients;
      # in the system closure via services/desktop.nix — standalone here
      # for standalone builds/checks). Same callPackage args as the
      # service module, so the flake packages and the system share paths.
      wlroots = eval.pkgs.callPackage ./pkgs/wlroots-geminipda.nix {
        mesaGeminipda = mesa;
      };
      gemwl = eval.pkgs.callPackage ./pkgs/gemwl.nix {
        inherit wlroots;
      };
      # gemini xkb symbols (layout "gemini", UK default / us variant;
      # config/xkb/symbols/gemini, vendored byte-identical from
      # GeminiPDA) as an xkbcommon include dir — wired into the gemwl and
      # gemshell units via XKB_CONFIG_EXTRA_PATH (services/desktop.nix,
      # services/gemshell.nix). Standalone here for keymap checks
      # (xkbcli compile-keymap --layout gemini -I <dir>).
      geminiXkb = eval.pkgs.callPackage ./pkgs/gemini-xkb.nix { };

      # gemini-xkeyboard-config — full xkeyboard-config tree with the gemini
      # layout registered in rules/evdev.xml, so libgnome-desktop's XkbInfo
      # (libxkbregistry) can find it and GNOME Shell actually selects it.
      # Wired into GNOME via XKB_CONFIG_ROOT (services/gnome.nix). Standalone
      # for checks: XKB_CONFIG_ROOT=$out/etc/X11/xkb xkbcli list.
      geminiXkeyboardConfig =
        eval.pkgs.callPackage ./pkgs/gemini-xkeyboard-config.nix { };

      # gemcli — Rust device-control CLI (backlight/battery/A72/WDT/boot/
      # GPU/speaker; script-parity ports, see docs/gemcli.md). In the rootfs
      # closure via services/gemini-pda.nix; standalone for builds/checks.
      gemcli = eval.pkgs.callPackage ./pkgs/gemcli.nix { };
      # gemdemo — minimal GLES 3.1 + ALSA template (docs/gemdemo.md; in
      # the rootfs via services/gemini-pda.nix — standalone builds/checks).
      gemdemo = eval.pkgs.callPackage ./pkgs/gemdemo.nix { };
      # gemshell — the native Wayland compositor + egui desktop shell
      # (docs/gemshell.md; in the system via services/gemshell.nix when
      # services.gemshellDesktop.enable — standalone builds/checks).
      gemshell = eval.pkgs.callPackage ./pkgs/gemshell.nix {
        mesa = mesa;
      };
      # gemini-exodus — GEMINI: EXODUS (Director's Cut): the cinematic
      # spacesynth + GPU stress test restored from the 0.2.0 engine and
      # upgraded (docs/gemini-exodus.md; in the rootfs via
      # services/gemini-pda.nix).
      gemini-exodus = eval.pkgs.callPackage ./pkgs/gemini-exodus.nix { };

      # The macOS half of the flake (darwin-only outputs: the devshell a
      # Mac uses, the CA bundle its build VM mounts). Kept in its own file
      # so the Linux model below is untouched by it — see the merge at the
      # bottom of `outputs` and docs/building.md.
      macos = import ./flake-macos.nix { inherit nixpkgs; };

      # Merge helper for the two halves. `//` alone is SHALLOW, so merging
      # the mac outputs straight in would REPLACE whole shared attrsets:
      # the first attempt swapped `packages` wholesale for the mac one and
      # broke packages.aarch64-linux ("attribute 'aarch64-linux' missing",
      # caught by the 2026-09-17 dispatch test). devShells/packages are
      # merged one level in; the Linux outputs win any other name clash.
      mergeOutputs = linuxOut: macOut:
        linuxOut // macOut // {
          devShells = (linuxOut.devShells or {}) // (macOut.devShells or { });
          packages = (linuxOut.packages or { }) // (macOut.packages or { });
        };
    in
    # The macOS half lives in its own file (flake-macos.nix, via the
    # `macos` binding above): darwin-only outputs — the devshell a Mac
    # uses plus the CA bundle its build VM mounts. It adds no Linux
    # output and changes none; this merge is the whole coupling.
    # mergeOutputs (not `//`, which is SHALLOW and would replace whole
    # shared attrsets — it clobbered packages.aarch64-linux the first
    # time, caught by the 2026-09-17 dispatch test).
    mergeOutputs
    {
      # nixosConfiguration for the device hostname (`networking.hostName`
      # = "gemini", config/gemini.nix) — the flake output that makes the
      # classic NixOS on-device loop work from a clone of this repo
      # (/root/gemini-nixos on the PDA):
      #
      #   nixos-rebuild switch --flake .          # hostname lookup -> .#gemini
      #   nixos-rebuild build|dry-build|list-generations --flake .
      #   nixos-rebuild switch --rollback --flake .
      #
      # nixos-rebuild-ng (the Python nixos-rebuild; the bash one is gone
      # from nixpkgs at this pin) evaluates `nixosConfigurations."gemini"
      # .config.system.build.{toplevel,nixos-rebuild}`, sets the system
      # profile and runs switch-to-configuration — the SAME toplevel
      # `packages.aarch64-linux.toplevel` exposes (mobile.outputs.toplevel
      # defaults to config.system.build.toplevel, so the two attributes
      # are literally the same derivation — verified 2026-09-09), and the
      # same activation bin/device-rebuild.sh / bin/deploy.sh do by hand.
      # Nothing else was needed on the device: nixos-rebuild lands in the
      # system closure by default (system.tools.nixos-rebuild.enable =
      # config.nix.enable, itself default-true at this pin — installer
      # tools are NOT disabled by Mobile NixOS), and /etc/NIXOS + the
      # /nix/var/nix/profiles/system profile were created by the MNX
      # rootfs postBootCommands at first boot.
      #
      # The value is the RAW evalConfig result (eval.eval) — the same
      # shape lib.nixosSystem returns (config/options/pkgs/lib/_module/…),
      # not the eval shim wrapper (which carries a __please-fail throw).
      nixosConfigurations.gemini = eval.eval;

      packages.${buildSystem} = {
        # android-fastboot-images: boot.img + system.img + flash script
        # (no recovery partition on this device; see the device module).
        default = outputs.default;

        bootimg = outputs.android.android-bootimg;
        rootfs  = outputs.generatedFilesystems.rootfs;
        initrd  = initrd; # minimal busybox initrd, for size measurement
        # Self-contained bring-up kernel (Linux v6.6 base + in-repo delta;
        # source model docs/library-deltas.md). Same derivation the device
        # module wires into mobile.boot.stage-1.kernel, exposed for
        # standalone builds/iteration (the toplevel/bootimg consume it).
        kernel  = eval.pkgs.callPackage ./devices/planet-geminipda/kernel { };
        toplevel = outputs.toplevel;
        mesa    = mesa;   # geminipda panfrost fork (Mali-T880, dma-buf import)
        wlroots = wlroots; # wlroots 0.18.2 pin (gemwl's library; R2)
        gemwl   = gemwl;   # GPU-direct fb compositor + tinytest clients
        gemini-xkb = geminiXkb; # xkb layout include dir (XKB_CONFIG_EXTRA_PATH)
        gemini-xkeyboard-config = geminiXkeyboardConfig; # + registry entry (XKB_CONFIG_ROOT, GNOME)
        # Rust device-control CLI (script-parity ports; in the rootfs via
        # services/gemini-pda.nix — standalone build/iteration target).
        gemcli = gemcli;
        # Minimal GLES 3.1 + ALSA template — the hw-interaction skeleton
        # (docs/gemdemo.md; in the rootfs via services/gemini-pda.nix —
        # standalone build/iteration target).
        gemdemo = gemdemo;
        # Native Wayland compositor + egui desktop shell
        # (docs/gemshell.md; in the system via services/gemshell.nix —
        # standalone build/iteration target).
        gemshell = gemshell;
        # GEMINI: EXODUS (Director's Cut) — cinematic spacesynth + GPU
        # stress test (docs/gemini-exodus.md; in the rootfs via
        # services/gemini-pda.nix).
        gemini-exodus = gemini-exodus;
        # READ-ONLY MT6351 fuel-gauge/AUXADC register probe module
        # (claude/power.md step 0). Light build: kernel derivation +
        # modules_prepare + one module, no full kernel rebuild.
        mt6351-probe = eval.pkgs.callPackage ./devices/planet-geminipda/kernel/modules/mt6351-probe {
          kernel = eval.pkgs.callPackage ./devices/planet-geminipda/kernel { };
        };
        # READ-ONLY MT6797 CPU clock/voltage probe (docs/cpu-dvfs.md step 1):
        # ARM PLLs under the vendor 0x1001A semaphore (mcu=1), CPULDO VSRAM,
        # CSPM state, A72 PLL via SMC (smc_b=1). Light build like mt6351-probe.
        mt6797-dvfs-probe = eval.pkgs.callPackage ./devices/planet-geminipda/kernel/modules/mt6797-dvfs-probe {
          kernel = eval.pkgs.callPackage ./devices/planet-geminipda/kernel { };
        };
        # MT6351 fuel gauge as a power_supply (development module, step 2+).
        mt6351-gauge = eval.pkgs.callPackage ./devices/planet-geminipda/kernel/modules/mt6351-gauge {
          kernel = eval.pkgs.callPackage ./devices/planet-geminipda/kernel { };
        };
      };

      # x86_64 host builds of the Rust apps — the nested dev loop runs on
      # the workstation (bin/gemshell-nested.sh). Same derivation source,
      # host nixpkgs (host Mesa/libglvnd). See docs/gemshell.md "Nested
      # mode".
      packages.x86_64-linux = let
        hostPkgs = import nixpkgs { system = "x86_64-linux"; };
      in {
        # nixpkgs' `mesa` does not ship libgbm in its lib output (that is
        # the separate `libgbm` package), unlike the geminipda fork —
        # pass libgbm where the derivation expects the GBM provider.
        gemshell = hostPkgs.callPackage ./pkgs/gemshell.nix {
          mesa = hostPkgs.libgbm;
        };
      };

      # Host tooling stays x86_64 (this flake is evaluated from an x86_64
      # host; the native aarch64 builds go to the remote builder).
      devShells.x86_64-linux.default =
        let
          pkgs = import (mnx + "/pkgs.nix") { system = "x86_64-linux"; };
        in
        pkgs.mkShell {
          packages = with pkgs; [
            python3 # boot.img header inspection
            git
            android-tools # adb/fastboot — host-side flash/recovery tooling (bin/boot-switch.sh, bin/flash-nixos.sh)
            usbutils # lsusb — device-state detection (POC 0e8d:2008, preloader 0e8d:2000, BROM 0e8d:0003, TWRP 18d1:4ee2)
            mtkclient # preloader/BROM recovery tooling (store pkg + Loader DAs for bin/run-mtk.sh; the CDC-ACM patched copy shadows it — docs/disaster-recovery/)
          ];
        };
    }
    macos;
}
