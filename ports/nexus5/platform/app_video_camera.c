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

/* --- exposure -----------------------------------------------------------
 *
 * camd exposes the sensor's exposure and gain over its control socket but
 * has no auto-exposure of its own, and the sensor comes up at a very short
 * exposure.  In daylight that is still usable; indoors it is a black frame.
 * The sensor's default is therefore replaced with a mid-range exposure on
 * start and nudged from the frames we already measure, so a dim room still
 * shows a picture and a QR still decodes.
 */
#define CAMD_CTL_SOCK "/data/kern/camd-ctl.sock"
#define AE_TARGET 80
#define AE_DEAD_BAND 20
#define AE_STEP_FRAMES 3
#define AE_EXP_MAX 0x0FFF
#define AE_GAIN_MAX 0x00FF

static int ae_exp = 0x0800;
static int ae_gain = 0x0080;
static int ae_frames;

static int camd_ctl_exposure(int exp, int gain) {
  struct sockaddr_un addr;
  unsigned char cmd[5];
  char reply[2] = {0};
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
  cmd[0] = 'E';
  cmd[1] = (unsigned char)(exp >> 8);
  cmd[2] = (unsigned char)(exp & 0xFF);
  cmd[3] = (unsigned char)(gain >> 8);
  cmd[4] = (unsigned char)(gain & 0xFF);
  if (write(s, cmd, sizeof(cmd)) != (ssize_t)sizeof(cmd)) {
    close(s);
    return -1;
  }
  if (read(s, reply, sizeof(reply)) != (ssize_t)sizeof(reply) ||
      reply[0] != 'O') {
    close(s);
    return -1;
  }
  close(s);
  return 0;
}

/* One step toward the target, from the frame we just read.  Runs on the
 * camera thread, so the socket work cannot touch the UI. */
static void ae_update(const uint8_t *y, size_t len) {
  unsigned long sum = 0;
  size_t n = 0;
  int mean;

  if (!y || len == 0)
    return;
  if (++ae_frames < AE_STEP_FRAMES)
    return;
  ae_frames = 0;

  for (size_t i = 0; i < len; i += 97) {
    sum += y[i];
    n++;
  }
  if (n == 0)
    return;
  mean = (int)(sum / n);
  if (mean < AE_TARGET - AE_DEAD_BAND) {
    ae_exp = ae_exp + ae_exp / 2 + 32;
    ae_gain = ae_gain + ae_gain / 2 + 8;
  } else if (mean > AE_TARGET + AE_DEAD_BAND) {
    ae_exp = ae_exp - ae_exp / 4;
    ae_gain = ae_gain - ae_gain / 4;
  } else {
    return;
  }
  if (ae_exp < 1)
    ae_exp = 1;
  if (ae_exp > AE_EXP_MAX)
    ae_exp = AE_EXP_MAX;
  if (ae_gain < 1)
    ae_gain = 1;
  if (ae_gain > AE_GAIN_MAX)
    ae_gain = AE_GAIN_MAX;
  (void)camd_ctl_exposure(ae_exp, ae_gain);
}

static void ae_start(void) {
  ae_exp = 0x0800;
  ae_gain = 0x0080;
  ae_frames = 0;
  (void)camd_ctl_exposure(ae_exp, ae_gain);
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
    ae_update(frame, (size_t)ysz);
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
  (void)level;
  return ESP_OK;
}

esp_err_t app_video_set_focus(uint32_t position) {
  (void)position;
  return ESP_OK;
}

bool app_video_has_focus_motor(void) { return false; }
bool app_video_has_ae_control(void) { return false; }

uint32_t app_video_ppa_snap_crop(uint32_t crop_max, uint32_t target) {
  return crop_max < target ? crop_max : target;
}
