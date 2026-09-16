#!/bin/sh
# bin/macos/build.sh — the macOS half of bin/build.sh: build this repo's
# aarch64-linux outputs on a Mac, with NO Linux machine involved (Apple's
# `container` runtime + a persisted aarch64 NixOS VM; probed 2026-09-12,
# implemented 2026-09-17).
#
# USE IT THROUGH THE DISPATCHER — `bash bin/build.sh <verb> [TARGET]` —
# which picks this path on Darwin and bin/build-linux.sh on Linux. This
# file is the mac implementation and can also be driven directly (same
# verbs, plus mac-only ones); docs/building.md is the map, and
# docs/macos-build.md carries the receipts + gotchas.
#
# WHY A VM: nix on macOS is aarch64-DARWIN — it evaluates foreign drvs and
# substitutes cached ones, but it can never RUN an aarch64-linux builder,
# so this repo's custom drvs (kernel, mesa, wlroots/gemwl, gemshell, all
# the NixOS config glue) are unbuildable on the bare Mac. The VM supplies
# the missing builder: same silicon, a native aarch64 nix, and a /nix
# store that survives between runs so iteration only rebuilds changes.
#
# WHAT IS MAC-NATIVE VS VM: everything nix-on-darwin can do is done on the
# Mac — the flake's darwin devshell (flake-macos.nix: `nix develop`) and
# the CA bundle it provides (`#caBundle`) — while the build itself runs in
# the VM. This script stages the tree, drives the container and polls.
#
# Usage (from the repo root):
#   bash bin/build.sh start [TARGET]   # stage + DETACHED build (rule 8)
#   bash bin/build.sh wait  [TARGET]   # poll: rc 0 done-ok, 1 failed, 2 running
#   bash bin/build.sh log   [TARGET]   # tail the build log
#   bash bin/build.sh status           # container, egress, artifacts, hashes
#   bash bin/build.sh net              # check/fix the VM's internet egress
#   bash bin/build.sh stage            # only sync the staging tree
#   bash bin/build.sh shell            # the darwin devshell (nix develop)
#   bash bin/build.sh vm               # a shell inside the build VM
#   bash bin/build.sh stop             # stop the VM + egress proxy (store kept)
#
#   TARGET (default bootimg) = any packages.aarch64-linux.* attribute:
#   bootimg (kernel + minimal initrd -> the flashable boot.img), kernel,
#   initrd, rootfs, toplevel, default, mesa, gemshell, gemcli, …
#
# Poll don't sleep (rule 8b): `wait` returns immediately while the build
# runs — re-run it, don't `sleep`.
#
# EGRESS: `start` checks whether the VM can reach the internet by itself.
# If it cannot (the host's default route is a VPN — a Tailscale exit node
# does this; see docs/macos-build.md), it lends the VM the Mac's working
# egress through bin/macos/proxy.py, bound to the container bridge gateway
# only. Nothing about the host's VPN is touched.
#
# Overrides: GEMINI_MACOS_CACHE (~/.cache/gemini-macos),
# GEMINI_MACOS_CONTAINER (gemini-macos-builder),
# GEMINI_MACOS_IMAGE (docker.io/rzmapp/nixos-vm:26.05),
# GEMINI_MACOS_CPUS (10), GEMINI_MACOS_MEMORY (12g),
# GEMINI_MACOS_PROXY_BIND (192.168.64.1), GEMINI_MACOS_PROXY_PORT (3128).
set -eu

repo=$(cd "$(dirname "$0")/../.." && pwd)
cache=${GEMINI_MACOS_CACHE:-$HOME/.cache/gemini-macos}
src=$cache/src
out=$cache/out
certs=$cache/certs
cname=${GEMINI_MACOS_CONTAINER:-gemini-macos-builder}
image=${GEMINI_MACOS_IMAGE:-docker.io/rzmapp/nixos-vm:26.05}
cpus=${GEMINI_MACOS_CPUS:-10}
memory=${GEMINI_MACOS_MEMORY:-12g}
# The VM's egress, when the Mac's own default route is a VPN (see `net`).
proxy_bind=${GEMINI_MACOS_PROXY_BIND:-192.168.64.1}
proxy_port=${GEMINI_MACOS_PROXY_PORT:-3128}
proxy_url=http://$proxy_bind:$proxy_port
proxy_pid=$cache/proxy.pid
proxy_log=$cache/proxy.log

die() { echo "macos/build: $*" >&2; exit 1; }

# ---- mac host tooling (rule 7 analogue) --------------------------------
#
# The build needs python3 (the egress proxy) and rsync (staging). A Mac
# with the Xcode CLT has /usr/bin/python3 and /usr/bin/rsync; one without
# does not. Rather than assume, re-exec once inside the flake's darwin
# devshell (flake-macos.nix), which carries both — the same trick the
# Linux device scripts use for adb/lsusb. GEMINI_MACOS_REEXEC guards the
# loop, and the devshell's tools win on PATH once inside.
reexec_in_devshell() {
    [ -n "${GEMINI_MACOS_REEXEC:-}" ] && return 0
    command -v python3 >/dev/null 2>&1 && command -v rsync >/dev/null 2>&1 && return 0
    command -v nix >/dev/null 2>&1 || \
        die "python3/rsync are missing and nix is not installed to supply them"
    echo "macos/build: python3/rsync missing on PATH — re-exec'ing in the darwin devshell" >&2
    GEMINI_MACOS_REEXEC=1 exec nix develop "$repo" --command bash "$0" "$@"
}

need_container_cli() {
    command -v container >/dev/null 2>&1 ||
        die "the 'container' CLI is not on PATH (Apple's container runtime)"
}

# ---- staging ----------------------------------------------------------
# The VM mounts this tree read-only at /build/src, so the Mac side is the
# source of truth: rsync the working tree, minus the things that must not
# be copied into the nix store (the 1.5 GiB cargo target dir, logs).
stage() {
    [ -f "$repo/flake.nix" ] || die "no flake.nix in $repo"
    mkdir -p "$src" "$out" "$certs"
    if [ ! -f "$certs/ca-certificates.crt" ]; then
        # The VM image's own CA bundle is stale, so the Mac lends it one.
        # Prefer the nix-provided bundle (flake-macos.nix `caBundle`) —
        # reproducible, maintained, no keychain scraping — and fall back to
        # the macOS keychain when nix cannot supply it.
        echo "macos/build: seeding the VM's CA bundle" >&2
        if command -v nix >/dev/null 2>&1 &&
           bundle=$(nix build --no-link --print-out-paths "$repo#caBundle" 2>/dev/null) &&
           [ -n "$bundle" ]; then
            cp "$bundle/etc/ssl/certs/ca-bundle.crt" "$certs/ca-certificates.crt"
        else
            security find-certificate -a -p \
                /System/Library/Keychains/SystemRootCertificates.keychain \
                > "$certs/ca-certificates.crt" ||
                die "no CA bundle: neither '$repo#caBundle' nor the system keychain"
        fi
    fi
    rsync -a --delete \
        --exclude .git \
        --exclude logs/ \
        --exclude pkgs/gemshell/target/ \
        --exclude .DS_Store \
        "$repo/" "$src/"
    echo "macos/build: staged $repo -> $src" >&2
}

# ---- container --------------------------------------------------------
running() { container list --quiet 2>/dev/null | grep -qx "$cname"; }
exists()  { container list --all --quiet 2>/dev/null | grep -qx "$cname"; }

ensure_container() {
    need_container_cli
    if exists; then
        if ! running; then
            echo "macos/build: starting $cname" >&2
            container start "$cname" >/dev/null
        fi
        return
    fi
    echo "macos/build: creating $cname from $image ($cpus cpus, $memory)" >&2
    # Same geometry as the 2026-09-12 probe: the repo ro, the Mac's CA
    # bundle over the VM's stale one, /out rw for artifacts.
    container create --name "$cname" --cpus "$cpus" --memory "$memory" \
        --mount type=virtiofs,source="$src",target=/build/src,readonly \
        --mount type=virtiofs,source="$certs",target=/etc/ssl/certs,readonly \
        --mount type=virtiofs,source="$out",target=/out \
        "$image" sleep infinity >/dev/null
    container start "$cname" >/dev/null
}

# ---- egress (the Tailscale-exit-node condition) ------------------------
#
# Apple's container vmnet NAT does NOT cope with a Mac whose default route
# is a VPN tunnel: with a Tailscale exit node active, the VM reaches its
# gateway and the Mac, but every NAT'd connection out times out (measured
# 2026-09-17 — see docs/macos-build.md). The fix is not to unplug the
# user's VPN but to lend the VM the Mac's working egress: a CONNECT proxy
# on the bridge gateway (bin/macos/proxy.py), which nix honours for
# both substituters and fixed-output fetches (proxyImpureEnvVars).

proxy_running() {
    [ -f "$proxy_pid" ] && kill -0 "$(cat "$proxy_pid")" 2>/dev/null
}

start_proxy() {
    if proxy_running; then return 0; fi
    [ "$(command -v python3 || true)" ] ||
        die "the VM has no egress and there is no python3 for the Mac-side \
egress proxy (run: nix develop, or install the Xcode CLT)"
    nohup python3 "$repo/bin/macos/proxy.py" \
        --bind "$proxy_bind" --port "$proxy_port" \
        > "$proxy_log" 2>&1 &
    echo $! > "$proxy_pid"
    sleep 1
    proxy_running || die "the egress proxy did not start — see $proxy_log"
    echo "macos/build: egress proxy up on $proxy_url (pid $(cat "$proxy_pid"))" >&2
}

stop_proxy() {
    if proxy_running; then
        kill "$(cat "$proxy_pid")" 2>/dev/null || true
        echo "macos/build: egress proxy stopped" >&2
    fi
    rm -f "$proxy_pid"
}

vm_online() {
    container exec "$cname" sh -c \
        'curl -sS -o /dev/null --max-time 8 https://cache.nixos.org/nix-cache-info' \
        >/dev/null 2>&1
}

# Idempotent: leave the VM with a working egress either way, and report
# which path it is. Proxy env stays empty when the VM is online directly.
net_check() {
    ensure_container
    if vm_online; then
        stop_proxy
        proxy_env=""
        echo "macos/build: VM egress DIRECT (no proxy needed)" >&2
        return 0
    fi
    echo "macos/build: VM has NO direct egress (host VPN/exit-node NAT) — \
lending it the Mac's egress" >&2
    start_proxy
    # no_proxy keeps the VM from trying to proxy loopback/bridge traffic.
    proxy_env="-e http_proxy=$proxy_url -e https_proxy=$proxy_url \
-e all_proxy=$proxy_url -e no_proxy=localhost,127.0.0.1,$proxy_bind"
    echo "macos/build: VM egress VIA PROXY $proxy_url" >&2
}

# ---- verbs ------------------------------------------------------------
start_build() {
    target=${1:-bootimg}
    stage
    net_check

    rev=$(git -C "$repo" rev-parse HEAD 2>/dev/null || true)
    if [ -n "$rev" ] &&
       [ -n "$(git -C "$repo" --no-optional-locks status --porcelain 2>/dev/null)" ]; then
        rev="$rev-dirty"
    fi

    rm -f "$out/$target.rc"
    : > "$out/$target.log"
    # Detached (rule 8): the VM process is set free, its output goes to
    # <target>.log and its exit status to <target>.rc — that pair is what
    # `wait` polls. -L on the VM side streams the nix build log. $proxy_env
    # is empty on a direct-egress host (word-split on purpose).
    container exec -d $proxy_env \
        -e "GEMINI_HOST_REV=$rev" -e "GEMINI_PROXY=${proxy_env:+$proxy_url}" \
        "$cname" sh -c \
        "/build/src/bin/macos/vm-build.sh $target > /out/$target.log 2>&1; echo \$? > /out/$target.rc" \
        >/dev/null

    echo "macos/build: $target building in $cname (rev ${rev:-unknown})" >&2
    echo "macos/build: poll with  bash bin/build.sh wait $target" >&2
}

wait_build() {
    target=${1:-bootimg}
    if [ ! -f "$out/$target.rc" ]; then
        tail -n "${GEMINI_MACOS_TAIL:-5}" "$out/$target.log" 2>/dev/null || true
        echo "macos/build: $target STILL RUNNING (rc=2)" >&2
        exit 2
    fi
    rc=$(cat "$out/$target.rc")
    tail -n "${GEMINI_MACOS_TAIL:-20}" "$out/$target.log"
    if [ "$rc" = 0 ]; then
        echo "macos/build: $target DONE rc=0" >&2
        exit 0
    fi
    echo "macos/build: $target FAILED rc=$rc — see $out/$target.log" >&2
    exit 1
}

log_build() {
    target=${1:-bootimg}
    tail -n "${GEMINI_MACOS_TAIL:-40}" -f "$out/$target.log"
}

status() {
    need_container_cli
    if exists; then
        echo "container: $cname ($(running && echo running || echo stopped))"
    else
        echo "container: $cname (absent)"
    fi
    echo "staging:   $src"
    [ -f "$src/flake.nix" ] && echo "           (staged)"
    if proxy_running; then
        echo "egress:    proxy $proxy_url (pid $(cat "$proxy_pid"))"
    else
        echo "egress:    direct (no proxy running)"
    fi
    echo "artifacts: $out"
    for f in "$out"/*; do
        [ -e "$f" ] || continue
        case $f in
            *.log) continue ;;
        esac
        printf '  %-34s %s\n' "$(basename "$f")" "$(du -h "$f" | cut -f1)"
    done
    for h in "$out"/*.sha256; do
        [ -f "$h" ] || continue
        echo "  --- $(basename "$h")"
        sed 's/^/    /' "$h"
    done
}

case ${1:-} in
    start)  shift; reexec_in_devshell start "$@"; start_build "$@" ;;
    wait)   shift; wait_build "$@" ;;
    log)    shift; log_build "$@" ;;
    status) status ;;
    stage)  reexec_in_devshell stage; stage ;;
    net)    reexec_in_devshell net; net_check ;;
    # `vm`/`sh` — a shell inside the build VM (where the images are built).
    vm|sh)  ensure_container
            exec container exec -it "$cname" sh -l ;;
    # `shell` — the mac HOST devshell (nix develop == flake-macos.nix's
    # aarch64-darwin devshell); the dispatcher handles this too.
    shell)  cd "$repo"
            exec nix develop ;;
    stop)   container stop "$cname"
            stop_proxy ;;
    ""|-h|--help|help)
            sed -n '20,53p' "$0" ;;
    *)      die "unknown verb '$1' (try: start|wait|log|status|stage|net|shell|vm|stop)" ;;
esac
