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

/* Append one line to the bounded camera log ring.  Safe to call from the
 * camera thread; truncates to KERN_CAMERA_LOG_LINE-1 characters. */
void kern_camera_log(const char *fmt, ...);

/* Diagnostics for development builds.  Returns a pointer into the ring,
 * valid until the next log call, or NULL when the index is out of range. */
const char *kern_camera_log_line(int index);
int kern_camera_log_count(void);

#define KERN_CAMERA_LOG_ENTRIES 128
#define KERN_CAMERA_LOG_LINE    160

#ifdef __cplusplus
}
#endif

#endif /* KERN_CAMERA_H */
