#!/usr/bin/env bash
# Cross-build kern-nexus as a static ARMv7 Linux binary.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
source "$ROOT/scripts/env.sh"

BUILD="${BUILD_DIR:-$KERN_NEXUS_BUILD/nexus5}"
OUT="${KERN_BIN:-$KERN_NEXUS_BUILD/kern-nexus}"
export MBEDTLS_ROOT="${MBEDTLS_ROOT:-$KERN_NEXUS_DEPS/mbedtls-armhf}"
export ZLIB_ROOT="${ZLIB_ROOT:-$KERN_NEXUS_DEPS/zlib-armhf}"

cmake -S ports/nexus5 -B "$BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/ports/nexus5/toolchain-armhf.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DMBEDTLS_ROOT="$MBEDTLS_ROOT" \
    -DZLIB_ROOT="$ZLIB_ROOT"
cmake --build "$BUILD" -j"${JOBS:-$(nproc)}"

mkdir -p "$(dirname "$OUT")"
cp "$BUILD/kern-nexus" "$OUT"
file "$OUT" | grep -q 'ELF 32-bit' || {
    echo "!! $OUT is not a 32-bit ARM ELF" >&2
    file "$OUT"
    exit 1
}
if readelf -d "$OUT" | grep -q NEEDED; then
    echo "!! $OUT is dynamically linked" >&2
    readelf -d "$OUT" | grep NEEDED
    exit 1
fi
echo "kern: $OUT"
