/* --smooth: objects drawn between simulation steps (smooth.c). */
#ifndef SMOOTH_H
#define SMOOTH_H
#include <stdint.h>
#include "guest.h"

extern int smooth_on;   /* --smooth */

/* After the arguments: opens SWGB_SMOOTH_LOG if it is set. */
void smooth_init(void);

/* Nonzero once smooth_init found --smooth or SWGB_SMOOTH_LOG: only then does the host's
 * recomp_lookup_manual ask smooth_lookup (it runs on every indirect call). */
extern int smooth_hooked;

/* The host's recomp_lookup_manual asks here (after widescreen.c). */
recomp_func_t smooth_lookup(uint32_t va);

#endif
