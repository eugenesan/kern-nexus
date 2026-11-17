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
	char line[KERN_CAMERA_LOG_LINE];
	int fd;

	pthread_mutex_lock(&kern_camera_log_lock);
	slot = kern_camera_ring_next;
	kern_camera_ring_next =
		(kern_camera_ring_next + 1) % KERN_CAMERA_LOG_ENTRIES;
	if (kern_camera_ring_count < KERN_CAMERA_LOG_ENTRIES)
		kern_camera_ring_count++;
	pthread_mutex_unlock(&kern_camera_log_lock);

	va_start(ap, fmt);
	written = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	snprintf(kern_camera_ring[slot], KERN_CAMERA_LOG_LINE, "%s", line);

	/* Append to a shared file so logs from the exec'd camera child
	 * survive its exit; the in-memory ring only lives in one process. */
	fd = open("/data/local/tmp/kern-camera-runtime.log",
		  O_WRONLY | O_APPEND | O_CREAT, 0600);
	if (fd >= 0) {
		if (write(fd, line, strlen(line)) != (ssize_t)strlen(line))
			(void)0;
		close(fd);
	}
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

void kern_camera_log_write(const char *path)
{
	FILE *out = fopen(path, "w");
	int i, count;

	if (!out)
		return;
	pthread_mutex_lock(&kern_camera_log_lock);
	count = kern_camera_ring_count;
	fprintf(out, "camera log ring: %d line(s)\n", count);
	for (i = 0; i < count; i++) {
		int slot = count == KERN_CAMERA_LOG_ENTRIES
				? kern_camera_ring_next + i
				: i;
		fprintf(out, "%03d %s\n", i,
			kern_camera_ring[slot % KERN_CAMERA_LOG_ENTRIES]);
	}
	pthread_mutex_unlock(&kern_camera_log_lock);
	fclose(out);
}

void kern_camera_log_poll_dump(void)
{
	static time_t last_check;

	if (time(NULL) == last_check)
		return;
	last_check = time(NULL);
	if (access("/data/kern/camera_log_dump", F_OK) != 0)
		return;
	/* Consume the trigger first so repeated polls cannot dump twice. */
	unlink("/data/kern/camera_log_dump");
	kern_camera_log_write("/data/local/tmp/kern-camera.log");
	/* Include the shared runtime log, which also holds lines written by
	 * exec'd camera children that have since exited. */
	{
		FILE *in = fopen("/data/local/tmp/kern-camera-runtime.log", "r");
		if (in) {
			FILE *out = fopen("/data/local/tmp/kern-camera.log", "a");
			char buf[KERN_CAMERA_LOG_LINE];
			if (out) {
				while (fgets(buf, sizeof(buf), in))
					fputs(buf, out);
				fclose(out);
			}
			fclose(in);
		}
	}
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
	kern_camera_log("[camera] child exec start\n");
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

	/* If a previous child is still quiescing, wait for it to exit before
	 * forking a replacement.  Otherwise two camds can fight over the
	 * ISP, or the new client can attach to a dying socket. */
	if (kern_camera_child_pid > 0) {
		int tries;
		for (tries = 0; tries < 200; tries++) {
			if (waitpid(kern_camera_child_pid, NULL, WNOHANG) ==
			    kern_camera_child_pid)
				break;
			if (waitpid(kern_camera_child_pid, NULL, WNOHANG) < 0)
				break;
			usleep(10000);
		}
		kern_camera_child_pid = 0;
	}
	while (waitpid(-1, NULL, WNOHANG) > 0)
		;
	/* The kernel's VFE/CPP release is asynchronous to the process exit.
	 * Give it time to settle before cold-starting a new pipeline. */
	usleep(1000000);
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
