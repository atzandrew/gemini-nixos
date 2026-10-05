# Kernel derivation for the Gemini PDA bring-up kernel — SELF-CONTAINED.
#
# [rewritten 2026-09-08] Source model: "published base + tracked
# file-tree delta" (docs/library-deltas.md, kernel entry) — the same
# two-layer decision as Mesa, with the delta as a plain file tree
# instead of a patch series (agents edit source files, not patch text;
# patches are banned for this repo's kernel).
#
#   base  = the published upstream Linux **v6.6.157** stable tarball
#           (cdn.kernel.org linux-6.6.157.tar.gz), fetched by hash.
#           [2026-10-02] Bumped from v6.6: the 61 delta files that exist
#           upstream were 3-way merged (base v6.6, ours = delta, theirs =
#           v6.6.157) so stable fixes are kept, not reverted; 41 were
#           untouched by stable, 18 merged clean, 2 had add/add conflicts
#           resolved by keeping both sides (mtk_drm_drv.c compatible list,
#           hci.h quirk enum). Of the 462 delta-only files, one needed
#           a fix: drv_bt/linux/hci_stp.c no longer sets hdev->dev_type
#           (6.6.y stable removed AMP support and the field).
#           Pre-bump state: git fc45e66. The v6.6 base was
#           the exact base the borrowed bring-up kernel grew from: the
#           geminipda-bringup line is v6.6 (commit
#           ffc253263a1375a65fa6c9f62a893e9767fbebfa) + 49 linear commits,
#           and the tarball was verified byte-identical to `git archive
#           v6.6` of that tree (2026-09-08). The same commit is pinned as
#           a git submodule at kernel/base (read-only pointer for local
#           base-source reading: `git submodule update --init --depth 1
#           kernel/base`).
#
#   delta = ./delta — the plain kernel-relative files the bring-up
#           branch changes over v6.6 (457 added + 55 modified, 0 deleted,
#           0 renamed; copy-replace is exact). Identity check:
#           v6.6-base + delta == geminipda-bringup @
#           d5bcde1a02d5ffba95f88145c983abba1edbc546 (2026-09-09: #329
#           tree + hci_stp BT driver); 05bbb33d39 (2026-09-09: + mtk_wcn
#           wake-before-send on every BTIF transport write - BT rx-stall
#           fix, see docs/bluetooth-bringup.md), verified 2026-09-09;
#           7dce5d17 (2026-09-09: + HCI_QUIRK_LE_INIT_BEST_EFFORT - MT6630
#           refuses LE write cmds at open (0x20); best-effort le_init3 so
#           BR/EDR comes up); 16e6d817 (2026-09-09: + BTIF WAK keep-awake
#           heartbeat - MCU dozes inside the reply window -> HCI cmd-sync
#           desync; pulse ~30 ms while BT traffic is active); f6b135a3
#           (2026-09-09: broaden to HCI_QUIRK_EXT_INIT_BEST_EFFORT - also
#           hci_init4/le_init4: MT6630 over-advertises (MWS etc.) and
#           refuses the extra commands); 440be2a1 (2026-09-09: gate the
#           WAK heartbeat on the BTIF link being OPEN - write-triggered
#           hold was too short for scans/connections); 7af6ee9c (2026-09-09:
#           record LMP_HOST_LE + HCI_LE_ENABLED locally when the chip refuses
#           0x200d - unblocks mgmt/LE discovery), verified byte-for-byte 2026-09-09
#           (previous identity: 188aade69 = the on-glass kernel #329
#           tree, verified 2026-09-08); c8f0787d (2026-09-10: + desktop
#           battery plumbing — bq25890 registers a voltage-derived
#           Battery power_supply next to the charger so upower/DEs see
#           battery state; no fuel gauge on this board; see
#           docs/desktop-plumbing.md), verified 2026-09-10; 06fd13e11
#           (2026-09-10: + drivers/power/reset/mt6797-power.c — LK's
#           full WDT restart sequence + MT6351 RTC BBPU poweroff, so
#           systemctl reboot/poweroff really work; maps the shared
#           TOPRGU block with devm_ioremap() so mtk_wdt still probes —
#           see docs/power-states.md), verified 2026-09-10.
#           [2026-09-10k] The delta DIVERGES from fork rev 06fd13e11 in
#           four files, all from the 2026-09-10 DRM/KMS bring-up plus this
#           session's fix to geminipda-drm.c:
#             - delta-only: drivers/gpu/drm/tiny/{geminipda-drm.c,Kconfig,
#               Makefile} (the LK-framebuffer DRM/KMS driver);
#             - modified:   arch/arm64/boot/dts/mediatek/mt6797-gemini-pda.dts
#               (the planet,geminipda-drm node).
#           [2026-10-02] Fifth delta-local change: drivers/power/supply/
#           bq25890_charger.c — glitch rejection + EMA smoothing of the
#           voltage-derived battery capacity (docs/desktop-plumbing.md).
#           geminipda-drm.c additionally drops the redundant per-pixel alpha
#           loop that pinned the DRM commit kworker at ~100 % CPU (see
#           docs/session-log.md 2026-09-10k).
#           bin/sync-kernel-delta.sh now REFUSES to clobber a diverged
#           delta (it lists these four and aborts; FORCE=1 overwrites).
#           Regenerate after fork commits with bin/sync-kernel-delta.sh.
#
#   config = ./config — the LEAN device config (default): generated by
#           bin/prune-kernel-config.sh from config.full-329 (the exact
#           #329 bring-up .config), which drops hardware that can never
#           exist on this unit (foreign SoCs/buses, media, debug bloat).
#           Both configs are the fbcon / EXCLUDE_DISPLAY build (CORE RULE
#           5): no mediatek-drm/panel stack — the rule-5 gate below
#           asserts that on the tracked config. Still carry
#           CONFIG_CMDLINE_FORCE=y with the full verified cmdline;
#           moving the cmdline into `boot.kernelParams` (docs R4) is a
#           deliberate A/B step on hardware, not part of the initial port.
{ mobile-nixos
, buildPackages
, lib
, fetchurl
, ...
}:

let
  # ---- Published base: upstream Linux v6.6 release tarball -----------
  # Same fetch idiom as the mesa fork base (pkgs/mesa-geminipda.nix): a
  # fixed-output fetchurl of the canonical published archive, pinned by
  # file hash — no vendored source in this repo. Re-verify when bumping:
  #   nix store prefetch-file --json https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.6.157.tar.gz
  # (sha256 = file hash, SRI form).
  baseTar = fetchurl {
    url = "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.6.157.tar.gz";
    hash = "sha256-ivJdzP2ko+vxcSL/3BGSHq8ie2oOmC4Vlqzz+MkNpkI=";
  };

  # ---- Rule-5 gate (CORE): never build the display landmines ---------
  # Eval-time assert on the tracked config, so no future config edit can
  # silently reintroduce the mediatek-drm/DSI/panel stack that burns the
  # NT36672 matrix (AGENTS.md golden rule 5). Two tiers:
  #   - hardBan: the panel-initializing stack — mediatek-drm (the
  #     pipeline owner), mtk-mmsys, the mtk PHYs. Banned in ANY state
  #     (=y or =m; the stale pre-fbcon config.aarch64 carried these as
  #     =m, which is exactly what the rule forbids merging into the
  #     image). The verified #329 config has them absent / "is not set".
  #   - yBan: tps65132-regulator (the display bias rail). The VERIFIED
  #     #329 config ships it as =m (inert without a DRM consumer — the
  #     on-glass kernel has it; nothing probes it, panel never biases),
  #     so =m is the blessed state; a =y (boot-active) would only appear
  #     alongside a full display config and is banned.
  configText = builtins.readFile ./config;
  hardBan = [
    "CONFIG_DRM_MEDIATEK"
    "CONFIG_MTK_MMSYS"
    "CONFIG_PHY_MTK_MIPI_DSI"
    "CONFIG_PHY_MTK_HDMI"
  ];
  yBan = [ "CONFIG_REGULATOR_TPS65132" ];
  # Line scan, NO regex over the whole file: builtins.match with a
  # leading ".*" on the 288 KB config stack-overflows the evaluator
  # (observed 2026-09-08). split on a plain "\n" is regex-safe; the
  # matched separators come back as empty LISTS (builtins.split
  # capture-group shape), non-matches as strings — keep the strings.
  configLines = builtins.filter builtins.isString (builtins.split "\n" configText);
  hasSym = sym: setting:
    builtins.any (line: line == sym + "=" + setting) configLines;
  bannedHit =
    builtins.any (sym: builtins.any (s: hasSym sym s) [ "y" "m" ]) hardBan
    || builtins.any (sym: hasSym sym "y") yBan;

  # Materialize the build tree: unpack the published base + copy-replace
  # the tracked delta over it (the delta files ARE the bring-up source;
  # nothing else differs from upstream v6.6). Runs on the build machine
  # (aarch64), so buildPackages tools. Also stage the CONSYS ROM patches
  # into the tree's firmware/ dir: both configs set
  # CONFIG_EXTRA_FIRMWARE_DIR="firmware" (relative → resolved against
  # $(srctree)), and the #329 config originally pointed at an absolute
  # GeminiPDA host path (a dependency this repo must not have).
  kernelSrc = buildPackages.runCommand "linux-geminipda-6.6.157-src" {
    nativeBuildInputs = [ buildPackages.gnutar buildPackages.gzip ];
  } ''
    mkdir -p $out/firmware
    ${buildPackages.gzip}/bin/gunzip -c ${baseTar} | ${buildPackages.gnutar}/bin/tar -x -C $out --strip-components=1
    echo ":: Copying tracked bring-up delta over base (${toString (builtins.length (builtins.attrNames (builtins.readDir ./delta)))} top entries)"
    cp -a ${./delta}/. $out/
    echo ":: Staging CONSYS ROM patches (CONFIG_EXTRA_FIRMWARE)"
    cp ${../../../pkgs/gemini-firmware}/ROMv3_patch_1_0_hdr.bin \
       ${../../../pkgs/gemini-firmware}/ROMv3_patch_1_1_hdr.bin \
       $out/firmware/
  '';

  # Out-of-tree A72 bring-up module (sramldo-smc.ko), built against this
  # exact tree in postInstall and shipped in the module dir (phase 3).
  # `let` so the postInstall string can interpolate them (Nix string
  # interpolation does not see sibling derivation attrs). modDirVersion
  # is the builder's `modDirify version`, which is the identity here.
  sramldoModuleSrc = ./modules/sramldo-smc;
  modDirVersion = "6.6.157";
in

assert lib.assertMsg (!bannedHit) ''
  kernel config ./config violates CORE RULE 5: it enables a banned
  display-landmine symbol (mediatek-drm/mtk-mmsys/phy-mtk-*/tps65132).
  The bring-up kernel must stay the fbcon / EXCLUDE_DISPLAY build; fix
  ./config (see the vendored #329 config) before building.
'';

mobile-nixos.kernel-builder {
  src = kernelSrc;
  # Makefile: VERSION=6 PATCHLEVEL=6 SUBLEVEL=157, no localversion (the
  # base tarball and the delta tree have no .git, so setlocalversion
  # produces no suffix; the builder verifies kernel.release against
  # `version`).
  version = "6.6.157";
  configfile = ./config;

  # The stock MediaTek LK bootloader gunzips the kernel payload and scans
  # its last 2 MiB for the DTB (docs §3.1-3.2). So: plain Image.gz here;
  # the DTB is appended by the android system type via `appendDTB`
  # (do NOT use isImageGzDtb: bootimg.nix consumes `target` = Image.gz).
  isCompressed = "gz";
  buildDTBs = true;

  # Boot logo (2026-10-05): the builder by default copies the Mobile
  # NixOS logo over drivers/video/logo/logo_linux_clut224.ppm and seds
  # fbmem.c to centre it (both axes, count forced to 1). Centring pushes
  # the console text below mid-screen; the replacement hid our own
  # logo. Keep the delta's logo (delta/drivers/video/logo/) and the
  # stock top-left placement (fbcon=logo-count:1 is on the cmdline).
  enableLinuxLogoReplacement = false;
  enableCenteredLinuxLogo = false;

  # The bring-up config has 1018 =m entries (panfrost, pwm-mtk-disp,
  # usb configfs, ...). Build the module tree so stage-2 can use them;
  # stage-1 itself stays non-modular (16 MiB boot partition, docs R1).
  isModular = true;

  # GCC 15 (nixpkgs 26.11pre) vs a 6.6 tree: mask -Werror failures.
  # The bring-up pipeline builds the same tree with GCC 15.2.0
  # successfully (docs §3.6); this is insurance for the mobile-nixos
  # builder's slightly different flag handling.
  enableRemovingWerror = true;

  # FIX (2026-09-05): the builder's prePatch sed rewires the top-level
  # `DEPMOD ?= ...` make variable, but the modules_install rule actually
  # runs `scripts/depmod.sh`, which looks up `depmod` in the *PATH* and
  # silently skips when it is missing ("Warning: 'make modules_install'
  # requires depmod" — observed in the kernel build log). Without it the
  # installed module tree has no modules.dep and stage-2 `modprobe` of
  # panfrost & co. cannot resolve names. Putting kmod in the build PATH
  # fixes it.
  nativeBuildInputs = [ buildPackages.kmod ];

  # FIX (2026-09-08, nix-build only — the delta stays byte-identical to
  # the fork): the vendored mediatek-connectivity Makefiles emit their
  # include paths as RELATIVE -I$(src)/... (e.g. the subdir-ccflags-y
  # block in drivers/misc/mediatek-connectivity/Makefile). Kbuild
  # compiles with cwd = the O= object tree, so relative -I paths resolve
  # against the WRONG tree in out-of-tree builds — wmt_core.c fails on
  # osal_typedef.h (observed in the first O= build). The fork always
  # built IN-TREE (no O=), so it never hit this. Anchor every vendor
  # -I$(src) on $(srctree), which the kernel top Makefile always sets
  # absolute (in-tree too), making the paths mode-independent.
  postPatch = ''
    echo ":: Anchoring mediatek-connectivity -I\$(src) on \$(srctree) (O= build fix)"
    find drivers/misc/mediatek-connectivity -name 'Makefile*' \
      -exec sed -i 's|-I\$(src)|-I\$(srctree)/\$(src)|g' {} +
    # The fork's gpio-aw9523b (keyboard matrix expander) uses
    # gpio_chip.irq, which only exists under CONFIG_GPIOLIB_IRQCHIP — a
    # promptless bool enabled purely by `select` from gpio drivers. In
    # the #329 build other defconfig gpio drivers selected it; the lean
    # config prunes them all, so add the select here (nix-build fix
    # 2026-09-08; the delta stays byte-identical to the fork).
    echo ":: Adding GPIOLIB_IRQCHIP select to gpio-aw9523b"
    sed -i '/AWINIC AW9523B I2C GPIO expander/,+5 { s|select REGMAP_I2C|select REGMAP_I2C\n\tselect GPIOLIB_IRQCHIP| }' drivers/gpio/Kconfig
  '';

  # Build the A72 bring-up module in the same tree/config (ABI-identical)
  # and ship it in the module dir under extra/ — modprobe/depmod index
  # that subdirectory. postInstall runs after zinstall + modules_install,
  # in the build dir (the builder's configurePhase ends with
  # `cd $buildRoot`), so this is the same invocation shape as the
  # builder's own dtbs step. Re-run depmod so modules.dep covers the
  # added module.
  postInstall = ''
    echo ":: Building out-of-tree module sramldo-smc (A72 bring-up)"
    # The module source is a store path (read-only); the out-of-tree
    # build writes .o/.o.d files next to the sources, so build from a
    # writable copy in the build dir (cwd is the kernel build dir).
    # M= must be absolute: a relative M= is resolved against the kernel
    # *source* tree (read-only store path), not the cwd (observed:
    # "../sramldo-smc-src/Makefile: No such file", 2026-09-05 build).
    cp -r ${sramldoModuleSrc} sramldo-smc-src
    chmod -R u+w sramldo-smc-src
    make $makeFlags "''${makeFlagsArray[@]}" M="$PWD/sramldo-smc-src" modules
    mkdir -p "$out/lib/modules/${modDirVersion}/extra"
    install -m 644 sramldo-smc-src/sramldo-smc.ko \
      "$out/lib/modules/${modDirVersion}/extra/sramldo-smc.ko"
    ${buildPackages.kmod}/bin/depmod -a ${modDirVersion} -b "$out"
  '';
}
