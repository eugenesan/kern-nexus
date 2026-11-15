# tools/bringup

`placeholder.c` is a minimal static ARM binary for the boot-chain test.  It
opens `/dev/graphics/fb0`, paints a checkerboard, logs one line to
`/dev/kmsg`, and sleeps.  It does not touch Android partitions.

```sh
arm-linux-gnueabihf-gcc -O2 -static -s -o /tmp/kern-placeholder placeholder.c

KERN_RELEASE=1 KERN_SEAL=1 KERN_BIN=/tmp/kern-placeholder \
  bash rootfs/build-stub-ramdisk.sh --standard --bundle --force
RAMDISK=artifacts/built/ramdisk-kern-bundled \
  bash rootfs/make-boot-image.sh \
  artifacts/twrp-3.7.0_9-HH.R.17-MADV_WIPEONFORK.img \
  ../krux-nexus/artifacts/built/kernel-final/zImage-dtb \
  artifacts/built/boot-kern-bootstrap.img
fastboot boot artifacts/built/boot-kern-bootstrap.img
```

Use this to prove TWRP, `start-kern`, `start-adb`, `start-serial`, the
`/sbin/recovery` symlink and the seal before kern is buildable.
