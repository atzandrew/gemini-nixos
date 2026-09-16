#!/bin/sh
# vm-build.sh — the IN-VM half of bin/build.sh: builds this
# repo's aarch64-linux flake outputs INSIDE a native aarch64 NixOS VM.
#
# WHY THIS EXISTS (2026-09-17, promoted from the 2026-09-12 probe):
# nix on macOS is aarch64-DARWIN. It can evaluate foreign drvs and
# substitute cached ones, but it can never RUN an aarch64-linux builder —
# so every custom drv in this repo (kernel, mesa, wlroots/gemwl, gemshell,
# the NixOS config glue) is unbuildable on the bare Mac. The pi5 technique
# solves that with Apple's `container` runtime + a persisted aarch64 NixOS
# VM (rzmapp/nixos-vm:26.05): inside the VM the build is a plain NATIVE
# aarch64 build — byte-for-byte the same drvs `bin/deploy.sh build` builds
# on the Linux workstation — and the VM's /nix survives between runs, so
# iteration only rebuilds what changed.
#
# This script is NOT macOS-specific: it is the native build, and is what
# the Mac wrapper (bin/macos/build.sh) executes inside the VM.
# Run it directly for iteration:
#
#   container start gemini-macos-builder
#   container exec gemini-macos-builder /build/src/bin/macos/vm-build.sh bootimg
#
# Usage (inside the VM):  vm-build.sh [TARGET ...]
#   TARGET = a `packages.aarch64-linux.*` attribute (bootimg = DEFAULT,
#            kernel, rootfs, initrd, toplevel, default, mesa, gemwl,
#            gemshell, gemcli, gemdemo, gemini-exodus, …). A dotted/
#            namespaced value is used verbatim if it contains a dot, so
#            `nixosConfigurations.gemini.config.system.build.toplevel`
#            also works.
#
# Environment:
#   GEMINI_SRC      staged repo (default /build/src — bind-mounted ro)
#   GEMINI_OUT      artifact dir (default /out — the Mac's out dir)
#   GEMINI_HOST_REV source revision string, recorded in the manifest
#                   (the wrapper passes the Mac's `git rev-parse HEAD`)
#   GEMINI_PROXY    egress proxy URL in use, recorded in the manifest (the
#                   wrapper sets it when the VM has no direct internet —
#                   see bin/build.sh `net`)
#
# Artifacts: the built images/artifacts are copied to $GEMINI_OUT with
# `<target>.paths` (store paths), `<target>.log`, `<target>.sha256` and
# `<target>.manifest` next to them — rule 0: never build something you
# cannot identify. Build output goes to stdout/stderr (the wrapper
# redirects it to `<target>.log`).
#
# NOTE: nothing here flashes anything; it only produces images.
set -eu

src=${GEMINI_SRC:-/build/src}
out=${GEMINI_OUT:-/out}

if [ "$#" -eq 0 ]; then
    set -- bootimg
fi

# NixOS's default NIX_CONFIG wants a build user; the VM runs as root with
# no nixbld users, and the CA bundle is the mount from the Mac (the VM
# image's own certs are stale) — both receipts from the 2026-09-12 probe.
export NIX_CONFIG="experimental-features = nix-command flakes
ssl-cert-file = /etc/ssl/certs/ca-certificates.crt
build-users-group =
keep-derivations = true"

mkdir -p /tmp /var/tmp "$out"
export TMPDIR=/tmp

if [ ! -f "$src/flake.nix" ]; then
    echo "vm-build: no flake at $src — stage the repo first" >&2
    exit 1
fi

# Copy the image(s)/artifact(s) a build output may hold. Most outputs are a
# directory (android-bootimg holds boot.img, rootfs a *system.img*, the
# kernel its Image.gz + dtbs); a few are a single file.
collect() {
    p=$1
    if [ -f "$p" ]; then
        cp -f "$p" "$out/$(basename "$p")"
        return
    fi
    for f in "$p"/*.img "$p"/Image.gz "$p"/Image "$p"/initrd "$p"/*.dtb; do
        if [ -f "$f" ]; then
            cp -f "$f" "$out/$(basename "$f")"
        fi
    done
}

for target in "$@"; do
    case $target in
        *.*|*/*) attr=$target ;;
        *)       attr=packages.aarch64-linux.$target ;;
    esac

    echo "=== [vm] building $attr (native aarch64, $(nproc) cores) ==="
    # --impure: the flake is read from a path (no git wrapper); the two
    # builtins.fetchTree inputs stay hash-pinned by flake.nix itself.
    # stdout = store paths ONLY (captured); stderr = the build log.
    nix build --impure --no-link --print-out-paths -L \
        --expr "let f = builtins.getFlake \"path:$src\"; in f.$attr" \
        > "$out/$target.paths"

    echo "=== [vm] $target built:"
    cat "$out/$target.paths"

    while read -r p; do
        collect "$p"
    done < "$out/$target.paths"

    # ---- identity (rule 0) -------------------------------------------
    {
        printf 'target: %s\n'   "$target"
        printf 'attr: %s\n'     "$attr"
        printf 'host-revision: %s\n' "${GEMINI_HOST_REV:-unknown}"
        printf 'egress-proxy: %s\n' "${GEMINI_PROXY:-direct}"
        printf 'built: %s\n'    "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        printf 'vm: %s %s\n'    "$(uname -m)" "$(uname -r)"
        printf 'nix: %s\n'      "$(nix --version)"
        printf 'source: %s\n'   "$src"
        printf 'store-paths:\n'
        sed 's/^/  /' "$out/$target.paths"
    } > "$out/$target.manifest"

    : > "$out/$target.sha256"
    for f in "$out"/*.img; do
        if [ -f "$f" ]; then
            sha256sum "$f" >> "$out/$target.sha256"
        fi
    done
    cat "$out/$target.sha256"
done

echo "=== [vm] done — artifacts in $out ==="
