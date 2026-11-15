#!/usr/bin/env bash
# Build a static ARM mbedTLS for the Linux 3.4 target.
#
#   MBEDTLS_SRC=/path/to/mbedtls-3.x bash scripts/build-mbedtls.sh
#
# Output: artifacts/deps/mbedtls-armhf/{include,lib}
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${MBEDTLS_SRC:-$ROOT/kern/third_party/mbedtls}"
BUILD="${MBEDTLS_BUILD:-$ROOT/build/mbedtls-armhf}"
PREFIX="${MBEDTLS_ROOT:-$ROOT/artifacts/deps/mbedtls-armhf}"

[ -f "$SRC/CMakeLists.txt" ] || {
    echo "no mbedTLS source at $SRC (set MBEDTLS_SRC)" >&2
    exit 1
}
[ -d "$SRC/include" ] || { echo "mbedTLS source looks incomplete: $SRC" >&2; exit 1; }

cmake -S "$SRC" -B "$BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/ports/nexus5/toolchain-armhf.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DENABLE_TESTING=OFF \
    -DENABLE_PROGRAMS=OFF \
    -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
    -DUSE_STATIC_MBEDTLS_LIBRARY=ON
cmake --build "$BUILD" -j"${JOBS:-$(nproc)}"
cmake --install "$BUILD"
echo "mbedTLS: $PREFIX"
