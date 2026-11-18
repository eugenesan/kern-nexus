/* kern_camera — seam for folding camd into the kern binary.
 *
 * This is the Stage 0 shim: it has no runtime effect by itself.  It gives
 * the future in-process camera code a bounded log ring (the sealed image
 * hides adbd and camd's stderr goes to /dev/null) and a place to put the
 * direct init/start/stop/control calls that replace camd's sockets.
 *
 * The external daemon remains the default; see KERN_CAMERA_IN_PROCESS in
 * ports/nexus5/CMakeLists.txt.
 */
#ifndef KERN_CAMERA_H
#define KERN_CAMERA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stage 1 + respawn hardening: the single binary runs camd in a child
 * process started with fork()+execv("/proc/self/exe", ..., "--camd").
 * Launch returns 0 when the child was created; sockets appear
 * asynchronously, exactly as with the external daemon. */
#ifdef KERN_CAMERA_IN_PROCESS
int kern_camera_launch(void);
int kern_camera_run_child(void);
/* Ask the camera child to abort a pending quiesce (SIGUSR2). */
void kern_camera_cancel_quiesce(void);
int camd_main(int argc, char **argv);
#endif

/* Append one line to the bounded camera log ring.  Safe to call from the
 * camera thread; truncates to KERN_CAMERA_LOG_LINE-1 characters. */
int kern_camera_log(const char *fmt, ...);

/* Diagnostics for development builds.  Returns a pointer into the ring,
 * valid until the next log call, or NULL when the index is out of range. */
const char *kern_camera_log_line(int index);
int kern_camera_log_count(void);

/* On-demand dump for development: if /data/kern/camera_log_dump exists,
 * write the whole ring to /data/local/tmp/kern-camera.log and consume the
 * trigger.  Safe to poll from the main loop; no-op when the ring is empty. */
void kern_camera_log_poll_dump(void);

/* Write the calling process's ring to an explicit path (used by the
 * camera child before it exits). */
void kern_camera_log_write(const char *path);

#define KERN_CAMERA_LOG_ENTRIES 128
#define KERN_CAMERA_LOG_LINE    160

#ifdef __cplusplus
}
#endif

#endif /* KERN_CAMERA_H */
