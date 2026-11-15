#!/usr/bin/env bash
# Cross-build kern-nexus as a static ARMv7 Linux binary.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

BUILD="${BUILD_DIR:-$ROOT/build/nexus5}"
OUT="${KERN_BIN:-$ROOT/artifacts/built/kern-nexus}"
export MBEDTLS_ROOT="${MBEDTLS_ROOT:-$ROOT/artifacts/deps/mbedtls-armhf}"

cmake -S ports/nexus5 -B "$BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/ports/nexus5/toolchain-armhf.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DMBEDTLS_ROOT="$MBEDTLS_ROOT"
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
