#!/usr/bin/env bash
# Build a kern-nexus boot image.
#
#   bash scripts/build-image.sh [standard|dev]
#
# standard (default): TWRP by default, kern on volume-up/start-kern,
#                     kern bundled in /opt/kern.
# dev:                kern by default, /data/kern over adb, TWRP on volume-up.
#
# KERN_SEAL=0 builds the audit variant (adb kept on).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

MODE="${1:-standard}"
KERNEL_IMG="${KERNEL_IMG:-$ROOT/artifacts/built/ko/arch/arm/boot/zImage-dtb}"
KERN_BIN="${KERN_BIN:-$ROOT/artifacts/built/kern-nexus}"
[ -f "$KERNEL_IMG" ] || { echo "no kernel: run scripts/build-kernel.sh" >&2; exit 1; }
[ -x "$KERN_BIN" ] || { echo "no kern binary: run scripts/build-kern.sh" >&2; exit 1; }
CAMD_BIN="${CAMD_BIN:-$ROOT/artifacts/built/camd}"
if [ ! -x "$CAMD_BIN" ]; then
  bash scripts/build-camd.sh
fi
export CAMD_BIN

export KERN_RELEASE=1
export KERN_SEAL="${KERN_SEAL:-1}"
export KERN_BIN

case "$MODE" in
  standard)
    name=standard
    [ "$KERN_SEAL" = "1" ] && name=standard-release || name=standard-release-audit
    bash rootfs/build-stub-ramdisk.sh --standard --bundle --force
    RAMDISK="$ROOT/artifacts/built/ramdisk-kern-bundled"
    OUT="$ROOT/artifacts/built/boot-kern-${name}.img"
    ;;
  dev)
    name=dev
    bash rootfs/build-stub-ramdisk.sh --force
    RAMDISK="$ROOT/artifacts/built/ramdisk-kern"
    OUT="$ROOT/artifacts/built/boot-kern-dev.img"
    ;;
  *)
    echo "usage: $0 [standard|dev]" >&2
    exit 2
    ;;
esac

RAMDISK="$RAMDISK" bash rootfs/make-boot-image.sh \
    "$ROOT/artifacts/twrp-3.7.0_9-HH.R.17-MADV_WIPEONFORK.img" \
    "$KERNEL_IMG" "$OUT"

if [ "$MODE" = "standard" ]; then
    python3 tools/check-airgap.py \
        --config "${KERNEL_CFG:-$ROOT/artifacts/built/ko/.config}" \
        --ramdisk "$RAMDISK"
fi
bash scripts/security-check.sh "$OUT"
echo "image: $OUT"
