#!/usr/bin/env bash
# Build the camera daemon from components/camera/camd.c as a static ARM
# binary.  This is the interim bridge until the camera is a kern thread.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${CAMD_BIN:-$ROOT/artifacts/built/camd}"
CC=${CC:-arm-linux-gnueabihf-gcc}
DEFS=""
[ "${KERN_RELEASE:-1}" = "1" ] && DEFS="-DKRUX_RELEASE"
$CC -O2 -static -s -pthread $DEFS \
    -I"$ROOT/components/camera" \
    -o "$OUT" "$ROOT/components/camera/camd.c" \
    -lpthread -lrt -lm
file "$OUT"
echo "camd: $OUT"
