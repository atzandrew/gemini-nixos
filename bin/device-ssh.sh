#!/bin/bash
# device-ssh.sh — SSH into the Gemini PDA over the USB gadget network.
# Usage: bash bin/device-ssh.sh [command...]   (no args = interactive shell)
# Device: 10.15.19.82, root. KEY-BASED auth: the key lives IN this repo —
# keys/gemini_ed25519 — and the device declares its public half in
# config/gemini.nix, so a from-scratch reflash needs NO manual
# provisioning (the old "log in with the `toor` password and append the
# pubkey" recipe is retired). keys/README.md has the rationale + the
# accepted security trade; GEMINI_SSH_KEY=<path> overrides the identity.
#
# [2026-09-17] Works on Linux AND macOS. The gadget is CDC-ECM since
# 2026-09-17 (it used to be RNDIS), which both hosts drive natively, and
# the host interface is found by its MAC rather than a hardcoded name.
# See bin/lib/host.sh and docs/usb-network.md. A reach-the-device-somewhere-
# else override is still honoured: GEMINI_DEV_IP=<ip> (Wi-Fi/LAN).
#
# GOLDEN RULE: the host side of the USB link is DOWN after any device
# power-off/power-cycle. If the link looks down, bring it up automatically
# via the shared helpers (passwordless sudo). This is why plain
# `bash bin/device-ssh.sh '<cmd>'` works right after the device boots.
#
# NOTE: while the device runs the *GeminiPDA Debian* rootfs (pre-NixOS),
# this reaches that rootfs over sshd. After the NixOS rootfs is flashed
# to p27, the same address/key reach the NixOS stage-2 sshd instead —
# nothing else changes.
#
# Ported from the GeminiPDA project (build/device-ssh.sh).
cd "$(dirname "$0")/.."
# Host-side link helpers (Linux `ip` / macOS `ifconfig`, MAC-based
# interface discovery, platform ping, the repo SSH identity) —
# bin/lib/host.sh.
#
# Remember whether the caller set GEMINI_DEV_IP BEFORE the lib defines its
# default: a value from the environment means "reach the device there and
# skip host-side setup" (Wi-Fi/LAN). The lib adopts it either way.
GEMINI_DEV_IP_OVERRIDE="${GEMINI_DEV_IP:-}"
. bin/lib/host.sh

KEY=$(gemini_ssh_key) || exit 1

if [ -n "$GEMINI_DEV_IP_OVERRIDE" ]; then
  exec ssh -i "$KEY" \
    -o BatchMode=yes -o IdentitiesOnly=yes \
    -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -o ConnectTimeout=8 root@"$GEMINI_DEV_IP" "$@"
fi

# Device unreachable (or address missing after a gadget drop) -> set the
# host end up again, then ssh. Needs sudo: passwordless when cached (Linux
# `sudo -n`), otherwise tell the user exactly what to run (macOS sudo is
# per-tty, so a fresh shell usually wants `sudo -v`).
if ! gemini_ping; then
  IFACE=$(gemini_usb_iface)
  if [ -z "$IFACE" ]; then
    echo "!! USB NIC interface not found (no host iface with MAC ${GEMINI_MAC_PREFIX}*) —" >&2
    echo "   is the device powered on and the USB cable in the LEFT (gadget) port?" >&2
    exit 1
  fi
  if gemini_net_up_auto; then
    echo "device-ssh: USB NIC brought up ($GEMINI_HOST_IP/24 on $IFACE)" >&2
  else
    echo "!! cannot configure $IFACE — run: sudo bash bin/net-up.sh" >&2
    echo "   (macOS: 'sudo -v' first — sudo's timestamp is per-tty)" >&2
    exit 1
  fi
fi

exec ssh -i "$KEY" \
  -o BatchMode=yes -o IdentitiesOnly=yes \
  -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
  -o ConnectTimeout=8 root@"$GEMINI_DEV_IP" "$@"
