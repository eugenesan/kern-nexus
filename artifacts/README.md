# artifacts/

Build inputs that must live in the checkout.  Everything generated goes to
`/tmp` by default:

| path | what |
|---|---|
| `/tmp/kern-nexus/artifacts/build/ko/` | kernel build (`zImage-dtb`, `.config`, log) |
| `/tmp/kern-nexus/artifacts/build/kern-nexus` | static ARM binary |
| `/tmp/kern-nexus/artifacts/build/ramdisk-kern*` | TWRP ramdisk with the kern stub |
| `/tmp/kern-nexus/artifacts/build/boot-kern-*.img` | boot images (`fastboot boot` or recovery flash) |
| `/tmp/kern-nexus/artifacts/deps/mbedtls-armhf/` | static mbedTLS install |
| `/tmp/kern-nexus/artifacts/deps/zlib-armhf/` | static zlib install |
| `twrp-3.7.0_9-HH.R.17-MADV_WIPEONFORK.img` | boot-image header/ramdisk template |

Override the roots with `KERN_NEXUS_TMP`, `KERN_NEXUS_BUILD` or
`KERN_NEXUS_DEPS`; see `scripts/env.sh`.

`scripts/build-image.sh` writes the standard image to
`.../build/boot-kern-standard-release.img` (or `...-audit.img` with
`KERN_SEAL=0`).  The image is flashed to the **recovery** partition
(`mmcblk0p20`), never to `boot`.

The build tree is on tmpfs, so it is lost on reboot.  Re-run
`scripts/build-mbedtls.sh` and `scripts/build-zlib.sh` after a reboot, or
point the roots at a persistent disk.

## Boot-chain bootstrap

`.../build/boot-kern-bootstrap*.img` use `tools/bringup/placeholder.c`, not
the real kern binary.  They exist to prove TWRP → kern-init → `/opt/kern` →
`start-kern`/Vol+ before kern is buildable.  Do not treat them as releases.
