#!/bin/sh
# charger-selftest.sh — run charger.sh's detection + key readers on the live
# Gemini using ONLY the initrd's busybox applets (Debian's coreutils are kept
# out of PATH), so busybox quirks show up before a flash.
#
#   On Hydra:
#     rm -rf /tmp/ird && mkdir /tmp/ird && cd /tmp/ird && \
#       zcat $(cd ~/Build/gemini-nixos && nix build .#packages.aarch64-linux.initrd \
#         --print-out-paths --no-link)/initrd | cpio -id 2>/dev/null
#     scp /tmp/ird/bin/busybox /tmp/ird/charger.sh \
#       ~/Build/gemini-nixos/bin/charger-selftest.sh atzero@10.0.20.216:/tmp/
#     ssh -t atzero@10.0.20.216 sudo sh /tmp/charger-selftest.sh
#   (sudo: /dev/input/event* is root-only.) During the 20 s window: tap Esc/On
#   once, then hold it ~1 s, then press a keyboard key.
set -u
D=/tmp/chg-selftest
BB=/tmp/busybox
rm -rf "$D"; mkdir -p "$D/bin" "$D/run"
for a in sh dd cmp printf tr sed cat mkdir basename sleep kill pkill wc cut; do
    ln -s "$BB" "$D/bin/$a"
done
export PATH="$D/bin"

"$D/bin/sh" -c '
    CHG_RUN='"$D"'/run
    . /tmp/charger.sh
    if charger_mode_detect; then echo "DETECT: YES (mode $CHG_BOOTMODE, reason $CHG_BOOTREASON)"
    else echo "DETECT: no (mode $CHG_BOOTMODE, reason $CHG_BOOTREASON)"; fi
    chg_log() { echo "log: $*"; }
    chg_start_readers
    echo "--- 20 s: tap Esc/On, hold Esc/On ~1 s, press a keyboard key"
    sleep 20
    chg_stop_readers
    echo "--- key log (\"116 1\" = Esc/On long-press, \"0 1\" = any other key press)"
    cat "$CHG_RUN/chg-keys"
'
