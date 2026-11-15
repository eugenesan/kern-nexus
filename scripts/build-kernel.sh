#!/usr/bin/env bash
# Build the hammerhead kernel with the kern-nexus patch set and profiles.
#
#   bash scripts/build-kernel.sh
#   KERNEL_OUT=/path/to/ko bash scripts/build-kernel.sh
#
# The output is a zImage-dtb (kernel + appended device tree).  A plain
# zImage is not bootable on this device.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

TREE="${KERNEL_TREE:-$ROOT/linux}"
OUT="${KERNEL_OUT:-$ROOT/artifacts/built/ko}"
PROFILE="${PROFILE:-$ROOT/tools/kernel/profiles/hammerhead.list}"
KEEPLISTS="${KEEPLISTS:-$ROOT/tools/kernel/profiles/hammerhead-keep.list $ROOT/tools/kernel/profiles/hammerhead-camera.list}"
DEFCONFIG_NAME="${DEFCONFIG_NAME:-kern_nexus_hammerhead_defconfig}"
JOBS="${JOBS:-$(nproc)}"

if [ ! -f "$TREE/Makefile" ]; then
    echo "kernel: no tree at $TREE; cloning lineage-23.2 and applying the patch"
    mkdir -p "$(dirname "$TREE")"
    git clone -b lineage-23.2 --depth 1 \
        https://github.com/eugenesan/android_kernel_lge_hammerhead "$TREE"
    git -C "$TREE" apply "$ROOT/tools/kernel/patches/kern-nexus-kernel.patch"
fi

# The modified defconfig is kept outside the tree (KernelSU removed).
cp "$ROOT/tools/kernel/config/lineageos_hammerhead_defconfig" \
   "$TREE/arch/arm/configs/$DEFCONFIG_NAME"

mkdir -p "$OUT"
DEFCONFIG_NAME="$DEFCONFIG_NAME" \
PROFILE="$PROFILE" \
KEEPLISTS="$KEEPLISTS" \
TOOLCHAIN=host \
    bash "$ROOT/tools/kernel/build-kernel.sh" \
        --tree "$TREE" --out "$OUT" --jobs "$JOBS"

test -f "$OUT/arch/arm/boot/zImage-dtb"
echo "kernel: $OUT/arch/arm/boot/zImage-dtb"
