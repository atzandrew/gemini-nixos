#!/bin/sh
# build-linux.sh — the Linux half of bin/build.sh: build this repo's
# aarch64-linux outputs ON the Linux workstation, exactly the way this
# repo has always done it. Nothing about that model changed when the
# macOS path was added (2026-09-17) — this file only gives it the same
# start/wait/log/status verb surface as the Mac path, so bin/build.sh can
# dispatch to either and agents have ONE interface.
#
# THE MODEL (unchanged; AGENTS.md "Build model & commands"):
#   * every drv is system=aarch64-linux (native; the x86_64 cross toplevel
#     is abandoned);
#   * the build runs as ROOT against the LOCAL store with
#     `--option builders @/etc/nix/machines --fallback`, so the Pi
#     (192.168.49.191) compiles and cache.nixos.org substitutes — the
#     daemon deliberately has no `builders =` line, hence `--store local`;
#   * long operations go through bin/run-job.sh (rule 8); never inline
#     nohup/pgrep loops.
#
# Verbs:
#   bash bin/build-linux.sh start [TARGET]   detached build (rule 8), default bootimg
#   bash bin/build-linux.sh wait  [TARGET]   rc 0 done-ok / 1 failed / 2 running / 3 no job
#   bash bin/build-linux.sh log   [TARGET]   tail the job log
#   bash bin/build-linux.sh status           known jobs + where artifacts land
#   bash bin/build-linux.sh shell            nix develop (project devshell)
#
# TARGET = packages.aarch64-linux.<TARGET> (bootimg, kernel, rootfs, initrd,
# default, mesa, gemshell, …). `toplevel` is routed to bin/deploy.sh build:
# that is the canonical generation loop and it additionally pins the GC
# roots (rule 0) — do not build the toplevel any other way.
#
# Artifacts stay where nix puts them: the store paths are printed by
# `nix build --print-out-paths` into the job log (`log <TARGET>`). No
# copies are made — that is the workstation's existing behaviour.
#
# Job state: logs/jobs/build-<TARGET>/{log,status,pid} (bin/run-job.sh).
set -eu

repo=$(cd "$(dirname "$0")/.." && pwd)

usage() {
    sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'
}

build_cmd() {
    case $1 in
        toplevel)
            # The canonical loop: builds the same toplevel AND gc-pins it.
            echo "bash $repo/bin/deploy.sh build"
            ;;
        *)
            # Exactly bin/deploy.sh's invocation, for any other attribute.
            echo "sudo nix build --store local $repo#packages.aarch64-linux.$1 --print-out-paths --no-link --option builders @/etc/nix/machines --fallback"
            ;;
    esac
}

verb=${1:-help}
[ "$#" -gt 0 ] && shift || true
target=${1:-bootimg}

# A target is a plain attribute name — catch a pasted full flake ref early.
case $target in
    *[!a-zA-Z0-9_.-]*)
        echo "build-linux: bad TARGET '$target' — pass the short attribute name (e.g. bootimg)" >&2
        exit 3 ;;
esac

jobname="build-$target"

case $verb in
    help|-h|--help)
        echo "build-linux: platform = Linux (native aarch64; Pi builder 192.168.49.191)"
        echo
        usage
        ;;
    start)
        cmd=$(build_cmd "$target")
        # Word-split on purpose: run-job takes CMD ARGS… after `--`.
        # shellcheck disable=SC2086
        exec bash "$repo/bin/run-job.sh" start "$jobname" -- $cmd
        ;;
    wait)
        exec bash "$repo/bin/run-job.sh" wait "$jobname"
        ;;
    log)
        exec bash "$repo/bin/run-job.sh" tail "$jobname" "${2:-40}"
        ;;
    status)
        echo "platform:  Linux (native aarch64, Pi builder 192.168.49.191, local store)"
        echo "next:      bash bin/build.sh start <TARGET>   (try: $jobname)"
        echo "artifacts: store paths, printed by the build; read them with"
        echo "           bash bin/build.sh log <TARGET>"
        echo "job state: logs/jobs/build-<TARGET>/"
        echo
        bash "$repo/bin/run-job.sh" list || true
        ;;
    shell)
        cd "$repo"
        exec nix develop
        ;;
    *)
        echo "build-linux: unknown verb '$verb' (start|wait|log|status|shell|help)" >&2
        exit 3
        ;;
esac
