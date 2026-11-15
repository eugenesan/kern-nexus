# kern-nexus

Kern on the LG Nexus 5 (`hammerhead`): a C Bitcoin signer on a Linux 3.4
recovery ramdisk with read-only firmware and /data/kern settings.  Single static ARMv7 binary, no Android
userspace, no Python, no network.  NFC (SeedKeeper/Satochip), camera QR,
framebuffer/touch UI, and the kernel-only RAM wipe from the Krux port.

This is the C counterpart of [`krux-nexus`](../krux-nexus).  It uses the
same TWRP recovery template, the same kernel tree and the same hardware
contract, but the payload is kern instead of Krux + MicroPython.

**Status:** port scaffold and documentation.  The first working target is
the static Linux/ARM build in `ports/nexus5`; see the phases in
[docs/bringup-plan.md](docs/bringup-plan.md).

**Start here:** [docs/kern-nexus-blueprint.md](docs/kern-nexus-blueprint.md)
is the overview and the coding-agent brief.  The index is
[docs/README.md](docs/README.md).

> **Warning:** the upstream kern README says it is unaudited and intended
> for research/testnet.  That applies here too.  This port is new and has
> no security audit.

## Target

| | |
|---|---|
| device | LG Nexus 5 (`hammerhead`), MSM8974, ARMv7-A + NEON, hard-float |
| kernel | hammerhead Linux 3.4 CAF fork, `airgap/cam-xz-wipe` |
| userspace | one static binary, `/opt/kern/kern-nexus` (or `/data/kern` in dev) |
| payload | kern (C, LVGL 9.5, libwally-core, cUR, k_quirc, BBQr) |
| external I/O | NFC (BCM2079x), camera, display/touch/keys; adb/UART only in dev |
| storage | settings/PIN in `/data/kern`; no seeds/plaintext secrets on eMMC |
| image | TWRP 3.7.0 ramdisk + kernel, flashed to **recovery** (`mmcblk0p20`) |

## Quick start

The build dependencies and exact commands are in
[docs/build-and-deploy.md](docs/build-and-deploy.md).

```sh
# one-time dependency builds: mbedTLS and zlib are vendored under kern/third_party
bash scripts/build-mbedtls.sh
bash scripts/build-zlib.sh

# kernel (patches + defconfig + air-gap profile)
bash scripts/build-kernel.sh

# kern + port layer (static ARM binary)
bash scripts/build-kern.sh

# init stub, TWRP ramdisk and boot image
bash scripts/build-image.sh

# development: boot without writing a partition
adb reboot bootloader
fastboot boot artifacts/built/boot-kern-dev.img

# delivered: recovery only; never flash boot
fastboot flash recovery artifacts/built/boot-kern-standard-release.img
```

## Layout

```
kern/        upstream kern checkout (C, LVGL, libwally-core, ...)
ports/       nexus5 CMake project and Linux/ARM platform layer
components/  ported camera, NFC, SeedKeeper components
rootfs/      init stub, ramdisk builder, boot-image packer
scripts/     kernel/kern/image/deploy wrappers
tools/       kernel patches/profiles, host serial helper, air-gap gate
linux/       kernel tree (submodule, airgap/cam-xz-wipe)
artifacts/   TWRP template and generated images (gitignored)
docs/        the guide and detail documents
```

## Rules

* **Never flash `boot`.**  The final image goes to `recovery`; dev images
  are `fastboot boot`ed.
* **No network.**  Wi-Fi, Bluetooth, cellular and USB network are compiled
  out or unused; NFC is the one wireless interface.
* **No Python.**  Camera and NFC are C components/threads, not daemons.
* **No persisted secrets.**  Settings/PIN live in `/data/kern` as in Krux;
  seeds and plaintext secrets are never written there and are
  `explicit_bzero()`ed.
* **Keep both chain paths.**  Standard Bitcoin uses SHA256; XBT changes the
  block/PoW to BLAKE2b and uses the same tagged-SHA256 unified sighash.  See
  [docs/bitcoin.md](docs/bitcoin.md).
* Copy only what the target needs from `krux-nexus`; record the source
  commit in each component README.

## Missing / to decide

The first draft omitted several things; they are tracked in the blueprint
and bring-up plan:

* mbedTLS and LVGL offline vendoring (the target build needs ARM static
  libraries; the simulator fetches LVGL from the network);
* the PIN/settings policy is `/data/kern` (resolved);
* the exact meaning of `k_run`/`k_next` (kernel phases assumed);
* camera geometry (keep Krux 720×960/960×1280);
* whether to keep the wired USB Ethernet driver in the threat model;
* whether the Linux binary gets any signature/verification at all.
