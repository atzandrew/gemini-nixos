#!/bin/bash
# usb-tether-nat.sh — HOST side of the Gemini PDA g_ether internet tether.
#
# The NixOS rootfs declares the g_ether link's internet assumptions
# (static 10.15.19.82/24, defaultGateway 10.15.19.1, nameserver 1.1.1.1 in
# config/gemini.nix) — but outbound internet only exists if THIS HOST NATs
# 10.15.19.0/24 to its own upstream. Link-local SSH (the phase-2 milestone,
# bin/device-ssh.sh) does NOT need this; internet-bound work on the device
# (nix/apt, DNS beyond the link) does.
#
# Idempotent: sysctl ip_forward + an nft masquerade table for
# 10.15.19.0/24 -> the host's upstream iface. The nft rules are RUNTIME
# (lost on host reboot / nft flush). To undo: `sudo nft delete table ip
# gemini-nat` (+ optionally sysctl net.ipv4.ip_forward=0).
#
# Usage: bash bin/usb-tether-nat.sh [upstream-iface]   (default: autodetect
# the iface with the default route — the sibling script defaulted to brlan;
# on hosts whose upstream is not the default-route iface, pass it: e.g.
# `bash bin/usb-tether-nat.sh wlan0`).
#
# Prereqs on the HOST (not the devshell): ip (iproute2), nft (nftables),
# sudo. This repo's devshell does NOT carry nft (it is host network
# tooling, not device tooling); if `which nft` is empty on the bare host
# PATH, install it host-wide (`sudo apt install nftables`, or on NixOS add
# it to configuration.nix environment.systemPackages) — the script fails
# loudly rather than half-configuring the host.
#
# [2026-09-17] LINUX-ONLY by design: it is `ip`/`nft`/`sysctl`. The Mac
# does not need it — the device's own NetworkManager connects to Wi-Fi
# (services/wifi.nix), and the USB NIC is a link-local debug path only.
# On macOS the equivalent would be pf; not implemented.
#
# Ported from the GeminiPDA project (build/usb-tether-nat.sh), 2026-09-08
# (outstanding.md item 7); delta: auto-detected default upstream iface +
# explicit tool checks. Device side (persistent): the static 10.15.19.82
# config in config/gemini.nix (mirrors the Debian
# rootfs-files/usb-tether/dhcpcd-tether.conf recipe).
set -euo pipefail

UPSTREAM="${1:-}"

for tool in ip nft sudo sysctl; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "!! missing host tool '$tool' — install it (see header)" >&2
    exit 1
  }
done

# Auto-detect: the iface the host's own internet route leaves by.
if [ -z "$UPSTREAM" ]; then
  UPSTREAM=$(ip route get 1.1.1.1 2>/dev/null | sed -n 's/.* dev \([^ ]*\).*/\1/p' | head -1)
fi
[ -n "$UPSTREAM" ] || { echo "!! no default route — cannot pick the upstream iface; pass one explicitly" >&2; exit 1; }
ip link show "$UPSTREAM" >/dev/null 2>&1 || { echo "!! iface '$UPSTREAM' not found" >&2; exit 1; }

if ! ip route get 1.1.1.1 2>/dev/null | grep -q "dev $UPSTREAM"; then
  echo "!! the route to 1.1.1.1 is not via $UPSTREAM — pass the right iface:" >&2
  ip route get 1.1.1.1 >&2 || true
  exit 1
fi

sudo sysctl -w net.ipv4.ip_forward=1 >/dev/null

if ! sudo nft list table ip gemini-nat >/dev/null 2>&1; then
  sudo nft add table ip gemini-nat
fi
if ! sudo nft list chain ip gemini-nat post >/dev/null 2>&1; then
  sudo nft add chain ip gemini-nat post '{ type nat hook postrouting priority srcnat; policy accept; }'
fi
if ! sudo nft list chain ip gemini-nat post | grep -q "oifname \"$UPSTREAM\""; then
  sudo nft add rule ip gemini-nat post ip saddr 10.15.19.0/24 ip daddr != 10.15.19.0/24 oifname "$UPSTREAM" masquerade
  echo "NAT rule added (10.15.19.0/24 -> $UPSTREAM)"
else
  echo "NAT rule already present"
fi
echo "tether NAT ready: device 10.15.19.82 -> host ($UPSTREAM) -> internet"
echo "device check: bash bin/device-ssh.sh 'ping -c1 1.1.1.1'"
