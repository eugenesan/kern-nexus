# artifacts/

Build inputs and outputs.  The directory is gitignored except for this
note.

| path | what |
|---|---|
| `twrp-3.7.0_9-HH.R.17-MADV_WIPEONFORK.img` | boot-image header/ramdisk template |
| `built/ko/` | kernel build (zImage-dtb, .config, log) |
| `built/kern-nexus` | static ARM binary |
| `built/ramdisk-kern*` | TWRP ramdisk with the kern stub |
| `built/boot-kern-*.img` | boot images (`fastboot boot` or recovery flash) |
| `deps/mbedtls-armhf/` | optional static mbedTLS install |

`scripts/build-image.sh` writes the standard image to
`built/boot-kern-standard-release.img` (or `...-audit.img` with
`KERN_SEAL=0`).  The image is flashed to the **recovery** partition
(`mmcblk0p20`), never to `boot`.

## Boot-chain bootstrap

`built/boot-kern-bootstrap*.img` use `tools/bringup/placeholder.c`, not the
real kern binary.  They exist to prove TWRP → kern-init → `/opt/kern` →
`start-kern`/Vol+ before kern is buildable.  Do not treat them as releases.
