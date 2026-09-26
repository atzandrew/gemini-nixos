#!/bin/sh
# install-usb-nic-daemon.sh — install (or remove) the root LaunchDaemon
# that keeps the Mac's side of the Gemini USB link configured, so the
# build/flash/reboot cycle needs NO password after this one-time install.
#
# WHAT IT DOES: copies bin/macos/usb-nic-up.sh to
# /usr/local/libexec/gemini-usb-nic.sh and com.gemini.usb-nic.plist to
# /Library/LaunchDaemons/, then bootstraps the daemon. The daemon polls
# for the gadget interface (MAC 42:00:15:19:82:*) and assigns
# 10.15.19.1/24 whenever it appears without the address.
#
# WHY A DAEMON: the interface and its address vanish on every device
# power cycle; `ifconfig` needs root; macOS `sudo` timestamps are per-tty,
# so scripts/agents cannot reliably (re)apply it. See docs/usb-network.md.
#
# MUST RUN AS ROOT (this is the intended one-time admin prompt):
#   sudo bash bin/macos/install-usb-nic-daemon.sh
# or without a terminal:
#   osascript -e 'do shell script "bash <abs path> install" with administrator privileges'
#
# Verbs:
#   install    copy + bootstrap + enable (default)
#   uninstall  bootout + remove the installed files
#   status     show whether the daemon is loaded and the link state
# Env: GEMINI_SELFTEST=1 with `install` also proves the auto-(re)assign by
# deleting the address and waiting for the daemon to put it back.
#
# Updating: re-run `install` after editing bin/macos/usb-nic-up.sh; the
# repo remains the source of truth.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
label=com.gemini.usb-nic
dst_lib=/usr/local/libexec/gemini-usb-nic.sh
dst_plist=/Library/LaunchDaemons/$label.plist
logfile=/var/log/gemini-usb-nic.log

MAC_PREFIX=${GEMINI_MAC_PREFIX:-42:00:15:19:82:}
HOST_IP=${GEMINI_HOST_IP:-10.15.19.1}

die() { echo "install-usb-nic: $*" >&2; exit 1; }
need_root() { [ "$(id -u)" = 0 ] || die "needs root — run: sudo bash $0 ${1:-install}"; }

iface() {
    ifconfig -a 2>/dev/null | awk -v m="$MAC_PREFIX" '
        /^[a-z0-9]+:/ { cur = substr($1, 1, length($1) - 1); next }
        { l = tolower($0) }
        l ~ tolower(m) { print cur; exit }'
}

do_install() {
    need_root install
    [ -f "$here/usb-nic-up.sh" ] || die "missing $here/usb-nic-up.sh"
    [ -f "$here/$label.plist" ] || die "missing $here/$label.plist"

    install -d -m 0755 /usr/local/libexec
    install -m 0755 "$here/usb-nic-up.sh" "$dst_lib"
    install -m 0644 "$here/$label.plist" "$dst_plist"
    chown root:wheel "$dst_lib" "$dst_plist"

    # Idempotent (re)load: bootout is a no-op when not loaded.
    launchctl bootout system "$dst_plist" >/dev/null 2>&1 || true
    launchctl bootstrap system "$dst_plist"
    launchctl enable "system/$label" >/dev/null 2>&1 || true
    launchctl kickstart -k "system/$label" >/dev/null 2>&1 || true
    echo "install-usb-nic: installed + started $label"
    echo "install-usb-nic: log = $logfile"

    [ "${GEMINI_SELFTEST:-}" = 1 ] && do_selftest
    return 0
}

do_uninstall() {
    need_root uninstall
    launchctl bootout system "$dst_plist" >/dev/null 2>&1 || true
    rm -f "$dst_plist" "$dst_lib"
    echo "install-usb-nic: removed $label"
}

do_selftest() {
    need_root selftest
    ifc=$(iface)
    [ -n "$ifc" ] || die "selftest: no gadget interface (MAC ${MAC_PREFIX}*) present"
    echo "install-usb-nic: selftest on $ifc — clearing ${HOST_IP}, expecting the daemon to restore it"
    ifconfig "$ifc" inet "$HOST_IP" delete 2>/dev/null || die "selftest: could not clear ${HOST_IP} on $ifc"
    i=0
    while [ "$i" -lt 8 ]; do
        if ifconfig "$ifc" 2>/dev/null | grep -q "inet ${HOST_IP} "; then
            echo "install-usb-nic: selftest PASS — ${HOST_IP} restored on $ifc"
            return 0
        fi
        i=$((i + 1))
        sleep 2
    done
    die "selftest FAIL — ${HOST_IP} was not restored within 16 s (see $logfile)"
}

do_status() {
    if [ "$(id -u)" = 0 ]; then
        launchctl print "system/$label" 2>/dev/null | sed -n '1,12p' || echo "daemon: not loaded"
    else
        sudo -n launchctl print "system/$label" 2>/dev/null | sed -n '1,12p' \
            || echo "daemon: (need root to query — sudo bash $0 status)"
    fi
    ifc=$(iface)
    if [ -n "$ifc" ]; then
        echo "iface: $ifc"
        ifconfig "$ifc" | grep -E "status:|inet " || true
    else
        echo "iface: (gadget not attached)"
    fi
}

case ${1:-install} in
    install)   do_install ;;
    uninstall) do_uninstall ;;
    selftest)  do_selftest ;;
    status)    do_status ;;
    *) die "unknown verb '$1' (install|uninstall|selftest|status)" ;;
esac
