# Build and deploy

This is the concrete build guide for `kern-nexus`.  It assumes the
repository is checked out at `~/Develop/src/kern-nexus` and the reference
port at `~/Develop/src/krux-nexus`.

## 0. Build locations

Generated files do not live in the checkout:

| path | what |
|---|---|
| `/tmp/kern-nexus/artifacts/build` | kernel `O=`, CMake tree, binaries, images |
| `/tmp/kern-nexus/artifacts/deps` | static mbedTLS and zlib installs |

`scripts/env.sh` defines them; override with `KERN_NEXUS_TMP`,
`KERN_NEXUS_BUILD` or `KERN_NEXUS_DEPS`.  The repository's `artifacts/`
holds only the TWRP template and its README.  The default is tmpfs, so a
reboot clears the build tree; re-run the dependency scripts after that.

## 1. Toolchain

| tool | version/use |
|---|---|
| `arm-linux-gnueabihf-gcc` | host cross compiler, 15.2.0 on this machine; ARMv7 hard-float |
| `arm-linux-gnueabihf-strip`, `ar`, `ranlib` | binutils |
| `cmake` ≥ 3.16 (4.2 here) | kern build |
| `make`, `bc`, `flex`, `bison`, `python3`, `cpio`, `gzip`, `xz`, `git` | kernel and image builds |
| `adb`, `fastboot` | deploy/diagnose |
| `dtc` | optional DTB inspection |
| `libz` (ARM static) | libwally/BBQr/deflate; vendored at `kern/third_party/zlib`, built by `scripts/build-zlib.sh` |
| **mbedTLS 3.x** (ARM static) | kern `crypto_utils`, PIN, KEF, SeedKeeper SC |
| **LVGL 9.5** | vendored tree, no `FetchContent` on an offline build |
| `libwally-core` | `kern/components/libwally-core/upstream` submodule |

The target is **not** built with ESP-IDF.  ESP-IDF remains only in `kern/`
as the upstream build; the nexus5 CMake project uses the host cross
compiler and the platform shims.

The host sysroot is `/usr/arm-linux-gnueabihf`.  Static `libc.a` and
`libpthread.a` are there; mbedTLS and zlib must be built for the same
sysroot.  Do not link the host's x86 libraries.

`ports/nexus5/toolchain-armhf.cmake` sets:

```
-march=armv7-a -mtune=cortex-a15 -mfpu=neon-vfpv4 -mfloat-abi=hard
-O2 -D_DEFAULT_SOURCE -static -pthread
-Wl,--gc-sections
```

`libwally-core` must be compiled with `HAVE_GETRANDOM` **off** (Linux 3.4
has no `getrandom(2)`) and `WALLY_ABI_NO_ELEMENTS`.

### mbedTLS — why the target needs it

kern uses mbedTLS directly on the ESP32-P4: `main/core/crypto_utils.c`,
`pin.c`, `kef.c` and the simulator's `efuse_hmac_sim.c` call mbedTLS for
AES, HMAC, PBKDF2 and PSA.  On ESP-IDF those libraries are supplied by the
IDF build; on Linux there is no IDF, so a static ARM mbedTLS is a real
dependency, not an optional one.  `scripts/build-mbedtls.sh` builds a
pinned source tree at `kern/third_party/mbedtls` (submodule, v3.6.7) into
`/tmp/kern-nexus/artifacts/deps/mbedtls-armhf` (static).  `scripts/build-zlib.sh` does the
same for `kern/third_party/zlib` (v1.3.1) into `/tmp/kern-nexus/artifacts/deps/zlib-armhf`.  `MBEDTLS_SRC` can override the
source path; the script does not download.  Required modules: `mbedtls`, `mbedcrypto`, `mbedx509`, PSA
crypto headers.

If the goal is to avoid mbedTLS entirely, the alternative is to port
`crypto_utils`/PIN/KEF to libwally-core's primitives and a small AES
implementation.  That is more work and more code to audit; the recommended
path is a pinned mbedTLS.

### LVGL — why it is not already there

`kern`'s ESP-IDF build pulls LVGL through
`main/idf_component.yml` (`lvgl/lvgl 9.5.*`) into `managed_components/` at
IDF configure time.  That directory is generated, not part of the repo.
The simulator CMake uses `managed_components/lvgl__lvgl` if it exists and
otherwise fetches LVGL from the network.  For the target, LVGL is now a
pinned submodule at `kern/third_party/lvgl` (tag `v9.5.0`); `ports/nexus5/
CMakeLists.txt` requires that path and fails rather than fetching.

`ports/nexus5/lv_conf.h` enables `LV_USE_LINUX_FBDEV` and
`LV_USE_EVDEV`, 32-bit color and the fonts kern uses.

## 2. Kernel

The kernel is the hammerhead 3.4 CAF tree with the changes in
`tools/kernel/patches/kern-nexus-kernel.patch` (camera/CPP, XZ, USB gadget, wipe).
The patch is against `lineage-23.2`.

```sh
bash scripts/build-kernel.sh
# -> /tmp/kern-nexus/artifacts/build/ko/arch/arm/boot/zImage-dtb
# -> /tmp/kern-nexus/artifacts/build/ko/.config
# -> /tmp/kern-nexus/artifacts/build/ko.build.log
```

The wrapper:

1. uses `linux/` if it is a kernel tree; otherwise clones
   `eugenesan/android_kernel_lge_hammerhead` branch `airgap/cam-xz-wipe`
   (or applies the patch to a `lineage-23.2` tree);
2. runs `lineageos_hammerhead_defconfig`;
3. applies `tools/kernel/profiles/hammerhead.list` and the keep-list;
4. verifies nothing re-enabled the radios;
5. builds with the host cross compiler.

**Use `zImage-dtb`**, not `zImage`.  A plain `zImage` has no appended DTB
and the bootloader refuses it.

The kernel features the port depends on:

* `CONFIG_KERNEL_XZ=y` — XZ-compressed kernel (~4 MB);
* `CONFIG_RD_XZ=y` — XZ ramdisk on the `fastboot boot` path;
* `CONFIG_MEMORY_WIPE=y`, `CONFIG_MEMORY_WIPE_ON_POWEROFF=y`;
* `CONFIG_NFC_BCM2079X=y`;
* camera/CPP/ISP options from `hammerhead-camera.list`;
* `CONFIG_DEVTMPFS=n` (TWRP's ueventd creates `/dev`; the VFE
  instrumentation build may override it through `KEEPLISTS`, never by
  editing the tracked profile).

`linux/README.md` explains the patch and how to regenerate it.

## 3. kern (the C binary)

```sh
bash scripts/build-kern.sh
# -> /tmp/kern-nexus/artifacts/build/kern-nexus
```

The script configures `ports/nexus5` with
`-DCMAKE_TOOLCHAIN_FILE=ports/nexus5/toolchain-armhf.cmake`, builds
`kern-nexus`, and fails if the result is not a static ARM ELF.  It does
**not** build the ESP32 boards.

The CMake target compiles:

* `kern/main/**` (core, pages, QR, UI) — excluding the ESP `video`
  component and hardware-specific BSPs;
* `kern/components/{libwally-core,cUR,k_quirc,bbqr,deflate_codec,bip138,secure_memory}`;
* `ports/nexus5/platform/**` — fbdev/evdev/BSP/power/FreeRTOS/NVS shims;
* `components/camera/**`, `components/nfc/**`, `components/seedkeeper/**`;
* LVGL 9.5 with fbdev/evdev.

It links `mbedtls`, `mbedcrypto`, `mbedx509`, `z`, `pthread`, `m`.

Expected output: ELF 32-bit ARM, statically linked, no `NEEDED` entries.
Check with:

```sh
file /tmp/kern-nexus/artifacts/build/kern-nexus
arm-linux-gnueabihf-readelf -d /tmp/kern-nexus/artifacts/build/kern-nexus | grep NEEDED
```

## 4. Ramdisk and boot image

```sh
bash scripts/build-image.sh            # standard + sealed, gzip
KERN_SEAL=0 bash scripts/build-image.sh  # audit (adb on)
```

The script builds the init stub, packs the TWRP ramdisk, installs kern and
the CPP firmware, and then makes the boot image:

```
/tmp/kern-nexus/artifacts/build/ramdisk-kern
/tmp/kern-nexus/artifacts/build/boot-kern-standard-release.img
/tmp/kern-nexus/artifacts/build/boot-kern-standard-release-audit.img  # KERN_SEAL=0
```

The template is
`artifacts/twrp-3.7.0_9-HH.R.17-MADV_WIPEONFORK.img`.  The header (load
addresses, page size, cmdline) is copied from it; do not invent those
numbers.  The maximum image size is 23,068,672 bytes.

The delivered image is **gzip** because it is written to a partition.  XZ
is only for `fastboot boot` development images (`COMPRESS=xz`).

## 5. Deployment modes

| mode | image | what starts | adb | use |
|---|---|---|---|---|
| **standard sealed** (default) | `boot-kern-standard-release.img` | TWRP; Vol+/`start-kern` starts kern | off | delivered |
| **standard audit** | same with `KERN_SEAL=0` | TWRP; kern on request | on | on-device audit |
| **development** | `boot-kern-dev.img` | kern by default; Vol+ TWRP | on | bench |

The sealed stub renames `/sbin/adbd` to `/sbin/adbd.off` in every path.
`start-adb` restores it; `start-serial` is the UART fallback.

## 6. Flash and boot

Development/audit: boot without writing a partition:

```sh
adb reboot bootloader
fastboot boot /tmp/kern-nexus/artifacts/build/boot-kern-dev.img
```

Delivered firmware: write **recovery only**:

```sh
adb reboot bootloader
fastboot flash recovery /tmp/kern-nexus/artifacts/build/boot-kern-standard-release.img
```

Then choose **Recovery mode** in the bootloader.  **Never flash `boot`.**
A plain power-on starts the boot partition (Android), not this image.

Sideload (also the way to repair a broken Android `/system`):

```sh
# In TWRP: Advanced -> ADB Sideload -> swipe
adb sideload lineage-*.zip
```

Sideload is reliable with adbd **off**.  If adbd is running, stop it
(`setprop ctl.stop adbd`) or reboot first.  TWRP's sideload child is
`execl("/sbin/recovery", "recovery", "--adbd", ...)`; the
`/sbin/recovery -> recovery.stock` symlink is mandatory.

## 7. Updating kern during development

The development image runs `/data/kern/kern-nexus` when present, else
`/opt/kern/kern-nexus`.  Deploy without repacking:

```sh
bash scripts/build-kern.sh
bash scripts/deploy.sh dev
# pushes to /data/kern/kern-nexus, stops/starts the kern service
```

`scripts/deploy.sh fastboot` uploads a boot image and `fastboot boot`s it.
`scripts/deploy.sh recovery` flashes a sealed image to recovery (explicit
confirmation required).

## 8. Boot-chain bootstrap

Before kern is buildable, prove the TWRP/init/service/seal path with the
placeholder:

```sh
bash scripts/build-bootstrap.sh
# /tmp/kern-nexus/artifacts/build/boot-kern-bootstrap.img       (sealed, TWRP default)
# /tmp/kern-nexus/artifacts/build/boot-kern-bootstrap-audit.img (adb on)
```

`tools/bringup/placeholder.c` draws a checkerboard and logs to `/dev/kmsg`.
Use it to check:

* TWRP boots with adb off in the sealed image;
* `start-kern`/Vol+ runs only the placeholder, not TWRP in parallel;
* `start-adb` and `start-serial` work;
* the `/sbin/recovery` symlink and `/system/bin/sh` are present.

It uses the kernel from `/tmp/kern-nexus/artifacts/build/ko/
zImage-dtb` by default; set `KERNEL_IMG=` to another kernel.

## 9. Troubleshooting

| symptom | cause/fix |
|---|---|
| adb absent after boot | expected in the sealed image; run `start-adb` in TWRP terminal |
| no serial output | `stty -F /dev/ttyUSB0 115200 raw`; run `start-serial` |
| `adbd status 255` on sideload | `/sbin/recovery` symlink missing or `/system/bin/sh` missing |
| TWRP and kern both draw | `start-kern` did not wait for `recovery` to exit; stop TWRP, wait, or kill the residual process |
| camera no preview | `/lib/firmware/cpp_firmware_v1_2_0.fw` zero/missing; `dmesg` CPP version `deadbeef` means empty firmware |
| `dtb not found` | built with `zImage`, not `zImage-dtb` |
| bootloop after wipe | charger auto-boots Android; Android `/system` broken; enter bootloader → Recovery |
| TWRP/Kern text tiny or offset | framebuffer 1080×1920 stride 4352, not 4320; use the pan/commit path |

## 10. What is deliberately not here

* no Android source tree;
* no ESP-IDF target build;
* no Python/MicroPython;
* no Wi-Fi/Bluetooth/cellular drivers or firmware;
* no SD card or file-browser in the delivered UI;
* settings/PIN under `/data/kern` as in Krux; firmware read-only, no seed storage;
* no kexec and no second kernel in the wipe path.
