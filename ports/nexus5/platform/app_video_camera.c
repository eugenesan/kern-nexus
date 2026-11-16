/*
 * app_video_* for the Nexus 5.
 *
 * Interim bridge: the Krux camd process owns the MSM/ISP/CPP pipeline and
 * publishes YUV420 frames over /data/kern/camd.sock.  This thread reads
 * them, converts to RGB565 for kern's scanner/PPA path, and invokes the
 * scanner callback.  The camera daemon can later be folded into this
 * thread; the public app_video_* contract stays the same.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "esp_err.h"
#include "video.h"

#define CAMD_SOCK "/data/kern/camd.sock"
#define CAMD_LOCAL "/data/kern/camd"
#define CAMD_BUNDLED "/opt/kern/camd"
#define CAMD_PID_FILE "/tmp/kern-camd.pid"

static pthread_t s_thread;
static volatile int s_stop;
static volatile int s_running;
static int s_sock = -1;
static app_video_frame_operation_cb_t s_cb;
static uint32_t s_w = 960, s_h = 1280;
static int s_started;

/* --- exposure and focus -------------------------------------------------
 *
 * camd exposes the sensor's exposure, gain, digital gain and frame length
 * over its control socket, but nothing drives them: the sensor comes up at
 * a fixed, very short exposure, so daylight is usable and a dim room is a
 * black frame.
 *
 * The controller below is Krux's camera.py, ported: the level is the 95th
 * percentile of the frame -- the bright pixels, which is where a QR code's
 * white modules are -- the digital gain is given back first when the
 * picture is too bright, and the exposure is the fine control, used last.
 * A manual override holds the exposure still.
 */
#define CAMD_CTL_SOCK "/data/kern/camd-ctl.sock"
#define AE_EXP_MIN 0x0100
#define AE_EXP_MAX 0x09C8
#define AE_EXP_DEFAULT 0x0400
#define AE_GAIN_MIN 0x0060
#define AE_GAIN_MAX 0x00FF
#define AE_GAIN_DEFAULT 0x00A5
#define AE_LUMA_MIN 1
#define AE_LUMA_MAX 4
#define AE_TARGET_DEFAULT 180
#define AE_SATURATED 250
#define AE_TOLERANCE 12
#define AE_CLIP_PCT 3
#define AE_VTS 0x09D6
/* A manual exposure nudge holds the auto-exposure for about five seconds
 * (the camera runs at roughly ten frames a second), then the camera takes
 * over again: a picture the user corrected in one scene must not stay
 * corrected in the next. */
#define AE_MANUAL_FRAMES 50

static int ae_exp = AE_EXP_DEFAULT;
static int ae_gain = AE_GAIN_DEFAULT;
static int ae_luma = AE_LUMA_MIN;
static int ae_luma_applied = AE_LUMA_MIN;
static int ae_target = AE_TARGET_DEFAULT;
static int ae_manual;
static int ae_manual_hold;

/* Opens the control socket and sends one command.  `cmd` holds the
 * command byte and its arguments. */
static int camd_ctl(const unsigned char *cmd, size_t len, char *reply,
                    size_t reply_len) {
  struct sockaddr_un addr;
  int s = socket(AF_UNIX, SOCK_STREAM, 0);

  if (s < 0)
    return -1;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", CAMD_CTL_SOCK);
  if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(s);
    return -1;
  }
  if (write(s, cmd, len) != (ssize_t)len) {
    close(s);
    return -1;
  }
  if (reply && reply_len) {
    ssize_t got = read(s, reply, reply_len);
    if (got <= 0) {
      close(s);
      return -1;
    }
  }
  close(s);
  return 0;
}

static int camd_ctl_exposure(int exp, int gain) {
  unsigned char cmd[5];
  char reply[2] = {0};

  cmd[0] = 'E';
  cmd[1] = (unsigned char)(exp >> 8);
  cmd[2] = (unsigned char)(exp & 0xFF);
  cmd[3] = (unsigned char)(gain >> 8);
  cmd[4] = (unsigned char)(gain & 0xFF);
  if (camd_ctl(cmd, sizeof(cmd), reply, sizeof(reply)) != 0)
    return -1;
  return reply[0] == 'O' ? 0 : -1;
}

static int camd_ctl_luma_gain(int gain) {
  unsigned char cmd[2];
  char reply[2] = {0};

  cmd[0] = 'G';
  cmd[1] = (unsigned char)gain;
  if (camd_ctl(cmd, sizeof(cmd), reply, sizeof(reply)) != 0)
    return -1;
  return reply[0] == 'O' ? 0 : -1;
}

/* The frame length: the exposure can only grow to the length of a frame,
 * so the QR path raises it to the vendor's 2518 lines. */
static int camd_ctl_set_vts(int vts) {
  unsigned char cmd[3];
  char reply[2] = {0};

  cmd[0] = 'V';
  cmd[1] = (unsigned char)(vts >> 8);
  cmd[2] = (unsigned char)(vts & 0xFF);
  if (camd_ctl(cmd, sizeof(cmd), reply, sizeof(reply)) != 0)
    return -1;
  return reply[0] == 'O' ? 0 : -1;
}

/* A manual focus position: the lens steps, as the focus slider asks. */
static int camd_ctl_set_focus(int step) {
  unsigned char cmd[2];
  char reply[2] = {0};

  cmd[0] = 'F';
  cmd[1] = (unsigned char)step;
  if (camd_ctl(cmd, sizeof(cmd), reply, sizeof(reply)) != 0)
    return -1;
  return reply[0] == 'O' ? 0 : -1;
}

/* A fresh autofocus sweep, the way every phone camera focuses on what is
 * tapped.  `full` searches the whole range; otherwise the sweep refines
 * the last good step, which is fast enough to repeat while aiming. */
static int camd_ctl_refocus(int full) {
  unsigned char cmd[1] = {(unsigned char)(full ? 'R' : 'r')};
  char reply[2] = {0};

  if (camd_ctl(cmd, sizeof(cmd), reply, sizeof(reply)) != 0)
    return -1;
  return reply[0] == 'O' ? 0 : -1;
}

/* floor(sqrt(v)), for the damped correction. */
static int isqrt256(int v) {
  int x = v, y = 0, bit = 1 << 14;

  while (bit > x)
    bit >>= 2;
  while (bit) {
    if (x >= y + bit) {
      x -= y + bit;
      y = (y >> 1) + bit;
    } else {
      y >>= 1;
    }
    bit >>= 2;
  }
  return y;
}

/* The 95th percentile of the frame and the share of pixels at the top.
 * The frame is sampled: a full histogram of 1.2 million pixels per frame
 * is not worth it at ten frames a second. */
static int ae_level(const uint8_t *y, size_t len, int *clipped_pct) {
  int hist[256];
  size_t i, n = 0, acc = 0, total;
  int clipped = 0;
  int p95 = 0;

  if (!y || len == 0)
    return -1;
  memset(hist, 0, sizeof(hist));
  for (i = 0; i < len; i += 16) {
    hist[y[i]]++;
    n++;
  }
  if (n == 0)
    return -1;
  for (i = AE_SATURATED; i < 256; i++)
    clipped += (size_t)hist[i];
  *clipped_pct = (int)((clipped * 100) / n);

  total = n;
  for (i = 0; i < 256; i++) {
    acc += (size_t)hist[i];
    if (acc * 100 >= total * 95) {
      p95 = (int)i;
      break;
    }
  }
  return p95;
}

static void ae_apply(void) {
  if (ae_luma != ae_luma_applied) {
    if (camd_ctl_luma_gain(ae_luma) == 0)
      ae_luma_applied = ae_luma;
  }
  (void)camd_ctl_exposure(ae_exp, ae_gain);
}

/* One step of the controller, from the frame just read.  `sweeping` says
 * the autofocus is measuring: its sharpness scores are only comparable
 * within one exposure, so the exposure holds still until it is done. */
static void ae_step(const uint8_t *y, size_t len, int sweeping) {
  int level, clipped_pct = 0;
  int error, factor;
  int step_up, step_down;

  if (ae_manual) {
    if (--ae_manual_hold > 0)
      return;
    /* The nudge has held for a few seconds; the camera takes over
     * again, so the next scene is exposed correctly. */
    ae_manual = 0;
  }
  if (sweeping)
    return;
  level = ae_level(y, len, &clipped_pct);
  if (level < 0)
    return;

  error = level - ae_target;
  if (error <= AE_TOLERANCE && error >= -AE_TOLERANCE)
    return;

  /* The brightness follows the exposure, so the correction is a factor and
   * not a number of lines: adding lines buys less and less brightness as
   * the exposure grows, and a fixed step overshoots.  The factor is in
   * 1/256 units. */
  if (level > 0)
    factor = (ae_target * 256) / level;
  else
    factor = 256 + 358; /* 1.4 */
  if (factor > 256 + 204) /* 1.8 */
    factor = 256 + 204;
  if (factor < 115) /* 0.45 */
    factor = 115;
  if (error > 2 * AE_TOLERANCE || error < -2 * AE_TOLERANCE) {
    /* A big change (a lamp, a torch, a hand over the lens) takes the full
     * correction. */
  } else {
    /* Near the target the step is damped, so the picture lands in the
     * dead band without swinging across it. */
    factor = isqrt256(factor * 256);
    if (factor > 256 + 51) /* 1.20 */
      factor = 256 + 51;
    if (factor < 256 - 38) /* 0.85 */
      factor = 256 - 38;
  }
  step_up = (ae_exp * (factor - 256)) / 256;
  step_down = (ae_exp * (256 - factor)) / 256;
  if (step_up < 0x30)
    step_up = 0x30;
  if (step_down < 0x30)
    step_down = 0x30;

  if (clipped_pct > AE_CLIP_PCT || level >= AE_SATURATED) {
    /* Over the top: the digital gain is a plain multiply, so it is the
     * cheapest thing to give back. */
    if (ae_luma > AE_LUMA_MIN)
      ae_luma--;
    else if (ae_exp > AE_EXP_MIN)
      ae_exp = (ae_exp * 85) / 100;
    else if (ae_gain > AE_GAIN_MIN)
      ae_gain -= 0x18;
  } else if (level > ae_target + AE_TOLERANCE) {
    if (ae_luma > AE_LUMA_MIN)
      ae_luma--;
    else if (ae_exp > AE_EXP_MIN)
      ae_exp -= step_down;
    else if (ae_gain > AE_GAIN_MIN)
      ae_gain -= 0x18;
  } else {
    /* Too dark: the exposure is the long exposure and has the least
     * noise, so it goes first, then the analog gain. */
    if (ae_exp < AE_EXP_MAX)
      ae_exp += step_up;
    else if (ae_gain < AE_GAIN_MAX)
      ae_gain += 0x18;
    else if (ae_luma < AE_LUMA_MAX && level * 2 < ae_target)
      ae_luma++;
  }
  if (ae_exp < AE_EXP_MIN)
    ae_exp = AE_EXP_MIN;
  if (ae_exp > AE_EXP_MAX)
    ae_exp = AE_EXP_MAX;
  if (ae_gain < AE_GAIN_MIN)
    ae_gain = AE_GAIN_MIN;
  if (ae_gain > AE_GAIN_MAX)
    ae_gain = AE_GAIN_MAX;
  ae_apply();
}

static void ae_start(void) {
  ae_exp = AE_EXP_DEFAULT;
  ae_gain = AE_GAIN_DEFAULT;
  ae_luma = AE_LUMA_MIN;
  ae_luma_applied = AE_LUMA_MIN;
  ae_manual = 0;
  ae_manual_hold = 0;
  /* The exposure can only reach the length of a frame, so the frame is
   * raised to the vendor's 2518 lines before the exposure goes there. */
  (void)camd_ctl_set_vts(AE_VTS);
  ae_apply();
}

static void klog(const char *fmt, ...) {
  int fd = open("/dev/kmsg", O_WRONLY);
  if (fd >= 0) {
    va_list ap; va_start(ap, fmt);
    vdprintf(fd, fmt, ap); va_end(ap);
    close(fd);
  }
}

static int read_exact(int fd, void *buf, size_t len) {
  uint8_t *p = buf;
  while (len) {
    ssize_t n = read(fd, p, len);
    if (n <= 0)
      return -1;
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

static int connect_camd(void) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un addr;
  if (fd < 0)
    return -1;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", CAMD_SOCK);
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static void *camera_thread(void *arg) {
  (void)arg;
  klog("kern-nexus camera: thread start\n");
  while (!s_stop) {
    uint8_t hdr[32];
    if (s_sock < 0) {
      s_sock = connect_camd();
      if (s_sock < 0) {
        usleep(200000);
        continue;
      }
    }
    if (write(s_sock, "F", 1) != 1) {
      close(s_sock);
      s_sock = -1;
      continue;
    }
    if (read_exact(s_sock, hdr, sizeof(hdr)) != 0) {
      close(s_sock);
      s_sock = -1;
      continue;
    }
    if (memcmp(hdr, "KCF1", 4) != 0)
      continue; /* no frame ready */
    uint32_t w = (uint32_t)hdr[8] | (uint32_t)hdr[9] << 8;
    uint32_t h = (uint32_t)hdr[10] | (uint32_t)hdr[11] << 8;
    int af_sweeping = hdr[18];
    uint32_t ysz = (uint32_t)hdr[24] | (uint32_t)hdr[25] << 8 |
                   (uint32_t)hdr[26] << 16 | (uint32_t)hdr[27] << 24;
    uint32_t uvsz = (uint32_t)hdr[28] | (uint32_t)hdr[29] << 8 |
                    (uint32_t)hdr[30] << 16 | (uint32_t)hdr[31] << 24;
    if (w == 0 || h == 0 || w > 4096 || h > 4096 ||
        ysz != (uint64_t)w * h || uvsz > w * h) {
      continue;
    }
    uint8_t *frame = malloc((size_t)ysz + uvsz);
    if (!frame)
      continue;
    if (read_exact(s_sock, frame, (size_t)ysz + uvsz) != 0) {
      free(frame);
      close(s_sock);
      s_sock = -1;
      continue;
    }
    ae_step(frame, (size_t)ysz, af_sweeping);
    if (s_cb && !s_stop) {
      static int frames;
      if (!frames)
        klog("kern-nexus camera: first frame %ux%u Y8 (y=%u uv=%u)\n", w, h,
             ysz, uvsz);
      frames++;
      s_w = w;
      s_h = h;
      /* Y8 only: camd's chroma is not a standard UV plane, and the scanner
       * and preview are grayscale.  The callback copies what it needs before
       * we free the frame. */
      s_cb(frame, 0, w, h, ysz);
    }
    free(frame);
    /* camd stays the expensive side; 10 fps keeps the UI responsive and is
     * ample for QR/entropy. */
    usleep(100000);
  }
  klog("kern-nexus camera: thread stop\n");
  return NULL;
}

esp_err_t app_video_init_once(i2c_master_bus_handle_t ignored) {
  (void)ignored;
  if (s_started)
    return ESP_OK;

  /* A previous kern process may have left camd running.  Reuse its socket
   * instead of starting a second daemon: two camds fight over the ISP and
   * one of them publishes frames nobody reads. */
  int existing = connect_camd();
  if (existing >= 0) {
    close(existing);
    s_started = 1;
    klog("kern-nexus camera: reusing running camd\n");
    return ESP_OK;
  }

  const char *bin = access(CAMD_LOCAL, X_OK) == 0 ? CAMD_LOCAL : CAMD_BUNDLED;
  if (access(bin, X_OK) != 0)
    return ESP_ERR_NOT_FOUND;
  const char *fw = "/lib/firmware/cpp_firmware_v1_2_0.fw";
  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    execl(bin, "camd", "1640", "1232", "0x40000000", "rear", "0",
          fw, "2", "binned", "cfg", "nocycle", "0", "0x2b", "0", "0",
          "300", "0", "0", "0", "0x12", "0", "1", "0xffffffff", "0",
          "0", "0", "1", "0", (char *)NULL);
    _exit(127);
  }
  if (pid < 0)
    return ESP_FAIL;
  klog("kern-nexus camera: spawned %s pid=%d\n", bin, pid);
  for (int i = 0; i < 100; i++) {
    if (access(CAMD_SOCK, F_OK) == 0) {
      s_started = 1;
      return ESP_OK;
    }
    usleep(100000);
  }
  return ESP_ERR_TIMEOUT;
}

bool app_video_is_ready(void) { return s_started || access(CAMD_SOCK, F_OK) == 0; }

esp_err_t app_video_start(app_video_frame_operation_cb_t cb, int core_id) {
  (void)core_id;
  if (!app_video_is_ready()) {
    klog("kern-nexus camera: start refused, not ready\n");
    return ESP_ERR_INVALID_STATE;
  }
  if (s_running) {
    klog("kern-nexus camera: start while already running\n");
    return ESP_OK;
  }
  s_cb = cb;
  s_stop = 0;
  ae_start();
  if (pthread_create(&s_thread, NULL, camera_thread, NULL) != 0) {
    klog("kern-nexus camera: pthread_create failed\n");
    return ESP_FAIL;
  }
  s_running = 1;
  klog("kern-nexus camera: thread created\n");
  return ESP_OK;
}

esp_err_t app_video_stop(void) {
  klog("kern-nexus camera: stop (running=%d)\n", s_running);
  if (!s_running)
    return ESP_OK;
  s_stop = 1;
  if (s_sock >= 0) {
    shutdown(s_sock, SHUT_RDWR);
    close(s_sock);
    s_sock = -1;
  }
  pthread_join(s_thread, NULL);
  s_running = 0;
  s_cb = NULL;
  return ESP_OK;
}

bool app_video_is_streaming(void) { return s_running; }

video_fmt_t app_video_get_format(void) { return APP_VIDEO_FMT_GREY; }

uint32_t app_video_get_buf_size(void) { return s_w * s_h; }

esp_err_t app_video_get_resolution(uint32_t *width, uint32_t *height) {
  if (width) *width = s_w;
  if (height) *height = s_h;
  return ESP_OK;
}

esp_err_t app_video_set_ae_target(uint32_t level) {
  if (level < 2)
    level = 2;
  if (level > 235)
    level = 235;
  ae_target = (int)level;
  return ESP_OK;
}

esp_err_t app_video_set_focus(uint32_t position) {
  /* The lens has a fixed number of steps; the slider covers 0..1023. */
  if (position > 1023)
    position = 1023;
  if (camd_ctl_set_focus((int)((position * 44) / 1023)) != 0)
    return ESP_FAIL;
  return ESP_OK;
}

esp_err_t app_video_refocus(void) {
  return camd_ctl_refocus(1) == 0 ? ESP_OK : ESP_FAIL;
}

/* The same, but a quick refinement of the last good focus step: what a
 * page asks for when its preview starts. */
esp_err_t app_video_refocus_cached(void) {
  return camd_ctl_refocus(0) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t app_video_adjust_exposure(int32_t delta) {
  /* A manual nudge holds the auto-exposure for a few seconds, as Krux
   * does: the picture stays where the user put it, and the camera takes
   * over again afterwards.  When the exposure has run out of room the
   * gain takes the step, or a picture that is already at its brightest
   * setting could not be made brighter at all. */
  ae_manual = 1;
  ae_manual_hold = AE_MANUAL_FRAMES;
  if (delta > 0) {
    ae_exp += 0x100;
    if (ae_exp > AE_EXP_MAX) {
      ae_exp = AE_EXP_MAX;
      if (ae_gain < AE_GAIN_MAX)
        ae_gain += (AE_GAIN_MAX - ae_gain < 0x20) ? AE_GAIN_MAX - ae_gain : 0x20;
    }
  } else {
    ae_exp -= 0x100;
    if (ae_exp < AE_EXP_MIN) {
      ae_exp = AE_EXP_MIN;
      if (ae_gain > AE_GAIN_MIN)
        ae_gain -= (ae_gain - AE_GAIN_MIN < 0x20) ? ae_gain - AE_GAIN_MIN : 0x20;
    }
  }
  ae_apply();
  return ESP_OK;
}

bool app_video_has_focus_motor(void) { return true; }

bool app_video_has_ae_control(void) { return true; }

uint32_t app_video_ppa_snap_crop(uint32_t crop_max, uint32_t target) {
  return crop_max < target ? crop_max : target;
}
