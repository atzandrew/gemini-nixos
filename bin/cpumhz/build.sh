#!/bin/sh
# Build the freestanding cpumhz binary (no libc, static, aarch64).
# Any host with clang + lld works (Hydra: clang from nix or Fedora).
set -eu
cd "$(dirname "$0")"
clang --target=aarch64-linux-gnu -O2 -static -nostdlib -ffreestanding \
  -fno-stack-protector -fno-builtin -fuse-ld=lld -Wall -Wextra \
  -o cpumhz cpumhz.c
echo "built $(pwd)/cpumhz"
