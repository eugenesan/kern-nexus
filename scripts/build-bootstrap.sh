#!/usr/bin/env bash
# Build the boot-chain bootstrap images with tools/bringup/placeholder.c.
#
#   bash scripts/build-bootstrap.sh
#
# Uses an existing kernel if KERNEL_IMG points at one; otherwise it tries
# the preserved Krux kernel tree.  Never flashes anything.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
KERNEL_IMG="${KERNEL_IMG:-$ROOT/../krux-nexus/artifacts/built/kernel-final/zImage-dtb}"
[ -f "$KERNEL_IMG" ] || { echo "no kernel at $KERNEL_IMG (set KERNEL_IMG)" >&2; exit 1; }
PLACEHOLDER="${PLACEHOLDER:-/tmp/kern-placeholder}"
arm-linux-gnueabihf-gcc -O2 -static -s -o "$PLACEHOLDER" tools/bringup/placeholder.c

# Standard sealed: TWRP by default, placeholder on Vol+/start-kern.
KERN_RELEASE=1 KERN_SEAL=1 KERN_BIN="$PLACEHOLDER" \
  bash rootfs/build-stub-ramdisk.sh --standard --bundle --force
RAMDISK="$ROOT/artifacts/built/ramdisk-kern-bundled" \
  bash rootfs/make-boot-image.sh \
  "$ROOT/artifacts/twrp-3.7.0_9-HH.R.17-MADV_WIPEONFORK.img" \
  "$KERNEL_IMG" "$ROOT/artifacts/built/boot-kern-bootstrap.img"

# Standard audit: same but adb on, for scripted start-kern checks.
KERN_RELEASE=1 KERN_SEAL=0 KERN_BIN="$PLACEHOLDER" \
  bash rootfs/build-stub-ramdisk.sh --standard --bundle --force
RAMDISK="$ROOT/artifacts/built/ramdisk-kern-bundled" \
  bash rootfs/make-boot-image.sh \
  "$ROOT/artifacts/twrp-3.7.0_9-HH.R.17-MADV_WIPEONFORK.img" \
  "$KERNEL_IMG" "$ROOT/artifacts/built/boot-kern-bootstrap-audit.img"

echo "built:"
echo "  artifacts/built/boot-kern-bootstrap.img"
echo "  artifacts/built/boot-kern-bootstrap-audit.img"
