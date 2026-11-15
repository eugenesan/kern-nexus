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
static uint32_t s_w = 720, s_h = 960;
static int s_started;

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

static void yuv420_to_rgb565(const uint8_t *yuv, uint32_t w, uint32_t h,
                             uint16_t *out) {
  /* Crucially, only the luma is trustworthy here: camd's chroma plane is
   * the sensor's own format (centred on 16, not 128), and the Python
   * client discards it too.  A grayscale preview is correct and enough
   * for QR/entropy; the decoder uses the same Y. */
  for (uint32_t i = 0; i < w * h; i++) {
    uint8_t y = yuv[i];
    out[i] = (uint16_t)(((y & 0xf8) << 8) | ((y & 0xfc) << 3) | (y >> 3));
  }
}

static void *camera_thread(void *arg) {
  (void)arg;
  uint16_t *rgb = NULL;
  uint32_t rgb_w = 0, rgb_h = 0;
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
    if (w != rgb_w || h != rgb_h) {
      free(rgb);
      rgb = malloc((size_t)w * h * sizeof(*rgb));
      rgb_w = w;
      rgb_h = h;
    }
    if (rgb && s_cb && !s_stop) {
      static int frames;
      if (!frames)
        klog("kern-nexus camera: first frame %ux%u (y=%u uv=%u)\n", w, h,
             ysz, uvsz);
      frames++;
      yuv420_to_rgb565(frame, w, h, rgb);
      s_w = w;
      s_h = h;
      s_cb((uint8_t *)rgb, 0, w, h, (size_t)w * h * 2);
    }
    free(frame);
    /* The scanner's software PPA is expensive; 10 fps keeps the UI
     * responsive and is ample for QR/entropy. */
    usleep(100000);
  }
  return NULL;
}

esp_err_t app_video_init_once(i2c_master_bus_handle_t ignored) {
  (void)ignored;
  if (s_started)
    return ESP_OK;
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
  if (!app_video_is_ready())
    return ESP_ERR_INVALID_STATE;
  if (s_running)
    return ESP_OK;
  s_cb = cb;
  s_stop = 0;
  if (pthread_create(&s_thread, NULL, camera_thread, NULL) != 0)
    return ESP_FAIL;
  s_running = 1;
  return ESP_OK;
}

esp_err_t app_video_stop(void) {
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

uint32_t app_video_get_buf_size(void) { return s_w * s_h * 2; }

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
