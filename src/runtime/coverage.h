/* What the host does when lifted code reaches code that was not lifted. */
#pragma once

#ifdef SWGB_WIN32HLE
#include <stdint.h>
/* The guest image's end, for "inside the game's image" (the Linux host sets it). */
extern uint32_t coverage_image_end;
#endif

/* Install the miss handler. Call once, after native32_init() (win32hle: recomp_host_init()). */
void coverage_init(void);

/* The last n indirect calls, newest first, to stderr. pcrecomp's
 * native32_dump_icalls compares with (int)g_icall_trace_idx, which is
 * negative after 2^31 calls (about ten minutes of a skirmish) and then prints
 * nothing: this one does not. */
void coverage_dump_icalls(int n);
