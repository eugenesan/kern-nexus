/*
 * Linux entry point for the Nexus 5 port.
 *
 * app_main() is kern's normal startup (main/main.c).  On ESP-IDF it runs
 * in the FreeRTOS app task and the LVGL port owns the timer task.  Here we
 * call it once and then run the LVGL timer loop on the main thread.
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "lvgl.h"
#include "sim_flash.h"
#include "sim_nvs.h"
#include "sim_sdcard.h"

void app_main(void);

static void prepare_tmpfs_paths(void) {
  mkdir("/data/kern", 0700);
  mkdir("/data/kern/nvs", 0700);
  mkdir("/data/kern/flash", 0700);
  mkdir("/data/kern/sd", 0700);
  sim_nvs_set_data_dir("/data/kern/nvs");
  sim_flash_set_data_dir("/data/kern/flash");
  sim_sdcard_set_data_dir("/data/kern/sd");
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  umask(077);
  (void)mlockall(MCL_CURRENT | MCL_FUTURE);
  (void)prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
  prepare_tmpfs_paths();

  app_main();

  for (;;) {
    if (!lvgl_port_lock(0))
      continue;
    uint32_t wait = lv_timer_handler();
    lvgl_port_unlock();
    if (wait > 33)
      wait = 33;
    usleep((wait ? wait : 1) * 1000);
  }
}
