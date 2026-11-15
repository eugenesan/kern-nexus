/*
 * BSP for the LG Nexus 5 (hammerhead).
 *
 * Display: /dev/graphics/fb0, 1080x1920, 32 bpp, stride 4352.  LVGL's
 * Linux fbdev driver draws into it; the MSM panel additionally needs
 * MSMFB_DISPLAY_COMMIT (and FBIOPAN_DISPLAY when the buffer changes).
 *
 * Touch/keys: LVGL evdev on /dev/input/event1 and event2.
 * Power: sysfs battery/charger, lm3630 backlight, reboot(2) for poweroff.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <time.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "bsp/pmic.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#if LV_USE_LINUX_FBDEV
#include "src/drivers/display/fb/lv_linux_fbdev.h"
#endif
#if LV_USE_EVDEV
#include "src/drivers/evdev/lv_evdev.h"
#endif

#define FB_NODE "/dev/graphics/fb0"
#define TOUCH_NODE "/dev/input/event1"
#define KEY_NODE "/dev/input/event2"
#define BACKLIGHT "/sys/class/backlight/lm3630/brightness"
#define BACKLIGHT_MAX "/sys/class/backlight/lm3630/max_brightness"

#ifndef MSMFB_IOCTL_MAGIC
#define MSMFB_IOCTL_MAGIC 'm'
#endif
struct mdp_display_commit {
  uint32_t flags;
  uint32_t wait_for_finish;
  struct fb_var_screeninfo var;
};
#define MSMFB_DISPLAY_COMMIT \
  _IOW(MSMFB_IOCTL_MAGIC, 164, struct mdp_display_commit)

static pthread_mutex_t s_lvgl_mutex;
static pthread_once_t s_lvgl_once = PTHREAD_ONCE_INIT;
static lv_display_t *s_display;
static int s_fb_fd = -1;
static void *s_fb_map;
static size_t s_fb_size;

static void init_mutex(void) {
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&s_lvgl_mutex, &attr);
  pthread_mutexattr_destroy(&attr);
}

static int read_int_file(const char *path, int fallback) {
  FILE *f = fopen(path, "r");
  int value = fallback;
  if (f) {
    if (fscanf(f, "%d", &value) != 1)
      value = fallback;
    fclose(f);
  }
  return value;
}

static void write_int_file(const char *path, int value) {
  FILE *f = fopen(path, "w");
  if (f) {
    fprintf(f, "%d", value);
    fclose(f);
  }
}

static int open_fb(void) {
  if (s_fb_fd >= 0)
    return 0;
  s_fb_fd = open(FB_NODE, O_RDWR);
  if (s_fb_fd < 0)
    return -1;
  struct fb_fix_screeninfo fix;
  if (ioctl(s_fb_fd, FBIOGET_FSCREENINFO, &fix) == 0) {
    s_fb_size = fix.smem_len ? fix.smem_len : (size_t)fix.line_length * 1920;
    s_fb_map = mmap(NULL, s_fb_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                    s_fb_fd, 0);
    if (s_fb_map == MAP_FAILED) {
      s_fb_map = NULL;
      close(s_fb_fd);
      s_fb_fd = -1;
      return -1;
    }
  }
  return 0;
}

static void panel_commit(void) {
  if (s_fb_fd < 0)
    return;
  struct mdp_display_commit dc;
  struct fb_var_screeninfo var;
  if (ioctl(s_fb_fd, FBIOGET_VSCREENINFO, &var) < 0)
    return;
  memset(&dc, 0, sizeof(dc));
  dc.var = var;
  dc.var.xoffset = 0;
  dc.var.yoffset = var.yoffset;
  dc.var.xres = var.xres;
  dc.var.yres = var.yres;
  ioctl(s_fb_fd, MSMFB_DISPLAY_COMMIT, &dc);
  ioctl(s_fb_fd, FBIOPAN_DISPLAY, &var);
}

/* ---------- lvgl_port API ---------- */

esp_err_t lvgl_port_init(const lvgl_port_cfg_t *cfg) {
  (void)cfg;
  pthread_once(&s_lvgl_once, init_mutex);
  return ESP_OK;
}

esp_err_t lvgl_port_deinit(void) { return ESP_OK; }

bool lvgl_port_lock(uint32_t timeout_ms) {
  pthread_once(&s_lvgl_once, init_mutex);
  if (timeout_ms == 0)
    return pthread_mutex_lock(&s_lvgl_mutex) == 0;
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += (long)(timeout_ms / 1000);
  ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_sec++;
    ts.tv_nsec -= 1000000000L;
  }
  return pthread_mutex_timedlock(&s_lvgl_mutex, &ts) == 0;
}

void lvgl_port_unlock(void) { pthread_mutex_unlock(&s_lvgl_mutex); }

void lvgl_port_flush_ready(lv_display_t *disp) { (void)disp; }

/* ---------- display ---------- */

lv_display_t *bsp_display_start(void) {
  lv_init();
  pthread_once(&s_lvgl_once, init_mutex);
  open_fb();
  s_display = lv_linux_fbdev_create();
  if (s_display)
    lv_linux_fbdev_set_file(s_display, FB_NODE);
#if LV_USE_EVDEV
  (void)lv_evdev_create(LV_INDEV_TYPE_POINTER, TOUCH_NODE);
  (void)lv_evdev_create(LV_INDEV_TYPE_KEYPAD, KEY_NODE);
#endif
  return s_display;
}

bool bsp_display_lock(uint32_t timeout_ms) { return lvgl_port_lock(timeout_ms); }

void bsp_display_unlock(void) {
  panel_commit();
  lvgl_port_unlock();
}

esp_err_t bsp_display_brightness_init(void) { return ESP_OK; }

esp_err_t bsp_display_brightness_set(int pct) {
  if (pct < 0)
    pct = 0;
  if (pct > 100)
    pct = 100;
  int maximum = read_int_file(BACKLIGHT_MAX, 255);
  write_int_file(BACKLIGHT, maximum * pct / 100);
  return ESP_OK;
}

esp_err_t bsp_display_backlight_on(void) {
  return bsp_display_brightness_set(100);
}

esp_err_t bsp_display_backlight_off(void) {
  write_int_file(BACKLIGHT, 0);
  return ESP_OK;
}

void bsp_display_scrub_buffers(void) {
  if (open_fb() != 0 || !s_fb_map)
    return;
  memset(s_fb_map, 0, s_fb_size);
  msync(s_fb_map, s_fb_size, MS_SYNC);
  panel_commit();
}

esp_err_t bsp_wifi_coproc_disable(void) { return ESP_OK; }

/* ---------- I2C / PMIC ---------- */

esp_err_t bsp_i2c_init(void) { return ESP_OK; }
esp_err_t bsp_i2c_deinit(void) { return ESP_OK; }
i2c_master_bus_handle_t bsp_i2c_get_handle(void) {
  return (i2c_master_bus_handle_t)1;
}

esp_err_t bsp_pmic_init(void) { return ESP_OK; }

esp_err_t bsp_pmic_power_off(void) {
  sync();
  reboot(RB_POWER_OFF);
  return ESP_FAIL;
}

esp_err_t bsp_pmic_get_battery_percent(uint8_t *pct) {
  if (!pct)
    return ESP_ERR_INVALID_ARG;
  *pct = (uint8_t)read_int_file("/sys/class/power_supply/battery/capacity", 0);
  return ESP_OK;
}

esp_err_t bsp_pmic_get_battery_mv(uint16_t *mv) {
  if (!mv)
    return ESP_ERR_INVALID_ARG;
  *mv = (uint16_t)(read_int_file("/sys/class/power_supply/battery/voltage_now",
                                 0) /
                   1000);
  return ESP_OK;
}

esp_err_t bsp_pmic_get_charge_status(bsp_pmic_chg_t *status) {
  if (!status)
    return ESP_ERR_INVALID_ARG;
  char buf[32] = {0};
  FILE *f = fopen("/sys/class/power_supply/battery/status", "r");
  if (!f) {
    *status = BSP_PMIC_CHG_ABSENT;
    return ESP_OK;
  }
  if (fscanf(f, "%31s", buf) != 1)
    buf[0] = '\0';
  fclose(f);
  if (strcmp(buf, "Charging") == 0)
    *status = BSP_PMIC_CHG_CHARGING;
  else if (strcmp(buf, "Full") == 0)
    *status = BSP_PMIC_CHG_FULL;
  else if (strcmp(buf, "Discharging") == 0)
    *status = BSP_PMIC_CHG_DISCHARGING;
  else
    *status = BSP_PMIC_CHG_ABSENT;
  return ESP_OK;
}

bool bsp_pmic_is_vbus_present(void) {
  return read_int_file("/sys/class/power_supply/usb/online", 0) != 0;
}

bool bsp_pmic_is_available(void) { return true; }
bool bsp_pmic_can_power_off(void) { return true; }
