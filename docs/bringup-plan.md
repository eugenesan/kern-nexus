# Bring-up plan and acceptance tests

**Bootstrap status (2026-11-14):** the standalone unified-sighash reference
passes all **166** embit vectors (`make -C tools/sighash`); the boot-chain
bootstrap image runs; the real static ARM kern binary builds and draws on
the Nexus 5 panel; touch works; and camd publishes 960×1280 frames through
the bridge at 10 fps.  QR decode with an external code, NFC/SeedKeeper and
the XBT block rules are next.

The order matters: each phase has an observable exit criterion.  Do not
start the unified sighash on hardware before the C vectors pass; do not
debug the camera before the panel and input work.

## Phase 0 — repository and build baseline

**Goal:** `ports/nexus5` configures and links a static ARM binary.

Tasks:

1. Pin LVGL 9.5 and mbedTLS sources locally (no network).
2. Add `ports/nexus5/toolchain-armhf.cmake`, `lv_conf.h`, and the CMake
   project.
3. Reuse `kern/simulator/platform/esp_idf_stubs` for FreeRTOS/NVS/ESP
   shims; copy only what compiles.
4. Add platform shims:

   | interface | file |
   |---|---|
   | `bsp_display_*`, `lvgl_port_*` | `platform/bsp_display_fbdev.c` |
   | `bsp_i2c_*`, `bsp_pmic_*` | `platform/bsp_power.c` |
   | `app_video_*` | `platform/app_video_camera.c` |
   | `esp_*`, FreeRTOS, NVS | `platform/esp_idf_stubs/` |
   | storage/SD | `platform/storage_data.c` |

5. Build:

   ```sh
   bash scripts/build-kern.sh
   file artifacts/built/kern-nexus
   ```

**Exit:** ELF 32-bit ARM, statically linked; no `NEEDED`; no SDL2; no
ESP-IDF.

## Phase 1 — display and input

1. Implement the LVGL fbdev display on `/dev/graphics/fb0`, 1080×1920,
   stride 4352, `MSMFB_DISPLAY_COMMIT` + `FBIOPAN_DISPLAY`.
2. Implement evdev touch (`EVIOCGNAME` detection, `ABS_MT_*`) and keys.
3. Set the portrait layout/font policy for 1080×1920.
4. Scrub the framebuffer on exit (black, commit, blank).

**Exit:** kern's splash and PIN/login screen draw at the right size; touch
and volume/power keys work; no stale TWRP image after a warm reset.

## Phase 2 — camera and QR

1. Copy `../krux-nexus/cam/camd.c` and headers/firmware into
   `components/camera/`.
2. Refactor `main()` into `kern_camera_init/start/stop`; remove the socket
   and the supervisor process.
3. Make the frame path deliver the same YUV420 geometry Krux uses
   (binned 720×960 default; 960×1280 full/PIX) to
   `app_video_frame_operation_cb_t`, and add a `KERN_NEXUS` path in
   `scanner.c` that crops/scales Y on the CPU instead of the P4 PPA.
4. Implement the `app_video_*` functions and a software
   `app_video_ppa_snap_crop` if needed.
5. Bundle `cpp_firmware_v1_2_0.fw` next to the binary; stage it to
   `/lib/firmware`; never self-copy/truncate.

**Exit:**

```sh
dmesg | grep -E 'CPP FW Version|Invalid frame|msm_cpp'
# CPP FW Version: 0x10020003, no Invalid frame
```

kern's scan page shows a preview and decodes a QR; the entropy page
captures frames.  No stripes; stream restarts only at the menu.

## Phase 3 — unified sighash in libwally

1. Add `wally_tx_get_unified_signature_hash()` and the PSBT wrapper to the
   libwally fork.
2. Derive the script type from scripts, not PSBT fields.
3. Implement the cache/validation invariants from `docs/bitcoin.md`.
4. Port the embit vectors and negotiation tests into a C test binary.
5. Wire kern's PSBT review to display `Unified 0x21` / `Standard 0x01`.

**Exit:** byte-for-byte digest parity for every vector; standard PSBT tests
unchanged; a `0x21` PSBT signs and verifies on the host.

## Phase 4 — NFC transport and SeedKeeper

1. Port the BCM2079x ioctl transport to `components/nfc`.
2. Port the NCI bring-up from `krux/src/nfc.py` with the same constants
   and timeouts; implement ISO-DEP APDU transceive.
3. Test with no card (chip answers), then a tag, then a card.
4. Port SeedKeeper plaintext flows (`seedkeeper.py`): SELECT, PIN,
   status, headers, import/export, chunking.
5. Optional secure channel (`seedkeeper_sc.py`): secp256k1 ECDH,
   HMAC-SHA1, AES-128-CBC, card signature recovery.
6. Wire seed loading and PSBT signing on-card (Satochip/TapSigner) through
   the unified digest when `0x20` is active.

**Exit:** status/PIN/import/export round-trip against a real SeedKeeper;
wrong PIN/blocked/no-applet paths produce the documented status words;
reconnect mid-walk is safe.

## Phase 5 — storage, power and wipe

1. Map NVS/SPIFFS/SD shims to `/data/kern/nvs`, `/data/kern/flash`,
   `/data/kern/sd` as Krux does; hide SD/file menus.
2. Implement `explicit_bzero()` on every secret/working buffer.
3. Backlight/battery/power sysfs; display scrub.
4. Wire kern's shutdown to `reboot(LINUX_REBOOT_CMD_POWER_OFF)`.
5. Verify the kernel marker wipe on the serial console.

**Exit:** settings/PIN persist under `/data/kern`; no seeds/plaintext
secrets are written; a power-off wipes ~1488 MB of free RAM and powers off;
the next boot does not re-wipe.

## Phase 6 — TWRP image, modes and seal

1. Build the init stub for kern (start kern/TWRP, no Python/camd).
2. Pack the TWRP ramdisk with:

   * `/sbin/recovery.stock` and the `/sbin/recovery` symlink;
   * `/sbin/kern-init`, `/sbin/start-kern`, `/sbin/start-adb`,
     `/sbin/start-serial`;
   * `/system/bin/sh -> /sbin/sh`;
   * `/opt/kern/kern-nexus`, camera firmware;
   * sealed `adbd.off`.

3. Standard image: TWRP by default, Vol+ / `start-kern` runs kern.
   Development image: kern by default, Vol+ TWRP.
4. Seal: hide adbd in every path; `start-adb` restores.
5. `start-kern` stops TWRP and waits for `recovery` to disappear, then
   starts kern.
6. Sideload test: Advanced → ADB Sideload with adbd off.

**Exit:** TWRP boots with adb off; Vol+ and `start-kern` start kern only
(no parallel TWRP); `start-adb` and `start-serial` work; sideload works.

## Phase 7 — release gates

* `tools/check-airgap.py` PASS on the kernel `.config` and ramdisk:
  no WLAN/BT/cellular/FM, NFC only.
* No `wpa_supplicant`, `hostapd`, `wificond`, Broadcom Wi-Fi/BT firmware,
  Bluetooth HCI tools.
* No SD/file/update UI in the delivered image.
* No `/data` or eMMC writes by the firmware.
* Static binary, no `NEEDED`, stripped; `explicit_bzero` in the secret
  paths.
* Image size < 23,068,672 bytes; gzip; `zImage-dtb`.
* Power-off trace: `wipe: marking RAM` → `restart reason = recovery` →
  `wipe: N MB wiped` → `wipe: powering off`.

## Tests to bring over or write

| test | source/notes |
|---|---|
| libwally standard PSBT | upstream `src/test/test_psbt.py` and C tests |
| unified sighash vectors | `krux/vendor/embit/tests/tests/data/unified_sighash.json` |
| negotiation tests | `test_unified_sighash_negotiation.py` |
| kern core tests | `kern/main/core/test`, `just test` on host |
| QR decode | `kern/main/qr/test`, `k_quirc` tests |
| camera smoke | `dmesg` CPP version + frame-done; QR scan |
| NFC | status/PIN/import/export; wrong PIN/blocked; no applet |
| display | 1080×1920, stride, pan/commit, touch/keys |
| wipe | serial trace + power off |
| air gap | `tools/check-airgap.py` |

## Open decisions (also in the blueprint)

1. `k_run`/`k_next` semantics: phases or slots?
2. PIN/settings persistence: **resolved** — use `/data/kern` as Krux.
3. mbedTLS and LVGL offline vendoring; pin/tag them.
4. libwally pin/tag for the unified patch.
5. Camera geometry: Krux 720×960 / 960×1280 is kept; only crop/scale if a
   hard scanner assert appears.
6. Compile out or just hide SD/file/update menus?
7. USB Ethernet (`CONFIG_USB_RTL8150`) in or out of the threat model?
8. Any binary signature/verification at all?  The ESP secure boot does not
   apply to this target.
