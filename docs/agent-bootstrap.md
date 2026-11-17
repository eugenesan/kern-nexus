# Bootstrap prompt for a new session

Copy the block below into the first message of a new session, then read
`docs/status.md` before touching code.  It is deliberately short; the
details live in the documents it points to.

---

Work in `/home/user/Develop/src/kern-nexus` (outer repo).  `kern/` is a
nested git checkout (our fork of kern) that the outer repo pins; deploy,
kernel, ramdisk, images and docs live outside it.

**Read first**

1. `docs/status.md` — what works today, known gaps, decisions, next tasks.
2. `docs/kern-nexus-blueprint.md` — architecture and boot/delivery model.
3. The detail document for the area: `build-and-deploy.md`,
   `hardware.md`, `bitcoin.md`, `kernel-and-wipe.md`, `bringup-plan.md`.

**Hard rules**

* LG Nexus 5 (`hammerhead`, MSM8974, ARMv7 hard-float, Linux 3.4), one
  static binary.  No Python/MicroPython, no network stack, Wi-Fi,
  Bluetooth or cellular.  NFC is the only wireless interface.
* Never flash `boot`.  Development is `fastboot boot …audit.img`;
  the delivered image is written to `recovery` (`mmcblk0p20`).
* `.sealed` images hide adbd; `start-adb` / `start-serial` are the
  escape hatches.
* Settings/PIN in `/data/kern`; never persist seeds or plaintext
  secrets.  Secret buffers must be scrubbed (see status §2).
* Build products go under `/tmp/kern-nexus` (`KERN_NEXUS_BUILD`), never
  into the checkout.
* Standard Bitcoin keeps SHA256; the XBT `0x20` opt-in is signer-scope
  only.  Do not invent XBT constants.
* `kern/` changes stay small and are committed in the kern fork; the
  outer repo records the new submodule pointer.
* Copy from `../krux-nexus` only what is needed, and record source commit
  and deviations in `components/<name>/README.md`.

**Where things are**

* `ports/nexus5/` — Linux/ARM platform: BSP (fbdev/evdev/power), the
  camera bridge and AE/AF (`app_video_camera.c`), the main loop.
* `components/` — `camera` (camd.c), `nfc`, `seedkeeper`, unused `xbt`.
* `kern/` — upstream C app (LVGL, libwally-core, cUR, k_quirc) plus our
  small patches: `main/qr/scanner.c`, `main/core/unified_sighash.c`,
  `main/pages/seedkeeper/*`, `main/pages/capture_entropy.c`, settings,
  `components/video/video.h`.
* `rootfs/`, `scripts/`, `tools/`, `linux/` — boot, build, host tools.
* `components/camera/re/` — **git-ignored local archive** of camera RE
  history, captures, logs and tools (start with its `README.md` and
  `STREAM-RESTART.md`).  Not part of the build; read it before changing
  the camera stream lifecycle.

**Device and test loop**

* serial/device id `0643efdcf0eca63a`; the device is often in fastboot.
* `fastboot boot …release-audit.img` → wait ~20 s → `adb shell
  /sbin/start-kern` (standard image boots TWRP by default).
* Deploy a rebuilt binary without reflashing:
  push to `/opt/kern`, kill `kern-nexus` and `camd`, remove
  `/data/kern/camd*.sock`, `/sbin/start-kern`.
* Touch injection over adb (`ktap` into `/dev/input/event1`) is
  unreliable; ask the operator to verify UI flows.  The camera and NFC
  can be exercised with the control sockets and with a real card.
* The device is battery-sensitive: kern+camd keep the CPU/ISP busy.

**First tasks** (unless told otherwise): see `docs/status.md` §7 —
userspace wipe of port buffers, hide SD entries, then the optional
in-process camera, then scan performance and the sealed release.

**Before any large change**: run `scripts/test.sh` (it includes cUR via
`scripts/build-cur-tests.sh`) and build with `scripts/build-all.sh` so the
images stay reproducible.

---

## Why a new session

This project ran a long hardware bring-up.  The context needed to continue
is in these documents, not in the old transcript; the transcript contains
experiments and dead ends (deliberately kept out of the docs).

## Focused follow-up session

For the camera stream stop/restart experiment, use
[preface-camera-stream-experiment.md](preface-camera-stream-experiment.md)
instead of pasting this file: it repeats the rules here, adds device/root
and partition-safety rules, and defines the experiment as an isolated
side branch of the operational project.

## What a new session must not have to rediscover

* The camera quirks that cost the most time are written down in
  `docs/status.md` §3 and `docs/hardware.md` §5: seed the AE from camd's
  cached sensor state, ignore settling frames, never read sensor registers
  from the control thread, never serve a frame older than ~300 ms, keep
  the 180 target.
* The SeedKeeper ordering and retry rules are in `docs/status.md` §3 and
  `docs/bitcoin.md`; the card page tears down its screen before the flow,
  hence `seedkeeper_page_set_cleanup`.
* The LVGL failure mode that freezes the UI silently is a NULL label
  (`while(1)`); see the same sections.
