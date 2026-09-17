#!/usr/bin/env bash
# Generate the LEAN Gemini PDA kernel config from the full bring-up config.
#
# Usage: bash bin/prune-kernel-config.sh [in-config] [out-config]
#   in-config  default: devices/planet-geminipda/kernel/config.full-329
#              (the exact #329 bring-up .config — everything arm64
#              defconfig enables, byte-identical to the on-glass build
#              except the EXTRA_FIRMWARE_DIR host path, fixed below)
#   out-config default: devices/planet-geminipda/kernel/config
#
# WHY: the #329 config is `defconfig + fragments` — it compiles the whole
# arm64 world (nouveau, exynos DRM, rockchip clocks, ACPI/EFI/XEN/KVM,
# SATA/NVMe, all vendor SoC drivers...). The Gemini PDA is ONE SoC
# (MT6797) with eMMC + USB (HID/mass-storage/CDC-ether/gadget-ether) +
# internal wifi. Drivers for hardware that can never exist on this unit
# are pure build-time + Image/module-tree waste. This prune keeps the
# "can physically be present" set and cuts the rest.
#
# METHOD: pure text edits (idempotent, no kconfig tooling needed — the
# kernel builder runs `make olddefconfig` anyway, which cascades the
# parent switches to dependent symbols). Rerun after any kernel-line
# change. The rule-5 gate in kernel/default.nix asserts the OUTPUT
# config still carries no mediatek-drm/panel landmines.
#
# Config files carry machine-absolute EXTRA_FIRMWARE_DIR (CONSYS ROM
# patches) — both configs are normalized to "firmware" (relative to
# $(srctree); the blobs are copied into the source tree at build time by
# kernel/default.nix from pkgs/gemini-firmware/).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IN="${1:-$REPO_ROOT/devices/planet-geminipda/kernel/config.full-329}"
OUT="${2:-$REPO_ROOT/devices/planet-geminipda/kernel/config}"

[ -f "$IN" ] || { echo "FAIL: input config $IN missing" >&2; exit 1; }
cp "$IN" "$OUT.tmp"

# Drop engine: reads the list of CONFIG_ names from $1 (one per line,
# exact symbol names) and disables them in $2 (already-disabled lines
# untouched → idempotent; existing comments preserved).
disable_from_list() {
  local list="$1" file="$2"
  [ -s "$list" ] || return 0   # empty list = nothing to do
  awk 'NR==FNR { d[$1]=1; next }
       /^# CONFIG_/ { print; next }
       /^CONFIG_/ {
         sym = $0; sub(/=.*/, "", sym);
         if (sym in d) { print "# " sym " is not set"; next }
       }
       { print }' "$list" "$file" > "$file.new"
  mv "$file.new" "$file"
}

# Drop every driver symbol of a family, keeping the regex matches (bare
# MASTER switches ending right after the prefix are excluded by the
# required trailing "_driver..." part).
#   $1 = CONFIG prefix WITHOUT trailing underscore (drivers only)
#   $2 = egrep keep pattern for symbols that must survive
drop_family() {
  local prefix="$1" keep="$2"
  grep -oE "^${prefix}_[A-Z0-9_]+=(y|m)" "$IN" | cut -d= -f1 \
    | grep -vE "$keep" > /tmp/fam-drop.txt || true
  disable_from_list /tmp/fam-drop.txt "$OUT.tmp"
}

# ---- 1. Foreign platforms (only ARCH_MEDIATEK stays) ------------------
grep -oE "^CONFIG_ARCH_[A-Z0-9_]+=y" "$IN" \
  | sed 's/=y//; /^CONFIG_ARCH_MEDIATEK$/d' \
  | grep -vE "^CONFIG_ARCH_(HAS|SUPPORTS|WANTS|SELECTS|USE|USES|ENABLE|ENABLES|MMAP|BINFMT|CC_|DEFAULT|KEEP|PROC|HAVE|CORRECT|DMA|STACKWALK|RCAR|R8A|R9A|RZG|TEGRA_[0-9]|SPARSEMEM|HIBERNATION|SUSPEND|THP|TOP|MLC)" \
  > /tmp/prune-platforms.txt || true
echo "==> dropping $(wc -l < /tmp/prune-platforms.txt) foreign ARCH_ platforms"
disable_from_list /tmp/prune-platforms.txt "$OUT.tmp"

# ---- 2. Buses/devices that can physically never exist here ------------
# NOTE: CONFIG_BT is deliberately NOT dropped — the Gemini PDA has the
# MT6630 Bluetooth half of CONSYS (hci_stp driver, on glass 2026-09-09).
# It is kept from config.full-329 (CONFIG_BT=m + BT_BREDR/BT_LE + the
# MTK_WCN_BT_HCI transport). [corrected 2026-09-10: prune used to drop
# it, which clobbered the hand-added BT fix on every regeneration]

cat > /tmp/prune-masters.txt <<'EOF'
CONFIG_ACPI
CONFIG_EFI
CONFIG_XEN
CONFIG_KVM
CONFIG_PCI
CONFIG_THUNDERBOLT
CONFIG_ATA
CONFIG_SATA_AHCI
CONFIG_BLK_DEV_NVME
CONFIG_SCSI_UFSHCD
CONFIG_CAN
CONFIG_NFC
CONFIG_IEEE802154
CONFIG_MEDIA_SUPPORT
CONFIG_DVB_CORE
CONFIG_VIDEO_DEV
CONFIG_SND_HDA_INTEL
CONFIG_SND_HDA
CONFIG_SND_DRIVERS
CONFIG_SND_USB
CONFIG_USB_SERIAL
CONFIG_ETHERNET
CONFIG_NET_DSA
CONFIG_MACVLAN
CONFIG_VETH
CONFIG_NET_9P
CONFIG_VIRTIO_MENU
CONFIG_VHOST_MENU
CONFIG_FB_EFI
CONFIG_FB_SIMPLE
CONFIG_FB_VESA
CONFIG_DRM_PANEL
CONFIG_MTD
CONFIG_EXT2_FS
CONFIG_EXT3_FS
CONFIG_XFS_FS
CONFIG_BTRFS_FS
CONFIG_FUSE_FS
CONFIG_NFS_FS
CONFIG_CIFS
CONFIG_ISO9660_FS
CONFIG_UDF_FS
CONFIG_SQUASHFS
CONFIG_9P_FS
CONFIG_OVERLAY_FS
CONFIG_DM_CRYPT
CONFIG_MD_RAID456
EOF
echo "==> dropping impossible-bus/device masters"
disable_from_list /tmp/prune-masters.txt "$OUT.tmp"

# ---- 3. Driver families (keep the gemini set) -------------------------
# HID: keep generic HID; every vendor driver is a device that cannot be
# attached (USB HID works through usbhid + hid-generic).
grep -oE "^CONFIG_HID_[A-Z0-9_]+=(y|m)" "$IN" | cut -d= -f1 \
  | grep -vE '^CONFIG_HID_(GENERIC|SUPPORT)$' > /tmp/prune-hid.txt || true
echo "==> HID: $(wc -l < /tmp/prune-hid.txt) vendor drivers dropped (keep generic+usbhid)"
disable_from_list /tmp/prune-hid.txt "$OUT.tmp"

# Touch: only the NT36xxx (the unit's TDDI).
grep -oE "^CONFIG_TOUCHSCREEN_[A-Z0-9_]+=(y|m)" "$IN" | cut -d= -f1 \
  | grep -vE '^CONFIG_TOUCHSCREEN_NOVATEK_NT36XXX' > /tmp/prune-touch.txt || true
disable_from_list /tmp/prune-touch.txt "$OUT.tmp"

# Crypto hardware accelerators (HISi/QCom/...) — none on MT6797.
grep -oE "^CONFIG_CRYPTO_DEV_[A-Z0-9_]+=(y|m)" "$IN" | cut -d= -f1 \
  > /tmp/prune-crypto.txt || true
disable_from_list /tmp/prune-crypto.txt "$OUT.tmp"

# Non-mtk clocks/pinctrl/gpio/phy/mfd/regulator/reset/rtc/watchdog/leds/
# nvmem/thermal — everything a foreign SoC would drive. Only the mtk +
# required-core drivers survive (keep regex; select-protected deps are
# restored by the builder's olddefconfig).
drop_family CONFIG_COMMON_CLK '^CONFIG_COMMON_CLK_MT6|^CONFIG_COMMON_CLK_MEDIATEK'
drop_family CONFIG_CLK        '^CONFIG_CLK_MT6'
drop_family CONFIG_PINCTRL    '^CONFIG_PINCTRL_MT'
drop_family CONFIG_GPIO       '^CONFIG_GPIO_(CDEV|AW9523B)'
drop_family CONFIG_PHY        '^CONFIG_PHY_MTK_TPHY'
drop_family CONFIG_MFD        '^CONFIG_MFD_(SYSCON|CORE|MT6)'
drop_family CONFIG_REGULATOR  '^CONFIG_REGULATOR_(MT6|FIXED_VOLTAGE)'
drop_family CONFIG_RESET      '^CONFIG_RESET_MEDIATEK'
drop_family CONFIG_RTC_DRV    '^CONFIG_RTC_DRV_MT6'
drop_family CONFIG_LEDS       '^CONFIG_LEDS_CLASS$'
drop_family CONFIG_NVMEM      '^CONFIG_NVMEM$'
drop_family CONFIG_SENSORS    '^NOMATCH$'
drop_family CONFIG_IIO        '^NOMATCH$'
drop_family CONFIG_TYPEC      '^NOMATCH$'
drop_family CONFIG_EXTCON     '^NOMATCH$'
drop_family CONFIG_POWER_RESET '^NOMATCH$'
drop_family CONFIG_POWER_AVS  '^NOMATCH$'
drop_family CONFIG_INTERCONNECT '^NOMATCH$'
drop_family CONFIG_SLIMBUS    '^NOMATCH$'
drop_family CONFIG_SPMI       '^NOMATCH$'
drop_family CONFIG_RPMSG      '^NOMATCH$'
drop_family CONFIG_REMOTEPROC '^NOMATCH$'
drop_family CONFIG_MAILBOX    '^CONFIG_MTK_CMDQ'
drop_family CONFIG_HWSPINLOCK '^NOMATCH$'
drop_family CONFIG_MEMORY     '^NOMATCH$'
drop_family CONFIG_MUX        '^NOMATCH$'
drop_family CONFIG_PWM        '^CONFIG_PWM_(MTK|SYSFS)'
drop_family CONFIG_DMA        '^CONFIG_DMA_(ENGINE|VIRTUAL_CHANNELS|OF|CMA|SHARED_BUFFER|DIRECT)'
drop_family CONFIG_VIDEO      '^NOMATCH$'
drop_family CONFIG_GNSS       '^NOMATCH$'

# ---- 4. Display stack: panfrost DRM chain (=m, stage-2 loaded) + the
# geminipda LK framebuffer. Two alternative userspace views of the LK
# buffer are built: FB_GEMINIPDA (fbdev/dma-buf, the gemwl path) and
# DRM_GEMINIPDA (the standard KMS path used by GNOME) — see section 10.
# Everything else DRM (nouveau/exynos/rockchip/rcar/... + panels) is a
# display that can never exist (rule 5: no mediatek-drm/DSI panel stack).
grep -oE "^CONFIG_DRM_[A-Z0-9_]+=(y|m)" "$IN" | cut -d= -f1 \
  | grep -vE '^CONFIG_DRM_(GEM_SHMEM_HELPER|SCHED|PANFROST|GEMINIPDA|KMS_HELPER)$' \
  > /tmp/prune-drm.txt || true
disable_from_list /tmp/prune-drm.txt "$OUT.tmp"
grep -oE "^CONFIG_FB_[A-Z0-9_]+=(y|m)" "$IN" | cut -d= -f1 \
  | grep -vE '^CONFIG_FB_(GEMINIPDA|CFB_|SYS_|DEFERRED_IO|SYSMEM_HELPERS|FRAMEBUFFER)' \
  > /tmp/prune-fb-drop.txt || true
disable_from_list /tmp/prune-fb-drop.txt "$OUT.tmp"

# ---- 5. Input: unit keyboard (matrix over AW9523B), touch, buttons. --
grep -oE "^CONFIG_(KEYBOARD|TOUCHSCREEN|MOUSE|JOYSTICK|GAMEPORT|INPUT_)[A-Z0-9_]+=(y|m)" "$IN" \
  | cut -d= -f1 \
  | grep -vE '^CONFIG_(INPUT_EVDEV|INPUT_KEYBOARD|INPUT_TOUCHSCREEN|INPUT_MISC|INPUT_MATRIXKMAP|KEYBOARD_GPIO$|KEYBOARD_MATRIX$|KEYBOARD_MT6351|TOUCHSCREEN_NOVATEK_NT36XXX)' \
  > /tmp/prune-input-drop.txt || true
disable_from_list /tmp/prune-input-drop.txt "$OUT.tmp"

# ---- 6. Sound: MT6797 AFE + MT6351 codec only (HDA/USB dropped above).-
grep -oE "^CONFIG_SND_(SOC_)?[A-Z0-9_]+=(y|m)" "$IN" | cut -d= -f1 \
  | grep -vE '^CONFIG_SND_(SOC$|SOC_MT6|SOC_GENERIC_DMAENGINE_PCM|PCM$|PCM_|TIMER$|JACK|HWDEP|PROC_FS|SEQUENCER)' \
  > /tmp/prune-snd-drop.txt || true
disable_from_list /tmp/prune-snd-drop.txt "$OUT.tmp"

# ---- 7. Wireless: mtk_wcn + wlan_gen3 (internal CONSYS) and rtw88 (USB
# dongle) stay; mainline mt76 + every other vendor's wifi die (the
# vendor menu gates ath/brcm/mwifiex/... too).
grep -oE "^CONFIG_WLAN_VENDOR_[A-Z0-9_]+=(y|m)" "$IN" | cut -d= -f1 \
  | grep -v '^CONFIG_WLAN_VENDOR_REALTEK$' > /tmp/prune-wlan.txt || true
disable_from_list /tmp/prune-wlan.txt "$OUT.tmp"

# ---- 8. DEBUG bloat (keep DEBUG_FS — wifi-internal's debugfs pwr-on) --
cat > /tmp/prune-debug.txt <<'EOF'
CONFIG_DEBUG_INFO
CONFIG_DEBUG_INFO_REDUCED
CONFIG_GDB_SCRIPTS
CONFIG_PROVE_LOCKING
CONFIG_LOCKDEP
CONFIG_LOCK_STAT
CONFIG_DEBUG_LOCK_ALLOC
CONFIG_DEBUG_MUTEXES
CONFIG_DEBUG_LIST
CONFIG_DEBUG_SG
CONFIG_DEBUG_NOTIFIERS
CONFIG_DEBUG_CREDENTIALS
CONFIG_DEBUG_OBJECTS
CONFIG_DEBUG_WX
CONFIG_DEBUG_PAGEALLOC
CONFIG_DEBUG_VM
CONFIG_DEBUG_VIRTUAL
CONFIG_KCOV
CONFIG_UBSAN
CONFIG_KASAN
CONFIG_KCSAN
EOF
# (kept on purpose: STRICT_KERNEL_RWX/DEBUG_ALIGN_RODATA hardening,
#  FTRACE/KPROBES/KGDB/SCHED_DEBUG = bring-up tooling, DEBUG_FS = the
#  wifi-internal debugfs pwr-on)
disable_from_list /tmp/prune-debug.txt "$OUT.tmp"

# ---- 9b. Enable the Gemini KMS stack (2026-09-10) -------------------
# The LK framebuffer exposed as a standard DRM/KMS device (delta
# drivers/gpu/drm/tiny/geminipda-drm.c) so userspace gets /dev/dri/card0
# and an ordinary compositor (GNOME Shell/mutter) can drive the panel.
# Built as a module because DRM itself is =m. The section-4 DRM prune
# drops every DRM_* symbol by default, so the symbols the driver needs
# are (re-)enabled here; the driver's `select`s cover the rest.
enable_syms() {
  local sym
  for sym in "$@"; do
    if grep -q "^${sym}=" "$OUT.tmp"; then
      sed -i "s|^${sym}=.*|${sym}=m|" "$OUT.tmp"
    elif grep -q "^# ${sym} is not set" "$OUT.tmp"; then
      sed -i "s|^# ${sym} is not set|${sym}=m|" "$OUT.tmp"
    else
      printf '%s=m\n' "$sym" >> "$OUT.tmp"
    fi
  done
}
enable_syms CONFIG_DRM_GEMINIPDA CONFIG_DRM_KMS_HELPER

# ---- 9c. USB gadget NIC: CDC-ECM, not RNDIS (2026-09-17) -------------
# The gadget is the legacy precomposed g_ether (CONFIG_USB_ETH=y; the
# MACs come from the g_ether.dev_addr/host_addr kernel params). With
# CONFIG_USB_ETH_RNDIS=y that driver registers an RNDIS configuration
# FIRST (drivers/usb/gadget/legacy/ether.c: "register our
# configuration(s); RNDIS first, if it's used"), so every host picks it
# and the unit enumerates as 0525:a4a2 — a protocol macOS has NO driver
# for. With RNDIS off, ether.c registers exactly ONE configuration:
# CDC-ECM, i.e. 0525:a4a1 / NetChip CDC_PRODUCT_NUM. That is natively
# bound by Linux (cdc_ether) AND by macOS (com.apple.driver.usb.cdc.ecm
# matches bInterfaceClass 2/subclass 6 control + class 10 data — exactly
# the descriptors f_ecm emits), which is what makes the Mac-side
# flash/reboot cycle possible over USB. See docs/usb-network.md.
# USB_F_RNDIS is promptless and is only selected by the two configs
# below, so olddefconfig resolves it away too.
cat > /tmp/prune-gadget.txt <<'EOF'
CONFIG_USB_ETH_RNDIS
CONFIG_USB_CONFIGFS_RNDIS
CONFIG_USB_F_RNDIS
EOF
echo "==> USB gadget: RNDIS off (CDC-ECM only — the macOS-capable NIC)"
disable_from_list /tmp/prune-gadget.txt "$OUT.tmp"

# ---- 9. Fix the CONSYS firmware dir (machine-absolute -> in-tree) ----
sed -i 's|^CONFIG_EXTRA_FIRMWARE_DIR=.*|CONFIG_EXTRA_FIRMWARE_DIR="firmware"|' "$OUT.tmp"

# Banner comment
sed -i '1i#\n# LEAN Gemini PDA config — generated by bin/prune-kernel-config.sh\n# from config.full-329 (the exact on-glass #329 config). Prunes\n# hardware that can never exist on this unit (foreign SoCs, PCI/SATA/\n# NVMe/UFS, media/DVB, BT/NFC/CAN, ACPI/EFI/XEN/KVM, vendor HID/panels,\n# debug bloat). Keep-list asserted in devices/planet-geminipda/kernel/default.nix.\n# Also RNDIS-free by policy: the g_ether gadget enumerates as CDC-ECM\n# (macOS + Linux — section 9c of the generator; docs/usb-network.md).\n#' "$OUT.tmp"

mv "$OUT.tmp" "$OUT"
echo "==> wrote $OUT"
echo "    enabled: $(grep -cE '^CONFIG_.*=(y|m)' "$OUT")  (=y: $(grep -cE '^CONFIG_.*=y' "$OUT"), =m: $(grep -cE '^CONFIG_.*=m' "$OUT"))"
