#!/bin/bash
# gemshell-nested.sh — run the gemshell UI in a window on THIS workstation,
# for fast iteration without touching the device.
#
# Two host modes:
#
#   Linux  — the original nested loop: build the x86_64-linux package
#            (`nix build .#packages.x86_64-linux.gemshell`) and run it as a
#            NESTED Wayland client under the current session
#            (GEMSHELL_NESTED=1; host EGL/GBM + wl_shm present).
#
#   macOS  — a native Cocoa window (no Wayland on macOS): build with the
#            host Rust toolchain (`cargo build` in pkgs/gemshell) and run
#            the same binary with the macOS backend (desktop GL 3.3 core).
#            This is a UI test harness — the shell chrome, the egui
#            settings panel and mouse/keyboard input all work; there are
#            no Wayland clients to host. See docs/gemshell.md.
#
# Both modes open the egui settings panel at startup
# (GEMSHELL_OPEN_SETTINGS=1, the default here) so the UI is visible
# immediately.
#
# Env knobs:
#   GEMSHELL_NESTED_SCALE   window size / logical scene size (default 0.5)
#   GEMSHELL_OPEN_SETTINGS  open the in-process settings panel (default 1)
#   GEMSHELL_AUTOSTART      extra client to launch (default none)
#   WAYLAND_DISPLAY         host socket (Linux; default wayland-0)
#   GEMSHELL_BUILD=0        Linux: skip the nix build (use the cached path)
#   GEMSHELL_PROFILE        macOS: "release" to build optimised (default debug)
#
# Design + receipts: docs/gemshell.md ("Nested mode on x86_64" /
# "macOS preview window").
#
# Usage: bash bin/gemshell-nested.sh [extra gemshell args...]
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo"

export GEMSHELL_NESTED=1
export GEMSHELL_NESTED_SCALE="${GEMSHELL_NESTED_SCALE:-0.5}"
# The gemini xkb layout (Fn layer) straight from the repo tree (Linux).
export XKB_CONFIG_EXTRA_PATH="$repo/config/xkb"
export XKB_DEFAULT_LAYOUT="${XKB_DEFAULT_LAYOUT:-gemini}"
# A UI font: find_font() checks GEMSHELL_FONT first; on Linux the
# workstation has no /usr/share/fonts, so ask fontconfig. (macOS falls
# back to /System/Library/Fonts inside find_font.)
if [ -z "${GEMSHELL_FONT:-}" ] && command -v fc-match >/dev/null 2>&1; then
    GEMSHELL_FONT=$(fc-match -f '%{file}' sans 2>/dev/null || true)
fi
export GEMSHELL_FONT
# The settings panel is in-process now; open it so the window has
# content immediately.
export GEMSHELL_OPEN_SETTINGS="${GEMSHELL_OPEN_SETTINGS:-1}"
export GEMSHELL_AUTOSTART="${GEMSHELL_AUTOSTART:-}"

if [ "$(uname -s)" = "Darwin" ]; then
    # --- macOS: native preview window -------------------------------------
    ws="$repo/pkgs/gemshell"
    if ! command -v cargo >/dev/null 2>&1; then
        echo "gemshell-nested: cargo not on PATH." >&2
        echo "  install Rust (e.g. 'brew install rust') or run inside 'nix shell nixpkgs#cargo'." >&2
        exit 1
    fi
    if [ "${GEMSHELL_PROFILE:-debug}" = "release" ]; then
        echo "== building gemshell (aarch64-darwin, release) ==" >&2
        (cd "$ws" && cargo build --release) >&2
        bin="$ws/target/release/gemshell"
    else
        echo "== building gemshell (macOS, debug) ==" >&2
        (cd "$ws" && cargo build) >&2
        bin="$ws/target/debug/gemshell"
    fi
    echo "== gemshell: $bin ==" >&2
    exec "$bin" "$@"
fi

# --- Linux: nested Wayland client under the current session ---------------
: "${XDG_RUNTIME_DIR:=/run/user/$(id -u)}"
export XDG_RUNTIME_DIR
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
if [ ! -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]; then
    echo "gemshell-nested: no Wayland socket at $XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" >&2
    echo "  (run from inside a graphical session, or set WAYLAND_DISPLAY)" >&2
    exit 1
fi
export HOME="${HOME:-/home/$(id -un)}"

if [ "${GEMSHELL_BUILD:-1}" != 0 ]; then
    echo "== building x86_64 gemshell ==" >&2
fi
out=$(nix build --no-link --print-out-paths ".#packages.x86_64-linux.gemshell" | tail -1)
echo "== gemshell: $out ==" >&2

export PATH="$out/bin:$PATH"

exec "$out/bin/gemshell" "$@"
