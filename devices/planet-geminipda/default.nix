# Device definition for the Planet Computers Gemini PDA (MT6797X "aeon").
#
# Out-of-tree device (path-based, per mobile-nixos lib/release-tools.nix).
# See docs/mobile-nixos-port-feasibility.md for the full rationale; the
# boot geometry below is measured from the verified bring-up boot.img.
{ config, lib, pkgs, ... }:
let
  # Minimal busybox initrd (docs R1). See initrd.nix for the rationale;
  # this is the ramdisk the stock LK copies to 0x45000000.
  # gaugeModule: mt6351-gauge.ko for the initrd's charger-mode screen
  # (charger.sh). Built from the same kernel derivation as
  # mobile.boot.stage-1.kernel below, so vermagic matches.
  minimalInitrd = pkgs.callPackage ./initrd.nix {
    gaugeModule = pkgs.callPackage ./kernel/modules/mt6351-gauge {
      kernel = pkgs.callPackage ./kernel { };
    };
  };
in
{
  imports = [
    # Out-of-tree MT6797 SoC fragment (see modules/hardware-soc.nix
    # assertion: it only requires the option to exist).
    ../../modules/hardware-soc-mediatek-mt6797.nix
  ];

  mobile.device.name = "planet-geminipda";
  mobile.device.identity = {
    name = "Gemini PDA";
    manufacturer = "Planet Computers";
  };
  # Phase 0 of the port: artifacts build and match the bring-up contract,
  # not yet verified on glass.
  mobile.device.supportLevel = "best-effort";

  mobile.hardware = {
    soc = "mediatek-mt6797";
    ram = 1024 * 4; # 4 GiB LPDDR4
    # 2160x1080 portrait panel; displayed rotated (fbcon=rotate:3).
    screen = { width = 1080; height = 2160; };
  };

  # ---- Boot: Android boot.img v0 consumed by the stock MediaTek LK ----
  #
  # Measured load addresses (docs §3.1), reproduced via base + offsets:
  #   kernel  0x40200000   (2 MiB-aligned, arm64 rule)
  #   second  0x40f00000   (unused, size 0)
  #   ramdisk 0x45000000
  #   tags    0x44000000   (LK copies the DTB here, then patches it)
  #   page    2048
  mobile.system.type = "android";
  mobile.system.android = {
    # There is no fastboot on this device; deployment goes through the
    # patched no-swipe TWRP or plain `dd` over adb (docs §3.5).
    # `flashingMethod` only drives generated scripts/docs, so the value
    # is cosmetic (an upstream "manual"/"twrp" value would be nicer).
    flashingMethod = "lk2nd";
    device_name = "geminipda";

    # No dedicated `recovery` partition on this device (recovery is the
    # patched no-swipe TWRP in the boot3 slot, entered via the `para`
    # partition's `boot-recovery` command, docs §3.5). Keep the default
    # output to boot.img + system.img only.
    has_recovery_partition = false;

    # The NixOS rootfs goes on the big `linux` partition (p27, ~58 GiB).
    # Since the 2026-09-10 repartition TWRP (p1 recovery) + NixOS are the
    # ONLY systems: Android's system/cache/userdata + the Debian `linux`
    # rootfs + boot2/boot3 were reclaimed into this single partition, every
    # boot-critical/hardware partition (p1..p26 + flashinfo) kept at its
    # exact offset. See docs/repartition-android-space.md §12.
    system_partition_destination = "linux";

    bootimg.flash = {
      offset_base    = "0x40000000";
      offset_kernel  = "0x00200000";
      offset_second  = "0x00f00000";
      offset_ramdisk = "0x05000000";
      offset_tags    = "0x04000000";
      pagesize       = "2048";
    };

    # LK requires the DTB appended to the (gzip) kernel payload; it scans
    # the last 2 MiB for the FDT magic and ignores the header dt_size.
    # `bootimg.nix` cats these onto the kernel image; paths are relative
    # to the kernel output root (buildDTBs = true in the kernel module).
    appendDTB = [
      "dtbs/mediatek/mt6797-gemini-pda.dtb"
    ];
  };

  # ---- Kernel config validation ------------------------------------------
  # Mobile NixOS's default structured kernel config (kernel-config.nix,
  # "Needed for firewall" block) unconditionally requires a set of
  # netfilter/nftables options as strict =y (it shadows `module` with
  # `yes`). The verified bring-up config ships those as =m, and no
  # firewall runs on this device (trusted g_ether link), so relax them to
  # the plain `module` meaning (=y or =m, warning only).
  #
  # Two merge subtleties:
  # - mkAfter: the default list comes from a later-evaluated module's
  #   plain assignment, so append after it to keep our entries in the list.
  # - lib.mkForce: the structured config is combined with lib.mkMerge,
  #   which conflicts on same-key leaf values of equal priority; forcing
  #   our items makes them win wholesale over the default block's.
  mobile.kernel.structuredConfig = lib.mkAfter [
    (helpers: with helpers; {
      BRIDGE                = lib.mkForce module;
      BRIDGE_NETFILTER      = lib.mkForce module;
      IP6_NF_IPTABLES       = lib.mkForce module;
      IP6_NF_RAW            = lib.mkForce module;
      NETFILTER_XT_MATCH_HASHLIMIT = lib.mkForce module;
      NETFILTER_XT_MATCH_PHYSDEV   = lib.mkForce module;
      NETFILTER_XT_MATCH_SOCKET    = lib.mkForce module;
      NF_TABLES_BRIDGE      = lib.mkForce module;
      NFT_BRIDGE_META       = lib.mkForce module;
      NFT_BRIDGE_REJECT     = lib.mkForce module;
      NFT_REJECT            = lib.mkForce module;
      NFT_REJECT_IPV4       = lib.mkForce module;
      NFT_REJECT_IPV6       = lib.mkForce module;
      NFT_REJECT_NETDEV     = lib.mkForce module;
      NFT_SOCKET            = lib.mkForce module;
      NFT_TPROXY            = lib.mkForce module;
      NF_TPROXY_IPV6        = lib.mkForce module;
    })
  ];

  # ---- Kernel / initrd (docs R1: 16 MiB boot partition) ----
  #
  # The android system type reads the kernel package from this option
  # (via the stage-0 specialization), and NixOS uses it as the system
  # kernel (boot.kernelPackages -> system.modulesTree ->
  # /run/booted-system/kernel-modules, where modprobe/udev look for
  # /lib/modules/$(uname -r)).
  #
  # SELF-CONTAINED kernel (2026-09-08 — the borrow is retired): built
  # from the published Linux v6.6 base + the tracked bring-up delta
  # (kernel/default.nix; source model per docs/library-deltas.md). The
  # source content equals the on-glass kernel #329 tree
  # (geminipda-bringup @ 188aade69), so this is the same kernel the
  # bring-up validated, rebuilt in-nix; uname -r is the self-consistent
  # "6.6.0" (no git in the source), and sramldo-smc is built in-tree
  # [2026-10-02: base bumped to v6.6.157 stable; uname -r is now
  # "6.6.157" — see kernel/default.nix]
  # with matching vermagic. Everything boot-critical (mmc block, ext4,
  # ...) is =y in the config; GPU/wifi modules (panfrost, mtk_wcn,
  # wlan_gen3, rtw88_*) are =m, loaded in stage-2 from this kernel's
  # module tree.
  mobile.boot.stage-1.kernel = {
    package = pkgs.callPackage ./kernel { };
    modular = false;
  };

  # The Mobile NixOS stage-1 initrd does not fit: measured 9.66 MiB xz
  # (39 MiB unpacked) for this kernel/config, which puts the boot image
  # at ~23.3 MiB against the 16 MiB cap (docs R1). Use the minimal
  # partition-scanning busybox initrd instead — the same design as the
  # verified bring-up image (~1.3 MiB gzip).
  #
  # Disabling stage-1 also keeps its initrd out of the system closure
  # (with stage-1 enabled it would leak in via system.build.initialRamdisk).
  mobile.boot.stage-1.enable = false;
  mobile.outputs.initrd = "${minimalInitrd}/initrd";
}
