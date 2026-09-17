#!/bin/bash
# net-up.sh — bring up the host side of the Gemini's USB NIC after the
# device has been power-cycled (the gadget interface disappears on
# power-off, so the host address must be re-added every cold boot / WDT-free
# power loss). Idempotent. Usage: sudo bash bin/net-up.sh
#
# Device side: none — the gadget auto-configures 10.15.19.82 at boot
# (usb0, from the g_ether.dev_addr kernel param).
#
# GOLDEN RULE: the host link is DOWN after any device power-off/power-cycle.
# bin/device-ssh.sh auto-runs the same setup (via passwordless sudo) when
# the link looks down, so plain `bash bin/device-ssh.sh '<cmd>'` works after
# power-on.
#
# [2026-09-17] Works on Linux AND macOS: the gadget moved from RNDIS to
# CDC-ECM, which both drive natively, and the interface is found by its
# MAC (42:00:15:19:82:00 — the host end the ECM descriptor carries) rather
# than by a hardcoded name, so USB port renumbering no longer breaks it.
# Linux uses `ip`, macOS `ifconfig`; see bin/lib/host.sh. Receipts:
# docs/usb-network.md.
#
# Ported from the GeminiPDA project (build/net-up.sh), which verified the
# mechanism on this hardware. See AGENTS.md.
set -u

cd "$(dirname "$0")/.."
. bin/lib/host.sh

[ "$(id -u)" = 0 ] || { echo "!! needs root to configure the interface — run: sudo bash bin/net-up.sh" >&2; exit 1; }

IFACE=$(gemini_usb_iface)
if [ -z "$IFACE" ]; then
  echo "!! USB NIC not found — no host interface carries ${GEMINI_MAC_PREFIX}*" >&2
  echo "   is the device powered on, and the cable in the LEFT (gadget) port?" >&2
  echo "   (override the search with GEMINI_USB_IFACE=<iface> if needed)" >&2
  exit 1
fi

gemini_net_up || { echo "!! could not configure $IFACE" >&2; exit 1; }

if gemini_ping; then
  echo "OK: $GEMINI_DEV_IP reachable on $IFACE"
else
  echo "!! $GEMINI_DEV_IP not answering yet on $IFACE (device still booting? retry in a few s)"
fi
