# flake-macos.nix — the macOS (aarch64-darwin) half of the flake, kept in
# its own file so that (a) the Linux build model in flake.nix is not
# touched by it and (b) it is obvious what exists only for Macs.
#
# WHAT THIS DELIBERATELY DOES NOT DO: build the device images on darwin.
# Nix on darwin can *evaluate* foreign-system derivations and *substitute*
# cached ones, but it can never RUN an aarch64-linux builder — so
# `nix build .#packages.aarch64-linux.bootimg` on a Mac substitutes what
# it can and then fails on this repo's custom drvs (kernel, mesa,
# gemshell, …). The images are built inside an Apple `container` aarch64
# NixOS VM instead: `bash bin/build.sh start bootimg`
# (docs/macos-build.md). This file supplies what IS darwin-native:
#
#   devShells.aarch64-darwin.default
#       `nix develop` on a Mac lands here (it is the default devshell for
#       the current system, exactly as devShells.x86_64-linux.default is
#       for the Linux workstation). It carries the host-side tools
#       bin/macos/* need — python3 (the egress proxy), rsync (staging the
#       tree for the build VM), cacert (the VM's CA bundle), adb (the host
#       half of the flash workflow to come) — so nothing depends on
#       Homebrew or the Xcode CLT. The Apple `container` CLI is NOT in
#       nixpkgs; that one stays a host prerequisite.
#
#   packages.aarch64-darwin.caBundle
#       The CA bundle that bin/macos/build.sh mounts into the VM at
#       /etc/ssl/certs (the VM image's own certs are stale). Preferred
#       over scraping the macOS keychain, which is the fallback.
{ nixpkgs }:
let
  # Lazy: only forced when one of the outputs below is actually evaluated,
  # so a Linux `nix build .#packages.aarch64-linux.*` never evaluates
  # nixpkgs for darwin at all.
  pkgs = import nixpkgs { system = "aarch64-darwin"; };
in
{
  devShells.aarch64-darwin.default = pkgs.mkShell {
    packages = with pkgs; [
      python3        # bin/macos/proxy.py — the VM's egress proxy (stdlib only)
      rsync          # staging the working tree for the build VM (real rsync,
                     # not macOS's openrsync, when the host one is missing)
      cacert         # packages.aarch64-darwin.caBundle — the VM's /etc/ssl/certs
      android-tools  # adb — the host side of the flash workflow (future work)
      git            # rule 0: the host revision the build records
      coreutils
      findutils
      curl           # bin/macos/build.sh net — egress diagnostics
    ];

    shellHook = ''
      echo "gemini-nixos — macOS devshell ($(uname -s) $(uname -m))"
      echo
      echo "  device images build in the aarch64 NixOS VM, not here:"
      echo "    bash bin/build.sh start bootimg     # stage + detached build"
      echo "    bash bin/build.sh wait  bootimg     # rc 0 ok / 1 failed / 2 running"
      echo "    bash bin/build.sh status            # VM, egress, artifacts, hashes"
      echo
      echo "  Do NOT run 'nix build .#packages.aarch64-linux.<target>' on a Mac:"
      echo "  darwin nix can only substitute those, never build them."
      echo "  Details: docs/macos-build.md and docs/building.md."
    '';
  };

  # The CA bundle the build VM mounts (see bin/macos/build.sh `stage`).
  packages.aarch64-darwin.caBundle = pkgs.cacert;
}
