#!/bin/sh
# gemini-usb-nic.sh — keep the macOS side of the Gemini PDA's USB link
# configured, so the whole build/flash/reboot cycle runs with NO password
# prompt.
#
# WHY: the gadget is CDC-ECM (docs/usb-network.md), so macOS brings the
# interface up by itself — but with NO host address. Assigning
# 10.15.19.1/24 needs root, and the gadget interface (and its address)
# disappears on every device power-off/reboot (AGENTS.md cheat sheet:
# "AFTER ANY DEVICE POWER-ON/POWER-CYCLE the host side is DOWN"). A root
# LaunchDaemon running this script re-applies the address within
# $INTERVAL seconds of the interface (re)appearing, removing the
# `sudo -v` / admin-prompt step from the workflow entirely.
#
# Interface discovery is by MAC prefix — the host end
# (42:00:15:19:82:00, from the kernel param g_ether.host_addr) reaches
# macOS inside the CDC-ECM functional descriptor, so the name does not
# matter and USB port renumbering is irrelevant. Same rule as
# bin/lib/host.sh gemini_usb_iface(), reused conceptually here so the
# daemon has no dependency on the repo checkout.
#
# Usage:  gemini-usb-nic.sh            # daemon: poll + assign forever
#         gemini-usb-nic.sh --once     # single pass (installer/selftest)
#
# Installed + supervised by bin/macos/install-usb-nic-daemon.sh as
# /Library/LaunchDaemons/com.gemini.usb-nic.plist. Env overrides:
# GEMINI_MAC_PREFIX (42:00:15:19:82:), GEMINI_HOST_IP (10.15.19.1),
# GEMINI_NETMASK (255.255.255.0), GEMINI_USB_NIC_INTERVAL (5).
set -u

MAC_PREFIX=${GEMINI_MAC_PREFIX:-42:00:15:19:82:}
HOST_IP=${GEMINI_HOST_IP:-10.15.19.1}
NETMASK=${GEMINI_NETMASK:-255.255.255.0}
INTERVAL=${GEMINI_USB_NIC_INTERVAL:-5}

log() { echo "$(date '+%Y-%m-%dT%H:%M:%S%z') gemini-usb-nic: $*"; }

# Print the host interface carrying the gadget MAC, or nothing.
link_iface() {
    ifconfig -a 2>/dev/null | awk -v m="$MAC_PREFIX" '
        /^[a-z0-9]+:/ { cur = substr($1, 1, length($1) - 1); next }
        { l = tolower($0) }
        l ~ tolower(m) { print cur; exit }'
}

sync_once() {
    iface=$(link_iface)
    [ -n "$iface" ] || return 0
    if ifconfig "$iface" 2>/dev/null | grep -q "inet ${HOST_IP} "; then
        return 0
    fi
    if ifconfig "$iface" inet "$HOST_IP" netmask "$NETMASK" up; then
        log "assigned ${HOST_IP}/${NETMASK} to ${iface}"
    else
        log "ifconfig failed on ${iface}"
        return 1
    fi
}

case ${1:-} in
    --once) sync_once; exit $? ;;
esac

while :; do
    sync_once || true
    sleep "$INTERVAL"
done
