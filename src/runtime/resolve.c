/*
 * resolve: a cache in front of the lifted code's indirect calls.
 *
 * Every indirect call in the lifted code (a C++ virtual call, a function
 * pointer, `call [IAT]`) resolves its guest address at run time
 * (recomp_resolve, recomp_types.h): the host's recomp_lookup_manual, then
 * native32's recomp_lookup, which walks its replacement list and the native
 * modules and binary-searches the dispatch table of 30,000+ functions. In the
 * menus that search alone was 31% of the game thread, and 13% in game
 * (tools/profile_report.py). pcrecomp's own cache (RECOMP_RESOLVE_CACHE) is
 * compiled into the generated code, so this one sits where the host can put
 * it: recomp_lookup_manual, which recomp_resolve asks first.
 *
 * A direct-mapped table of guest address -> lifted function, filled on a
 * miss from recomp_lookup itself, and only with what the dispatch table says
 * for that address: an answer that depends on more than the address
 * (native32's __local_unwind2 funclet, which depends on the caller) is never
 * kept, and while the caller is a __local_unwind2 the cache is not asked at
 * all. Addresses of native code are kept as "not lifted", so they go on to
 * recomp_lookup and the import path exactly as before. One thread runs
 * lifted code at a time (native32's machine lock), so the table needs no
 * lock of its own.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>

#include "native32.h"
#include "recomp_types.h"
#include "resolve.h"

#define SLOTS 4096                       /* a power of two */
typedef struct { uint32_t va; recomp_func_t fn; } slot_t;
static slot_t g_slot[SLOTS];
static int g_off;                        /* SWGB_NO_RESOLVE_CACHE: the old path, for comparison */

/* __local_unwind2's copies in the image (native32_local_unwind2's pattern):
 * recomp_lookup answers differently while one of them is the caller. */
#define LU2_MAX 16
static uint32_t g_lu2[LU2_MAX];
static int g_nlu2;

static recomp_func_t exact(uint32_t va) {
    uint32_t lo = 0, hi = recomp_dispatch_count;
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2;
        if (recomp_dispatch_table[m].address < va) lo = m + 1; else hi = m;
    }
    return lo < recomp_dispatch_count && recomp_dispatch_table[lo].address == va ? recomp_dispatch_table[lo].func : NULL;
}

void resolve_init(uint32_t image_base) {
    static const uint8_t pre[] = { 0x53, 0x56, 0x57, 0x8B, 0x44, 0x24, 0x10, 0x50, 0x6A, 0xFE, 0x68 };
    static const uint8_t post[] = { 0x64, 0xFF, 0x35, 0, 0, 0, 0, 0x64, 0x89, 0x25, 0, 0, 0, 0 };
    char v[8];
    g_off = GetEnvironmentVariableA("SWGB_NO_RESOLVE_CACHE", v, sizeof v) > 0;
    const uint8_t* b = (const uint8_t*)(uintptr_t)image_base;
    uint32_t size = ((const IMAGE_NT_HEADERS32*)(b + ((const IMAGE_DOS_HEADER*)b)->e_lfanew))->OptionalHeader.SizeOfImage;
    for (uint32_t o = 0x1000; o + 40 < size && g_nlu2 < LU2_MAX; o++)
        if (b[o] == 0x53 && !memcmp(b + o, pre, sizeof pre) && !memcmp(b + o + 15, post, sizeof post))
            g_lu2[g_nlu2++] = image_base + o;
}

recomp_func_t resolve_cached(uint32_t va) {
    if (g_off) return NULL;
    for (int i = 0; i < g_nlu2; i++)
        if (g_cur_func == g_lu2[i]) return NULL;
    slot_t* s = &g_slot[(va ^ (va >> 12)) & (SLOTS - 1)];
    if (s->va == va) return s->fn;
    recomp_func_t f = recomp_lookup(va);
    if (!f || f == exact(va)) { s->va = va; s->fn = f; }
    return f;
}
