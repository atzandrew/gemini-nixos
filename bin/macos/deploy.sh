#!/bin/bash
# macos/deploy.sh — deploy a system generation to the Gemini PDA from a
# macOS host (the Mac counterpart of bin/deploy.sh, which is Linux-only:
# it needs the host Nix store + the 192.168.49.191 builder).
#
# Why this exists: nix-on-darwin can EVALUATE and COPY aarch64-linux
# store paths but can never RUN their builders, so the toplevel is built
# by the Apple `container` aarch64 NixOS VM (bin/macos/build.sh, same as
# `bash bin/build.sh start toplevel`). The VM can reach the device over
# the USB-NIC network, so it also does the `nix copy` delta — no Mac Nix
# store involvement, no Linux host.
#
# Flow (mirrors deploy.sh):
#   1. build packages.aarch64-linux.toplevel in the VM (unless a path is
#      given);
#   2. make sure the VM can `ssh root@<dev>` with keys/gemini_ed25519
#      (the VM image has no /etc/passwd — ssh needs a local root entry;
#      the device's root must already accept the repo key — see
#      docs/usb-network.md if a reflash dropped it);
#   3. `nix copy --to ssh://root@<dev>` the closure DELTA (only missing
#      paths — the kernel + config glue + the changed drv, seconds);
#   4. set the device's system profile and run `switch-to-configuration
#      switch` (no reflash; old generations stay for rollback).
#
# Usage (from anywhere; the script finds the repo):
#   bash bin/macos/deploy.sh status              device generations
#   bash bin/macos/deploy.sh deploy              build + ship + switch
#   bash bin/macos/deploy.sh deploy PATH         ship + switch an existing toplevel
#   bash bin/macos/deploy.sh rollback [N]        switch the device profile N back
#
# Long builds: this script builds in the background and polls (rule 8/8b).
# Device access after a deploy: `bash bin/device-ssh.sh` (the repo key is
# the device's login + host key once the toplevel is active).
set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$repo"

dev="${GEMINI_DEV_IP:-10.15.19.82}"
key="${GEMINI_SSH_KEY:-$repo/keys/gemini_ed25519}"
profile=/nix/var/nix/profiles/system
out="$HOME/.cache/gemini-macos/out"
cname="${GEMINI_MACOS_CONTAINER:-gemini-macos-builder}"

die() { echo "deploy(mac): $*" >&2; exit 1; }
need_container() {
    command -v container >/dev/null 2>&1 ||
        die "the 'container' CLI is not on PATH (Apple's container runtime)"
}
chmod 600 "$key" 2>/dev/null || true
[ -f "$key" ] || die "no ssh key at $key (committed at keys/gemini_ed25519)"
[ "$(uname -s)" = "Darwin" ] || die "this is the macOS path — use bin/deploy.sh on Linux"

device_ssh() { bash "$repo/bin/device-ssh.sh" "$1"; }

# The VM image is minimal: no /etc/passwd → ssh cannot resolve the local
# uid ("No user exists for uid 0"). Seed a root entry and the private key.
vm_ssh_setup() {
    need_container
    local b64
    b64=$(base64 < "$key" | tr -d '\n')
    container exec "$cname" sh -c '
        [ -f /etc/passwd ] || printf "root:x:0:0:root:/root:/bin/sh\n" > /etc/passwd
        [ -f /etc/group ]  || printf "root:x:0:\n" > /etc/group
        mkdir -p /root/.ssh && chmod 700 /root/.ssh
        printf "%s" '"'$b64'"' | base64 -d > /root/.ssh/id_ed25519
        chmod 600 /root/.ssh/id_ed25519
    '
}

# Confirm the device's root accepts the repo key before we rely on it.
ensure_device_key() {
    ssh -i "$key" -o BatchMode=yes -o IdentitiesOnly=yes \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=8 root@"$dev" true 2>/dev/null ||
        die "root@$dev rejects the repo key — install keys/gemini_ed25519.pub
     into /root/.ssh/authorized_keys (see docs/usb-network.md; a gen-54
     rootfs predates commit 1020d4e)."
}

build_toplevel() {
    echo "deploy(mac): building toplevel in the VM (bash bin/build.sh start toplevel)..." >&2
    bash "$repo/bin/build.sh" start toplevel >&2
    # poll (rule 8b): rc 0 done-ok / 1 failed / 2 still running. `set +e`
    # so the polling loop can read the rc without tripping `set -e`.
    set +e
    while :; do
        bash "$repo/bin/build.sh" wait toplevel >/dev/null 2>&1
        rc=$?
        [ "$rc" -eq 0 ] && break
        [ "$rc" -eq 1 ] && die "toplevel build FAILED (see $out/toplevel.log)"
        sleep 10
    done
    set -e
    tail -n1 "$out/toplevel.paths"
}

ship_and_switch() {
    local tl=$1
    [ -d "$tl" ] || die "no such toplevel: $tl"
    ensure_device_key
    vm_ssh_setup
    echo "deploy(mac): copying closure delta to $dev (nix copy from the VM)..." >&2
    container exec "$cname" sh -c "
        export HOME=/root
        export NIX_CONFIG='experimental-features = nix-command flakes'
        export NIX_SSHOPTS='-i /root/.ssh/id_ed25519 -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null'
        nix copy --to ssh://root@$dev '$tl'
    "
    echo "deploy(mac): switching device profile -> ${tl##*/}" >&2
    device_ssh "nix-env -p $profile --set '$tl'"
    device_ssh "'$tl/bin/switch-to-configuration' switch" | grep -vE '^$' || true
    echo "deploy(mac): done — current device generation:"
    device_ssh "readlink -f $profile"
}

case "${1:-}" in
    status)
        device_ssh "nix-env -p $profile --list-generations 2>/dev/null | tail -8; echo; echo -n 'profile -> '; readlink -f $profile 2>/dev/null || echo '(none)'"
        ;;
    deploy)
        if [ -n "${2:-}" ]; then
            ship_and_switch "$2"
        else
            ship_and_switch "$(build_toplevel)"
        fi
        ;;
    rollback)
        n="${2:-1}"
        echo "deploy(mac): switching device profile $n generation(s) back..." >&2
        device_ssh "nix-env -p $profile --rollback $n 2>/dev/null || nix-env -p $profile --rollback"
        tl=$(device_ssh "readlink -f $profile")
        device_ssh "'$tl/bin/switch-to-configuration' switch" | grep -vE '^$' || true
        echo "deploy(mac): now on ${tl##*/}"
        ;;
    *)
        echo "usage: $0 status|deploy [PATH]|rollback [N]" >&2
        exit 2
        ;;
esac
