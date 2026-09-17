#!/bin/bash
# device-reboot.sh — REMOTE-REBOOT the Gemini PDA.
#
# The device is reachable over the USB gadget network (10.15.19.82).
# [updated 2026-09-10] `systemctl reboot` now works on glass: the in-repo
# mt6797-power driver registers a restart handler that replays LK's
# mtk_wdt_reset(1) (docs/power-states.md). This script still uses the
# independent WDT-EXRST arm (no systemd dependency — usable when the
# guest is too broken to shut down), kept as the robust fallback.
# Before the driver, software resets (systemctl reboot / WDT SWRST)
# POWERED THE DEVICE OFF on this unit (verified 2026-08-31) — only the
# LK-configured WDT EXRST path self-boots. So we set the WDT timeout to
# 2s via /dev/mem and let it
# expire: the WDT fires the external PMIC reset, the device power-cycles
# and boots on its own. Gadget + SSH are back in ~5-10s.
#
# [added 2026-09-10] The A72 cluster bring-up (services/scripts/cl2-up.sh,
# run by gemini-a72-up on every boot) leaves LK's WDT MODE disarmed
# (0x10007000 = 0) — with MODE clear, the 0x48 LENGTH arm silently
# no-ops and no EXRST ever fires (the documented "reboot trap",
# docs/phase-2-on-glass.md §2b; the register values were re-confirmed on
# glass 2026-09-10: MODE=0, LENGTH=0x40). So restore LK's mode value
# (key | 0x5D) first, then arm. Without this the reboot looks like it
# did nothing and the unit stays up (or `systemctl reboot` powers it off).
#
# [2026-09-17] Works on Linux AND macOS, and no longer needs `lsusb`:
# presence is judged by the USB NIC interface + ping/ssh, which is what
# actually matters here (the gadget is CDC-ECM since 2026-09-17 —
# docs/usb-network.md; helpers in bin/lib/host.sh). `lsusb` had made this
# Linux-only, which is what blocked the Mac reboot cycle.
#
# Device-side equivalent (from a root shell, no host): the gemini-wdt-reboot
# unit/CLI in services/scripts does the same devmem write.
#
# Ported from the GeminiPDA project (build/device-reboot.sh).
set -e
cd "$(dirname "$0")/.."
. bin/lib/host.sh
DEV="$GEMINI_DEV_IP"
KEY="${GEMINI_SSH_KEY:-$HOME/.ssh/id_ed25519_gemini}"
[ -f "$KEY" ] || { echo "!! SSH key $KEY not found — see bin/device-ssh.sh header" >&2; exit 1; }
SSH() { ssh -i "$KEY" \
  -o BatchMode=yes -o IdentitiesOnly=yes \
  -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
  -o ConnectTimeout=8 root@"$DEV" "$@"; }
die() { echo "!! $*" >&2; exit 1; }

gemini_ping "$DEV" ||
  die "$DEV not reachable — is the device booted (USB cable in the LEFT/gadget port)?"

BEFORE_ID=$(SSH 'cat /proc/sys/kernel/random/boot_id' 2>/dev/null || true)

echo ">> restoring WDT MODE (key|0x5D — A72 bring-up disarms it) + arming WDT at 2s (0x48) — EXRST-reboot in ~5s"
SSH "busybox devmem 0x10007000 32 0x2200005D; busybox devmem 0x10007004 32 0x48"

# Wait for the link to DROP first. Without this the "is the device back?"
# loop could succeed before the reset fired (USB stays up for the ~2 s WDT
# window and can re-enumerate fast), reporting a reboot that had not
# happened — observed 2026-09-10. Either signal counts: the gadget NIC
# stops answering ping, or ssh stops answering.
echo ">> reboot triggered; waiting for the link to drop (proves the reset fired)..."
dropped=0
for i in $(seq 1 15); do
  sleep 2
  if ! gemini_ping "$DEV" 2 || ! SSH 'true' 2>/dev/null; then
    dropped=1
    echo ">> link dropped after ~$((i * 2))s"
    break
  fi
done
if [ "$dropped" != 1 ]; then
  die "link never dropped — the WDT did not fire (A72 MODE disarm trap? see docs/phase-2-on-glass.md section 2b); device is still up"
fi

# The gadget re-enumerates WITHOUT its host-side address, so re-apply it
# before every probe (idempotent).
echo ">> waiting for the link to return + ssh..."
warned=0
deadline=$((SECONDS + 240))
while [ "$SECONDS" -lt "$deadline" ]; do
  sleep 3
  iface=$(gemini_usb_iface)
  [ -n "$iface" ] || continue
  if ! gemini_net_up_auto 2>/dev/null && [ "$warned" = 0 ]; then
    echo "!! cannot set $GEMINI_HOST_IP on $iface (needs sudo) — run 'sudo -v' first" >&2
    echo "   (macOS sudo is per-tty), or: sudo bash bin/net-up.sh" >&2
    warned=1
  fi
  if gemini_ping "$DEV" 2; then
    # Reachable != rebooted: require the boot id to have changed.
    AFTER_ID=$(SSH 'cat /proc/sys/kernel/random/boot_id' 2>/dev/null || true)
    if [ -n "$BEFORE_ID" ] && [ "$AFTER_ID" = "$BEFORE_ID" ]; then
      die "device reachable but boot_id is unchanged — it did not reboot"
    fi
    echo ">> device back at $DEV (new boot_id) OK"
    exit 0
  fi
done
die "device not back within 240s — may need a physical power-on"
