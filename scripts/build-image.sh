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
source "$ROOT/scripts/env.sh"

MODE="${1:-standard}"
KERNEL_IMG="${KERNEL_IMG:-$KERN_NEXUS_BUILD/ko/arch/arm/boot/zImage-dtb}"
KERN_BIN="${KERN_BIN:-$KERN_NEXUS_BUILD/kern-nexus}"
[ -f "$KERNEL_IMG" ] || { echo "no kernel: run scripts/build-kernel.sh" >&2; exit 1; }
[ -x "$KERN_BIN" ] || { echo "no kern binary: run scripts/build-kern.sh" >&2; exit 1; }
CAMD_BIN="${CAMD_BIN:-$KERN_NEXUS_BUILD/camd}"
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
    OUT="$KERN_NEXUS_BUILD/ramdisk-kern-bundled" \
      bash rootfs/build-stub-ramdisk.sh --standard --bundle --force
    RAMDISK="$KERN_NEXUS_BUILD/ramdisk-kern-bundled"
    OUT="$KERN_NEXUS_BUILD/boot-kern-${name}.img"
    ;;
  dev)
    name=dev
    OUT="$KERN_NEXUS_BUILD/ramdisk-kern" \
      bash rootfs/build-stub-ramdisk.sh --force
    RAMDISK="$KERN_NEXUS_BUILD/ramdisk-kern"
    OUT="$KERN_NEXUS_BUILD/boot-kern-dev.img"
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
        --config "${KERNEL_CFG:-$KERN_NEXUS_BUILD/ko/.config}" \
        --ramdisk "$RAMDISK"
fi
bash scripts/security-check.sh "$OUT"
echo "image: $OUT"
