#!/usr/bin/env bash
# Vendor LVGL 9.5 into kern/managed_components/lvgl__lvgl for the offline target build.
# Requires network on first run only.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${LVGL_DIR:-$ROOT/kern/managed_components/lvgl__lvgl}"
TAG="${LVGL_TAG:-v9.5.0}"
if [ -f "$DEST/src/lvgl.h" ]; then
  echo "LVGL already present: $DEST"
  exit 0
fi
git clone --depth 1 --branch "$TAG" https://github.com/lvgl/lvgl.git "$DEST"
test -f "$DEST/src/lvgl.h"
echo "LVGL $TAG: $DEST"
