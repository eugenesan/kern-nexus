/*
 * kern-init -- the hammerhead init stub for kern-nexus.
 *
 * TWRP's init stays PID 1.  This program runs as the `kern` service and
 * chooses between TWRP and the static kern binary:
 *
 *   standard image: no key -> TWRP; volume-up / start-kern -> kern
 *   development image: no key -> kern; volume-up -> TWRP
 *
 * It is not PID 1 and never mounts Android.  `/data` is mounted by TWRP
 * for the development image; the standard image runs entirely from the
 * ramdisk and tmpfs.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <linux/input.h>

#define KEYS_NODE "/dev/input/event2"
#define KEY_WINDOW_MS 8000

#ifdef KERN_SEAL
#define USB_WAIT_S 0
#else
#define USB_WAIT_S 8
#endif

#define KERN_DIR "/data/kern"
#define BUNDLED_KERN_DIR "/opt/kern"
#define KERN_BINARY "kern-nexus"
#define TWRP_BINARY "/sbin/recovery.stock"
#define AUTOSTART_FILE "/data/kern/state/autostart"

#ifdef KERN_RELEASE
#define KERN_LOG "/dev/null"
#else
#define KERN_LOG "/data/kern/kern.log"
#endif

static void say(const char *fmt, ...) {
#ifdef KERN_RELEASE
  (void)fmt;
#else
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
#endif
}

static int standard_image(void) {
#ifdef DEFAULT_STANDARD
  return 1;
#else
  return 0;
#endif
}

static int autostart_kern(void) {
  int autostart = !standard_image();
#ifndef KERN_RELEASE
  FILE *f = fopen(AUTOSTART_FILE, "r");
  if (f) {
    int v = -1;
    if (fscanf(f, "%d", &v) == 1 && (v == 0 || v == 1))
      autostart = v;
    fclose(f);
  }
#endif
  return autostart;
}

static int run_program(const char *path, char *const argv[]) {
  pid_t pid = fork();
  int status;
  if (pid < 0)
    return -1;
  if (pid == 0) {
    execv(path, argv);
    _exit(127);
  }
  if (waitpid(pid, &status, 0) == pid && WIFEXITED(status))
    return WEXITSTATUS(status);
  return -1;
}

static int stop_service(const char *name) {
  char *argv[4] = {(char *)"setprop", (char *)"ctl.stop", (char *)name, NULL};
  return run_program("/sbin/setprop", argv);
}

/* TWRP's process has argv[0] == "recovery" (the stub execs
 * /sbin/recovery.stock with that argv[0]). */
static pid_t find_twrp(void) {
  DIR *proc = opendir("/proc");
  struct dirent *entry;
  if (!proc)
    return -1;
  while ((entry = readdir(proc)) != NULL) {
    char path[64], cmdline[64];
    ssize_t n;
    int fd;
    if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
      continue;
    snprintf(path, sizeof(path), "/proc/%s/cmdline", entry->d_name);
    fd = open(path, O_RDONLY);
    if (fd < 0)
      continue;
    n = read(fd, cmdline, sizeof(cmdline) - 1);
    close(fd);
    if (n <= 0)
      continue;
    cmdline[n] = '\0';
    if (strcmp(cmdline, "recovery") == 0) {
      pid_t pid = (pid_t)atoi(entry->d_name);
      closedir(proc);
      return pid;
    }
  }
  closedir(proc);
  return -1;
}

static void stop_twrp(void) {
  int i;
  pid_t pid;
  stop_service("kern");
  stop_service("recovery");
  for (i = 0; i < 50; i++) {
    if (find_twrp() < 0)
      return;
    usleep(100000);
  }
  pid = find_twrp();
  if (pid > 0) {
    say("TWRP still running; killing %d", pid);
    kill(pid, SIGKILL);
    sleep(1);
  }
}

static int the_key_is_down(int fd) {
  unsigned char state[128];
  memset(state, 0, sizeof(state));
  if (ioctl(fd, EVIOCGKEY(sizeof(state)), state) < 0)
    return 0;
  if (KEY_VOLUMEUP / 8 >= (int)sizeof(state))
    return 0;
  return (state[KEY_VOLUMEUP / 8] >> (KEY_VOLUMEUP % 8)) & 1;
}

static int wait_for_key(int window_ms) {
  struct pollfd pfd;
  int fd = open(KEYS_NODE, O_RDONLY | O_NONBLOCK);
  int elapsed = 0;
  if (fd < 0) {
    say("no key device");
    return 0;
  }
  if (the_key_is_down(fd)) {
    close(fd);
    return 1;
  }
  while (elapsed < window_ms) {
    struct input_event ev;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, 200) > 0 && (pfd.revents & POLLIN)) {
      if (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev) &&
          ev.type == EV_KEY && ev.code == KEY_VOLUMEUP && ev.value != 0) {
        close(fd);
        return 1;
      }
    }
    elapsed += 200;
  }
  close(fd);
  return 0;
}

static void start_twrp(void) {
  say("starting TWRP");
  execl(TWRP_BINARY, "recovery", (char *)NULL);
  say("could not start TWRP");
}

static void mount_data(void) {
  mkdir("/data", 0755);
  if (mount("/dev/block/bootdevice/by-name/userdata", "/data", "ext4", 0,
            NULL) != 0 &&
      errno != EBUSY)
    say("could not mount /data");
}

static void set_backlight_half(void) {
  FILE *m = fopen("/sys/class/backlight/lm3630/max_brightness", "r");
  FILE *f = fopen("/sys/class/backlight/lm3630/brightness", "w");
  int maximum = 255;
  if (m) {
    if (fscanf(m, "%d", &maximum) != 1 || maximum <= 0)
      maximum = 255;
    fclose(m);
  }
  if (f) {
    fprintf(f, "%d", maximum / 2);
    fclose(f);
  }
}

static void start_kern(int stop_twrp_first) {
  const char *dir;
  char binary[PATH_MAX];
  int bundled = access(BUNDLED_KERN_DIR "/" KERN_BINARY, X_OK) == 0;
  int on_data = access(KERN_DIR "/" KERN_BINARY, X_OK) == 0;

  if (stop_twrp_first) {
    stop_twrp();
#ifndef KERN_RELEASE
    mount_data();
#endif
  }
  if (standard_image() && bundled) {
    dir = BUNDLED_KERN_DIR;
  } else if (on_data) {
    dir = KERN_DIR;
  } else if (bundled) {
    dir = BUNDLED_KERN_DIR;
  } else {
    say("no kern binary on /data or /opt");
    return;
  }

  set_backlight_half();
  snprintf(binary, sizeof(binary), "%s/%s", dir, KERN_BINARY);
  say("starting %s", binary);
  chdir(dir);
#ifdef KERN_RELEASE
  setenv("KERN_RELEASE", "1", 1);
#else
  setenv("KERN_RELEASE", "0", 1);
#endif
  setenv("KERN_DELIVERY", dir == BUNDLED_KERN_DIR ? "bundled" : "development",
         1);
  execl(binary, KERN_BINARY, (char *)NULL);
  say("could not start kern");
}

int main(int argc, char **argv) {
  {
    struct rlimit no_core = {0, 0};
    setrlimit(RLIMIT_CORE, &no_core);
  }

#ifdef KERN_SEAL
  /* Hide adbd in every path; start-adb restores it. */
  if (rename("/sbin/adbd", "/sbin/adbd.off") != 0 && errno != ENOENT)
    say("could not hide adbd");
  stop_service("adbd");
#endif

  if (argc > 1 && strcmp(argv[1], "start") == 0) {
    start_kern(1);
    return 0;
  }

  sleep(USB_WAIT_S);
  mount_data();

  if (autostart_kern()) {
    if (wait_for_key(KEY_WINDOW_MS))
      start_twrp();
    else
      start_kern(0);
    return 0;
  }

  if (wait_for_key(KEY_WINDOW_MS)) {
    say("volume-up: kern");
    start_kern(0);
  } else {
    say("no key: TWRP");
    start_twrp();
  }
  return 0;
}
