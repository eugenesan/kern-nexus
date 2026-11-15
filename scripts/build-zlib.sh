#!/usr/bin/env bash
# Build the vendored zlib as a static ARM library for the Linux target.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${ZLIB_SRC:-$ROOT/kern/third_party/zlib}"
PREFIX="${ZLIB_ROOT:-$ROOT/artifacts/deps/zlib-armhf}"
BUILD="${ZLIB_BUILD:-$ROOT/build/zlib-armhf}"

[ -f "$SRC/configure" ] || { echo "no zlib source at $SRC" >&2; exit 1; }
rm -rf "$BUILD"; mkdir -p "$BUILD"
cp -a "$SRC/." "$BUILD/"
cd "$BUILD"
CC=arm-linux-gnueabihf-gcc \
AR=arm-linux-gnueabihf-ar \
RANLIB=arm-linux-gnueabihf-ranlib \
CFLAGS="-O2 -fPIC" \
  ./configure --static --prefix="$PREFIX"
make -j"${JOBS:-$(nproc)}" libz.a
make install
echo "zlib: $PREFIX"
