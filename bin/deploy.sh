#!/bin/sh
# deploy.sh — build/switch generations of the gemini-nixos flake the
# workstation way: build the NATIVE aarch64 toplevel (flake is now
# buildSystem = aarch64-linux; cross toplevel abandoned 2026-09-07 at
# the nixpkgs Qt6CoreTools wall — docs/handover-2026-09-07-lxqt-native.md),
# ship the DELTA to the device's nix store over ssh, then switch the
# device's system profile + activate — no reflash, no TWRP. The device
# boots whatever /nix/var/nix/profiles/system points at (dual-boot
# initrd gen lookup), so a reboot lands on the new generation and old
# generations stay selectable for rollback.
#
# Build model (2026-09-07 → native): every drv is system=aarch64-linux.
# The host daemon has NO `builders =` line (adding it needs a daemon
# restart), so the build runs as ROOT against the LOCAL store with
# `--option builders @/etc/nix/machines`: the Pi (192.168.49.191, 8
# cores, /etc/nix/machines ssh://cjdell@…) compiles and the host pulls
# each finished path back over ssh (the Pi's own cache.nixos.org link
# drops large NARs — HTTP 206 — so never build directly on the Pi for
# cache fetches). `--fallback`: substitute from cache.nixos.org — since
# the 2026-09-08 repin the pinned nixpkgs rev IS the hydra-built channel
# snapshot dc5d91f84032, so the Qt6 closure fetches; the custom
# drvs (mesa-geminipda fork, wlroots/gemwl pins, kernel) are NOT
# on any cache — they compile on the builder.
#
# GC hygiene (rule 0): every deployed toplevel is pinned with
# bin/gc-pin.sh (per-user root) — and the milestone closures get a
# root-level root too (`sudo nix-store --add-root
# /nix/var/nix/gcroots/<name> -r <out>`) — before shipping.
# `nix-collect-garbage` on the HOST would otherwise sweep the closure
# (observed 2026-09-07: gen3 re-cross-compiled ~259 packages after a
# host GC).
#
# Usage (run from the repo root):
#   bash bin/deploy.sh status                 device generations + booted gen
#   bash bin/deploy.sh build                  build the native toplevel + pin it
#   bash bin/deploy.sh deploy                 build (if needed) + ship + switch
#   bash bin/deploy.sh deploy PATH            ship + switch an existing toplevel
#   bash bin/deploy.sh rollback [N]           switch device profile N gens back
# Long ops (the build, or a big first `nix copy` of a native closure):
# wrap in `bash bin/run-job.sh start <name> -- bash bin/deploy.sh ...`.
#
# Device access: bin/device-ssh.sh (auto net-up). The admin key is
# committed in this repo (keys/gemini_ed25519 — see keys/README.md), and
# the same file is what NIX_SSHOPTS below hands to `nix copy`.
set -eu

repo=/home/cjdell/Projects/gemini-nixos
# Device address: the g_ether USB gadget default; override with
# GEMINI_DEV_IP when the unit is reachable another way (Wi-Fi/LAN) —
# bin/device-ssh.sh honours the same variable.
dev=${GEMINI_DEV_IP:-10.15.19.82}
# The key lives in the repo (no important file outside it). chmod first:
# git cannot store 0600 and ssh refuses a world-readable private key.
key=${GEMINI_SSH_KEY:-$repo/keys/gemini_ed25519}
[ -f "$key" ] || { echo "deploy: no ssh key at $key — it is committed at keys/gemini_ed25519 (see keys/README.md)" >&2; exit 1; }
chmod 600 "$key" 2>/dev/null || true
profile=/nix/var/nix/profiles/system

sudo_build() { sudo nix build --store local "$@"; }

device_ssh() { bash "$repo/bin/device-ssh.sh" "$1"; }

build() {
    # stdout = the toplevel store path ONLY (callers capture it). Root
    # + --store local: the daemon has no `builders =` line (restart
    # deferred), so the distributed build must bypass it. The remote
    # builder (192.168.49.191) compiles; the host substitutes + pulls.
    echo "deploy: building native toplevel (aarch64, remote builder)..." >&2
    sudo_build "$repo#packages.aarch64-linux.toplevel" \
        --print-out-paths --no-link \
        --option builders @/etc/nix/machines --fallback 2>/dev/null
}

pin() {
    local tl=$1
    local stamp
    stamp=$(date +%Y%m%d-%H%M)
    bash "$repo/bin/gc-pin.sh" "toplevel-$stamp" "$tl"
}

ship_and_switch() {
    local tl=$1
    echo "deploy: shipping closure delta to $dev (nix copy)..."
    # known_hosts independence: device-ssh.sh pins UserKnownHostsFile=/dev/null
    # (the freshly reinstalled rootfs has a new host key; 2026-09-10
    # repartition).  nix copy does not, so set NIX_SSHOPTS for the same
    # behaviour — otherwise a new rootfs host key aborts the deploy.
    # [added 2026-09-10p] | Use root@ explicitly: with no ssh-config
    # entry for the address (e.g. GEMINI_DEV_IP on the LAN) ssh would
    # otherwise log in as the local user (cjdell), which is an untrusted
    # nix user → the daemon enforces require-sigs and rejects every
    # unsigned locally-built path. [fixed 2026-09-11]
    NIX_SSHOPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i $key" \
        nix copy --to "ssh://root@$dev" "$tl"
    echo "deploy: switching device profile -> ${tl##*/}"
    device_ssh "nix-env -p $profile --set '$tl'"
    device_ssh "'$tl/bin/switch-to-configuration' switch" \
        | grep -vE '^$' || true
    echo "deploy: done — current device generation:"
    device_ssh "readlink $profile; readlink -f $profile"
}

status() {
    echo "device system generations:"
    device_ssh "nix-env -p $profile --list-generations 2>/dev/null | tail -8; echo; echo 'profile -> \$(readlink -f $profile 2>/dev/null || echo (none))'"
}

rollback() {
    local n=${1:-1}
    echo "deploy: switching device profile $n generation(s) back + activating..."
    device_ssh "nix-env -p $profile --rollback $n 2>/dev/null || nix-env -p $profile --rollback"
    local tl
    tl=$(device_ssh "readlink -f $profile")
    device_ssh "'$tl/bin/switch-to-configuration' switch" | grep -vE '^$' || true
    echo "deploy: now on ${tl##*/}"
}

cmd=${1:-status}
case "$cmd" in
status) status ;;
build)  tl=$(build); pin "$tl"; echo "built+pin: $tl" ;;
deploy)
    if [ $# -ge 2 ]; then
        tl=$2
        [ -e "$tl" ] || { echo "not a store path: $tl" >&2; exit 2; }
    else
        # reuse ./result if it is a toplevel of this flake, else build
        if [ -L "$repo/result" ] && [ -d "$repo/result/init" ]; then
            tl=$(readlink -f "$repo/result")
            echo "deploy: reusing built toplevel $tl (run 'deploy build' to rebuild)"
        else
            tl=$(build)
        fi
        pin "$tl"
    fi
    ship_and_switch "$tl"
    ;;
rollback) rollback "${2:-1}" ;;
*)
    echo "usage: deploy.sh status|build|deploy [PATH]|rollback [N]" >&2
    exit 2
    ;;
esac
