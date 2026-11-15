#!/usr/bin/env bash
# Host-side tests that do not need the target or a device.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
make -C "$ROOT/tools/sighash"
if [ -x "$ROOT/kern/scripts/test.sh" ]; then
  (cd "$ROOT/kern" && ./scripts/test.sh)
fi
