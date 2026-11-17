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
#include <time.h>
#include <unistd.h>

#include "esp_err.h"
#include "video.h"
#ifdef KERN_CAMERA_IN_PROCESS
#include "kern_camera.h"
#endif

#define CAMD_SOCK "/data/kern/camd.sock"
#define CAMD_LOCAL "/data/kern/camd"
#define CAMD_BUNDLED "/opt/kern/camd"
#define CAMD_PID_FILE "/tmp/kern-camd.pid"

static pthread_mutex_t s_video_lock = PTHREAD_MUTEX_INITIALIZER;
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
/* A sweep the user asked for: the scores are only comparable within one
 * exposure, so the AE holds still until it is done.  The camera's own
 * sweep at stream start is not held: the exposure follows the scene and
 * camd abandons that sweep when it changes. */
static int ae_hold_sweep;
static int64_t ae_hold_since;
static int ae_frames;
/* The first frames after streaming starts are the sensor settling; they
 * are not the picture the user is looking at yet. */
static int ae_warmup;
/* A single P95 frame can be a rolling band on a screen, or a bright
 * speck in a dark room, so a correction is only taken when two frames in
 * a row agree on the side of the target.  The picture is then locked
 * once it has sat in the dead band for a few frames, so a measurement
 * that is never exactly on target does not move the exposure forever. */
static int ae_side;
static int ae_side_count;
static int ae_settle_count;
static int ae_locked;
/* A change is given time to reach the picture before the next decision.
 * The frame the app has in hand is behind the sensor -- camd buffers, the
 * socket copies, the preview pass -- so a correction applied every frame
 * is decided on a picture from before the last one, and the exposure
 * swings from white to black.  The wait is in milliseconds, not frames:
 * the scanner runs at four frames a second in a dark room and less than
 * one on a dense code, and a frame count that suits one starves the
 * other. */
static int64_t ae_changed_at;

static int64_t ae_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

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
    size_t done = 0;
    while (done < reply_len) {
      ssize_t got = read(s, reply + done, reply_len - done);
      if (got <= 0) {
        close(s);
        return -1;
      }
      done += (size_t)got;
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

/* What the sensor is doing right now: exposure, analog gain and the
 * digital luma gain camd applies.  -1 when the daemon cannot say. */
static int camd_ctl_snapshot(int *exp, int *gain, int *luma) {
  unsigned char cmd[1] = {'q'};
  unsigned char reply[6] = {0};

  if (camd_ctl(cmd, sizeof(cmd), (char *)reply, sizeof(reply)) != 0)
    return -1;
  if (reply[0] != 'O')
    return -1;
  *exp = (reply[1] << 8) | reply[2];
  *gain = (reply[3] << 8) | reply[4];
  *luma = reply[5] ? reply[5] : AE_LUMA_MIN;
  return 0;
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
  ae_changed_at = ae_now_ms();
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

  ae_frames++;
  if (ae_warmup) {
    ae_warmup--;
    return;
  }
  if (ae_changed_at && ae_now_ms() - ae_changed_at < 800)
    return;
  if (ae_manual) {
    if (--ae_manual_hold > 0)
      return;
    /* The nudge has held for a few seconds; the camera takes over
     * again, so the next scene is exposed correctly. */
    ae_manual = 0;
    ae_locked = 0;
    ae_side_count = 0;
  }
  if (sweeping && ae_hold_sweep) {
    /* A sweep that never reports that it is done must not freeze the
     * exposure for the rest of the session. */
    if (ae_now_ms() - ae_hold_since < 20000)
      return;
    ae_hold_sweep = 0;
  }
  level = ae_level(y, len, &clipped_pct);
  if (level < 0)
    return;

  error = level - ae_target;
  if (!sweeping)
    ae_hold_sweep = 0;
  if (error <= AE_TOLERANCE && error >= -AE_TOLERANCE) {
    /* The picture is in the dead band: hold it there.  Three frames in
     * a row and it is locked, and only a real change wakes it. */
    ae_side_count = 0;
    if (++ae_settle_count >= 3)
      ae_locked = 1;
    return;
  }
  ae_settle_count = 0;
  if (ae_locked) {
    /* Stay locked through a small drift; a real change wakes it on the
     * first measurement that is outside. */
    if (error <= 18 && error >= -18 && clipped_pct <= AE_CLIP_PCT)
      return;
    ae_locked = 0;
  }

  /* One frame on a side is not enough: two in a row are. */
  {
    int side = error > 0 ? 1 : -1;
    if (side != ae_side) {
      ae_side = side;
      ae_side_count = 1;
      return;
    }
    if (++ae_side_count < 2)
      return;
    ae_side_count = 0;
  }

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
     * cheapest thing to give back.  The exposure then takes the whole
     * correction, not a fixed fifteen per cent: a bright screen keeps
     * the bright pixels at the ceiling over a wide range of exposures,
     * so a small step leaves it clipped for many frames, and the
     * scanner only takes a few frames a second. */
    if (ae_luma > AE_LUMA_MIN)
      ae_luma--;
    else if (ae_exp > AE_EXP_MIN)
      ae_exp = (ae_exp * 85) / 100;
    else if (ae_gain > AE_GAIN_MIN)
      ae_gain -= (ae_gain - AE_GAIN_MIN < 0x30) ? ae_gain - AE_GAIN_MIN : 0x30;
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
  int exp = 0, gain = 0, luma = 0;

  /* Start from what the sensor is already showing.  The last session left
   * it on a picture the user could see; seeding from a fixed default made
   * the first frames darker than the ones before them, and the controller
   * then climbed back over several seconds. */
  if (camd_ctl_snapshot(&exp, &gain, &luma) != 0 || exp <= 0) {
    exp = ae_exp;
    gain = ae_gain;
    luma = ae_luma_applied;
  }
  if (exp < AE_EXP_MIN)
    exp = AE_EXP_MIN;
  if (exp > AE_EXP_MAX)
    exp = AE_EXP_MAX;
  if (gain < AE_GAIN_MIN)
    gain = AE_GAIN_MIN;
  if (gain > AE_GAIN_MAX)
    gain = AE_GAIN_MAX;
  if (luma < AE_LUMA_MIN || luma > AE_LUMA_MAX)
    luma = AE_LUMA_MIN;
  ae_exp = exp;
  ae_gain = gain;
  /* The daemon has already applied this digital gain, so it must not be
   * sent again (that would make the picture jump). */
  ae_luma = luma;
  ae_luma_applied = luma;
  ae_manual = 0;
  ae_manual_hold = 0;
  ae_hold_sweep = 0;
  ae_frames = 0;
  ae_side = 0;
  ae_side_count = 0;
  ae_settle_count = 0;
  ae_locked = 0;
  ae_changed_at = 0;
  ae_warmup = 3;
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

#ifdef KERN_CAMERA_IN_PROCESS
  /* Stage 1: run camd on a dedicated thread inside kern.  The sockets,
   * frame format and AE/AF controller are unchanged; only the process
   * boundary moved.  No camera ioctl runs on this (UI) thread. */
  if (kern_camera_launch() != 0)
    return ESP_FAIL;
  klog("kern-nexus camera: launched in-process camd\n");
#else
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
#endif
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
  esp_err_t e;

  pthread_mutex_lock(&s_video_lock);
#ifdef KERN_CAMERA_IN_PROCESS
  /* If the camera child quiesced after an idle period, the socket files
   * are stale.  Re-launch it on the first camera page.  The lock makes
   * sure only one page can decide to spawn. */
  {
    int probe = connect_camd();
    if (probe < 0) {
      unlink(CAMD_SOCK);
      unlink(CAMD_CTL_SOCK);
      s_started = 0;
      e = app_video_init_once(0);
      if (e != ESP_OK) {
        klog("kern-nexus camera: camera respawn failed: %d\n", e);
        pthread_mutex_unlock(&s_video_lock);
        return e;
      }
    } else {
      close(probe);
    }
  }
#endif
  if (!app_video_is_ready()) {
    klog("kern-nexus camera: start refused, not ready\n");
    pthread_mutex_unlock(&s_video_lock);
    return ESP_ERR_INVALID_STATE;
  }
  if (s_running) {
    klog("kern-nexus camera: start while already running\n");
    pthread_mutex_unlock(&s_video_lock);
    return ESP_OK;
  }
  s_cb = cb;
  s_stop = 0;
  ae_start();
  if (pthread_create(&s_thread, NULL, camera_thread, NULL) != 0) {
    klog("kern-nexus camera: pthread_create failed\n");
    pthread_mutex_unlock(&s_video_lock);
    return ESP_FAIL;
  }
  s_running = 1;
  klog("kern-nexus camera: thread created\n");
  pthread_mutex_unlock(&s_video_lock);
  return ESP_OK;
}

esp_err_t app_video_stop(void) {
  pthread_mutex_lock(&s_video_lock);
  klog("kern-nexus camera: stop (running=%d)\n", s_running);
  if (!s_running) {
    pthread_mutex_unlock(&s_video_lock);
    return ESP_OK;
  }
  s_stop = 1;
  if (s_sock >= 0) {
    shutdown(s_sock, SHUT_RDWR);
    close(s_sock);
    s_sock = -1;
  }
  pthread_join(s_thread, NULL);
  s_running = 0;
  s_cb = NULL;
  pthread_mutex_unlock(&s_video_lock);
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
  ae_hold_sweep = 0;
  return ESP_OK;
}

esp_err_t app_video_refocus(void) {
  if (camd_ctl_refocus(1) != 0)
    return ESP_FAIL;
  ae_hold_sweep = 1;
  ae_hold_since = ae_now_ms();
  return ESP_OK;
}

/* The same, but a refinement of the last good step: the whole range is
 * only searched when there is no previous good focus. */
esp_err_t app_video_refocus_cached(void) {
  if (camd_ctl_refocus(0) != 0)
    return ESP_FAIL;
  ae_hold_sweep = 1;
  ae_hold_since = ae_now_ms();
  return ESP_OK;
}

/* Hand the lens back to the sweep state machine: a fixed position set by
 * the Focus slider is released, and a lens that has already focused stays
 * where it is. */
esp_err_t app_video_auto_focus(void) {
  unsigned char cmd[1] = {'A'};
  char reply[2] = {0};

  if (camd_ctl(cmd, sizeof(cmd), reply, sizeof(reply)) != 0)
    return ESP_FAIL;
  ae_hold_sweep = 0;
  ae_frames = 0;
  return reply[0] == 'O' ? ESP_OK : ESP_FAIL;
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
