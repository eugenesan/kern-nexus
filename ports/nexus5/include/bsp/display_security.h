#pragma once

/* The ESP BSP wipes its registered LCD buffers.  The nexus5 port owns the
 * framebuffer directly; this is called by main/ui/display_cleanup.c. */
void bsp_display_scrub_buffers(void);
