# Hardware, wiring and on-device interfaces

Everything here was measured on the hammerhead in `krux-nexus`; it is the
contract the `ports/nexus5` platform layer implements.  Nothing is inferred
from a datasheet when a measurement exists.

## 1. Board and partitions

* SoC: Qualcomm MSM8974 (Snapdragon 800), 4× Cortex-A15, ARMv7-A, NEON,
  hard-float.  2 GB RAM, 32 GB eMMC.
* Bootloader: `HHZ20h`, unlocked (`secure-boot: yes`, `unlocked: yes`).
* Display: 1080×1920 MIPI command panel, ORISE.
* Recovery template: `artifacts/twrp-3.7.0_9-HH.R.17-MADV_WIPEONFORK.img`.

Partitions measured with `fastboot getvar all`:

| name | block | size | type |
|---|---|---|---|
| boot | `mmcblk0p?` | `0x1600000` (23,068,672) | emmc |
| recovery | `mmcblk0p20` | `0x1600000` | emmc |
| system | `mmcblk0p25` | `0x9d1e2c00` (~2.4 GiB) | ext4 |
| cache | `mmcblk0p27` | `0x10327e00` (~259 MiB) | ext4 |
| userdata | `mmcblk0p28` | `0x68d0efc00` (~26 GiB) | ext4 |
| persist | `mmcblk0p?` | `0x1000000` | ext4 |

The delivered kernel-nexus image goes to **recovery only**.  `boot` is
never written by this project.  `system`, `cache` and `userdata` belong to
the phone's Android installation and are not part of the image.

## 2. Display (`/dev/graphics/fb0`)

Measured:

```
resolution     1080 x 1920
bpp            32
stride         4352 bytes (not 1080*4 = 4320)
buffers        triple (yres_virtual / yres = 3)
node           /dev/graphics/fb0
```

The panel is a **command-mode** panel.  Drawing into the mmap'd framebuffer
is not enough:

* `MSMFB_DISPLAY_COMMIT` copies the selected plane into the scanout buffer;
* `FBIOPAN_DISPLAY` selects the buffer/offset the panel scans out.

The working Krux port (`krux/firmware/micropython/ports/unix/lcd_bcm.c`)
does both, on a separate commit thread, with `wait_for_finish` on preview.
The LVGL fbdev driver must be adapted to do the same; the stock
`LV_USE_LINUX_FBDEV` flush alone leaves stale/torn images.

Kern's UI is designed for 720×720/720×1280 etc.  On 1080×1920 it needs a
portrait layout and font policy.  `ports/nexus5` should set
`LV_COLOR_DEPTH 32`, the 1080×1920 geometry, and the font scale; do not
reuse the 720×720 simulator constants.

The framebuffer is **reserved RAM**: the kernel power-off wipe does not
zero it.  `k_run` must paint black/commit/blank before shutdown.

## 3. Touch and keys

| device | purpose | notes |
|---|---|---|
| `/dev/input/event1` | Synaptics DSX touch | `ABS_MT_SLOT` 0–9, X 0–1079, Y 0–1919, `TRACKING_ID`, `PRESSURE` |
| `/dev/input/event0` | power key | `KEY_POWER` |
| `/dev/input/event2` | volume keys | `KEY_VOLUMEUP`, `KEY_VOLUMEDOWN` |

The input devices are raw evdev; there is no Android input layer.  Use
`EVIOCGNAME` to identify them rather than assuming the numbers.  Kern/LVGL
`LV_USE_EVDEV` handles touch; the volume/power keys are read directly by
the port and mapped to kern's key events.

The touch coordinates are in panel space (1080×1920); do not swap X/Y
unless the UI is rotated.  Krux's `touch.py` has the region semantics and
the swipe threshold (35 px) if kern needs to mirror them.

## 4. Backlight, battery, power

| resource | path |
|---|---|
| backlight | `/sys/class/backlight/lm3630/brightness` (max in `max_brightness`) |
| battery capacity | `/sys/class/power_supply/battery/capacity` |
| battery voltage/current | `/sys/class/power_supply/battery/voltage_now`, `current_now` |
| USB charger | `/sys/class/power_supply/usb/online` (or `present`) |
| AC charger | `/sys/class/power_supply/ac/online` |
| camera flash | `/sys/class/leds/led:flash_torch/brightness` |
| power off | `reboot(LINUX_REBOOT_CMD_POWER_OFF)` |

Backlight: write 0..max (the driver accepts a percentage-like value; the
Krux stub writes `max_brightness / 2` at start).  Battery: the fuel gauge
is `max17048`; the charger is `bq24192`.  The values are read-only.

Power-off goes through the kernel wipe path.  Do not `exec` a userspace
poweroff helper.

## 5. Camera

### 5.1 Nodes and sensors

| node | role |
|---|---|
| `/dev/video0` | MSM camera config/event node |
| `/dev/video1`, `/dev/video2` | capture nodes (ISP output) |
| `/dev/v4l-subdev*` | CSID, ISPIF, ISP, CPP, sensor, actuator |
| `/dev/ion` | ION buffer allocation |
| `/dev/media*` | media controller (optional) |

Rear sensor: **IMX179** (Sony, 8 MP), binned 1640×1232 and full 3280×2464.
Front sensor: **MT9M114B** (720p), not needed for kern.  The actuator is a
**DW9714** VCM (`/dev/v4l-subdev*`, I2C).

The vendor camera HAL is not used.  Camd talks V4L2/Media Controller and
the MSM private ioctls directly; the kernel changes in `linux/patches`
make the ISP/CPP path work.

### 5.2 Path and format

The default is the vendor's processed preview:

```
sensor -> CSID -> ISP (PIX_VIEWFINDER) -> CPP -> buffers
```

`stream=0` is the raw Bayer through the CPP; `stream=1` is the processed
preview.  **1 is the default** and is the path verified for QR and entropy.
The CPP does **not** scale to 1280×960; do not assume it will resize.

Krux's camera already produces the right picture: binned **720×960**
by default, and 960×1280 on the full/PIX path.  Keep those geometries; the
camera component should hand the same YUV420 frames to kern.  `scanner.c`
already uses the callback's actual `w`/`h` and only logs a warning when
they differ from `CAMERA_INPUT_WIDTH/HEIGHT`; do not force the sensor to
800×800 unless a hard assert appears.  If a fixed 800×800 is ever needed,
crop/scale in the port, not in the sensor path.

**Format mismatch to fix in the port:** Krux's `camd` produces YUV420, but
kern's `scanner.c` currently expects **RGB565** and uses the ESP32-P4 PPA to
crop/scale/convert it to YUV420 for `k_quirc`.  The Nexus 5 has no PPA, so
the port must either:

* add a `KERN_NEXUS` path in `scanner.c` that takes the YUV420 frame,
  crops/scales the Y plane on the CPU, and skips the PPA; or
* convert YUV420→RGB565 on the CPU and keep the per-frame crop/scale in a
  software `ppa_do_scale_rotate_mirror()`.

The first is less work and avoids a needless colour round-trip.  The
callback is `app_video_frame_operation_cb_t(uint8_t *buf, uint8_t index,
uint32_t w, uint32_t h, size_t len)`.

The camera component comes from `../krux-nexus/cam`:

* `camd.c` — the daemon; refactor `main()` into `kern_camera_init/start/stop`;
* `cam_*.h`, `isp.h`, `ispif.h`, `ion.h`, `mipi.h`, `sensor*.h`,
  `imx179_regs.h`, `v4l2_caf.h`, `sensor_config.h`, `sensor_live.h`;
* `fw/cpp_firmware_v1_2_0.fw` — 16 KB, version 0x10020003.

The CPP firmware must be reachable at `/lib/firmware/cpp_firmware_v1_2_0.fw`.
Camd stages the blob there and asks the CPP to load it.  **Never copy a
file onto itself with `O_TRUNC`**: if the only source is already
`/lib/firmware/...`, that truncates it to zero; the kernel then loads a
zero-byte blob (`CPP FW Version 0xdeadbeef`, `Invalid frame message`).
Bundle a distinct copy in `/opt/kern/cpp_firmware_v1_2_0.fw`.

### 5.3 Camd parameters and controls

The Krux stub starts camd with:

```
camd 1640 1232 0x40000000 rear 0 <fw> 2 binned cfg nocycle \
     0 0x2b 0 0 300 0 0 0 0x12 0 1 0xffffffff 0 0 0 1 0
```

See `camd.c:main()` for the full positional contract.  For kern, keep the
same defaults and expose only what the UI uses:

* `app_video_set_ae_target(level)` — P95 target; QR target 180, entropy 0x80;
* `app_video_set_focus(position)` — DW9714 position; 0..1023 (inverted in
  the UI);
* `app_video_has_focus_motor()` / `app_video_has_ae_control()`;
* `app_video_get_resolution()`, `app_video_get_buf_size()`;
* `app_video_ppa_snap_crop()` — on the ESP this is the PPA; on Linux
  implement the same exact integer crop (no hardware PPA).

### 5.4 Lifecycle and pitfalls

* The stream is **continuous**; an on-demand pause was tried and reverted.
  A long pause desynchronises the ISP/CPP (stitched frames, pumping AE).
* Restarting the service **while streaming** can leave ISP write masters
  held and hang the next start.  Restart at the menu, not on a camera page.
* The CPP queue must be drained every frame; the kernel returns `-ENODATA`
  when empty.  Do not resend timeouts.
* The camd supervisor restarts a dead child; in-process, make the thread
  restart the pipeline rather than leaving a half-open device.
* The camera thread must not call LVGL; deliver frames through the
  callback and lock only around the preview buffer.
* The QR decoder is `k_quirc`; it wants a Y plane, not RGB.  The preview
  can use the same YUV420 buffer through LVGL's canvas.

## 6. NFC (BCM2079x)

### 6.1 Hardware and kernel

* BCM20793M, I2C bus 0, address `0x77`.
* Kernel driver `CONFIG_NFC_BCM2079X=y`, misc device `/dev/bcm2079x`
  (char 10:65).
* The driver owns the I2C address; do not open `/dev/i2c-0` directly.
* Firmware is already in the chip; no download is needed for the normal
  bring-up.  `nfc/blobs/*.ncd` are kept for recovery experiments only.

Ioctls (`_IO(0xFA, nr)`, no argument/direction bit):

| nr | name | use |
|---|---|---|
| 0x01 | `BCMNFC_POWER_CTL` | power on/off |
| 0x02 | `BCMNFC_CHANGE_ADDR` | client address |
| 0x03 | `BCMNFC_READ_FULL_PACKET` | one packet |
| 0x04 | `BCMNFC_SET_WAKE_ACTIVE_STATE` | wake polarity |
| 0x05 | `BCMNFC_WAKE_CTL` | wake assert/deassert |
| 0x06 | `BCMNFC_READ_MULTI_PACKETS` | multi-packet read |
| 0x07 | `BCMNFC_SET_CLIENT_ADDR` | client address |

Power-on wake polarity is `NFC_WAKE_ASSERTED_ON_POR` (asserted low).  The
driver allows multiple opens, but only one client should drive the chip.

### 6.2 Framing

One `read()` returns one whole packet:

```
byte 0   HCIT type: 0x10 NFC, 0x04 event
byte 1   MT (0xE0) | PBF (0x10) | GID (0x0F)
byte 2   OID (0x3F)
byte 3   payload length
byte 4+  payload
```

Message types: `MT_DATA 0x00`, `MT_CMD 0x20`, `MT_RSP 0x40`, `MT_NTF 0x60`,
`MT_CFG 0x80`.

### 6.3 Bring-up sequence

Captured from the vendor HAL and replayed in `krux/src/nfc.py`:

```
chip -> 10 60 00 02 00 01        Core Reset notification
->     10 2f 04 00               GET_BUILD_INFO
->     10 2f 2d 00               GET_PATCH_VERSION
->     10 20 00 01 01            CORE_RESET
->     10 20 01 00               CORE_INIT
->     10 20 02 39 01 b9 36 ...  SET_CONFIG (LPTD)
->     10 20 02 66 0c cb 01 ...  SET_CONFIG (start-up + antenna)
->     10 20 02 06 01 b7 03 ...  SET_CONFIG (poll)
->     10 21 00 07 02 04 03 ...  DISCOVER
```

The exact `SET_CONFIG` payloads and the RF routing/discovery tables are in
`nfc.py` as hex constants.  They were taken from the vendor trace; copy them
verbatim.  The chip reports a tag as an OID `0x06` notification, then the
host sends `RF_DISCOVER_SELECT` (`01 04 02`).

The ISO-DEP APDU path is:

1. wait for the tag notification;
2. select discovery id 1, protocol ISO-DEP, interface 2;
3. send the APDU as NCI data (`MT_DATA`), wait for the data response;
4. reassemble the response and return `(data, sw1, sw2)`.

`nfc.py` has the full state machine and timeouts (poll window 8 s,
activation settle 200 ms, APDU timeout 5 s).  Port it to C with the same
values; do not invent shorter timeouts.

### 6.4 Card interface for kern

Expose a small C API:

```c
typedef struct {
  int (*transmit)(void *ctx, const uint8_t *apdu, size_t len,
                  uint8_t *resp, size_t *resp_len,
                  uint8_t *sw1, uint8_t *sw2);
} kern_nfc_transport_t;

int nfc_open(void);
void nfc_close(void);
int nfc_transceive(const uint8_t *apdu, size_t len, uint8_t *resp,
                   size_t *resp_len, uint8_t *sw1, uint8_t *sw2);
```

SeedKeeper is layered on this, not on the chip driver.

## 7. UART

* `ttyHSL0`, 115200 8N1, on the headset jack.
* Host: FT232 → `/dev/ttyUSB0`.
* Kernel console and `start-serial` share the line; kernel messages
  interleave.  `stty -F /dev/ttyUSB0 115200 cs8 -cstopb -parenb raw`.
* Use `scripts/serial-cmd.py` for scripted commands.

## 8. Air gap

The kernel is built without:

* `CONFIG_WLAN`, `CFG80211`, `WEXT`, `BCMDHD`, `MAC80211`;
* `CONFIG_BT`, `BT_*`, `RFCOMM`, `BNEP`, `HIDP`;
* `CONFIG_MSM_PIL_MSS_QDSP6V5`, `MSM_RMNET*`, `USB_NET_QMI_WWAN`,
  `USB_HSO`, `USB_IPHETH`, `USB_SIERRA_NET`;
* FM radio tuner drivers.

`CONFIG_NFC_BCM2079X=y` is the only wireless interface.  `rfkill` is a
framework with no radio drivers behind it.  The standard image carries no
`wpa_supplicant`, `hostapd`, `wificond`, Broadcom Wi-Fi/Bluetooth firmware
or Bluetooth HCI tools.  The build gate `tools/check-airgap.py` enforces
this on the kernel `.config` and the ramdisk.

`CONFIG_USB_USBNET` and `CONFIG_USB_RTL8150` are now stripped by
`tools/kernel/profiles/hammerhead.list`; the target has no USB networking
at all.
