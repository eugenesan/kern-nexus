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

static pthread_mutex_t kern_camera_log_lock = PTHREAD_MUTEX_INITIALIZER;
static char kern_camera_ring[KERN_CAMERA_LOG_ENTRIES][KERN_CAMERA_LOG_LINE];
static int kern_camera_ring_count;
static int kern_camera_ring_next;

void kern_camera_log(const char *fmt, ...)
{
	va_list ap;
	int slot;

	pthread_mutex_lock(&kern_camera_log_lock);
	slot = kern_camera_ring_next;
	kern_camera_ring_next =
		(kern_camera_ring_next + 1) % KERN_CAMERA_LOG_ENTRIES;
	if (kern_camera_ring_count < KERN_CAMERA_LOG_ENTRIES)
		kern_camera_ring_count++;
	pthread_mutex_unlock(&kern_camera_log_lock);

	va_start(ap, fmt);
	vsnprintf(kern_camera_ring[slot], KERN_CAMERA_LOG_LINE, fmt, ap);
	va_end(ap);
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
