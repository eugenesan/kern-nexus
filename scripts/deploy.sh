#!/usr/bin/env bash
# Deploy kern-nexus.
#
#   bash scripts/deploy.sh dev        push to /data/kern and restart the service
#   bash scripts/deploy.sh fastboot   fastboot boot a dev image
#   bash scripts/deploy.sh recovery   flash a sealed image to recovery (asks)
#
# Never flashes boot.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
MODE="${1:-dev}"

case "$MODE" in
  dev)
    BIN="${KERN_BIN:-$ROOT/artifacts/built/kern-nexus}"
    [ -x "$BIN" ] || { echo "no binary: run scripts/build-kern.sh" >&2; exit 1; }
    adb wait-for-device
    adb shell 'mkdir -p /data/kern'
    adb push "$BIN" /data/kern/kern-nexus
    adb shell 'chmod 755 /data/kern/kern-nexus; setprop ctl.restart kern || (setprop ctl.stop kern; sleep 1; setprop ctl.start kern)'
    ;;
  fastboot)
    IMG="${2:-$ROOT/artifacts/built/boot-kern-dev.img}"
    [ -f "$IMG" ] || { echo "no image: $IMG" >&2; exit 1; }
    adb reboot bootloader 2>/dev/null || true
    sleep 3
    fastboot devices
    fastboot boot "$IMG"
    ;;
  recovery)
    IMG="${2:-$ROOT/artifacts/built/boot-kern-standard-release.img}"
    [ -f "$IMG" ] || { echo "no image: $IMG" >&2; exit 1; }
    echo "This writes the recovery partition only: $IMG"
    printf "Type RECOVERY to continue: "
    read answer
    [ "$answer" = "RECOVERY" ] || { echo "aborted"; exit 1; }
    adb reboot bootloader 2>/dev/null || true
    sleep 3
    fastboot devices
    fastboot flash recovery "$IMG"
    ;;
  *)
    echo "usage: $0 [dev|fastboot|recovery]" >&2
    exit 2
    ;;
esac
