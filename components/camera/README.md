# components/camera

The MSM8974 camera pipeline from the Krux port.  Source:
`krux-nexus/cam` at commit `a782afb` (camd.c) and the matching headers;
`fw/cpp_firmware_v1_2_0.fw` is the vendor blob (16 KB, CPP FW version
0x10020003).

This is copied as the reference implementation, **not yet compiled** into
the port.  The first integration task is to refactor `camd.c`'s `main()`
and socket daemon into:

```c
int kern_camera_init(void);       /* open nodes, configure ISP/CPP */
int kern_camera_start(void (*cb)(uint8_t *yuv420, uint32_t w, uint32_t h,
                                 size_t len)); /* spawn the thread */
int kern_camera_stop(void);
void kern_camera_set_ae_target(int level);
void kern_camera_set_focus(int position);
```

The port's `app_video_*` functions call these.  Keep Krux's geometry
(binned 720×960 by default; 960×1280 on the full/PIX path) and its YUV420
frame.  kern's `scanner.c` currently expects RGB565 + PPA; add a
`KERN_NEXUS` YUV420 path (CPU crop/scale of Y) or a software PPA shim.  See
`docs/hardware.md#camera`.

Keep from camd:

* the V4L2/Media Controller open/configure sequence and MSM private ioctls;
* the ISP/CPP buffer rotation and the CPP event drain (`-ENODATA` is
  normal);
* the AE/AF logic (QR P95 target 180, entropy 0x80; DW9714 focus);
* the continuous stream (no long pause/restart while streaming);
* the CPP firmware staging, but never copy a file onto itself with
  `O_TRUNC` (a zero-byte blob gives `CPP FW Version 0xdeadbeef`).

Drop from camd:

* `main()`, the Unix socket (`/data/krux/camd.sock`), the supervisor fork;
* all `/data/krux` state/dump paths;
* the release/debug `#ifdef` split (the port has its own release profile).

The full hardware contract and pitfalls are in
[../../docs/hardware.md](../../docs/hardware.md#camera) and
`../../../krux-nexus/docs/cam-state.md`.

Build the component as part of `ports/nexus5` only after the refactor; the
current `app_video_camera.c` is a safe stub (`ESP_ERR_NOT_SUPPORTED`) so
the rest of the UI can link.
