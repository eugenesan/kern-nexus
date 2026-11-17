/* Stage 0 of folding camd into kern: an inert, bounded log ring.
 *
 * No constructors, no ioctls, no threads, no global side effects beyond
 * the ring itself.  It is compiled into every kern build but is only used
 * when KERN_CAMERA_IN_PROCESS is enabled in a later stage.
 */
#include "kern_camera.h"

#include <pthread.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static pthread_mutex_t kern_camera_log_lock = PTHREAD_MUTEX_INITIALIZER;
static char kern_camera_ring[KERN_CAMERA_LOG_ENTRIES][KERN_CAMERA_LOG_LINE];
static int kern_camera_ring_count;
static int kern_camera_ring_next;

int kern_camera_log(const char *fmt, ...)
{
	va_list ap;
	int slot, written;

	pthread_mutex_lock(&kern_camera_log_lock);
	slot = kern_camera_ring_next;
	kern_camera_ring_next =
		(kern_camera_ring_next + 1) % KERN_CAMERA_LOG_ENTRIES;
	if (kern_camera_ring_count < KERN_CAMERA_LOG_ENTRIES)
		kern_camera_ring_count++;
	pthread_mutex_unlock(&kern_camera_log_lock);

	va_start(ap, fmt);
	written = vsnprintf(kern_camera_ring[slot], KERN_CAMERA_LOG_LINE,
			    fmt, ap);
	va_end(ap);
	return written;
}

const char *kern_camera_log_line(int index)
{
	const char *line = NULL;
	int count;

	pthread_mutex_lock(&kern_camera_log_lock);
	count = kern_camera_ring_count;
	if (index >= 0 && index < count) {
		/* Entry 0 is the oldest retained line. */
		int start = count == KERN_CAMERA_LOG_ENTRIES
				? kern_camera_ring_next : 0;
		line = kern_camera_ring[(start + index) %
					KERN_CAMERA_LOG_ENTRIES];
	}
	pthread_mutex_unlock(&kern_camera_log_lock);
	return line;
}

int kern_camera_log_count(void)
{
	return kern_camera_ring_count;
}

#ifdef KERN_CAMERA_IN_PROCESS

/* The benchmark command line used by app_video_init_once for the external
 * daemon.  Keep it byte-identical: Stage 1 only moves the process boundary,
 * it does not change the camera configuration. */
static void klog_line(const char *msg)
{
#ifndef KERN_CAMERA_TRACE
	(void)msg;
#else
	int fd = open("/dev/kmsg", O_WRONLY | O_APPEND);
	int tf = open("/data/local/tmp/respawn-trace.log",
		       O_WRONLY | O_APPEND | O_CREAT, 0600);
	if (fd >= 0) { write(fd, msg, strlen(msg)); close(fd); }
	if (tf >= 0) { write(tf, msg, strlen(msg)); close(tf); }
#endif
}

/* Entry point for the exec'd camera child.  The child process image is a
 * fresh copy of this binary invoked as "kern-nexus --camd", so camd's
 * globals start zeroed on every quiesce/respawn cycle. */
int kern_camera_run_child(void)
{
	char *argv[] = {
		(char *)"camd",
		(char *)"1640", (char *)"1232", (char *)"0x40000000",
		(char *)"rear", (char *)"0",
		(char *)"/lib/firmware/cpp_firmware_v1_2_0.fw",
		(char *)"2", (char *)"binned", (char *)"cfg",
		(char *)"nocycle", (char *)"0", (char *)"0x2b",
		(char *)"0", (char *)"0", (char *)"300", (char *)"0",
		(char *)"0", (char *)"0", (char *)"0x12", (char *)"0",
		(char *)"1", (char *)"0xffffffff", (char *)"0",
		(char *)"0", (char *)"0", (char *)"1", (char *)"0",
	};

	klog_line("[SPAWN] child: camd_main start (exec'd)\n");
	camd_main((int)(sizeof(argv) / sizeof(argv[0])), argv);
	klog_line("[SPAWN] child: camd_main returned\n");
	return 0;
}

/* Fork + exec of /proc/self/exe.  Between fork() and execv() the child
 * calls only async-signal-safe functions (execv, write, _exit), which is
 * the POSIX-safe way to spawn from a multithreaded process. */
static pid_t kern_camera_child_pid;

int kern_camera_launch(void)
{
	pid_t pid;
	char *const argv[] = { (char *)"kern-nexus", (char *)"--camd", NULL };

	/* Reap any previous quiesced child before spawning another. */
	while (waitpid(-1, NULL, WNOHANG) > 0)
		;
	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		execv("/proc/self/exe", argv);
		_exit(127);
	}
	kern_camera_child_pid = pid;
	{
		char b[96];
		snprintf(b, sizeof(b), "[SPAWN] parent exec'd child pid=%d\n", pid);
		klog_line(b);
	}
	return 0;
}

#endif /* KERN_CAMERA_IN_PROCESS */
