/* An indirect call or jump to an address with no lifted function.
 *
 * pcrecomp's RECOMP_ICALL prints "ICALL: unresolved VA" and carries on with
 * eax = 0, as if the callee had returned. The callee's stack arguments are
 * never popped (a stdcall method's `ret 0x10`), nothing it would have written
 * is written, and the caller goes on with a zero it takes for a pointer: the
 * fault comes later, in a function that has nothing to do with the miss
 * (0x005648A0, a vtable slot the catalog lost, ended a skirmish with a null
 * read in sub_005C45C0, docs/lifting.md).
 *
 * There is no safe way to run the target. It is inside the image, so the
 * original x86 is there, but run natively it would use the real TEB for the
 * guest's fs:[0] chain, call imports through the bound IAT outside native32's
 * bridges, and reach the host's replacements (the inflate coroutine) as the
 * original code. A wrong answer that looks like a right one is worse than a
 * stop, so the run stops here, at the miss, and says what to seed.
 *
 * The miss reaches us through recomp_eh_jump_hook: the macros ask it whether
 * the address is an exception handler continuing inside a body further up
 * the stack (eh32) before they report a miss, so after eh32 has said no, the
 * address is no function and no handler. SWGB_ICALL_CONTINUE=1 keeps
 * pcrecomp's behaviour (one line per miss, eax = 0, carry on): to collect
 * several misses from one run, knowing the run is no longer trustworthy. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "native32.h"
#include "recomp_trace.h"
#include "softdd.h"
#include "coverage.h"

static int (*g_eh_prev)(uint32_t va);
static int g_continue;

/* The lifted function at or below va (the dispatch table is sorted). */
static uint32_t lifted_below(uint32_t va) {
    uint32_t lo = 0, hi = recomp_dispatch_count;
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2;
        if (recomp_dispatch_table[m].address <= va) lo = m + 1;
        else hi = m;
    }
    return lo ? recomp_dispatch_table[lo - 1].address : 0;
}

static int coverage_miss(uint32_t va) {
    static volatile LONG once;
    static uint32_t seen[64];
    static LONG nseen;
    /* eh32 first: a handler's continuation longjmps there and does not return. */
    if (g_eh_prev && g_eh_prev(va)) return 1;

    if (g_continue) {
        for (LONG i = 0; i < nseen && i < 64; i++)
            if (seen[i] == va) return 0;
        LONG k = InterlockedIncrement(&nseen) - 1;
        if (k < 64) seen[k] = va;
        fprintf(stderr, "[coverage] unresolved indirect call to 0x%08X from sub_%08X: carrying on "
                        "(SWGB_ICALL_CONTINUE): what follows is not trustworthy\n", va, g_cur_func);
        native32_dump_icalls(8);
        return 0;       /* the macro prints its "ICALL: unresolved VA" line, eax = 0 */
    }

    if (InterlockedExchange(&once, 1)) {    /* another thread is already reporting */
        Sleep(INFINITE);
    }
    /* The line seed_from_log.py and tools/seeds_from_logs.py read, first. */
    fprintf(stderr, "\nICALL: unresolved VA 0x%08X from 0x%08X\n", va, g_cur_func);
    fprintf(stderr, "=== unresolved indirect call, thread %lu ===\n", GetCurrentThreadId());
    fprintf(stderr, "  target 0x%08X, from lifted sub_%08X, last native call %s\n",
            va, g_cur_func, g_cur_import ? g_cur_import : "(none)");
    if (va < 0x10000u)
        fprintf(stderr, "  a call through a null (or near-null) pointer: the original faults here too,\n"
                        "  or an earlier miss left a zero where a pointer was\n");
    else if (native32_in_guest(va)) {
        uint32_t below = lifted_below(va);
        fprintf(stderr, "  inside the game's image, no lifted function starts there; nearest below: "
                        "sub_%08X (+0x%X)\n", below, va - below);
        fprintf(stderr, "  if it is a function (a vtable slot, a callback), it is an entry the catalog\n"
                        "  missed: `py -3 tools\\seeds_from_logs.py --merge` adds it to work\\run_seeds.json,\n"
                        "  then re-lift (run_lift.py) and rebuild. docs/lifting.md, \"Pointers the catalog dropped\"\n");
    } else
        fprintf(stderr, "  outside the game's image and no import: a pointer into data or freed memory\n");
    fprintf(stderr, "  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
    if (g_esp >= 0x10000u && !IsBadReadPtr((void*)(uintptr_t)g_esp, 16))
        fprintf(stderr, "  stack: %08X %08X %08X %08X\n",
                MEM32(g_esp), MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp + 12));
    native32_dump_icalls(16);
    recomp_dump_trace("unresolved indirect call");
    fprintf(stderr, "stopped at the miss (exit code 5) rather than run on with eax = 0 into an unrelated fault;\n"
                    "SWGB_ICALL_CONTINUE=1 carries on instead\n");
    fflush(stderr);
    recomp_trace_flush();
    softdd_finish();
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 5);
    return 0;
}

void coverage_init(void) {
    const char* c = getenv("SWGB_ICALL_CONTINUE");
    g_continue = c && *c && *c != '0';
    g_eh_prev = recomp_eh_jump_hook;
    recomp_eh_jump_hook = coverage_miss;
}
