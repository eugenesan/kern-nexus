# ports/nexus5

The Linux/ARM port of kern.  This is not the ESP32-P4 build: it links kern's
application code against a small platform layer and produces
`artifacts/built/kern-nexus`.

## What is here

| file | purpose |
|---|---|
| `CMakeLists.txt` | static ARM build; globs kern `main/` + portable components |
| `toolchain-armhf.cmake` | `arm-linux-gnueabihf`, hard-float, `-static` |
| `lv_conf.h` | LVGL 9.5, 32-bit fbdev + evdev |
| `platform/bsp_nexus5.c` | fb0 + MSM commit/pan, evdev, backlight, battery, poweroff |
| `platform/main_nexus5.c` | Linux `main()`, `/data/kern` NVS paths, LVGL timer loop |
| `platform/app_video_camera.c` | `app_video_*` adapter; stub until camd is refactored |
| `include/bsp/` | declarations missing from the simulator BSP headers |

The ESP-IDF/FreeRTOS/NVS/storage stubs are reused directly from
`kern/simulator/platform`.  Do not copy them unless a target-specific
change is needed.

## Build

```sh
bash scripts/build-kern.sh
# -> artifacts/built/kern-nexus
```

Needs:

* `kern/third_party/lvgl` (LVGL 9.5) or `-DLVGL_DIR=`;
* ARM static mbedTLS at `artifacts/deps/mbedtls-armhf` or `MBEDTLS_ROOT=`;
* ARM static zlib;
* `kern/components/libwally-core/upstream` submodule.

## Current state / TODO

1. `bsp_nexus5.c` compiles against LVGL 9.5's fbdev/evdev drivers.  Verify
   the header paths for the chosen LVGL tag; the MSM commit path is wired
   into `bsp_display_unlock()` and `bsp_display_scrub_buffers()`.
2. `app_video_*` is a safe stub (`ESP_ERR_NOT_SUPPORTED`).  Replace it with
   the camera component (`components/camera`) delivering the same 720×960 / 960×1280 YUV420 frames as Krux.
3. `main_nexus5.c` calls `app_main()` once; if app_main's FreeRTOS timers
   do not fire under the stubs, move the splash/session timers to the LVGL
   loop (the simulator's `main_sim.c` is the reference).
4. `main/core/storage.c`, `pbkdf2.c`, `fw_update.c` are replaced by the
   simulator shims; point them at `/data/kern` and disable the update UI.
5. The framebuffer is 1080×1920; the UI font/layout policy in
   `main/ui/font_policy.c` may need a nexus5 profile.
