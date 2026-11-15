# Shared build locations for kern-nexus.
#
# Nothing generated is written into the checkout: kernel objects, CMake
# trees, installed dependencies, binaries and boot images all live under
# /tmp by default.  Override the roots for a different disk:
#
#   KERN_NEXUS_TMP=/scratch/kern bash scripts/build-kernel.sh
#   KERN_NEXUS_BUILD=/scratch/build KERN_NEXUS_DEPS=/scratch/deps bash ...
#
# Sourced by the build/deploy scripts; CMake sees KERN_NEXUS_DEPS in the
# environment and uses the same dependency prefixes.

KERN_NEXUS_TMP="${KERN_NEXUS_TMP:-${TMPDIR:-/tmp}/kern-nexus}"
KERN_NEXUS_ARTIFACTS="${KERN_NEXUS_ARTIFACTS:-$KERN_NEXUS_TMP/artifacts}"
KERN_NEXUS_BUILD="${KERN_NEXUS_BUILD:-$KERN_NEXUS_ARTIFACTS/build}"
KERN_NEXUS_DEPS="${KERN_NEXUS_DEPS:-$KERN_NEXUS_ARTIFACTS/deps}"

export KERN_NEXUS_TMP KERN_NEXUS_ARTIFACTS KERN_NEXUS_BUILD KERN_NEXUS_DEPS

mkdir -p "$KERN_NEXUS_BUILD" "$KERN_NEXUS_DEPS"
