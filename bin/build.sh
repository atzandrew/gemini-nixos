#!/bin/sh
# build.sh — THE build entry point for this repo. ONE command, whichever
# machine you are on: it detects the platform and dispatches to that
# platform's implementation. Use this, not a bare `nix build`, so that
# humans and agents always get the model that actually works here.
#
#   macOS (Darwin)  -> bin/macos/build.sh
#       Images are built in an Apple `container` aarch64 NixOS VM, because
#       nix on darwin can only *evaluate/substitute* aarch64-linux drvs —
#       it can never RUN those builders, so this repo's custom drvs
#       (kernel, mesa, wlroots/gemwl, gemshell, the NixOS glue) would fail
#       on the bare Mac. Artifacts + identity files land in
#       ~/.cache/gemini-macos/out/.  See docs/macos-build.md.
#
#   Linux -> bin/build-linux.sh
#       The model this repo has always used, unchanged: native aarch64
#       drvs built as root against the LOCAL store with the Pi
#       (192.168.49.191) compiling and cache.nixos.org substituting.
#       `toplevel` is routed to bin/deploy.sh build (same build + GC pins).
#
# Verbs — the SAME on both platforms, with the repo's job rc protocol
# (rule 8/8b), so an agent can poll identically wherever it is:
#
#   bash bin/build.sh start [TARGET]   build DETACHED; returns immediately
#   bash bin/build.sh wait  [TARGET]   poll: rc 0 done-ok / 1 failed /
#                                      2 still running (re-run) / 3 no job
#   bash bin/build.sh log   [TARGET]   tail the build log
#   bash bin/build.sh status           jobs, container/egress, artifacts, hashes
#   bash bin/build.sh shell            the platform devshell (`nix develop`)
#   bash bin/build.sh vm               macOS only: a shell in the build VM
#   bash bin/build.sh help             this text
#
# TARGET = any `packages.aarch64-linux.*` attribute (default `bootimg`):
#
#   bootimg   boot.img — kernel + minimal initrd (THE firmware image; p22 `boot`)
#   kernel    the kernel package alone (Image.gz + dtbs + modules)
#   rootfs    the NixOS rootfs image (→ p27 `linux`)
#   initrd    the minimal busybox initrd (size checks)
#   default   boot.img + rootfs + flash script
#   toplevel  the system generation (the deploy/switch path on Linux)
#   + mesa, wlroots, gemwl, gemshell, gemcli, gemdemo, gemini-xkb, …
#
# Platform-specific verbs are forwarded to the platform script: on macOS
# `net` (egress check/fix), `stage` (re-sync the tree), `stop` (VM +
# proxy), `sh` (VM shell).
#
# NOTE: flashing is NOT part of this yet — this builds images only.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)

usage() {
    sed -n '2,50p' "$here/build.sh" | sed 's/^# \{0,1\}//'
}

case $(uname -s) in
    Darwin) impl=$here/macos/build.sh; plat="macOS (container build VM) — $impl" ;;
    Linux)  impl=$here/build-linux.sh; plat="Linux (native aarch64) — $impl" ;;
    *)      echo "build: unsupported platform '$(uname -s)': the device images build on Linux (native aarch64) or on macOS (container VM)" >&2
            exit 1 ;;
esac

verb=${1:-help}
[ "$#" -gt 0 ] && shift || true

case $verb in
    help|-h|--help)
        echo "build: platform = $plat"
        echo
        usage
        ;;
    shell)
        # The platform default devshell: devShells.x86_64-linux.default on
        # the workstation, devShells.aarch64-darwin.default on a Mac.
        cd "$repo"
        exec nix develop
        ;;
    vm)
        if [ "$(uname -s)" = Darwin ]; then
            exec bash "$impl" sh
        fi
        echo "build: 'vm' is macOS-only — on Linux the build runs natively on this host" >&2
        exit 1
        ;;
    start|wait|log|status)
        exec bash "$impl" "$verb" "$@"
        ;;
    *)
        # Platform extras (macOS: net/stage/stop/sh) are forwarded so the
        # dispatcher never has to know the full mac-side verb list.
        exec bash "$impl" "$verb" "$@"
        ;;
esac
