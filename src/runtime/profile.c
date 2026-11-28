/*
 * profile: where the game thread's time goes, by sampling (--profile FILE).
 *
 * A thread of its own wakes about every millisecond (a high-resolution
 * waitable timer, at a random point of each interval), suspends the game thread, reads its EIP and ESP
 * (GetThreadContext), copies the top of its host stack, and lets it go. The
 * lifted code is ordinary C on the host stack, one C function per guest
 * function, so the stack holds the chain of lifted callers: values that point
 * into the host executable just after a call instruction are kept as frames
 * (the generated code has no frame pointers, so the stack is scanned and a
 * few stale return addresses come along; the chain is what to read).
 *
 * Each sample is recorded as: the game's mode (2 menus, 4 in game, ...);
 * whether the thread ran since the last sample (its cycle count moved: a
 * sample of a thread asleep in MsgWaitForMultipleObjects is not CPU time);
 * the native call in progress (g_cur_import) when EIP is outside the
 * executable; EIP; and the frames, innermost first. Identical samples are
 * counted together. Nothing is resolved here: the file lists the modules and
 * raw host addresses, and tools/profile_report.py names them with the PDB
 * (lifted functions are sub_<VA>, so the guest address comes with the name).
 *
 * While the game thread is suspended this thread takes no lock and allocates
 * nothing: the game thread may hold the heap's. The file is rewritten every
 * 10 s and at the end (the watchdog, ExitProcess, main's return).
 *
 * The scan, the table, the file and the options are profile_core.c's, which
 * the Linux host's sampler (src/linux/profile.c) shares; this file is how
 * Windows stops the thread and reads it.
 *
 *   --profile FILE              sample the whole run
 *   --profile-window A:B        only seconds A to B after the start
 *   --profile-hz N              samples a second (default 1000)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <psapi.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native32.h"
#include "recomp_trace.h"
#include "profile.h"
#include "profile_core.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static HANDLE    g_thread;          /* the game thread */
static uintptr_t g_stack_base;      /* its host stack's top */

static uint64_t g_cpu_thread0, g_cpu_proc0, g_cpu_thread1, g_cpu_proc1;   /* 100 ns, over the window */
static CRITICAL_SECTION g_cs;
static volatile LONG g_done;
static volatile LONG g_started;     /* profile_start ran: g_cs is there */

static uint64_t ft64(FILETIME f) { return (uint64_t)f.dwHighDateTime << 32 | f.dwLowDateTime; }
static void cpu_now(uint64_t* thread, uint64_t* proc) {
    FILETIME c, e, k, u;
    *thread = GetThreadTimes(g_thread, &c, &e, &k, &u) ? ft64(k) + ft64(u) : 0;
    *proc = GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u) ? ft64(k) + ft64(u) : 0;
}

static void write_modules(FILE* o) {
    HMODULE mods[512];
    DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return;
    for (DWORD i = 0; i < need / sizeof mods[0] && i < 512; i++) {
        MODULEINFO mi;
        char name[MAX_PATH];
        if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi)) continue;
        if (!GetModuleFileNameA(mods[i], name, sizeof name)) continue;
        fprintf(o, "module %08X %08X %s\n", (uint32_t)(uintptr_t)mi.lpBaseOfDll, (uint32_t)mi.SizeOfImage, name);
    }
}

static void write_file(void) {
    profile_write((g_cpu_thread1 - g_cpu_thread0) / 1e7, (g_cpu_proc1 - g_cpu_proc0) / 1e7, write_modules);
}

static DWORD WINAPI sampler(LPVOID unused) {
    (void)unused;
    static uint8_t buf[PROFILE_COPY];
    uint32_t f[PROFILE_MAXF];
    HANDLE tm = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!tm) tm = CreateWaitableTimerW(NULL, FALSE, NULL);
    LARGE_INTEGER due;
    uint32_t rng = GetTickCount() | 1;
    LARGE_INTEGER qf, t0, t, last_t;
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&t0);
    last_t = t0;
    ULONG64 last_cyc = 0, cyc;
    uint64_t last_tsc = __rdtsc(), tsc;
    QueryThreadCycleTime(g_thread, &last_cyc);
    int in_window = 0;
    DWORD next_write = GetTickCount() + 10000;
    while (!g_done) {
        /* one shot at a random point of each interval (0.5 to 1.5 of it): a
         * steady 1 ms beat samples a thread that paces itself to the same
         * clock interrupt always at the same moment, just before it wakes */
        rng = rng * 1103515245u + 12345u;
        due.QuadPart = -(int64_t)(10000000LL / profile_hz * (512 + (rng >> 22)) / 1024);
        SetWaitableTimer(tm, &due, 0, NULL, NULL, FALSE);
        if (WaitForSingleObject(tm, 50) != WAIT_OBJECT_0) continue;
        QueryPerformanceCounter(&t);
        double at = (double)(t.QuadPart - t0.QuadPart) / qf.QuadPart;
        QueryThreadCycleTime(g_thread, &cyc);
        tsc = __rdtsc();
        /* ran for at least a quarter of the interval: CPU time, not a wait */
        uint8_t run = (cyc - last_cyc) * 4 > (tsc - last_tsc);
        last_cyc = cyc;
        last_tsc = tsc;
        if (at < profile_from || at > profile_to) {
            if (in_window && at > profile_to) { EnterCriticalSection(&g_cs); cpu_now(&g_cpu_thread1, &g_cpu_proc1); LeaveCriticalSection(&g_cs); in_window = 0; }
            last_t = t;
            continue;
        }
        EnterCriticalSection(&g_cs);
        if (!in_window) { cpu_now(&g_cpu_thread0, &g_cpu_proc0); in_window = 1; last_t = t; }
        profile_wall += (double)(t.QuadPart - last_t.QuadPart) / qf.QuadPart;
        last_t = t;
        uint8_t mode = (uint8_t)profile_mode();
        int depth = 0;
        size_t n = 0;
        uintptr_t esp = 0;
        const char* imp = NULL;
        if (SuspendThread(g_thread) != (DWORD)-1) {
            CONTEXT c;
            c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(g_thread, &c)) {   /* also waits until it has stopped */
                f[depth++] = c.Eip;
                esp = c.Esp;
                if (esp && esp < g_stack_base) {
                    n = g_stack_base - esp;
                    if (n > PROFILE_COPY) n = PROFILE_COPY;
                    n &= ~(size_t)3;
                    memcpy(buf, (const void*)esp, n);
                }
                if (c.Eip < profile_img_lo || c.Eip >= profile_img_hi) imp = g_cur_import;
            }
            ResumeThread(g_thread);
        }
        /* the game thread runs again; the copy is ours */
        if (depth) profile_record(f, profile_scan(f, depth, buf, n), mode, run, imp);
        cpu_now(&g_cpu_thread1, &g_cpu_proc1);
        LeaveCriticalSection(&g_cs);
        if ((LONG)(GetTickCount() - next_write) >= 0) {
            next_write = GetTickCount() + 10000;
            EnterCriticalSection(&g_cs);
            write_file();
            LeaveCriticalSection(&g_cs);
        }
    }
    return 0;
}

void profile_start(void) {
    if (!profile_path[0]) return;
    InitializeCriticalSection(&g_cs);
    if (!profile_table_init()) { fprintf(stderr, "[profile] out of memory\n"); profile_path[0] = 0; return; }
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_thread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    g_stack_base = (uintptr_t)((NT_TIB*)NtCurrentTeb())->StackBase;
    MODULEINFO mi;
    if (GetModuleInformation(GetCurrentProcess(), GetModuleHandleA(NULL), &mi, sizeof mi)) {
        profile_img_lo = (uintptr_t)mi.lpBaseOfDll;
        profile_img_hi = profile_img_lo + mi.SizeOfImage;
    }
    timeBeginPeriod(1);
    InterlockedExchange(&g_started, 1);
    HANDLE t = CreateThread(NULL, 0, sampler, NULL, 0, NULL);
    SetThreadPriority(t, THREAD_PRIORITY_TIME_CRITICAL);
    CloseHandle(t);
    printf("  profile: %d samples a second -> %s\n", profile_hz, profile_path);
}

void profile_finish(void) {
    if (!g_started) return;
    EnterCriticalSection(&g_cs);
    InterlockedExchange(&g_done, 1);
    write_file();
    LeaveCriticalSection(&g_cs);
    fprintf(stderr, "[profile] %ld samples (%ld running, %ld lost) -> %s\n", profile_samples, profile_running,
            profile_lost, profile_path);
}
