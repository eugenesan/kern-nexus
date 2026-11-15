# Complete Porting Blueprint, Parity Report & Agent Specification: `kern-nexus` (LG Nexus 5)

## 1. Architecture & Target Design Summary

* **Target Device**: LG Nexus 5 (`hammerhead`, Qualcomm Snapdragon 800 / MSM8974, ARMv7-A Cortex-A15 + NEON HardFloat).
* **kernel & Userspace**: Existing Linux 3.4 Android recovery kernel (`msm8974`) and ramdisk created from pre-exisitng TWRP recovery (artifacts/twrp-3.7.0_9-HH.R.17-MADV_WIPEONFORK.img) running in stateless `initramfs`. While TWRP init does mounts various filesystems we do not use that (except during developlment when it useful to deploy and test without reboot. Zero wireless/network connectivity except NFC used to communicate with JavaCards (SeedKeeper).
* **Binary Output**: Single statically linked executable (`kern-nexus`), compiled with `-march=armv7-a -mtune=cortex-a15 -mfpu=neon-vfpv4 -mfloat-abi=hard -static` targeting the Linux `3.2.0` ABI.
* **Storage & Transport**:
  * **Stateless RAM Operation**: Ephemeral config/NVS mapped to `/tmp/kern_nvs` (`tmpfs`), wiped cleanly via `explicit_bzero()` on timeout/shutdown.
  * **NFC SecureCard (Sole External I/O)**: Broadcom BCM2079x (`/dev/bcm2079x`) C NCI/ISO-7816 driver from `krux-nexus` linked directly into the binary for Satochip / Seedkeeper / TapSigner APDU communication.
  * **Camera QR Scanner**: Reverse-engineered Qualcomm `msm` camera C service (`/dev/video*`, `/dev/media*`, `/dev/v4l-subdev*`, `/dev/ion`) from `krux-nexus` integrated as an internal `pthread` feeding 8-bit Y-plane luminance frames directly into kern's `k_quirc` decoder and LVGL preview canvas.
  * **Display & Touch**: Linux `fbdev` (`/dev/graphics/fb0` with `FBIOPAN_DISPLAY` ioctl refresh) and `evdev` (`/dev/input/event*` for Synaptics DSX touch and GPIO volume/power keys) driven directly by LVGL v9.

---

## 2. Bitcoin Feature Parity & XBT Unified Sighash (BLAKE2) Analysis

**kern** (`odudex/kern`) achieves ~95% standard Bitcoin functional parity with **Krux** (`selfcustody/krux` / `eugenesan/krux-nexus`) in pure C (`libwally-core`, `cUR`, `k_quirc`, `LVGL v9`).

| Feature Category | Krux (`krux-nexus`) | kern (`odudex/kern`) | Porting Impact for `kern-nexus` |
| :--- | :--- | :--- | :--- |
| **Mnemonics & Backups** | BIP-39 (12/24), SeedQR, Compact SeedQR, KEF encrypted backups, BIP-39 passphrase, BIP-85, SeedXOR, Binary Grids | BIP-39 (12/24), SeedQR, Compact SeedQR, KEF encrypted backups, BIP-39 passphrase, BIP-85 | **Full Parity** on core formats (KEF backups are cross-compatible). SeedXOR/Binary Grids omitted in kern. |
| **Wallet Policies** | Legacy, SegWit (BIP-49/84), Taproot (BIP-86), Multisig, `wsh` Miniscript | Legacy, SegWit (BIP-49/84), Taproot (BIP-86), Multisig, **`wsh` & `tr` (Taproot) Miniscript**, **BIP-138** descriptor backups | **kern Ahead** (adds Taproot Miniscript & BIP-138 encrypted descriptor backups). |
| **Signing (BTC)** | PSBT signing, BIP-137 message signing, partial BIP-322 | PSBT signing (`libwally-core`), BIP-137, **PSBT-based BIP-322** (SegWit & Taproot) | **kern Ahead** (faster PSBT parsing, full BIP-322). |
| **XBT Fork Unified Sighash (BLAKE2)** | Custom **BLAKE2 unified opt-in signature hash** implemented in `eugenesan/embit` (`unified-sighash-v0.8.1`, commits `fff7ffa..de0e363`) | Standard BTC SHA256d / BIP-143 / BIP-341 sighash in `libwally-core` | **Port Required**: Port the 10-commit unified BLAKE2 sighash specification from `eugenesan/embit` (`unified-sighash-v0.8.1`) into `libwally-core` + kern. |
| **QR Protocols** | Static QR, BC-UR (`uUR`), BBQr, Specter `pMofN` via `quirc`/`zbar` | Static QR, BC-UR (`cUR`), BBQr, Specter `pMofN` via **`k_quirc`** (~2x faster) | **Full Parity + Speed Gain** (100% compatible with Sparrow, Specter, BlueWallet, Nunchuk, Coldcard Q). |
| **NFC SecureCards** | Satochip / Seedkeeper / TapSigner over `bcm2079x` APDUs | Not in upstream kern (uses QR + SD card) | **Port Required**: Link `krux-nexus` `bcm2079x` C driver into kern's seed loader & PSBT signer. |
| **File / Printer I/O** | SD card + ESC/POS thermal printer | SD card (no printer) | Disable SD/file menus in `kern-nexus` to enforce stateless NFC + QR operation. |

### 2.1 Porting XBT Unified Opt-In Sighash (BLAKE2) to `libwally-core`

Because **XBT (BLAKE2)** uses a custom **unified opt-in signature hash** rather than standard Bitcoin SHA256d / BIP-143 / BIP-341 sighash algorithms, upstream `libwally-core` does **not** implement it out of the box. You will need to maintain a clean patchset on top of `libwally-core` mirroring the exact behavior of `eugenesan/embit` branch `unified-sighash-v0.8.1` (`git diff fff7ffa..de0e363`):

1. **Cryptographic Primitive (`BLAKE2b` / `BLAKE2s`)**:
   * Check which BLAKE2 variant (`blake2b` or `blake2s`, personalization string, and output digest length) `eugenesan/embit` (`9198d5f`) uses for the unified digest.
   * Vendor a compact, standalone public-domain C BLAKE2 implementation (e.g., official `BLAKE2/libb2` or `rfc7693` single-file C implementation) directly into `libwally-core/src/` (or `third_party/`) so there are zero external dependencies.
2. **Exact Semantic Invariants from `eugenesan/embit` (`fff7ffa..de0e363`)**:
   * **Opt-In Flag & Negotiation (`9198d5f`, `270f406`, `2b1828c`)**: Preserve the exact opt-in sighash flag/bitmask negotiation so standard BTC sighashes remain untouched unless the unified opt-in is explicitly requested/negotiated.
   * **Committed Script Type (`de0e363`)**: Determine the committed script type directly from the input's script (`scriptPubKey` / `witnessScript` / `redeemScript`), **not** inferred from which PSBT fields (`PSBT_IN_NON_WITNESS_UTXO` vs. `PSBT_IN_WITNESS_UTXO`) are populated.
   * **Strict Input Validation (`bfa8545`, `67a79a0`)**: Reject any hash type that the XBT reference implementation rejects, fail one input at a time with clear error codes, and confine `SIGHASH_SINGLE` skip behavior strictly to the unified opt-in path.
   * **Sibling Cache & Memory Footprint (`dc59267`, `7e4b9c1`)**: Keep the caller's own input values isolated from any shared/cached preimage digests across inputs (`prevouts` / `sequences` / `outputs` / `spent_amounts` / `scriptpubkeys`) while keeping PSBT memory overhead constant.
3. **Target Files in `libwally-core` & `kern`**:
   * **`libwally-core/src/psbt.c` & `src/transaction.c`**: Add `wally_tx_get_xbt_unified_signature_hash()` (or extend `wally_psbt_sign_input()` / `wally_psbt_get_input_signature_hash()`) and expose the opt-in flag in `include/wally_psbt.h` / `include/wally_transaction.h`.
   * **Unit Test Parity (`1bd4b0a`, `2b1828c`)**: Translate every unit test vector from `eugenesan/embit` (`unified-sighash-v0.8.1`) into a C test binary (`test_xbt_sighash`) linked against your patched `libwally-core` to guarantee byte-for-byte digest parity before testing on hardware.
   * **kern PSBT Signer & UI**: Update kern's PSBT inspection/signing flow to recognize the XBT opt-in sighash flag, display an explicit indicator on the transaction review screen when unified XBT sighash is active, and pass the flag into `libwally-core` (and/or your Satochip APDU sighash builder if signing on-card).

---

## 3. External Source Trees & Dependencies Required

Beyond **`odudex/kern`**, **`eugenesan/krux-nexus`**, **`eugenesan/embit` (`unified-sighash-v0.8.1`)**, and the **Nexus 5 Linux 3.4 kernel headers/source**, the offline static CMake build requires:

| Dependency / Source Tree | Role in `kern-nexus` | Notes for Offline Static ARMv7 Build |
| :--- | :--- | :--- |
| **`lvgl` (v9.x)** | UI engine + built-in Linux `fbdev` (`LV_USE_LINUX_FBDEV`) & `evdev` (`LV_USE_EVDEV`) drivers. | Match exact v9 tag in `kern/idf_component.yml` or `kern/simulator/CMakeLists.txt`. |
| **`libwally-core` (v1.6.1+)** | Bitcoin crypto, BIP-32/39/85, PSBT signing, Miniscript (includes `secp256k1` submodule) + **XBT unified BLAKE2 sighash patch**. | Build statically (`--enable-static --disable-shared --enable-elements=no`), disable `HAVE_GETRANDOM` for Linux 3.4. |
| **Standalone C BLAKE2 (`blake2b` / `blake2s`)** | Cryptographic hash function for XBT unified signature hashes. | Single-file reference C implementation (`rfc7693` / `BLAKE2`) compiled into `libwally-core`. |
| **`quirc` / `k_quirc`** | SIMD-accelerated C QR code decoder. | Included in `odudex/kern/components` (or `github.com/dlbeer/quirc`). |
| **`qrcodegen` (Nayuki)** | C QR generator for displaying animated UR / BBQr / signed PSBTs. | Included in `odudex/kern/components` (or `github.com/nayuki/QR-Code-generator`). |
| **Managed ESP-IDF Components** | Any C helper libraries referenced in `kern/idf_component.yml` (`cUR`, `cJSON`, etc.). | Check `kern/components/` and `idf_component.yml`; vendor locally so CMake builds 100% offline. |
| **Linux 3.4 MSM kernel Headers** | Qualcomm UAPI headers (`msm_ion.h`, `msmb_camera.h`, `msmb_isp.h`, `bcm2079x.h`, `mdss_mdp.h`). | Extracted from your Nexus 5 Linux 3.4 kernel tree. |
| **ARMv7 HardFloat Sysroot** | `arm-linux-gnueabihf` GCC 15.2 static toolchain (`--enable-kernel=3.2.0`). | Host cross-compiler (`libc.a`, `libpthread.a`, `libm.a`). |

---

## 4. Phased Engineering Roadmap

* **Phase 1 — Offline Dependency Vendoring & Cross-Build Baseline**: Vendor all external components (`lvgl`, `libwally-core`, BLAKE2 C source, etc.), configure `nexus5-armhf.cmake` (`-march=armv7-a -mtune=cortex-a15 -mfpu=neon-vfpv4 -mfloat-abi=hard -static`), disable `HAVE_GETRANDOM` for Linux 3.4, and verify a static `3.2.0` ABI binary.
* **Phase 2 — Port XBT Unified Sighash (BLAKE2) to `libwally-core` & Verify Vectors**: Extract `git diff fff7ffa..de0e363` from `eugenesan/embit`, implement the BLAKE2 unified opt-in digest in `libwally-core`, and run an automated C unit-test suite verifying 100% byte-for-byte parity with `embit`'s test vectors.
* **Phase 3 — Display (`fbdev`) & Input (`evdev`) Bring-Up**: Replace SDL2 in `kern/simulator/` with LVGL `LV_USE_LINUX_FBDEV` (`/dev/graphics/fb0` + `FBIOPAN_DISPLAY` ioctl from `krux-nexus`) and `LV_USE_EVDEV` (Synaptics DSX touch + GPIO keys auto-detected via `EVIOCGNAME`), and tune DPI scaling for `1080x1920`.
* **Phase 4 — Camera Service Thread Integration**: Link `krux-nexus`'s reverse-engineered Qualcomm MSM camera C code (`/dev/video*`, `/dev/ion`) directly into `kern-nexus` as an internal `pthread` controlled by `camera_start()`/`camera_stop()`, feeding 8-bit Y-plane frames to `k_quirc` and LVGL's preview canvas.
* **Phase 5 — NFC SecureCard Driver, Stateless Hardening & `initramfs` Packaging**:
  * Link `krux-nexus`'s Broadcom `/dev/bcm2079x` C driver and wire ISO-7816 APDU flows for encrypted seed loading (Seedkeeper / KEF-over-NFC) and on-card sighash signing (Satochip / TapSigner).
  * Route NVS to `/tmp/kern_nvs` (`tmpfs`), hide SD/file UI menus, enforce `explicit_bzero()` memory wiping, wire sysfs backlight/battery/poweroff, strip binary, and pack into recovery `initramfs`.

---

## 5. Prompt for Local Coding Agent

Copy and paste the prompt below into your local coding agent (Claude Code, Cursor, Aider, or Cline) inside your workspace:

```markdown
# Task: Port `odudex/kern` to LG Nexus 5 (`kern-nexus`) with XBT Unified Sighash (BLAKE2) Support

## Workspace Layout
You are working in a workspace containing:
1. `./kern/` — Upstream `odudex/kern` (pure C + LVGL v9 + `libwally-core` + `k_quirc` + `cUR` air-gapped Bitcoin signer).
2. `../krux-nexus/` — Working reference port of Krux for the LG Nexus 5 (`hammerhead`, MSM8974, Linux 3.4 Android recovery kernel without Android userspace).
3. `./embit/` — Fork `eugenesan/embit` checked out on branch `unified-sighash-v0.8.1` (commits `fff7ffa..de0e363`), implementing the unified BLAKE2 opt-in signature hash for XBT.
4. `./kernel-headers/` — Linux 3.4 UAPI and Qualcomm MSM kernel headers (`msm_ion.h`, `msmb_camera.h`, `msmb_isp.h`, `bcm2079x.h`).

## Architectural Requirements (Strict)
1. **Single Static Executable**: Build a single static binary (`kern-nexus`) using `arm-linux-gnueabihf-gcc` with flags:
   `-march=armv7-a -mtune=cortex-a15 -mfpu=neon-vfpv4 -mfloat-abi=hard -O2 -D_DEFAULT_SOURCE -static -Wl,--gc-sections`
   targeting the Linux `3.2.0` ABI. No Python, no SDL2, no dynamic `.so` dependencies.
2. **Linux 3.4 Compatibility**: Ensure `HAVE_GETRANDOM` is disabled in `libwally-core` and all dependencies so entropy reads directly from `/dev/urandom` without calling the `getrandom()` syscall (unsupported on Linux 3.4).
3. **Port XBT Unified Opt-In Sighash (BLAKE2) to `libwally-core` & kern**:
   - Inspect `git diff fff7ffa..de0e363` in `./embit/` (`unified-sighash-v0.8.1`) to extract the exact BLAKE2 unified opt-in signature hash specification, negotiation rules, and unit tests.
   - Add a standalone C BLAKE2 implementation and port the unified sighash logic into `libwally-core` (`src/psbt.c`, `src/transaction.c`) preserving all invariants from `./embit/`:
     * Derive committed script type strictly from the script itself (`de0e363`), never from which PSBT UTXO fields are populated.
     * Keep caller-specific input values out of any shared sibling digest cache (`dc59267`) and maintain constant PSBT memory overhead (`7e4b9c1`).
     * Reject unsupported hash types per-input (`bfa8545`) and confine `SIGHASH_SINGLE` skip behavior strictly to the unified opt-in (`67a79a0`).
   - Build and run a C unit-test binary (`test_xbt_sighash`) against all test vectors from `./embit/` (`1bd4b0a`, `2b1828c`) to verify byte-for-byte digest parity.
   - Update `./kern/`'s PSBT inspection/signing UI to support and display the XBT unified opt-in sighash flag.
4. **Stateless / Air-Gapped Operation**:
   - Zero network stack or wireless interfaces.
   - Zero persistent eMMC storage: map kern's simulator NVS/storage shim to `/tmp/kern_nvs` (`tmpfs`) and remove/hide SD-card and file-browser UI options.
   - Wipe all sensitive buffers (mnemonics, PINs, APDU session keys, decrypted PSBTs) using `explicit_bzero()`.
5. **Display (`fbdev`) & Touch (`evdev`)**:
   - Replace SDL2 in `kern/simulator/` with LVGL v9's built-in `LV_USE_LINUX_FBDEV` and `LV_USE_EVDEV` drivers (`LV_COLOR_DEPTH 32`).
   - Open `/dev/graphics/fb0` (`1080x1920`) and port the `FBIOPAN_DISPLAY` ioctl refresh mechanism from `../krux-nexus/` into LVGL's display flush path so Qualcomm `mdss_fb` updates reliably.
   - Auto-detect the Synaptics DSX touchscreen and GPIO key `/dev/input/event*` nodes by device name via `EVIOCGNAME`.
   - Adjust DPI/font scaling constants so kern's LVGL UI renders cleanly on a 5-inch `1080x1920` display.
6. **Integrated Camera Service Thread**:
   - Port the reverse-engineered Qualcomm MSM camera C driver (`/dev/video*`, `/dev/media*`, `/dev/v4l-subdev*`, `/dev/ion`) from `../krux-nexus/` directly into `kern-nexus` as an internal `pthread` (not a separate daemon).
   - Start/stop the sensor thread cleanly from kern's `camera_start()` / `camera_stop()` hooks and feed 8-bit Y-plane luminance frames directly into kern's `k_quirc` QR decoder and LVGL preview canvas.
7. **Integrated NFC SecureCard Driver (`bcm2079x`)**:
   - Port the Broadcom BCM2079x (`/dev/bcm2079x`) C NCI / ISO-7816 driver from `../krux-nexus/` directly into `kern-nexus`.
   - Expose a C APDU transceive API and wire it into kern's Bitcoin workflows for both encrypted seed loading (Seedkeeper / KEF-over-NFC) and on-card sighash signing (Satochip / TapSigner, passing the unified XBT sighash digest when active).
8. **System Controls**:
   - Wire backlight brightness to `/sys/class/leds/lcd-backlight/brightness`, battery gauge to `/sys/class/power_supply/battery/capacity`, and clean power-off to `reboot(LINUX_REBOOT_CMD_POWER_OFF)`.

## Step-by-Step Execution Plan
- **Step 1**: Audit `./kern/simulator/CMakeLists.txt`, `./kern/components/`, `../krux-nexus/`, and run `git diff fff7ffa..de0e363` in `./embit/` to document the exact XBT BLAKE2 unified sighash specification and hardware file mappings. Generate the `ports/nexus5/` CMake scaffold and `nexus5-armhf.cmake` toolchain file.
- **Step 2**: Port the XBT BLAKE2 unified sighash into `libwally-core` and validate against all `./embit/` unit test vectors via `test_xbt_sighash`.
- **Step 3**: Implement the `fbdev` (`FBIOPAN_DISPLAY`) + `evdev` display/touch HAL and verify a clean static cross-compile.
- **Step 4**: Integrate the `../krux-nexus/` C camera service as a `pthread` wired to `k_quirc` and the LVGL viewfinder.
- **Step 5**: Integrate the `bcm2079x` NFC APDU driver, wire kern's XBT sighash PSBT UI/signing flow, and connect sysfs battery/backlight/poweroff hooks.

Begin with **Step 1**: inspect all repositories, summarize the exact `git diff fff7ffa..de0e363` changes from `./embit/`, report the hardware file mappings, and scaffold `ports/nexus5/`.
```
