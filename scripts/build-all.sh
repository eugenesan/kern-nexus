#!/usr/bin/env bash
# Kernel -> kern -> sealed standard image, the full from-scratch path.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
bash scripts/build-kernel.sh
bash scripts/build-kern.sh
bash scripts/build-image.sh standard
echo "done: artifacts/built/boot-kern-standard-release.img"
