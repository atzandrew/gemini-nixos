#!/usr/bin/env bash
# Parse the AOSP v0 boot.img header and print the fields LK cares about.
#
# Usage: bash bin/dump-bootimg-header.sh <boot.img>
#
# Used to verify the phase-0 exit criterion (docs §9): the built
# `android-bootimg` header fields must match the verified bring-up
# boot.img (GeminiPDA build/out-6.6/cleanup-boot.img):
#   kernel 0x40200000, second 0x40f00000, ramdisk 0x45000000,
#   tags 0x44000000, page size 2048.
set -euo pipefail

img="${1:?usage: dump-bootimg-header.sh <boot.img>}"

magic=$(dd if="$img" bs=1 count=8 2>/dev/null)
[ "$magic" = "ANDROID!" ] || { echo "!! not a boot.img (bad magic: $magic)"; exit 1; }

# AOSP v0 header (android_boot_header), little-endian u32:
#   0x08 kernel_size  0x0c kernel_addr  0x10 ramdisk_size  0x14 ramdisk_addr
#   0x18 second_size  0x1c second_addr  0x20 tags_addr     0x24 page_size
#   0x28 header_size  0x2c os_version   0x30 os_patch_level  0x34 cmdline[512]
u32() { od -An -tu4 -j"$1" -N4 "$img" | tr -d ' \n'; }
hex() { printf '0x%x' "$1"; }

kernel_size=$(u32 0x08); kernel_addr=$(u32 0x0c)
ramdisk_size=$(u32 0x10); ramdisk_addr=$(u32 0x14)
second_size=$(u32 0x18); second_addr=$(u32 0x1c)
tags_addr=$(u32 0x20); page_size=$(u32 0x24); header_size=$(u32 0x28)

printf 'magic:          ANDROID!\n'
printf 'kernel:         size=%d (%.2f MiB)  addr=%s\n' "$kernel_size" "$(awk "BEGIN{printf \"%.2f\", $kernel_size/1048576}")" "$(hex "$kernel_addr")"
printf 'ramdisk:        size=%d (%.2f MiB)  addr=%s\n' "$ramdisk_size" "$(awk "BEGIN{printf \"%.2f\", $ramdisk_size/1048576}")" "$(hex "$ramdisk_addr")"
printf 'second:         size=%d  addr=%s\n' "$second_size" "$(hex "$second_addr")"
printf 'tags_addr:      %s\n' "$(hex "$tags_addr")"
printf 'page_size:      %d\n' "$page_size"
printf 'header_size:    %s\n' "$(hex "$header_size")"
printf 'cmdline:        %s\n' "$(dd if="$img" bs=1 skip=$((0x34)) count=512 2>/dev/null | tr -d '\0' | sed 's/[[:space:]]*$//')"
# `stat -c%s` is GNU; macOS/BSD needs `-f%z` (verified on a build made
# entirely from a Mac, 2026-09-17 — see docs/macos-build.md).
size=$(stat -c%s "$img" 2>/dev/null || stat -f%z "$img")
printf 'total size:     %d bytes\n' "$size"
