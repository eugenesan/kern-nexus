#!/usr/bin/env bash
# Host-side tests that do not need the target or a device.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"

# If scripts/build-mbedtls-host.sh has been run, use the pinned mbedTLS
# (3.6.7) for the host suites instead of an older system libmbedcrypto.
# kern/scripts/test.sh probes for psa/crypto.h and -lmbedcrypto via CC.
HOST_MBEDTLS="${MBEDTLS_HOST_ROOT:-$KERN_NEXUS_DEPS/mbedtls-host}"
if [ -f "$HOST_MBEDTLS/include/psa/crypto.h" ] &&
   [ -f "$HOST_MBEDTLS/lib/libmbedcrypto.a" ]; then
    export CPATH="$HOST_MBEDTLS/include${CPATH:+:$CPATH}"
    export LIBRARY_PATH="$HOST_MBEDTLS/lib${LIBRARY_PATH:+:$LIBRARY_PATH}"
    echo "using host mbedTLS from $HOST_MBEDTLS"
fi

# kern's QR test links cUR; keep its objects out of the submodule.
export UR_BUILD="${UR_BUILD:-$KERN_NEXUS_BUILD/cur-test}"
make -C "$ROOT/tools/sighash"
make -C "$ROOT/components/seedkeeper/test" test
bash "$ROOT/scripts/build-cur-tests.sh"
if [ -x "$ROOT/kern/scripts/test.sh" ]; then
  (cd "$ROOT/kern" && ./scripts/test.sh)
fi
