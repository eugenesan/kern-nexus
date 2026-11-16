# Bring-up plan and acceptance tests

**Status (2026-11-15):** everything through the SeedKeeper secure channel
runs on hardware, and the unified sighash is wired into PSBT signing
(166 vectors pass).  The camera is an external `camd` process with the
AE/AF controller in the bridge; an in-process refactor is optional.  The
open work is the userspace wipe of port buffers, hiding the SD/file
entries, the sealed flash and wipe verification, and XBT constants.
See [status.md](status.md) for the short version and the per-phase notes
below for what is done.

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
   file /tmp/kern-nexus/artifacts/build/kern-nexus
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

## Phase 2 — camera and QR  — **done (2026-11-15)**

The camera is still a separate `camd` process, not a `kern` pthread; the
bridge exposes the same `app_video_*` contract.  In-process is an optional
future task (see `docs/status.md` §5).

1. Copy `../krux-nexus/cam/camd.c` and headers/firmware into
   `components/camera/`.  **Done.**
2. Refactor `main()` into `kern_camera_init/start/stop`; remove the socket
   and the supervisor process.  **Not done** (optional; the daemon works).
3. Frame path: Y8 960×1280 from the sensor, CPU crop/scale of the Y plane
   in `scanner.c` (no PPA), 1:1 crop and L8 image on the entropy page.
   **Done.**  The AE/AF controller, tap-to-focus, the −/+ exposure strip,
   the settings sliders, the fresh-frame gate and the sensor-seeded
   exposure are in `ports/nexus5/platform/app_video_camera.c`.
4. `app_video_*` functions implemented, including the ae target/focus
   controls and the crop helper.  **Done.**
5. `cpp_firmware_v1_2_0.fw` bundled in `/opt/kern` and staged to
   `/lib/firmware`.  **Done.**

**Exit:**

```sh
dmesg | grep -E 'CPP FW Version|Invalid frame|msm_cpp'
# CPP FW Version: 0x10020003, no Invalid frame
```

kern's scan page shows a preview and decodes a QR; the entropy page
captures frames.  No stripes; stream restarts only at the menu.  Verified
on hardware, including a real QR and the entropy capture path.

## Phase 3 — unified sighash in kern  — **done (2026-11-15)**

1. Keep the digest in `kern/main/core/unified_sighash.c` and the 166-vector
   test in `tools/sighash`; do not fork libwally.
2. Sign ECDSA and taproot (key path and script path) with libwally's EC
   primitives, then insert the partial signatures into the PSBT maps.
3. Accept `UNIFIED|ALL` (`0x21`) in the gate and label it on the review
   screen; refuse mixed/partial requests.
4. Require an amount for every input, because the digest commits to all of
   them.

**Exit:** all 166 digest vectors and the P2WPKH/BIP86 signing tests pass;
standard PSBT tests are unchanged.  Verified; the only remaining item is
the XBT activation/network constants, which are blocked.


## Phase 4 — NFC transport and SeedKeeper  — **done (2026-11-15)**

1. Port the BCM2079x ioctl transport to `components/nfc`.
2. Port the NCI bring-up from `krux/src/nfc.py` with the same constants
   and timeouts; implement ISO-DEP APDU transceive.
3. Test with no card (chip answers), then a tag, then a card.
4. Port SeedKeeper plaintext flows (`seedkeeper.py`): SELECT, PIN,
   status, headers, import/export, chunking.  **Done** in
   `components/seedkeeper` with 118 mock-APDU checks.
5. Optional secure channel (`seedkeeper_sc.py`): secp256k1 ECDH,
   HMAC-SHA1, AES-128-CBC, card signature recovery.
6. Wire the flows: Load Mnemonic → From SeedKeeper, plus Settings →
   SeedKeeper (new seed, backup, list, properties, delete, card info,
   free space).  **Done and hardware-tested** (secure channel included).
   The PIN is asked first; the backup collects label/passphrase and then
   asks for the card; every write attempt re-selects and re-logs in;
   `6E00`/`6D00` are treated as a lost session.

**Exit:** status/PIN/import/export round-trip against a real SeedKeeper;
wrong PIN/blocked/no-applet paths produce the documented status words;
reconnect mid-walk is safe.  Round-trip verified; the wrong-PIN/blocked
matrix still needs a written test pass.

## Phase 5 — storage, power and wipe  — **mostly done**

1. Map NVS/SPIFFS/SD shims to `/data/kern/nvs`, `/data/kern/flash`,
   `/data/kern/sd` as Krux does; hide SD/file menus.
2. Implement `explicit_bzero()` on every secret/working buffer.
3. Backlight/battery/power sysfs; display scrub.
4. Wire kern's shutdown to `reboot(LINUX_REBOOT_CMD_POWER_OFF)`.
5. Verify the kernel marker wipe on the serial console.

**Exit:** settings/PIN persist under `/data/kern`; no seeds/plaintext
secrets are written; a power-off wipes ~1488 MB of free RAM and powers off;
the next boot does not re-wipe.

Status: settings/PIN and the kernel wipe are done; **userspace scrubbing of
the port buffers (NFC APDUs, SeedKeeper keys/PIN, camera frames,
framebuffer) is not** — the kernel does not cover reserved memory.  The
power-off trace still needs a serial-console pass.

## Phase 6 — TWRP image, modes and seal  — **done, final flash pending**

1. Build the init stub for kern (start kern/TWRP, no Python/camd).
2. Pack the TWRP ramdisk with:

   * `/sbin/recovery.stock` and the `/sbin/recovery` symlink;
   * `/sbin/kern-init`, `/sbin/start-kern`, `/sbin/start-adb`,
     `/sbin/start-serial`;
   * `/system/bin/sh -> /sbin/sh`;
   * `/opt/kern/kern-nexus`, `/opt/kern/camd`, camera firmware;
   * sealed `adbd.off`.

3. Standard image: TWRP by default, Vol+ / `start-kern` runs kern.
   Development image: kern by default, Vol+ TWRP.
4. Seal: hide adbd in every path; `start-adb` restores.
5. `start-kern` stops TWRP and waits for `recovery` to disappear, then
   starts kern.
6. Sideload test: Advanced → ADB Sideload with adbd off.

**Exit:** TWRP boots with adb off; Vol+ and `start-kern` start kern only
(no parallel TWRP); `start-adb` and `start-serial` work; sideload works.
Images build and pass the checks; the sealed flash to `recovery` and the
wipe verification are still pending.

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

Status: the air-gap and image/static checks pass.  Still open: hide the
SD/file menu entries, scrub the port buffers in userspace, and verify the
power-off trace on the serial console with the sealed image.

## Phase 8 — camera in-process (optional/future)

Documented in `docs/status.md` §5 and the blueprint §6.  Same exit as
Phase 2, with the daemon removed: one binary, no sockets, no per-frame
copy; a restart path for the pipeline inside the process.

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
5. Camera geometry: **resolved** — Y8 960×1280, CPU crop/scale (no PPA),
   1:1 crop on the entropy page.  In-process camera is optional/future.
6. Compile out or just hide SD/file/update menus?
7. USB Ethernet (`CONFIG_USB_RTL8150`) in or out of the threat model?
8. Any binary signature/verification at all?  The ESP secure boot does not
   apply to this target.
