#!/usr/bin/env bash
# Everything from source: dependencies, kernel, kern, camd, host tests and
# both standard images.  This is the reproducibility check; it writes only
# under $KERN_NEXUS_BUILD.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
source "$ROOT/scripts/env.sh"

bash scripts/build-mbedtls.sh
bash scripts/build-zlib.sh
bash scripts/build-kernel.sh
bash scripts/build-kern.sh
bash scripts/build-camd.sh

# Host suites.  The bip138/core throw the board firmware's digests; they
# are skipped on a machine without host mbedTLS, with a message.
bash scripts/test.sh

bash scripts/build-image.sh standard
KERN_SEAL=0 bash scripts/build-image.sh standard

echo "done:"
echo "  $KERN_NEXUS_BUILD/boot-kern-standard-release.img"
echo "  $KERN_NEXUS_BUILD/boot-kern-standard-release-audit.img"
