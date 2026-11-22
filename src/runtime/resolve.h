/* resolve: a cache in front of the lifted code's indirect calls (resolve.c). */
#ifndef RESOLVE_H
#define RESOLVE_H
#include <stdint.h>
#include "recomp_types.h"

/* Once the image is mapped: finds what must bypass the cache. */
void resolve_init(uint32_t image_base);

/* recomp_lookup's answer for va, from the cache when it can be; NULL: not
 * a lifted function (the caller goes on as without the cache). */
recomp_func_t resolve_cached(uint32_t va);

#endif
