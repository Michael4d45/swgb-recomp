/*
 * The runtime a game-specific helper is built on, picked once for every host:
 *
 *   Windows (host.c, softdd.c): pcrecomp's runtime/native32, the real Win32
 *   underneath, a 32-bit process in which a guest address is a host address.
 *
 *   Linux and macOS (src/linux): pcrecomp's runtime/win32hle, which answers
 *   the imports itself. 32-bit (Linux -m32) a guest address is a host
 *   address; 64-bit (macOS, Linux x86-64) the guest's 4 GB is one block of
 *   the host's and every guest address is converted (win32hle.h, hle_ptr).
 *
 * A helper that only touches guest memory and the register file (inflate.c)
 * includes this instead of either header, and turns a guest address into a
 * host pointer with GUEST_PTR. docs/linux-macos.md has the design.
 */
#ifndef SWGB_GUEST_H
#define SWGB_GUEST_H
#include <stdint.h>

#ifdef SWGB_WIN32HLE
#include "win32hle.h"
#define GUEST_PTR(va) hle_ptr((uint32_t)(va))
#else
#include "native32.h"
#define GUEST_PTR(va) ((void*)(uintptr_t)(uint32_t)(va))
#endif

/* A guest dword the host reads on its own account, off the game's own path
 * (the frame pacer, the profiler's sampler), through pointers the game may
 * have let go of since: 0 for an address the guest cannot own, below 64 KB
 * (null and the page Windows never maps) or from 0xC0000000 (the kernel's on
 * Windows, beyond the 3 GB the game was built for). On Windows a page that was
 * freed reads 0 as well (SEH); POSIX has no such cheap guard, so on win32hle
 * the range is the whole check. Both hosts read the same chains through this:
 * the game object (always there once the game runs), then what it points to,
 * each pointer read checked before it is followed. */
#ifdef _WIN32
#include <excpt.h>
static __inline uint32_t guest_read32(uint32_t va) {
    if (va < 0x10000u || va >= 0xC0000000u) return 0;
    __try { return MEM32(va); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
#else
static inline uint32_t guest_read32(uint32_t va) {
    return va < 0x10000u || va >= 0xC0000000u ? 0 : MEM32(va);
}
#endif

#endif
