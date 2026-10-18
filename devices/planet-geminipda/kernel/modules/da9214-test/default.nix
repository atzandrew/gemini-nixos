# da9214-test.ko — VPROC2 (DA9214 BUCKB) awake power test (notes/vproc-research.md).
# Same light build as mt6797-spm-load (see there for the details).
#
#   nix build .#packages.aarch64-linux.da9214-test --print-out-paths --no-link
#   -> $out/da9214-test.ko   (also $out/lib/modules/6.6.157/extra/)
#
# Light build: reuses the kernel derivation (same source = base tarball +
# delta, same .config, same GCC, same makeFlags/postPatch) via
# overrideAttrs, but replaces the kernel build/install with
# `modules_prepare` + this one module. The kernel's own configurePhase
# already runs `make prepare` and checks kernel.release == 6.6.157, so
# vermagic matches the flashed kernel ("6.6.157 SMP preempt mod_unload
# aarch64"; MODVERSIONS/signing are off). No full kernel rebuild.
#
# KBUILD_MODPOST_WARN=1: there is no Module.symvers without a full
# build, so modpost cannot see the kernel's exports; with MODVERSIONS
# off that is only a build-time check — symbols resolve by name at
# insmod time.
{ kernel }:

kernel.overrideAttrs (old: {
  name = "da9214-test-${old.version}";

  buildFlags = [ ];
  # The kernel's pre/postInstall (zinstall bookkeeping, DTBs, the
  # sramldo-smc module build) do not apply to this derivation.
  preInstall = "";
  postInstall = "";
  dontFixup = true;

  buildPhase = ''
    runHook preBuild
    echo ":: modules_prepare (cwd $PWD)"
    make $makeFlags "''${makeFlagsArray[@]}" -j$NIX_BUILD_CORES modules_prepare

    echo ":: Building da9214-test.ko"
    cp -r ${./.} da9214-test-src
    chmod -R u+w da9214-test-src
    make $makeFlags "''${makeFlagsArray[@]}" M="$PWD/da9214-test-src" \
      KBUILD_MODPOST_WARN=1 modules

    vm="$(modinfo -F vermagic da9214-test-src/da9214-test.ko)"
    echo ":: vermagic: $vm"
    case "$vm" in
      "6.6.157 SMP preempt mod_unload aarch64"*) ;;
      *) echo "error: unexpected vermagic '$vm' (flashed kernel: 6.6.157 SMP preempt mod_unload aarch64)"; exit 1 ;;
    esac
    runHook postBuild
  '';

  installPhase = ''
    mkdir -p "$out/lib/modules/${old.version}/extra"
    install -m 644 da9214-test-src/da9214-test.ko "$out/da9214-test.ko"
    install -m 644 da9214-test-src/da9214-test.ko \
      "$out/lib/modules/${old.version}/extra/da9214-test.ko"
  '';
})
