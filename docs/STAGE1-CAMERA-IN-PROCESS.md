# Stage 1 — camd linked into the kern binary (2026-11-17)

This documents the first functional stage of folding `camd` into kern.
The external daemon remains the shipped default; Stage 1 is behind a
build flag.

## Tree state

| item | value |
|---|---|
| outer repo branch | `master` |
| Stage 1 commit | `70d5b03` ("camera: Stage 1 - link camd into kern behind KERN_CAMERA_IN_PROCESS") |
| idle quiesce merge | `aaa93c5` (merged `camera-inproc-quiesce`: `91dec87`, `97ecf4c`, `e5d4459`, `661f778`) |
| prerequisites | `a3bee97` (camd idle CPU), `b2e2795` (CPP timeout patch), `3fcbd64` (Stage 0 seam), `1292199` (kernel branch pin), `d9d224c` (safe-point docs) |
| kernel submodule branch | `airgap/cam-xz-wipe-kern` |
| kernel submodule commit | `3e3f2ace562` |
| safe snapshot (pre-integration) | branch `stable-pre-camd-integration`, tag `pre-camd-integration-2026-11-17`, commit `1292199` |
| rollback | see [SAFE-POINTS.md](SAFE-POINTS.md) |

Experimental branches (not merged): `camera-stream-exp` (teardown/RE),
`camera-re-instr` (instrumented kernel with `[REI]` printks),
`camera-inproc-quiesce` (idle quiesce; NOT safe, see below).

## Build flag

`ports/nexus5/CMakeLists.txt`:

```
-DKERN_CAMERA_IN_PROCESS=ON
```

Default **OFF**. With it ON:

* `components/camera/camd.c` is compiled into the `kern-nexus` target
  with `-Dmain=camd_main` and `-Dprintf=kern_camera_log`.
* `components/camera/kern_camera.c/.h` provide a bounded log ring
  (`KERN_CAMERA_LOG_ENTRIES=128`, `KERN_CAMERA_LOG_LINE=160`) and
  `kern_camera_launch()`.
* `app_video_init_once()` in
  `ports/nexus5/platform/app_video_camera.c` launches the camera as a
  `fork()`+`execv("/proc/self/exe", "--camd")` child of the single binary
  instead of `fork()/execl()` of `/opt/kern/camd`. No separate daemon
  binary is used. `app_video_start()` respawns the child if it exited.
* `components/camera/isp.h` gained
  `struct msm_vfe_axi_stream_release_cmd`; the generated camera-config
  headers now share a `KERN_CAM_UPLOAD_DEFINED` guard so `camd.c` can be
  compiled in the same target.
* camd's `signal(SIGPIPE, SIG_IGN)` is compiled out in-process so kern's
  handlers stay intact.

Sockets, frame format (Y8 960x1280), the 300 ms fresh-frame gate and the
AE/AF controller are unchanged. The sealed image still hides adbd, so
camd's diagnostics go to the log ring instead of stdout.

## What was verified on hardware

Image: audit build with `KERN_CAMERA_IN_PROCESS=ON`, kernel
`airgap/cam-xz-wipe-kern` (CPP guard in-tree), `fastboot boot` only.
Host suites (`scripts/test.sh`) pass with the flag both OFF and ON.

* `ps` shows two `kern-nexus` processes: the signer parent and the
  exec'd camera child (no separate `/opt/kern/camd`); both
  `/data/kern/camd.sock` and `camd-ctl.sock` are created.
* Frame socket returns a fresh 960x1280 frame; `q` snapshot
  `exp=0x09c4 gain=0x00e0 luma=1`.
* Full refocus (`R`) works (`sweeping=1` in the frame header); 100/100
  frame-client cycles returned fresh frames; no fd leak (37 fds).
* QR scanning and entropy preview were verified by the operator as
  working with the Stage 1 image.
* No `No free write masters`, `Invalid close`, ping-pong mismatch,
  tasklet overflow, `wdog_bark`, overlap or panic in dmesg. Only the
  known VCM table-edge warnings during AF sweeps.

## Known issues / out of scope for Stage 1

* The log ring has no dump path yet (needed before sealed release).
* Stage 2 (direct control calls instead of sockets) has not been done;
  the control socket remains in-process, which is safe because its
  ioctls run on camd's control thread, not the UI thread.
* Idle power quiesce is now merged (`aaa93c5`). With
  `KERN_CAMERA_IN_PROCESS=ON`, the camera runs as a `fork()`+`execv()` child
  of the same binary; a full teardown exits it and `app_video_start()`
  respawns it on the next camera page. It is gated by the persistent
  `idle_stop_after` knob, default **0**, so nothing changes unless it is
  explicitly enabled. Verified: 5 cycles, 10-min soak at 45→36 C and
  ~6.7 mA, and a post-soak QR preview. Longer soak is in progress.

## Build/test commands

```sh
# default (external daemon, shipped behaviour)
KERN_NEXUS_BUILD=/tmp/kern-nexus bash scripts/build-all.sh

# Stage 1 in-process dev build
cmake -S ports/nexus5 -B /tmp/opencode/stage1-build-inproc \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/ports/nexus5/toolchain-armhf.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DMBEDTLS_ROOT=/tmp/kern-nexus/artifacts/deps/mbedtls-armhf \
  -DZLIB_ROOT=/tmp/kern-nexus/artifacts/deps/zlib-armhf \
  -DKERN_CAMERA_IN_PROCESS=ON
cmake --build /tmp/opencode/stage1-build-inproc -j8

# image (dev only, fastboot boot)
KERN_NEXUS_BUILD=/tmp/opencode/stage1-build \
  KERNEL_IMG=/tmp/opencode/stage1-build/ko/arch/arm/boot/zImage-dtb \
  KERN_BIN=/tmp/opencode/stage1-build-inproc/kern-nexus \
  CAMD_BIN=/tmp/kern-nexus/artifacts/build/camd \
  KERN_SEAL=0 bash scripts/build-image.sh standard
```
