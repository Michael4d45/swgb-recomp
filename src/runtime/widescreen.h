/* --res: any resolution in place of the game's 1280x1024 (widescreen.c). */
#ifndef WIDESCREEN_H
#define WIDESCREEN_H
#include <stdint.h>
#include "guest.h"

extern int widescreen_w, widescreen_h;   /* 0: --res not given */

/* "WxH" or "desktop"; 0 on a bad value. */
int widescreen_parse(const char* s);

/* The host's recomp_lookup_manual asks here first. */
recomp_func_t widescreen_lookup(uint32_t va);

/* A string from LoadStringA, buf holding n characters of max: the 1280 x 1024
 * the Options dialog names becomes WxH. Returns the new length. */
int widescreen_string(char* buf, int max, int n);

#ifndef SWGB_WIN32HLE
/* The LoadStringA shim (host.c's table). */
void widescreen_LoadStringA(void);
#endif

#endif
