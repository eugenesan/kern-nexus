/* Stage 0 of folding camd into kern: an inert, bounded log ring.
 *
 * No constructors, no ioctls, no threads, no global side effects beyond
 * the ring itself.  It is compiled into every kern build but is only used
 * when KERN_CAMERA_IN_PROCESS is enabled in a later stage.
 */
#include "kern_camera.h"

#include <pthread.h>
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

	if (index < 0 || index >= kern_camera_ring_count)
		return NULL;
	pthread_mutex_lock(&kern_camera_log_lock);
	/* Entry 0 is the oldest retained line. */
	int start = kern_camera_ring_count == KERN_CAMERA_LOG_ENTRIES
			? kern_camera_ring_next : 0;
	line = kern_camera_ring[(start + index) % KERN_CAMERA_LOG_ENTRIES];
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
static void *kern_camera_thread(void *arg)
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
	(void)arg;
	kern_camera_log("kern_camera: camd_main on dedicated thread\n");
	camd_main((int)(sizeof(argv) / sizeof(argv[0])), argv);
	return NULL;
}

/* The camera runs as a child of the *same* kern binary.  This keeps the
 * single-binary delivery while giving the camera its own address space:
 * a full pipeline teardown can exit the child, which is the only sequence
 * proven to leave the 3.4 kernel's ISP/CPP/write-master state clean for a
 * cold init (see components/camera/re/EXPERIMENT-2026-11-16.md). */
static pid_t kern_camera_child_pid;

int kern_camera_launch(void)
{
	pid_t pid;

	/* Reap any previous quiesced child before spawning another. */
	waitpid(-1, NULL, WNOHANG);
	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		kern_camera_thread(NULL);
		_exit(0);
	}
	kern_camera_child_pid = pid;
	kern_camera_log("kern_camera: forked camera child pid=%d\n", pid);
	return 0;
}

#endif /* KERN_CAMERA_IN_PROCESS */
