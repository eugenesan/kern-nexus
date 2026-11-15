#!/usr/bin/env bash
# Release gate for kern-nexus.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"
BIN="${KERN_BIN:-$KERN_NEXUS_BUILD/kern-nexus}"
IMG="${1:-$KERN_NEXUS_BUILD/boot-kern-standard-release.img}"
fail=0
say() { echo "security-check: $*"; }
bad() { echo "  !! $*" >&2; fail=1; }

[ -x "$BIN" ] || bad "no kern binary at $BIN"
if [ -x "$BIN" ]; then
  file "$BIN" | grep -q 'ELF 32-bit' || bad "not a 32-bit ARM ELF"
  file "$BIN" | grep -q 'statically linked' || bad "not statically linked"
  readelf -d "$BIN" 2>/dev/null | grep -q NEEDED && bad "dynamic dependencies"
  for s in save_ppm /data/krux micropython; do
    strings "$BIN" 2>/dev/null | grep -q "$s" && bad "binary has $s"
  done
fi
if [ -f "$IMG" ]; then
  size=$(stat -c %s "$IMG")
  [ "$size" -le 23068672 ] || bad "image is $size bytes (limit 23,068,672)"
  for s in wpa_supplicant hostapd wificond bcmdhd brcmfmac; do
    strings "$IMG" 2>/dev/null | grep -q "$s" && bad "image has $s"
  done
fi

if [ "$fail" -eq 0 ]; then say "PASS"; else say "FAIL"; exit 1; fi
