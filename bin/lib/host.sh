#!/usr/bin/env bash
# host.sh — host-side helpers shared by the device scripts (net-up,
# device-ssh, device-reboot, boot-switch, flash-nixos), so the Linux and
# macOS halves live in ONE reviewed place instead of five copies.
#
# WHY THIS EXISTS (2026-09-17): the device's USB NIC moved from RNDIS to
# CDC-ECM, which macOS drives natively — so the Mac can now do the whole
# reboot/flash cycle over the USB cable, and the host scripts must work on
# both platforms (docs/usb-network.md). The differences are small but not
# nil: Linux spells the link `ip`, macOS `ifconfig`; Linux's `ping -W`
# takes SECONDS while macOS's takes MILLISECONDS; `lsusb` exists only on
# Linux (macOS = system_profiler); and adb / GNU coreutils
# (`timeout`, `stat -c`, `readlink -f`, `du`) come from the flake devshell
# on BOTH platforms — on macOS that is flake-macos.nix's
# devShells.aarch64-darwin.default. Use `gemini_devshell_reexec` for that.
#
# Sourcing, not executing: `source bin/lib/host.sh`. Safe under `set -euo
# pipefail` (nothing here is expected to fail at source time).
#
# The link is the g_ether gadget (network device `usb0` on the device):
#   device 10.15.19.82 / MAC 42:00:15:19:82:01  (kernel param g_ether.dev_addr)
#   host   10.15.19.1  / MAC 42:00:15:19:82:00  (kernel param g_ether.host_addr)
# The host MAC reaches the host through the CDC-ECM functional descriptor
# (f_ecm -> gether_get_host_addr_cdc), and both Linux's cdc_ether and
# macOS's AppleUSBCDCECMData adopt it — which is what lets us FIND the
# interface by MAC instead of guessing its name. GEMINI_USB_IFACE
# overrides the search when a driver does not adopt it.

# shellcheck shell=bash
[ -n "${GEMINI_LIB_HOST_SH:-}" ] && return 0
GEMINI_LIB_HOST_SH=1

GEMINI_DEV_MAC="${GEMINI_DEV_MAC:-42:00:15:19:82:01}"
GEMINI_HOST_MAC="${GEMINI_HOST_MAC:-42:00:15:19:82:00}"
# Shared prefix: which end gets which address varies by driver, so match
# the pair rather than one exact value. Derived from GEMINI_DEV_MAC (its
# last octet stripped: 42:00:15:19:82:01 -> 42:00:15:19:82:), so an
# override carries.
GEMINI_MAC_PREFIX="${GEMINI_MAC_PREFIX:-${GEMINI_DEV_MAC%:*}:}"
GEMINI_HOST_IP="${GEMINI_HOST_IP:-10.15.19.1}"
GEMINI_DEV_IP="${GEMINI_DEV_IP:-10.15.19.82}"

# linux | macos | other
gemini_platform() {
  case "$(uname -s)" in
    Darwin) echo macos ;;
    Linux)  echo linux ;;
    *)      echo other ;;
  esac
}
gemini_is_macos() { [ "$(gemini_platform)" = macos ]; }
gemini_is_linux() { [ "$(gemini_platform)" = linux ]; }

# ---- the USB NIC -------------------------------------------------------

# Print the host interface carrying the gadget link, or nothing.
# $GEMINI_USB_IFACE wins when set (escape hatch for drivers that rewrite
# the MAC). Otherwise: match the gadget MAC pair, then fall back to
# platform naming heuristics.
gemini_usb_iface() {
  local iface=""
  if [ -n "${GEMINI_USB_IFACE:-}" ]; then
    echo "$GEMINI_USB_IFACE"
    return 0
  fi
  case "$(gemini_platform)" in
    linux)
      iface=$(ip -o link show 2>/dev/null | awk -v m="$GEMINI_MAC_PREFIX" '
        { l = tolower($0) }
        l ~ m { sub(/:$/, "", $2); print $2; exit }')
      if [ -z "$iface" ]; then
        # USB NIC naming: enp<bus>s<slot>f<func>u<port> (predictable
        # names), or usbN. Kept as a fallback only.
        iface=$(ip -o link show 2>/dev/null | awk -F': ' '
          $2 ~ /^enp.*u[0-9]/ || $2 ~ /^usb[0-9]/ { print $2; exit }')
      fi
      ;;
    macos)
      iface=$(ifconfig -a 2>/dev/null | awk -v m="$GEMINI_MAC_PREFIX" '
        /^[a-z0-9]+:/ { cur = substr($1, 1, length($1) - 1); next }
        cur != "" && tolower($0) ~ ("ether " m) { print cur; exit }')
      if [ -z "$iface" ]; then
        # Hardware-port map: a CDC-ECM gadget shows up as its own port
        # (its USB product string is "Ethernet Gadget", ether.c
        # DRIVER_DESC with the RNDIS prefix gone).
        iface=$(networksetup -listallhardwareports 2>/dev/null | awk '
          /^Hardware Port:/ { port = tolower($0) }
          /^Device: /       { if (port ~ /(ethernet gadget|cdc|ecm|usb.*ethernet)/) { print $2; exit } }')
      fi
      ;;
  esac
  [ -n "$iface" ] && echo "$iface"
  return 0
}

# Bring the host end of the link up with its static address. Needs root.
# Idempotent — safe to call on every device power-cycle (the address is
# lost whenever the gadget drops and the interface re-appears).
# Usage: gemini_net_up            (assume root)
gemini_net_up() {
  local iface
  iface=$(gemini_usb_iface)
  [ -n "$iface" ] || return 1
  case "$(gemini_platform)" in
    linux)
      ip link set "$iface" up
      ip addr replace "$GEMINI_HOST_IP/24" dev "$iface"
      ;;
    macos)
      ifconfig "$iface" inet "$GEMINI_HOST_IP" netmask 255.255.255.0 up
      ;;
  esac
}

# Same, but for scripts that are NOT already root: try passwordless sudo,
# and explain exactly what to run when that is not available (the macOS
# `sudo` cache is per-tty, so a fresh shell usually needs `sudo -v`).
gemini_net_up_auto() {
  if [ "$(id -u)" = 0 ]; then
    gemini_net_up
    return $?
  fi
  local iface
  iface=$(gemini_usb_iface)
  [ -n "$iface" ] || return 1
  case "$(gemini_platform)" in
    linux)
      sudo -n ip link set "$iface" up 2>/dev/null &&
        sudo -n ip addr replace "$GEMINI_HOST_IP/24" dev "$iface" 2>/dev/null
      ;;
    macos)
      sudo -n ifconfig "$iface" inet "$GEMINI_HOST_IP" netmask 255.255.255.0 up 2>/dev/null
      ;;
  esac
}

# ping the device. rc 0 = it answered. Linux: -W is SECONDS, macOS: ms.
gemini_ping() {
  local ip="${1:-$GEMINI_DEV_IP}" secs="${2:-2}"
  case "$(gemini_platform)" in
    macos) ping -c 1 -W "$((secs * 1000))" "$ip" >/dev/null 2>&1 ;;
    *)     ping -c 1 -W "$secs" "$ip" >/dev/null 2>&1 ;;
  esac
}

# ---- USB bus introspection (the `lsusb` replacement) -------------------

# Print "vid:pid" (lowercase) for every USB device the host can see.
# Linux: lsusb (devshell/root — rule 7). macOS: no usbutils exists, so
# system_profiler is the source; it reports the pair per device as
# "Product ID: 0x...." then "Vendor ID: 0x....". Best-effort by design —
# callers treat an empty list as "cannot see the bus", never as "absent".
gemini_usb_ids() {
  case "$(gemini_platform)" in
    linux)
      lsusb 2>/dev/null |
        sed -n 's/.*ID \([0-9a-fA-F]\{4\}\):\([0-9a-fA-F]\{4\}\).*/\1:\2/p'
      ;;
    macos)
      # system_profiler prints the ids as 0xNNNN, and the ``Product ID``
      # line comes before the ``Vendor ID`` line — pair them, then strip
      # the 0x so the output matches the Linux ``vid:pid`` form.
      system_profiler SPUSBDataType 2>/dev/null | awk '
        /Product ID:/ { p = $3 }
        /Vendor ID:/  { if (p != "") { print tolower($3) ":" tolower(p); p = "" } }' |
        sed 's/0x//g'
      ;;
  esac
}

# gemini_usb_present <vid:pid> — rc 0 when that device is on the bus.
gemini_usb_present() {
  local want
  want=$(printf '%s' "$1" | tr 'A-F' 'a-f')
  [ -n "$want" ] || return 1
  gemini_usb_ids | tr 'A-F' 'a-f' | grep -qx "$want"
}

# ---- SSH identity (the keypair is committed IN this repo) --------------
# No important file lives outside the repo: the admin keypair is
# keys/gemini_ed25519 (private) + .pub, committed alongside the code, and
# the device trusts both its login half (config/gemini.nix
# authorizedKeys.keyFiles) and uses it as its PINNED sshd host key — so a
# from-scratch reflash needs no key provisioning and does not change the
# device's SSH identity. Rationale and the (accepted) security trade:
# keys/README.md. Override with GEMINI_SSH_KEY=<path> to use another key.
GEMINI_REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
GEMINI_KEY_DIR="${GEMINI_KEY_DIR:-$GEMINI_REPO_ROOT/keys}"
GEMINI_SSH_KEY="${GEMINI_SSH_KEY:-$GEMINI_KEY_DIR/gemini_ed25519}"
GEMINI_KNOWN_HOSTS="${GEMINI_KNOWN_HOSTS:-$GEMINI_KEY_DIR/known_hosts}"

# gemini_ssh_key — print the admin private key path, tightening its mode
# first. Git stores only the exec bit, so a fresh clone arrives 0644 and
# ssh refuses it ("permissions are too open"); fixing it here means the
# scripts work straight after a clone. rc 1 + a hint when the key is gone.
gemini_ssh_key() {
  local key="$GEMINI_SSH_KEY" mode=""
  if [ ! -f "$key" ]; then
    echo "!! SSH key '$key' not found" >&2
    echo "   It is committed in this repo (keys/gemini_ed25519 — see keys/README.md);" >&2
    echo "   set GEMINI_SSH_KEY=<path> to use a different identity." >&2
    return 1
  fi
  # coreutils stat on Linux + in the devshell; BSD stat on bare macOS.
  mode=$(stat -c '%a' "$key" 2>/dev/null || stat -f '%Lp' "$key" 2>/dev/null || true)
  case "$mode" in
    400|600|"") : ;; # fine (or unknowable — let ssh decide)
    *)
      if chmod 600 "$key" 2>/dev/null; then
        echo "$(basename "$key"): chmod 600 (git cannot store permissions, ssh requires them)" >&2
      fi
      ;;
  esac
  printf '%s\n' "$key"
}

# ---- tooling / devshell (rule 7) ---------------------------------------

# gemini_ensure_tools <tool>... — rc 0 when all are on PATH.
gemini_ensure_tools() {
  local c
  for c in "$@"; do
    command -v "$c" >/dev/null 2>&1 || return 1
  done
  return 0
}

# gemini_need_devshell — rc 0 when a device script should re-exec inside
# the flake devshell, i.e. when the standard toolset is not on PATH:
# `adb` on either platform, plus `lsusb` on Linux (usbutils) and the GNU
# coreutils (`timeout`) on macOS. (After the re-exec the tools exist, so
# the check is false and no loop is possible; GEMINI_DEVSH_REEXEC in
# gemini_devshell_reexec is the belt-and-braces guard.)
gemini_need_devshell() {
  if gemini_ensure_tools adb; then
    # adb is there — but a bare host still lacks the extras these scripts
    # use: `lsusb` (usbutils) on Linux, the GNU coreutils (`timeout`) on
    # macOS. Both come from the flake devshell.
    if gemini_is_macos; then
      gemini_ensure_tools timeout || return 0
    else
      gemini_ensure_tools lsusb || return 0
    fi
    return 1
  fi
  return 0
}

# gemini_devshell_reexec <script> [args...] — exec this script inside the
# flake devshell (Linux: devShells.x86_64-linux.default; macOS:
# devShells.aarch64-darwin.default from flake-macos.nix). Only call it
# after gemini_need_devshell said yes. On success it REPLACES the process
# and never returns; it returns 1 when the tools are missing even inside
# the devshell (GEMINI_DEVSH_REEXEC guards the loop). The repo root is
# passed to `nix develop` as an explicit flake path — nix does not search
# upward for flake.nix, so the script's cwd cannot be relied on.
gemini_devshell_reexec() { # <script> [args...]
  local script="$1"; shift
  local abs dir
  abs=$(cd "$(dirname "$script")" 2>/dev/null && pwd)/$(basename "$script")
  dir=$(dirname "$abs")
  if [ -n "${GEMINI_DEVSH_REEXEC:-}" ]; then
    echo "!! required tools are missing even inside the devshell" >&2
    echo "   (run: nix develop --command bash $abs)" >&2
    return 1
  fi
  command -v nix >/dev/null 2>&1 || {
    echo "!! tools missing and nix is not installed to supply them" >&2
    return 1
  }
  echo ">> $(basename "$script"): re-exec'ing inside the flake devshell" >&2
  GEMINI_DEVSH_REEXEC=1 exec nix develop "$dir" --command bash "$abs" "$@"
}
