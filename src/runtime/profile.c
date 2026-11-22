/*
 * profile: where the game thread's time goes, by sampling (--profile FILE).
 *
 * A thread of its own wakes about every millisecond (a high-resolution
 * waitable timer), suspends the game thread, reads its EIP and ESP
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

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static char   g_path[MAX_PATH];
static double g_from, g_to = 1e30;
static int    g_hz = 1000;

static HANDLE    g_thread;          /* the game thread */
static uintptr_t g_stack_base;      /* its host stack's top */
static uintptr_t g_img_lo, g_img_hi;

#define MAXF   48                   /* frames kept per sample */
#define COPY   (96 * 1024)          /* stack bytes read per sample */
#define TAB    (1 << 15)            /* distinct samples */
typedef struct {
    uint32_t n;                     /* 0: free */
    uint8_t  mode, run, depth, pad;
    const char* imp;
    uint32_t hash;
    uint32_t f[MAXF];               /* f[0] is EIP */
} stk_t;
static stk_t* g_tab;
static long   g_used, g_lost, g_samples, g_running;
static double g_wall;               /* seconds sampled */
static uint64_t g_cpu_thread0, g_cpu_proc0, g_cpu_thread1, g_cpu_proc1;   /* 100 ns, over the window */
static CRITICAL_SECTION g_cs;
static volatile LONG g_done;

/* ---- reading the guest ------------------------------------------------------- */
#define GAME      0x006A0FD4u
#define GAME_MODE 0x1DC
static uint32_t peek(uint32_t a) {
    __try { return a >= 0x10000 ? MEM32(a) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static uint32_t mode_of(void) { uint32_t g = peek(GAME); return g ? peek(g + GAME_MODE) : 0; }

/* A return address: just after a call (E8 rel32, FF /2 with a 0, 1 or 4 byte
 * displacement, FF /2 register). */
static int after_call(uintptr_t p) {
    if (p < g_img_lo + 8 || p >= g_img_hi) return 0;
    const uint8_t* b = (const uint8_t*)p;
    return b[-5] == 0xE8 || (b[-2] == 0xFF && (b[-1] & 0x38) == 0x10) ||
           (b[-3] == 0xFF && (b[-2] & 0x38) == 0x10) || (b[-6] == 0xFF && (b[-5] & 0x38) == 0x10) ||
           (b[-7] == 0xFF && (b[-6] & 0x38) == 0x10);
}

static uint64_t ft64(FILETIME f) { return (uint64_t)f.dwHighDateTime << 32 | f.dwLowDateTime; }
static void cpu_now(uint64_t* thread, uint64_t* proc) {
    FILETIME c, e, k, u;
    *thread = GetThreadTimes(g_thread, &c, &e, &k, &u) ? ft64(k) + ft64(u) : 0;
    *proc = GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u) ? ft64(k) + ft64(u) : 0;
}

static void record(const uint32_t* f, int depth, uint8_t mode, uint8_t run, const char* imp) {
    uint32_t h = 2166136261u ^ mode ^ (run << 8) ^ (uint32_t)(uintptr_t)imp;
    for (int i = 0; i < depth; i++) h = (h ^ f[i]) * 16777619u;
    g_samples++;
    g_running += run;
    for (uint32_t i = h & (TAB - 1), k = 0; k < 64; k++, i = (i + 1) & (TAB - 1)) {
        stk_t* s = &g_tab[i];
        if (!s->n) {
            if (g_used >= TAB * 3 / 4) break;
            s->hash = h; s->mode = mode; s->run = run; s->imp = imp; s->depth = (uint8_t)depth;
            memcpy(s->f, f, depth * sizeof *f);
            s->n = 1;
            g_used++;
            return;
        }
        if (s->hash == h && s->mode == mode && s->run == run && s->imp == imp && s->depth == depth &&
            !memcmp(s->f, f, depth * sizeof *f)) { s->n++; return; }
    }
    g_lost++;
}

static void write_file(void) {
    FILE* o = fopen(g_path, "w");
    if (!o) { fprintf(stderr, "[profile] cannot write %s\n", g_path); return; }
    fprintf(o, "# swgb-recomp profile 1\n");
    fprintf(o, "hz %d\nwall_s %.3f\nsamples %ld\nrunning %ld\nlost %ld\n", g_hz, g_wall, g_samples, g_running, g_lost);
    fprintf(o, "cpu_thread_s %.3f\ncpu_process_s %.3f\n", (g_cpu_thread1 - g_cpu_thread0) / 1e7, (g_cpu_proc1 - g_cpu_proc0) / 1e7);
    HMODULE mods[512];
    DWORD need = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) {
        for (DWORD i = 0; i < need / sizeof mods[0] && i < 512; i++) {
            MODULEINFO mi;
            char name[MAX_PATH];
            if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi)) continue;
            if (!GetModuleFileNameA(mods[i], name, sizeof name)) continue;
            fprintf(o, "module %08X %08X %s\n", (uint32_t)(uintptr_t)mi.lpBaseOfDll, (uint32_t)mi.SizeOfImage, name);
        }
    }
    for (int i = 0; i < TAB; i++) {
        stk_t* s = &g_tab[i];
        if (!s->n) continue;
        fprintf(o, "s %u %u %c %s", s->n, s->mode, s->run ? 'R' : 'W', s->imp ? s->imp : "-");
        for (int k = 0; k < s->depth; k++) fprintf(o, " %X", s->f[k]);
        fputc('\n', o);
    }
    fclose(o);
}

static DWORD WINAPI sampler(LPVOID unused) {
    (void)unused;
    static uint8_t buf[COPY];
    uint32_t f[MAXF];
    HANDLE tm = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!tm) tm = CreateWaitableTimerW(NULL, FALSE, NULL);
    LARGE_INTEGER due;
    due.QuadPart = -(10000000LL / g_hz);
    SetWaitableTimer(tm, &due, 1000 / g_hz ? 1000 / g_hz : 1, NULL, NULL, FALSE);
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
        if (WaitForSingleObject(tm, 50) != WAIT_OBJECT_0) continue;
        QueryPerformanceCounter(&t);
        double at = (double)(t.QuadPart - t0.QuadPart) / qf.QuadPart;
        QueryThreadCycleTime(g_thread, &cyc);
        tsc = __rdtsc();
        /* ran for at least a quarter of the interval: CPU time, not a wait */
        uint8_t run = (cyc - last_cyc) * 4 > (tsc - last_tsc);
        last_cyc = cyc;
        last_tsc = tsc;
        if (at < g_from || at > g_to) {
            if (in_window && at > g_to) { EnterCriticalSection(&g_cs); cpu_now(&g_cpu_thread1, &g_cpu_proc1); LeaveCriticalSection(&g_cs); in_window = 0; }
            last_t = t;
            continue;
        }
        EnterCriticalSection(&g_cs);
        if (!in_window) { cpu_now(&g_cpu_thread0, &g_cpu_proc0); in_window = 1; last_t = t; }
        g_wall += (double)(t.QuadPart - last_t.QuadPart) / qf.QuadPart;
        last_t = t;
        uint8_t mode = (uint8_t)mode_of();
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
                    if (n > COPY) n = COPY;
                    memcpy(buf, (const void*)esp, n & ~(size_t)3);
                }
                if (c.Eip < g_img_lo || c.Eip >= g_img_hi) imp = g_cur_import;
            }
            ResumeThread(g_thread);
        }
        /* the game thread runs again; the copy is ours */
        for (size_t k = 0; k + 4 <= n && depth < MAXF; k += 4) {
            uintptr_t v = *(const uint32_t*)(buf + k);
            if (after_call(v) && v != f[depth - 1]) f[depth++] = (uint32_t)v;
        }
        if (depth) record(f, depth, mode, run, imp);
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

int profile_arg(int argc, char** argv, int i) {
    if (!strcmp(argv[i], "--profile") && i + 1 < argc) {
        GetFullPathNameA(argv[i + 1], MAX_PATH, g_path, NULL);   /* the game runs in its own folder */
        return 2;
    }
    if (!strcmp(argv[i], "--profile-window") && i + 1 < argc) {
        if (sscanf(argv[i + 1], "%lf:%lf", &g_from, &g_to) != 2 || g_to <= g_from) {
            fprintf(stderr, "--profile-window A:B (seconds, A < B)\n");
            exit(1);
        }
        return 2;
    }
    if (!strcmp(argv[i], "--profile-hz") && i + 1 < argc) {
        g_hz = atoi(argv[i + 1]);
        if (g_hz < 1 || g_hz > 1000) { fprintf(stderr, "--profile-hz: 1 to 1000\n"); exit(1); }
        return 2;
    }
    return 0;
}

void profile_start(void) {
    if (!g_path[0]) return;
    InitializeCriticalSection(&g_cs);
    g_tab = (stk_t*)calloc(TAB, sizeof *g_tab);
    if (!g_tab) { fprintf(stderr, "[profile] out of memory\n"); g_path[0] = 0; return; }
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_thread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    g_stack_base = (uintptr_t)((NT_TIB*)NtCurrentTeb())->StackBase;
    MODULEINFO mi;
    if (GetModuleInformation(GetCurrentProcess(), GetModuleHandleA(NULL), &mi, sizeof mi)) {
        g_img_lo = (uintptr_t)mi.lpBaseOfDll;
        g_img_hi = g_img_lo + mi.SizeOfImage;
    }
    timeBeginPeriod(1);
    HANDLE t = CreateThread(NULL, 0, sampler, NULL, 0, NULL);
    SetThreadPriority(t, THREAD_PRIORITY_TIME_CRITICAL);
    CloseHandle(t);
    printf("  profile: %d samples a second -> %s\n", g_hz, g_path);
}

void profile_finish(void) {
    if (!g_path[0] || !g_tab) return;
    EnterCriticalSection(&g_cs);
    InterlockedExchange(&g_done, 1);
    write_file();
    LeaveCriticalSection(&g_cs);
    fprintf(stderr, "[profile] %ld samples (%ld running, %ld lost) -> %s\n", g_samples, g_running, g_lost, g_path);
}
