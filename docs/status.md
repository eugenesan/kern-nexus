# kern-nexus — state of the port

**Date:** 2026-11-15.  This is the short, current picture; the blueprint is
the design, this document is what is true today.

| | |
|---|---|
| outer tree | `kern-nexus` @ the commit that added this file |
| kern subtree | see `git -C kern log --oneline -1` (pinned by the outer commit) |
| dev image | `/tmp/kern-nexus/artifacts/build/boot-kern-standard-release-audit.img` |
| release image | `/tmp/kern-nexus/artifacts/build/boot-kern-standard-release.img` |
| device | `0643efdcf0eca63a`, TWRP recovery, `fastboot boot` for dev images |
| build output | `/tmp/kern-nexus` (`KERN_NEXUS_BUILD`); nothing is written into the checkout |

Nothing has been pushed from this machine; push both repositories from a
machine that has the remotes.

## 1. What works

| area | state |
|---|---|
| boot/image | TWRP ramdisk + kern, standard/dev modes, `start-kern`, seal hides adb; `check-airgap` and `security-check` pass |
| kern UI | display (fbdev + MSM commit), touch, keys, PIN/settings in `/data/kern` |
| camera | Y8 960×1280 frames; AE, AF, tap-to-focus, −/+ exposure, exposure/focus sliders; entropy preview with its own −/Focus/+ controls and 1:1 crop |
| QR | `k_quirc` on the Y plane, multi-part (UR/BBQr), auto-refocus after 15 silent frames, fresh-frame gate |
| NFC | BCM2079x NCI/ISO-DEP/APDU; SeedKeeper applet v0.1 with secure channel |
| SeedKeeper | load, new seed from card, backup to card, list, per-secret properties, delete, card info, free space; PIN first, card asked for when the write is ready |
| unified sighash | digest in `kern/main/core`, PSBT signing (ECDSA + taproot key/script path), `UNIFIED\|ALL` gate + label, 166 vectors |
| host tests | `scripts/test.sh` (sighash, SeedKeeper incl. applet-emulator flows, cUR out-of-tree, QR/k_quirc and kern suites) |

## 2. Known limitations and rough edges

* **SD/file UI is still reachable**: `Public Key → Save to SD Card` and
  descriptor storage `Save to SD Card` exist even though the SD shim is
  unavailable.  The blueprint says hide/disable; not done.
* **No userspace wipe in the port components**: there is no
  `secure_memzero()` in `components/nfc`, `components/seedkeeper`,
  `components/camera` or `ports/`.  PINs, SeedKeeper session keys, ECDH
  material and camera frames are not scrubbed before power-off.  The
  kernel wipe does not cover reserved memory (framebuffer, ION), so this
  is a real release gate.
* **camd is a separate process** (`/opt/kern/camd`, spawned at boot,
  unix sockets + shared memory).  See §5 for the integration task.
* **Scan throughput is decode-bound**: 4 fps in a dark room, under 1 fps
  on a dense code.  AF sweeps are therefore slow; the scanner's design
  (tap = full sweep, cached sweep on the second request) absorbs it, but a
  performance pass is still open.
* **First-preview AE** was fixed by seeding from the sensor's cached
  exposure and ignoring the settling frames; the settings default is now
  180.  If a device has an old stored `ae_tgt` (80), the picture is dark
  until the Exposure slider is moved.
* **Development switch** `/data/kern/dev_no_sleep` exists in the tree
  (`settings.c`) to keep the device awake while testing; it must not be
  present in a delivered `/data`.
* **Docs** were partly stale (this file and `agent-bootstrap.md` address
  that; the blueprint/bringup/bitcoin/build docs were updated with it).

## 3. Design decisions that are easy to get wrong

* **AE/AF live in the bridge** (`ports/nexus5/platform/app_video_camera.c`),
  not in camd: Krux's controller ported to C, with a settle window after
  each change, a two-frame same-side gate, locking, and a manual hold of
  ~5 s.  camd only applies exposure/gain/luma and reports the sweep flag.
* **AE is seeded from the sensor, not a default**: camd caches the
  exposure/gain it last applied (read from the registers once at startup,
  before streaming) and answers `q` on the control socket.  Do not read
  camera registers from the control thread while streaming — the CCI bus
  is shared and the writes start failing.
* **The first frames are ignored** (3) because the sensor is still
  settling; the AE target default is 180.
* **A new frame client never gets a stale frame**: camd serves a frame
  only if it was published in the last 300 ms, otherwise the page waits
  for the next capture.  This fixed both a false QR detection and the
  start-of-preview brightness dip.
* **SeedKeeper card order**: PIN → (label/passphrase) → "tap the card" →
  write.  Every write attempt — including a retry — re-selects the applet
  and re-logs in.  `6E00`/`6D00` mean "the applet is not selected" and are
  treated as a lost session wherever a retry is allowed.
* **The card page owns a cleanup hook** (`seedkeeper_page_set_cleanup`)
  that runs before its screen is deleted; a flow's widgets must be
  destroyed there, not after (`lv_label_get_text(NULL)` is a `while(1)`).
* **Camera entropy page takes Y8**: 1:1 central crop, flipped 180°, L8
  image, 256-bin entropy estimate, digest over one byte per pixel.
* **Display commits on `LV_EVENT_RENDER_READY`, page 0** (`yoffset = 0`);
  trusting the live yoffset can show the empty second page.

## 4. Verification recipes

```sh
# host tests (no device)
bash scripts/test.sh                      # includes cUR's suite
bash scripts/build-cur-tests.sh           # cUR alone, built from a copy in /tmp

# full build (kernel, mbedTLS, zlib, kern, camd, images)
bash scripts/build-all.sh

# dev boot without writing a partition
fastboot boot /tmp/kern-nexus/artifacts/build/boot-kern-standard-release-audit.img
# then, from TWRP's adb:
adb shell /sbin/start-kern

# deploy a new kern/camd while testing
adb push /tmp/kern-nexus/artifacts/build/kern-nexus /opt/kern/kern-nexus
adb push /tmp/kern-nexus/artifacts/build/camd /opt/kern/camd
adb shell 'chmod 755 /opt/kern/kern-nexus /opt/kern/camd;
           kill -9 $(pidof kern-nexus); kill -9 $(pidof camd);
           rm -f /data/kern/camd.sock /data/kern/camd-ctl.sock;
           /sbin/start-kern'
```

Camera control socket (`/data/kern/camd-ctl.sock`, mode 0600) commands:
`E` exposure+gain (4 bytes), `G` digital luma gain (1), `V` VMAX (2),
`F` focus step (1), `A` release manual focus, `R` full refocus,
`r` cached refocus, `q` snapshot (returns exposure, gain, luma).

## 5. Optional/future: camd inside the kern binary

The blueprint's original shape.  Today `camd` is a static binary spawned
by `app_video_init_once`; the bridge reads frames from
`/data/kern/camd.sock` (one ~1.2 MB Y8 copy per frame) and controls it over
`camd-ctl.sock`.

**Shape of the change**

1. Split `camd.c:main()` into `camera_init/dev_start/dev_stop`: sensor
   power-up and mode table, ISP/CPP client, actuator, frame thread.
2. Replace the socket frame service with an internal publish; consume the
   frame directly (ideally zero-copy from `latest_y` into the scanner's
   frame pool).
3. Replace the control socket with direct calls (`set_exposure_gain`,
   `set_focus`, `refocus`, `snapshot`) and move the AE/AF controller into
   the same module (or keep it in the bridge behind the same functions).
4. Drop `/opt/kern/camd` from the ramdisk and the spawn/socket code; keep
   an internal "restart the pipeline" path.
5. Keep the external daemon behind a build flag until the in-process path
   has been validated on hardware.

**Risks**

* Fault isolation is lost: a camera/ISP fault currently kills a daemon,
  not the signer.
* Restarting the pipeline in-process is fragile (STREAMOFF/STREAMON can
  leave ISP write masters held); without the supervisor a wedged pipeline
  means restarting kern.
* Blocking ioctls (CPP/PPA/CCI) must stay on the frame thread; a stray
  call on the UI thread stalls the screen.
* Losing the standalone binary's logs and socket tools makes debugging
  harder; add an in-process log ring first.
* Frame back-pressure: the app consumes slower than the sensor produces;
  an internal queue must drop frames deliberately.

**Gains**: one binary to build/sign/deploy, no socket permissions, no
per-frame copy, no orphaned daemon, and a cleaner base for scan
performance work.

## 6. Repository layout decisions (2026-11-15)

* `components/` stays the home of the target components (`camera`, `nfc`,
  `seedkeeper`, unused `xbt`).  They are Linux/ARM-specific in this tree;
  if they are ever moved into `kern/`, guard them per target and do it as
  one deliberate move.
* `ports/` stays outside `kern/`: it is the Linux/ARM build and platform
  layer; folding it into kern's ESP-IDF build is possible (the simulator
  is precedent) but not worth the merge friction while the camera is still
  being integrated.
* `tools/sighash` is a host test harness, not a firmware component, so it
  stays under `tools/`.  Its unused duplicate `unified_sighash.c`/
  `sha256.c` copies were removed; the test compiles
  `kern/main/core/unified_sighash.c` so it validates the shipped code.
* `rootfs/stub` is boot/ramdisk glue, not a kern component; it stays under
  `rootfs/`.  The `kern-init` binary is now built into
  `$KERN_NEXUS_BUILD`, not `rootfs/stub/`.
* cUR is an upstream submodule and is left unmodified.  Its host test
  suite is built from a copy under `$KERN_NEXUS_BUILD/cur` by
  `scripts/build-cur-tests.sh`, so `src/obj` and `src/libur.a` are never
  written into the checkout.  The firmware build compiles cUR's sources
  through CMake and does not use that Makefile.

## 7. Next tasks, in order

1. **Userspace wipe of port buffers** (NFC APDUs, SeedKeeper keys/PIN,
   camera frames, framebuffer) — a release gate.
2. **Hide the SD/file entries** in Public Key export and descriptor
   storage.
3. **camd in-process** (§5), if the deployment/simplification is wanted.
4. **Scan performance**: the decoder dominates; consider decoding fewer
   frames per second on purpose and reusing the preview path.
5. **Sealed release**: flash the release image to `recovery`, verify the
   power-off wipe trace on serial, confirm no `dev_no_sleep` in `/data`.
6. **Boot-chain bootstrap images**: device verification (built, untested).
7. **Duplicate-secret window**: a backup whose response is lost can store
   a second copy on the card; decide whether to detect/merge by label.
8. **XBT**: activation height/network constants (blocked; do not invent).
9. **Push** both repositories.
