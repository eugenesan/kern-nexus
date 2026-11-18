#!/usr/bin/env bash
# Build the pinned mbedTLS (3.6.7) as a native static library so kern's
# host test suites (bip138, main/core) can run against the same version
# as the firmware instead of an older system libmbedcrypto.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"

SRC="${MBEDTLS_SRC:-$ROOT/kern/third_party/mbedtls}"
BUILD="${MBEDTLS_HOST_BUILD:-$KERN_NEXUS_BUILD/mbedtls-host}"
PREFIX="${MBEDTLS_HOST_ROOT:-$KERN_NEXUS_DEPS/mbedtls-host}"

[ -f "$SRC/CMakeLists.txt" ] || {
    echo "no mbedTLS source at $SRC (set MBEDTLS_SRC)" >&2
    exit 1
}

cmake -S "$SRC" -B "$BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DENABLE_TESTING=OFF \
    -DENABLE_PROGRAMS=OFF \
    -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
    -DUSE_STATIC_MBEDTLS_LIBRARY=ON
cmake --build "$BUILD" -j"${JOBS:-$(nproc)}"
cmake --install "$BUILD"

grep -q 'MBEDTLS_VERSION_STRING         "3.6.7"' \
    "$PREFIX/include/mbedtls/build_info.h" || {
    echo "unexpected mbedTLS version in $PREFIX" >&2
    exit 1
}
echo "host mbedTLS: $PREFIX"
