#!/usr/bin/env bash
# Build and run cUR's host test suite without writing into the checkout.
#
# cUR is an upstream submodule whose Makefile builds objects in src/obj and
# the archive in src/.  Running it in place leaves those behind (and dirties
# the tree between builds), so the suite is built from a copy under the
# build root instead.  The firmware build does not use this Makefile; the
# Linux CMake target compiles cUR's sources directly.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"

SRC="$ROOT/kern/components/cUR"
WORK="${CUR_TEST_BUILD:-$KERN_NEXUS_BUILD/cur}"
[ -d "$SRC/src" ] || { echo "no cUR at $SRC (submodule not checked out)" >&2; exit 1; }

rm -rf "$WORK"
mkdir -p "$WORK"
cp -a "$SRC"/. "$WORK"/
rm -rf "$WORK/.git"

make -C "$WORK" test
