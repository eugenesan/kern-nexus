# kern-nexus — implementation guide and agent brief

**Status:** working plan, 2026-11-14.  This document replaces the earlier
`kern-nexus-blueprint.md` draft.  It is both the project overview and the
starting prompt for a coding agent.  The standalone unified-sighash
reference already passes all 166 embit vectors; the boot-chain bootstrap
images are built (device verification pending).  Detailed material is
split into:

| document | contents |
|---|---|
| [build-and-deploy.md](build-and-deploy.md) | toolchain, kernel, kern build, ramdisk/image, TWRP modes, flash/deploy |
| [hardware.md](hardware.md) | measured device nodes, display/input/power, camera, NFC wiring |
| [bitcoin.md](bitcoin.md) | unified opt-in sighash (corrected), SeedKeeper, Krux parity |
| [kernel-and-wipe.md](kernel-and-wipe.md) | kernel patch set, power-off wipe, `k_run`/`k_next` |
| [bringup-plan.md](bringup-plan.md) | ordered work, exit criteria, tests, open decisions |

---

## 1. What `kern-nexus` is

`kern-nexus` runs **kern**, a C Bitcoin signer, on the LG Nexus 5
(`hammerhead`, MSM8974, ARMv7-A Cortex-A15) as a single statically linked
Linux binary.  It is the C counterpart of the Krux port in `../krux-nexus`.

There is no Android userspace, no Python, no MicroPython, no network stack
and no persisted seed/plaintext storage in the delivered image.  The only external
I/O is:

* the **BCM2079x NFC** reader for JavaCard APDUs (SeedKeeper / Satochip /
  TapSigner);
* the **camera** for QR codes and camera entropy;
* the **display, touch and keys**;
* optional **adb/UART** for development and recovery, off in the sealed
  image.

The target use is an air-gapped signer, not a general-purpose phone.

## 2. Source layout

```
kern-nexus/
  kern/                 upstream kern (C, LVGL, libwally-core, cUR, k_quirc)
  ports/nexus5/         the Linux/ARM port layer and CMake project
  components/           ported camera and NFC components (C)
  rootfs/               init stub, ramdisk builder, boot-image packer
  scripts/              build, image and deploy wrappers
  tools/                kernel profiles/build helper, host serial helper
  linux/                kernel tree (submodule, airgap/cam-xz-wipe)
  tools/kernel/         kernel patches, defconfig, profiles, builder
  artifacts/            TWRP template and generated images (gitignored)
  docs/                 this guide and the detail documents
```

`kern/` is a nested git checkout, not a copy.  Keep port changes in
`ports/`, `components/` and `rootfs/` where possible; changes inside
`kern/` must be small and maintainable as patches or a branch.

## 3. Boot and delivery architecture

The delivered image is written to the **recovery** partition
(`/dev/block/mmcblk0p20`) and started from the bootloader.  **Never flash
`boot`.**  The boot partition stays untouched, so the phone can always be
returned to Android/recovery.

The image is TWRP 3.7.0's ramdisk plus:

* TWRP binary renamed to `/sbin/recovery.stock`;
* a symlink `/sbin/recovery -> recovery.stock` (TWRP's sideload child
  hardcodes that path);
* `/sbin/kern-init`, the init stub, installed as the `kern` service;
* a disabled `kern-firmware` service for `start-kern`;
* `/sbin/start-kern`, `/sbin/start-adb`, `/sbin/start-serial`;
* `/system/bin/sh -> /sbin/sh`, because TWRP's `system()` needs it;
* the kern binary and its assets in `/opt/kern` (standard image), or the
  development copy in `/data/kern` (development image);
* the camera CPP firmware in `/lib/firmware`.

TWRP's init remains PID 1, so mounts and USB come up normally.  The stub
decides which payload runs:

| image | plain boot | volume-up | from TWRP/adb |
|---|---|---|---|
| **standard** (shipped) | TWRP, adb **off** | kern | `start-kern` |
| **development** | kern | TWRP | `start-kern` / deploy |

The **sealed** build (`KERN_SEAL=1`) hides `/sbin/adbd` as
`/sbin/adbd.off` in every stub path.  `start-adb` restores it;
`start-serial` gives a root shell on `ttyHSL0` (115200 8N1), which is the
fallback when adb is unavailable.

`start-kern` must **stop TWRP and wait for the `recovery` process to
disappear** before starting kern.  `ctl.stop` is asynchronous; starting the
payload too early leaves TWRP and kern sharing the panel.  TWRP's sideload
needs adbd **off**; use Advanced → ADB Sideload from a fresh boot, not
`start-adb`.

The image is **gzip**; the stock bootloader only unpacks gzip for a
partitioned image.  XZ is allowed only on the `fastboot boot` path.
The kernel is `zImage-dtb`; a plain `zImage` has no DTB and will not boot.
Full procedure: [build-and-deploy.md](build-and-deploy.md).

## 4. Port architecture

`ports/nexus5/` builds the same kern application sources as the simulator,
with a different platform layer:

| kern dependency | ESP32-P4 implementation | nexus5 implementation |
|---|---|---|
| display | LCD panel + LVGL port | LVGL `LV_USE_LINUX_FBDEV` on `/dev/graphics/fb0`, plus the MSM pan/commit path |
| touch/keys | GT911/FT5x06 + GPIO | LVGL `LV_USE_EVDEV`; `/dev/input/event1` touch, `event2` keys |
| camera | `esp_video` + PPA | `components/camera` (MSM V4L2/ION/CPP) as an internal pthread |
| NFC | none | `components/nfc` (BCM2079x NCI/ISO-DEP/APDU) |
| NVS/flash | ESP NVS/SPIFFS | `/data/kern/nvs`, `/data/kern/flash` (same policy as Krux) |
| FreeRTOS | ESP-IDF | `pthread` shims (the simulator's `esp_idf_stubs`) |
| power/battery | PMIC/BSP | sysfs: backlight, battery, charger, `reboot(2)` |
| update | ESP OTA | disabled; the image is replaced through recovery |
| network | ESP32-C6 (held in reset) | none; kernel built without Wi-Fi/BT/cellular |

The simulator's `simulator/platform/esp_idf_stubs/` is the reference for
FreeRTOS/NVS/ESP API shims.  Copy only the pieces the target needs.  Do not
link SDL2, ESP-IDF, the ESP `video`/`sd_card`/BSP components, or any
network stack into the target.

Build a **single static binary** with `arm-linux-gnueabihf-gcc`, hard-float,
ARMv7-A/NEON, Linux 3.2 ABI (`-static -pthread`).  Disable
`HAVE_GETRANDOM` in libwally-core: the 3.4 kernel has no `getrandom(2)`.
`kern_wally.c` already installs the secure allocator; keep it.

The port layer must provide the interfaces in `main/CMakeLists.txt`:
`bsp_display_*`, `bsp_pmic_*`, `bsp_i2c_*`, `app_video_*`, `esp_*`,
FreeRTOS, NVS, SPIFFS-like storage, `sd_card` (or a stubbed, hidden
implementation).  See [hardware.md](hardware.md) for the exact nodes and
[build-and-deploy.md](build-and-deploy.md) for the CMake/toolchain setup.

## 5. Hardware map (measured; details in hardware.md)

| resource | path / value |
|---|---|
| display | `/dev/graphics/fb0`, 1080×1920, 32 bpp, **stride 4352**, triple-buffered |
| display refresh | `FBIOPAN_DISPLAY` + MSM `MSMFB_DISPLAY_COMMIT` |
| touch | `/dev/input/event1`, `ABS_MT_*`, X 0–1079, Y 0–1919 |
| keys | `/dev/input/event2` (`KEY_VOLUMEUP`, `KEY_VOLUMEDOWN`, `KEY_POWER`) |
| backlight | `/sys/class/backlight/lm3630/brightness` (max in `max_brightness`) |
| battery/charger | `/sys/class/power_supply/battery`, `.../usb`, `.../ac` |
| camera | `/dev/video*`, `/dev/v4l-subdev*`, `/dev/ion` |
| NFC | `/dev/bcm2079x` (I2C 0x77, BCM20793M), ioctls `0xFA01`–`0xFA07` |
| UART | `ttyHSL0`, 115200 8N1, host `/dev/ttyUSB0` |
| recovery partition | `/dev/block/mmcblk0p20` |
| system/cache/userdata | `mmcblk0p25`, `p27`, `p28` |

## 6. Camera (details in hardware.md and components/camera)

The camera is not a Python daemon.  It is a C component started as a
**pthread** by the port layer and exposed through the `app_video_*`
functions kern already calls:

```c
esp_err_t app_video_init_once(i2c_master_bus_handle_t ignored);
esp_err_t app_video_start(app_video_frame_operation_cb_t cb, int core);
esp_err_t app_video_stop(void);
bool app_video_is_ready(void);
bool app_video_is_streaming(void);
```

The camera keeps Krux's geometry (binned **720×960** default; **960×1280**
full/PIX) and YUV420.  `scanner.c` currently expects RGB565 and uses the
P4 PPA; the Nexus 5 has no PPA, so add a `KERN_NEXUS` path that crops/scales
the Y plane on the CPU (or a software PPA shim).  Do not change the sensor
geometry just to satisfy 800×800.

The source is `../krux-nexus/cam/camd.c` (4,758 lines) plus its headers and
`cam/fw/cpp_firmware_v1_2_0.fw`.  Refactor `main()` into
`kern_camera_init/start/stop`, remove the Unix socket and supervisor
process, and keep the frame path.  The CPP firmware is 16 KB and must be
staged into `/lib/firmware`; never copy a file onto itself with `O_TRUNC`.
The kernel loads it through `request_firmware`/the MSM CPP driver.

Keep the stream continuous; long pauses and restarts while streaming are
the fragile path.  Keep the AE/AF controls and the QR/entropy target
semantics from camd.  See [hardware.md](hardware.md#camera).

## 7. NFC and SeedKeeper (details in hardware.md and bitcoin.md)

The BCM2079x transport is seven ioctls on `/dev/bcm2079x`; the chip speaks
NCI.  The kernel driver delivers **one whole framed packet per read()**:

```
byte 0   HCIT type 0x10 (NFC)
byte 1   MT | PBF | GID
byte 2   OID
byte 3   payload length
byte 4+  payload
```

The bring-up sequence and the vendor's exact `SET_CONFIG` payloads are in
`../krux-nexus/krux/src/nfc.py` and `docs/nfc-transport.md`.  Port that
sequence to C; the Python file is the protocol reference, not a runtime
dependency.  The chip's firmware is already in place; the `.ncd` blobs are
kept only for recovery/reference.

`SeedKeeper` is a JavaCard applet.  Port `krux/src/krux/seedkeeper.py` (plain
APDUs, PIN, secret headers, import/export) and, if required,
`seedkeeper_sc.py` (secure channel).  The secure channel uses secp256k1
ECDH, HMAC-SHA1, AES-128-CBC; libwally-core and mbedTLS provide those.
Keep the AID, status words and wire formats byte-compatible with
SeedSigner/SeedKeeper.  Details and test plan: [bitcoin.md](bitcoin.md).

## 8. Unified opt-in sighash — corrected specification

The unified opt-in is an **additional** mechanism, not a replacement for
SHA256.  Standard Bitcoin keeps SHA256d/BIP143/BIP341.  XBT ("blake2b
Bitcoin") changes the **block header / proof of work** to BLAKE2b and
activates the unified sighash at the same buried deployment
(`Blake2bHeight`); the unified signature hash itself is still
`TaggedHash("UnifiedSighash", ...)` with SHA256.

Both block structures and both signing paths coexist.  The SeedSigner fork
(`/home/user/Develop/src/krux-mobile/seedsigner`, branch
`privkeio/unified-sighash`, embit pin `5f700aa`) is the clean reference: it
has the 166 vectors, Knots interop, CVE-2020-14199 and the full UI flow.
Re-port from there rather than carrying the Krux patch forward.

The reference implementations are:

* `krux/vendor/embit` branch `unified-sighash-v0.8.1`, commits
  `fff7ffa..de0e363`;
* `krux/src/krux/sighash.py` (Krux's selection/validation UI policy);
* `krux/vendor/embit/tests/tests/data/unified_sighash.json` and
  `test_unified_sighash*.py` (vectors and negotiation tests).

Port it into `libwally-core` as a transaction/PSBT API, add a C vector test,
then wire kern's PSBT review/signing to show and use the opt-in.  The full
message layout, validation rules, cache invariants and libwally design are
in [bitcoin.md](bitcoin.md).  This is the highest-risk work: get the vectors
byte-identical before touching hardware.

## 9. Kernel and memory wipe

The kernel is the same hammerhead 3.4 CAF fork used by `krux-nexus`, branch
`airgap/cam-xz-wipe`.  The patch series in `tools/kernel/patches/` carries the
camera/CPP fixes, XZ kernel/ramdisk support, USB gadget/adb fixes and the
kernel-only power-off wipe.  Build it with `lineageos_hammerhead_defconfig`
and the air-gap profile; use `zImage-dtb`.

The wipe is **kernel and RAM only**:

1. `sys_reboot(LINUX_REBOOT_CMD_POWER_OFF)` writes a magic/CRC marker at
   `0x100F0000` in the persistent-RAM tail and restarts with reason
   `recovery` (IMEM `0x77665502`).
2. The next boot sees the marker at `mem_init()`, wipes every free page,
   and powers off from `kernel_init()` before userspace starts.
3. The marker is cleared at that power-off.  There is no NAND/BCB write.

For kern-nexus this has two layers:

* **`k_run`** — the running kern process: `explicit_bzero()` its heap,
  framebuffer/scanout, camera buffers and NFC/APDU buffers before requesting
  power-off.  The kernel's free-page wipe does not cover reserved memory.
* **`k_next`** — the next boot's early kernel: the marker-driven free-page
  wipe described above, before any userspace exists.

If `k_run`/`k_next` are meant as two separate binaries or boot slots rather
than these two phases, say so and this section will be renamed.  Full
design, limits and recovery caveats: [kernel-and-wipe.md](kernel-and-wipe.md).

**Bootloop caveat:** after a wipe the charger or a normal power-on boots the
**boot partition**.  If Android there is broken (`/system/bin/init`
missing), the phone bootloops in Android, not in this firmware.  Enter the
bootloader (Volume Down + Power), select Recovery, or repair Android.

## 10. Ordered work

1. **Make the port configure and link** with the simulator stubs, then
   replace display/input with fbdev/evdev.  Exit: a static ARM binary that
   draws kern's UI.
2. **Integrate the camera pthread** and prove `k_quirc` decodes a QR.
3. **Port the unified sighash into libwally** and pass all embit vectors.
4. **Integrate NFC transport**, then SeedKeeper plaintext flows, then the
   secure channel.
5. **Storage and wipe**: settings/PIN in `/data/kern`, explicit zeroing of
   secrets, kernel marker wipe (unchanged; build flags for dev).
6. **TWRP ramdisk/image**, standard/sealed modes, start scripts and seal.
7. **Hardening**: no radios, no SD menus, no update path, `security-check`.

Exit criteria and test commands: [bringup-plan.md](bringup-plan.md).

## 11. Open decisions / things missing from the first draft

These need an explicit answer before the affected work is final:

1. **`k_run`/`k_next` — resolved:** they are the kernel's live/next-boot
   phases.  The kernel wipe is self-contained; use `KERN_RELEASE`/`KERN_SEAL`
   for development.
2. **PIN/settings — resolved:** use `/data/kern` as Krux does.  Keep the
   PIN verifier and settings there; do not put seeds or plaintext secrets on
   eMMC.  The Nexus 5 has no ESP eFuse HMAC: replace
   `efuse_hmac_sim.c`'s fixed test key with a per-device random salt stored
   in `/data/kern` (or a kernel-provided secret), and keep the same PIN
   verifier format.
3. **mbedTLS** — kern uses mbedTLS (`mbedtls`, `mbedcrypto`, `mbedx509`) and
   PSA headers in `crypto_utils`, `pin` and `kef`.  ESP-IDF supplies it on
   the P4; the Linux target needs a pinned ARM static build
   (`scripts/build-mbedtls.sh`).
4. **LVGL vendoring** — the simulator uses `FetchContent` (network).  The
   offline target must vendor LVGL 9.5 or use a pinned local tree.
5. **libwally pin** — `components/libwally-core/upstream` is the fork.  Keep
   a tag for every pin; record it in `dependencies.lock`/README.
6. **KernelSU** — the Krux kernel branch contains KernelSU.  kern-nexus does
   not need it (TWRP/kern run as root).  The patch series excludes it; do
   not enable it unless there is a reason.
7. **Camera format — resolved:** keep Krux's 720×960 / 960×1280 YUV420;
   scale/crop only if a hard scanner assert appears.
8. **Storage menus** — the blueprint says disable SD/file menus.  They are
   still compiled into kern; either compile them out or make the storage
   shim return "unavailable" and hide the menu entries.
9. **Sideload / Android** — the recovery image deliberately leaves `boot`
   and Android alone.  If Android is broken, normal boot loops; document
   the recovery-key procedure.
10. **Secure boot / signing** — kern's ESP32 secure boot does not apply.
    Decide whether the Linux binary gets any signature/verification at all
    (the kernel/bootloader path does not verify it in this design).
11. **XBT / BLAKE2b** — the SeedSigner reference is available; the
    remaining work is BLAKE2b block-header support (not a second signature
    digest) and the XBT network/activation rules.

## 12. Prompt for the coding agent

> Work in `kern-nexus`.  The target is the LG Nexus 5 (`hammerhead`,
> MSM8974), Linux 3.4, ARMv7 hard-float, single static binary.  `kern/` is
> upstream C; `../krux-nexus/` is the working Linux port of Krux.  Read
> `docs/kern-nexus-blueprint.md` and its detail documents first.
>
> Start with the buildable port: `ports/nexus5/` CMake, the simulator's
> ESP-IDF/FreeRTOS stubs, LVGL fbdev/evdev, and the BSP/`app_video_*`
> shims.  Do not link SDL2 or ESP-IDF.  Then integrate the camera pthread
> from `components/camera`, then the NFC transport, then the unified
> sighash in libwally.
>
> Never flash `boot`.  Dev images use `fastboot boot`; the delivered image
> goes to `recovery`.  Keep adb off in the sealed image.  Do not add a
> network stack, Wi-Fi, Bluetooth or cellular code.  Preserve SeedKeeper
> wire compatibility.
>
> Standard Bitcoin keeps SHA256.  XBT changes the block/PoW to BLAKE2b but
> keeps the tagged-SHA256 unified signature hash.  Validate the 166 vectors
> first, then the XBT block/activation rules.
>
> For every ported component, record the source commit and the deviations
> in `components/<name>/README.md`.  Copy only what the target needs.
