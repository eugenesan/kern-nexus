#!/usr/bin/env bash
# Host-side tests that do not need the target or a device.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"
# kern's QR test links cUR; keep its objects out of the submodule.
export UR_BUILD="${UR_BUILD:-$KERN_NEXUS_BUILD/cur-test}"
make -C "$ROOT/tools/sighash"
make -C "$ROOT/components/seedkeeper/test" test
bash "$ROOT/scripts/build-cur-tests.sh"
if [ -x "$ROOT/kern/scripts/test.sh" ]; then
  (cd "$ROOT/kern" && ./scripts/test.sh)
fi
