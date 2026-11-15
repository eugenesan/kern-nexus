/*
 * app_video_* adapter for the Nexus 5.
 *
 * The real implementation is components/camera (MSM V4L2/ION/CPP as an
 * internal pthread).  Until that refactor lands, this stub keeps the rest
 * of the UI linkable and reports the camera as unavailable.  Replace the
 * bodies with calls into kern_camera_*; keep the Krux YUV420 geometry (720x960 default).
 */
#include <stdint.h>

#include "esp_err.h"
#include "video.h"

esp_err_t app_video_init_once(i2c_master_bus_handle_t i2c_bus_handle) {
  (void)i2c_bus_handle;
  return ESP_ERR_NOT_SUPPORTED;
}

bool app_video_is_ready(void) { return false; }

uint32_t app_video_get_buf_size(void) { return 720 * 960 * 3 / 2; }

esp_err_t app_video_get_resolution(uint32_t *width, uint32_t *height) {
  if (width)
    *width = 720;
  if (height)
    *height = 960;
  return ESP_OK;
}

bool app_video_is_streaming(void) { return false; }

esp_err_t app_video_start(app_video_frame_operation_cb_t operation_cb,
                          int core_id) {
  (void)operation_cb;
  (void)core_id;
  return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t app_video_stop(void) { return ESP_OK; }

esp_err_t app_video_set_ae_target(uint32_t level) {
  (void)level;
  return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t app_video_set_focus(uint32_t position) {
  (void)position;
  return ESP_ERR_NOT_SUPPORTED;
}

bool app_video_has_focus_motor(void) { return false; }
bool app_video_has_ae_control(void) { return false; }

uint32_t app_video_ppa_snap_crop(uint32_t crop_max, uint32_t target) {
  (void)target;
  return crop_max;
}
