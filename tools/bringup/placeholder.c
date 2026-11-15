/* Minimal fb0 placeholder for the kern-nexus boot-chain test. */
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void) {
  int fd = open("/dev/graphics/fb0", O_RDWR);
  if (fd < 0) { perror("fb0"); return 1; }
  struct fb_var_screeninfo var;
  struct fb_fix_screeninfo fix;
  if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 ||
      ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) { perror("fb info"); return 1; }
  size_t size = fix.smem_len ? fix.smem_len
                             : (size_t)fix.line_length * var.yres;
  size = (size + 4095) & ~(size_t)4095;
  uint32_t *fb = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (fb == MAP_FAILED) { perror("mmap"); return 1; }
  for (unsigned y = 0; y < var.yres; y++)
    for (unsigned x = 0; x < var.xres; x++)
      fb[(size_t)y * (fix.line_length / 4) + x] =
          ((x / 120 + y / 120) & 1) ? 0x0000d000u : 0x00000040u;
  ioctl(fd, FBIOPAN_DISPLAY, &var);
  int k = open("/dev/kmsg", O_WRONLY);
  if (k >= 0) { dprintf(k, "kern-nexus placeholder: fb0 %ux%u stride %u\n", var.xres, var.yres, fix.line_length); close(k); }
  for (;;) pause();
}
