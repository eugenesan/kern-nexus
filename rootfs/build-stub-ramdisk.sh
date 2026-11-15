#!/bin/bash
# Build the kern-nexus ramdisk from the TWRP template.
#
# The ramdisk is TWRP 3.7.0 with the `kern` service (the init stub) and
# TWRP kept as recovery.stock.  The standard image bundles kern in
# /opt/kern; the development image runs /data/kern on demand.
#
# Usage: ./build-stub-ramdisk.sh [--force] [--bundle] [--standard] [TWRP.img]
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
FORCE=""
BUNDLE=${BUNDLE:-0}
STANDARD=${STANDARD:-0}
COMPRESS=${COMPRESS:-gzip}
while [ $# -gt 0 ]; do
  case "$1" in
    --force) FORCE=1; shift ;;
    --bundle) BUNDLE=1; shift ;;
    --standard) STANDARD=1; shift ;;
    *) break ;;
  esac
done
TWRP_IMG=${1:-$ROOT/artifacts/twrp-3.7.0_9-HH.R.17-MADV_WIPEONFORK.img}
if [ "$BUNDLE" = "1" ]; then
  OUT=${OUT:-$ROOT/artifacts/built/ramdisk-kern-bundled}
else
  OUT=${OUT:-$ROOT/artifacts/built/ramdisk-kern}
fi
WORK=${WORK:-/tmp/opencode/kern-ramdisk}
[ -f "$TWRP_IMG" ] || { echo "no TWRP image at $TWRP_IMG" >&2; exit 1; }
mkdir -p "$(dirname "$OUT")"

rm -rf "$WORK"; mkdir -p "$WORK"
python3 "$HERE/bootimg.py" unpack "$TWRP_IMG" "$WORK/twrp" >/dev/null
cp "$WORK/twrp/ramdisk" "$WORK/ramdisk.gz"
mkdir -p "$WORK/rd"
(cd "$WORK/rd" && gzip -dc ../ramdisk.gz | cpio -idm --quiet)

# TWRP pieces the stub never uses.
rm -rf "$WORK/rd/sbin/etc/terminfo" "$WORK/rd/etc/python"
if [ "${KEEP_KEYSTORE:-0}" != "1" ]; then
  rm -f "$WORK/rd/sbin/keystore" "$WORK/rd/sbin/keystore_auth" \
        "$WORK/rd/sbin/keystore_cli" \
        "$WORK/rd/sbin/libwifikeystorehal.so" \
        "$WORK/rd/sbin/android.system.wifi.keystore@1.0.so"
fi
rm -f "$WORK/rd/sbin/libclang_rt.ubsan_standalone-arm-android.so"

STRIP=${STRIP:-arm-linux-gnueabihf-strip}
if command -v "$STRIP" >/dev/null 2>&1; then
  for f in adbd linker libc.so libdexfile.so; do
    [ -f "$WORK/rd/sbin/$f" ] && "$STRIP" "$WORK/rd/sbin/$f"
  done
fi

# The stub, TWRP, and the sideload child's hardcoded /sbin/recovery path.
STUB_FLAGS=""
[ "$STANDARD" = "1" ] && STUB_FLAGS="--standard"
bash "$HERE/build-stub-init.sh" $STUB_FLAGS "$HERE/stub/kern-init" >/dev/null
mv "$WORK/rd/sbin/recovery" "$WORK/rd/sbin/recovery.stock"
cp "$HERE/stub/kern-init" "$WORK/rd/sbin/kern-init"
chmod 755 "$WORK/rd/sbin/kern-init" "$WORK/rd/sbin/recovery.stock"
ln -sf recovery.stock "$WORK/rd/sbin/recovery"

cat > "$WORK/rd/sbin/start-kern" <<'SCRIPT'
#!/sbin/sh
# Start kern from TWRP.  The service has its own cgroup so stopping TWRP
# does not kill this request.
setprop ctl.start kern-firmware
echo "kern is starting"
SCRIPT
chmod 755 "$WORK/rd/sbin/start-kern"
cp "$HERE/stub/start-adb" "$WORK/rd/sbin/start-adb"
cp "$HERE/stub/start-serial" "$WORK/rd/sbin/start-serial"
chmod 755 "$WORK/rd/sbin/start-adb" "$WORK/rd/sbin/start-serial"
cp "$HERE/stub/init.recovery.service.rc" "$WORK/rd/init.recovery.service.rc"

# TWRP's system() goes through /bin/sh; /bin -> /system/bin.
mkdir -p "$WORK/rd/system/bin"
ln -sf /sbin/sh "$WORK/rd/system/bin/sh"

# CPP firmware for the camera; camd stages it into /lib/firmware.
CPP_FW="$ROOT/components/camera/fw/cpp_firmware_v1_2_0.fw"
[ -f "$CPP_FW" ] || { echo "missing $CPP_FW" >&2; exit 1; }
mkdir -p "$WORK/rd/lib/firmware"
cp "$CPP_FW" "$WORK/rd/lib/firmware/cpp_firmware_v1_2_0.fw"

if [ "$BUNDLE" = "1" ]; then
  KERN_BIN=${KERN_BIN:-$ROOT/artifacts/built/kern-nexus}
  [ -x "$KERN_BIN" ] || { echo "no kern binary at $KERN_BIN" >&2; exit 1; }
  BUNDLE_DIR="$WORK/rd/opt/kern"
  rm -rf "$BUNDLE_DIR"
  mkdir -p "$BUNDLE_DIR"
  cp "$KERN_BIN" "$BUNDLE_DIR/kern-nexus"
  chmod 755 "$BUNDLE_DIR/kern-nexus"
  cp "$CPP_FW" "$BUNDLE_DIR/cpp_firmware_v1_2_0.fw"
  echo "  bundled kern: $BUNDLE_DIR"
fi

(cd "$WORK/rd" && find . | cpio -o -H newc --quiet) > "$WORK/ramdisk.cpio"
case "$COMPRESS" in
  gzip) gzip -9 -c "$WORK/ramdisk.cpio" > "$WORK/ramdisk-kern.new" ;;
  xz) xz -9 --check=crc32 -c "$WORK/ramdisk.cpio" > "$WORK/ramdisk-kern.new" ;;
  *) echo "unknown COMPRESS=$COMPRESS" >&2; exit 1 ;;
esac
rm -f "$WORK/ramdisk.cpio"

if [ -f "$OUT" ] && [ -z "$FORCE" ]; then
  rm -rf "$WORK/cmp-old" "$WORK/cmp-new"
  mkdir -p "$WORK/cmp-old" "$WORK/cmp-new"
  gzip -dc "$OUT" 2>/dev/null | (cd "$WORK/cmp-old" && cpio -idm --quiet 2>/dev/null) || true
  gzip -dc "$WORK/ramdisk-kern.new" | (cd "$WORK/cmp-new" && cpio -idm --quiet 2>/dev/null)
  if diff -rq --no-dereference "$WORK/cmp-old" "$WORK/cmp-new" >/dev/null 2>&1; then
    echo "  ramdisk content unchanged; keeping $OUT (use --force to replace)"
    exit 0
  fi
fi
cp "$WORK/ramdisk-kern.new" "$OUT"
echo "  -> $OUT ($(stat -c %s "$OUT") bytes, $COMPRESS)"
