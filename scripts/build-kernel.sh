#!/usr/bin/env bash
# Build the hammerhead kernel with the kern-nexus patch set and profiles.
#
#   bash scripts/build-kernel.sh
#   KERNEL_OUT=/path/to/ko bash scripts/build-kernel.sh
#   bash scripts/build-kernel.sh --no-wipe     # development
#
# The output is a zImage-dtb (kernel + appended device tree).  A plain
# zImage is not bootable on this device.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

NO_WIPE="${KERN_WIPE:-1}"
for arg in "$@"; do
    case "$arg" in
        --no-wipe) NO_WIPE=0 ;;
        --wipe)    NO_WIPE=1 ;;
        *) echo "usage: $0 [--no-wipe|--wipe]" >&2; exit 2 ;;
    esac
done

cd "$ROOT"
source "$ROOT/scripts/env.sh"

TREE="${KERNEL_TREE:-$ROOT/linux}"
OUT="${KERNEL_OUT:-$KERN_NEXUS_BUILD/ko}"
PROFILE="${PROFILE:-$ROOT/tools/kernel/profiles/hammerhead.list}"
KEEPLISTS="${KEEPLISTS:-$ROOT/tools/kernel/profiles/hammerhead-keep.list $ROOT/tools/kernel/profiles/hammerhead-camera.list}"
if [ "$NO_WIPE" = "0" ]; then
    KEEPLISTS="$KEEPLISTS $ROOT/tools/kernel/profiles/hammerhead-dev-nowipe.list"
fi
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
# Install it only for the build, then remove it so the kernel submodule
# stays clean.
DEFCONFIG_DST="$TREE/arch/arm/configs/$DEFCONFIG_NAME"
cp "$ROOT/tools/kernel/config/lineageos_hammerhead_defconfig" "$DEFCONFIG_DST"
trap 'rm -f "$DEFCONFIG_DST"' EXIT

mkdir -p "$OUT"
DEFCONFIG_NAME="$DEFCONFIG_NAME" \
PROFILE="$PROFILE" \
KEEPLISTS="$KEEPLISTS" \
TOOLCHAIN=host \
    bash "$ROOT/tools/kernel/build-kernel.sh" \
        --tree "$TREE" --out "$OUT" --jobs "$JOBS"

test -f "$OUT/arch/arm/boot/zImage-dtb"
if [ "$NO_WIPE" = "0" ]; then
    if grep -q '^CONFIG_MEMORY_WIPE_ON_POWEROFF=y' "$OUT/.config" ||
       grep -q '^CONFIG_MEMORY_WIPE=y' "$OUT/.config"; then
        echo "!! requested --no-wipe but the kernel config still enables it" >&2
        exit 1
    fi
    echo "kernel: poweroff wipe DISABLED (development build)"
else
    echo "kernel: poweroff wipe ENABLED (release build)"
fi
echo "kernel: $OUT/arch/arm/boot/zImage-dtb"
