/*
 * framerate: how often the game draws and how often it simulates, and
 * --fps N, a frame rate of the player's choosing.
 *
 * What the engine does (docs/framerate.md has the measurements):
 *  - In game it draws one frame per pass of the game screen's idle,
 *    sub_004F6EA0, which returns at once unless timeGetTime() - [screen+0x125C]
 *    >= [screen+0x1258]: a gate of 1 ms here, so up to 1000 frames a second.
 *    A pass runs the world update (world vtable +0xD4, sub_00601630 ->
 *    sub_0061AB70), then UpdateWindow, and the frame is drawn in WM_PAINT.
 *  - The world update steps the simulation only when 50 ms or more have passed
 *    since its last step (single player), by the real time elapsed, at most
 *    100 ms, times the game speed: about 20 steps a second at any frame rate
 *    from 20 up, and the game clock runs at speed x the wall clock.
 *
 * So the frame rate is the gate's, and the simulation does not depend on it
 * (down to 10 frames a second, below which a step is cut to 100 ms and the
 * game slows). --fps N sets the gate every pass so that frames start N times a
 * second (16.67 ms is 16 and 17 ms in turn), and between frames the host waits
 * for the next one, or for input, instead of spinning a core in the game's
 * PeekMessage loop. Without --fps nothing is changed.
 *
 * --fps-log prints once a second against the wall clock: presents, the
 * game's own Blts/Flips/Locks/DCs on the primary and other surfaces, frames
 * that differ from the last, the world clock and its steps, and timeGetTime
 * calls by the lifted function that made them (the engine imports no other
 * clock). FPS_STACK (below) prints who called. Neither changes anything the
 * game sees.
 */
#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#include <windows.h>
#include <mmsystem.h>
#include <ddraw.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native32.h"
#include "guest_addrs.h"
#include "softdd.h"
#include "softdd_int.h"
#include "framerate.h"
#include "present.h"

#define ARG(n) MEM32(g_esp + 4 + 4 * (n))

static int    g_log;             /* --fps-log */
static double g_fps;             /* --fps N; 0: the display's refresh (present.c) */
static int    g_pace = 1;        /* --fps off: 0, the game's own rate and its spin */

/* ---- the engine's state, read from guest memory ----------------------------- */
#define GAME          G_GAME        /* the game object */
#define GAME_MODE     0x1DC         /* 4: in game */
#define GAME_SCREEN   0x17B4        /* the game screen, while one exists */
#define GAME_WORLD    0x420
#define SCREEN_VTABLE G_SCREEN_VTABLE   /* set by its constructor */
#define SCREEN_GATE   G_SCREEN_GATE /* ms between passes; from [[game+0x24]+0x888] */
#define SCREEN_LAST   G_SCREEN_LAST /* timeGetTime of the last pass */
#define WORLD_TIME    0x10          /* game ms */
#define WORLD_SPEED   0x24          /* float: 1.5 is Normal */

static uint32_t peek(uint32_t a) {
    __try { return a >= 0x10000 ? MEM32(a) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static uint32_t world_of(void) { uint32_t g = peek(GAME); return g ? peek(g + GAME_WORLD) : 0; }
static uint32_t mode_of(void)  { uint32_t g = peek(GAME); return g ? peek(g + GAME_MODE) : 0; }

/* The game screen, only in game and only if it is one: the pointer outlives
 * a game. */
static uint32_t game_screen(void) {
    uint32_t g = peek(GAME), m = g ? peek(g + GAME_MODE) : 0, s = g ? peek(g + GAME_SCREEN) : 0;
    return (m == 4 || m == 5) && s && peek(s) == SCREEN_VTABLE ? s : 0;
}

/* ---- counters ------------------------------------------------------------- */

enum { C_BLT_P, C_BLT, C_FLIP, C_LOCK_P, C_LOCK, C_DC_P, C_DC, C_TGT, C_N };
static const char* const c_name[C_N] = {
    "blt>primary", "blt", "flip", "lock primary", "lock", "dc primary", "dc", "timeGetTime",
};

/* Per counter, the lifted functions that made the calls (a small open hash). */
#define SLOTS 64
typedef struct { uint32_t va; long n; } slot_t;
static slot_t g_by[C_N][SLOTS];
static long   g_total[C_N];
static CRITICAL_SECTION g_cs;

static int g_stack_c = -1;
static uint32_t g_stack_from;   /* FPS_STACK=c:VA: only calls from that function */
static void stack_dump(int c);

static void count(int c) {
    uint32_t va = g_cur_func;
    EnterCriticalSection(&g_cs);
    g_total[c]++;
    slot_t* t = g_by[c];
    for (uint32_t i = (va >> 4) % SLOTS, k = 0; k < SLOTS; k++, i = (i + 1) % SLOTS)
        if (t[i].va == va || !t[i].va) { t[i].va = va; t[i].n++; break; }
    LeaveCriticalSection(&g_cs);
    if (c == g_stack_c && (!g_stack_from || va == g_stack_from)) stack_dump(c);
}

/* ---- who called: the host stack, read back as lifted functions ------------- *
 * FPS_STACK=<counter number>[:VA] with --fps-log: every 3 seconds, the lifted
 * functions whose host return addresses are on the stack at that call
 * (from that function), innermost first. The generated code omits frame
 * pointers, so the stack is scanned: a value is kept when it points into
 * lifted code just after a call instruction. Stale values make some noise;
 * the chain from the message loop down is what to read. */
typedef struct { uintptr_t host; uint32_t va; } fn_t;
static fn_t* g_fns;
static uint32_t g_nfns;

static int by_host(const void* a, const void* b) {
    uintptr_t x = ((const fn_t*)a)->host, y = ((const fn_t*)b)->host;
    return (x > y) - (x < y);
}

static uint32_t va_of(uintptr_t p) {
    uint32_t lo = 0, hi = g_nfns;
    if (!g_nfns || p < g_fns[0].host || p > g_fns[g_nfns - 1].host + 0x40000) return 0;
    while (hi - lo > 1) { uint32_t m = (lo + hi) / 2; if (g_fns[m].host <= p) lo = m; else hi = m; }
    return g_fns[lo].va;
}

static int after_call(uintptr_t p) {
    const uint8_t* b = (const uint8_t*)p;
    return b[-5] == 0xE8 || (b[-2] == 0xFF && (b[-1] & 0x38) == 0x10) ||
           (b[-3] == 0xFF && (b[-2] & 0x38) == 0x10) || (b[-6] == 0xFF && (b[-5] & 0x38) == 0x10);
}

static void stack_dump(int c) {
    static DWORD next;
    if (GetTickCount() < next) return;
    next = GetTickCount() + 3000;
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    uintptr_t* sp = (uintptr_t*)&c, *top = (uintptr_t*)tib->StackBase;
    char line[2048];
    int n = _snprintf(line, sizeof line, "[fps-stack] %s from %08X:", c_name[c], g_cur_func);
    uint32_t last = 0;
    for (int k = 0; sp < top && k < 40 && n < (int)sizeof line - 16; sp++) {
        uint32_t va = va_of(*sp);
        if (!va || va == last || !after_call(*sp)) continue;
        n += _snprintf(line + n, sizeof line - n, " %08X", va);
        last = va;
        k++;
    }
    fprintf(stderr, "%s\n", line);
}

static void stack_init(void) {
    char v[32];
    if (!GetEnvironmentVariableA("FPS_STACK", v, sizeof v)) return;
    g_stack_c = atoi(v);
    if (strchr(v, ':')) g_stack_from = strtoul(strchr(v, ':') + 1, NULL, 16);
    g_fns = (fn_t*)malloc(recomp_dispatch_count * sizeof *g_fns);
    for (uint32_t i = 0; i < recomp_dispatch_count; i++) {
        const uint8_t* f = (const uint8_t*)recomp_dispatch_table[i].func;
        if (f[0] == 0xE9) f += 5 + *(const int32_t*)(f + 1);   /* an incremental link's jump stub */
        g_fns[i].host = (uintptr_t)f;
        g_fns[i].va = recomp_dispatch_table[i].address;
    }
    g_nfns = recomp_dispatch_count;
    qsort(g_fns, g_nfns, sizeof *g_fns, by_host);
}

/* ---- the surface methods, wrapped ----------------------------------------- *
 * One set for each interface (v1 and v3 share the argument layout); the
 * originals are softdd's, saved before the vtables are patched. */
static int is_primary(void* iface) {
    Surf* s = softdd_surf(iface);
    return s && (s->caps & DDSCAPS_PRIMARYSURFACE);
}

typedef HRESULT (WINAPI *blt_t)(void*, LPRECT, void*, LPRECT, DWORD, LPDDBLTFX);
typedef HRESULT (WINAPI *bltfast_t)(void*, DWORD, DWORD, void*, LPRECT, DWORD);
typedef HRESULT (WINAPI *flip_t)(void*, void*, DWORD);
typedef HRESULT (WINAPI *lock_t)(void*, LPRECT, LPDDSURFACEDESC, DWORD, HANDLE);
typedef HRESULT (WINAPI *getdc_t)(void*, HDC*);

#define WRAP_SET(V) \
static blt_t V##_blt; static bltfast_t V##_bltfast; static flip_t V##_flip; \
static lock_t V##_lock; static getdc_t V##_getdc; \
static HRESULT WINAPI V##_Blt(void* p, LPRECT d, void* s, LPRECT r, DWORD f, LPDDBLTFX fx) { \
    count(is_primary(p) ? C_BLT_P : C_BLT); return V##_blt(p, d, s, r, f, fx); } \
static HRESULT WINAPI V##_BltFast(void* p, DWORD x, DWORD y, void* s, LPRECT r, DWORD t) { \
    count(is_primary(p) ? C_BLT_P : C_BLT); return V##_bltfast(p, x, y, s, r, t); } \
static HRESULT WINAPI V##_Flip(void* p, void* o, DWORD f) { \
    count(C_FLIP); return V##_flip(p, o, f); } \
static HRESULT WINAPI V##_Lock(void* p, LPRECT r, LPDDSURFACEDESC d, DWORD f, HANDLE e) { \
    count(is_primary(p) ? C_LOCK_P : C_LOCK); return V##_lock(p, r, d, f, e); } \
static HRESULT WINAPI V##_GetDC(void* p, HDC* dc) { \
    count(is_primary(p) ? C_DC_P : C_DC); return V##_getdc(p, dc); }
WRAP_SET(v1)
WRAP_SET(v3)

#define PATCH(vt, V) do { \
    DWORD old_; VirtualProtect(vt, sizeof *(vt), PAGE_READWRITE, &old_); \
    V##_blt = (blt_t)(vt)->Blt;          (vt)->Blt = (void*)V##_Blt; \
    V##_bltfast = (bltfast_t)(vt)->BltFast; (vt)->BltFast = (void*)V##_BltFast; \
    V##_flip = (flip_t)(vt)->Flip;       (vt)->Flip = (void*)V##_Flip; \
    V##_lock = (lock_t)(vt)->Lock;       (vt)->Lock = (void*)V##_Lock; \
    V##_getdc = (getdc_t)(vt)->GetDC;    (vt)->GetDC = (void*)V##_GetDC; \
    VirtualProtect(vt, sizeof *(vt), old_, &old_); } while (0)

/* Frames that differ from the one before: what the player can see change. */
static volatile LONG g_changed;
static void on_present(const uint8_t* bits, int w, int h, int pitch, long frame) {
    /* against a copy of the last one: a 64-bit hash of every word cost more than
     * the game's own frame at 2560x1440, and --fps-log is for measuring */
    static uint8_t* last;
    static size_t cap, size;
    size_t n = (size_t)h * pitch;
    (void)w; (void)frame;
    if (n != size || !last || memcmp(bits, last, n)) {
        if (cap < n) { free(last); last = (uint8_t*)malloc(n); cap = last ? n : 0; }
        if (last) memcpy(last, bits, n);
        size = last ? n : 0;
        InterlockedIncrement(&g_changed);
    }
}

void framerate_on_ddraw(void* dd) {
    static int done;
    if (!g_log || done++) return;
    /* A surface of our own, for the vtables every surface shares. */
    IDirectDraw* d = (IDirectDraw*)dd;
    DDSURFACEDESC sd = { sizeof sd };
    sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    sd.dwWidth = sd.dwHeight = 1;
    IDirectDrawSurface* s = NULL;
    if (IDirectDraw_CreateSurface(d, &sd, &s, NULL) != DD_OK || !s) return;
    Surf* o = softdd_surf(s);
    PATCH(o->lpVtbl, v1);
    PATCH(o->v3, v3);
    IDirectDrawSurface_Release(s);
    if (!softdd_on_present) softdd_on_present = on_present;
}

/* ---- the world clock --------------------------------------------------------- *
 * A change of the world clock between two passes of the message loop is a
 * simulation step. */
static volatile LONG g_steps, g_step_ms, g_step_max;
static void watch_world(void) {
    static uint32_t last;
    uint32_t w = world_of(), wt = w ? peek(w + WORLD_TIME) : 0;
    if (wt == last) return;
    if (last && wt > last) {
        LONG d = (LONG)(wt - last);
        g_steps++; g_step_ms += d;
        if (d > g_step_max) g_step_max = d;
    }
    last = wt;
}

/* ---- --fps N ------------------------------------------------------------------ */
static double g_due;          /* timeGetTime ms when the next pass may start */
static uint32_t g_seen;       /* the screen's last pass, as the host last saw it */

/* With the machine let go: input or a sent message ends the wait early. */
static void wait_ms(DWORD ms) {
    uint32_t esp = g_esp;
    mach_leave();
    MsgWaitForMultipleObjects(0, NULL, FALSE, ms, QS_ALLINPUT);
    mach_enter();
    g_esp = esp;
}

/* Called when the game's PeekMessage found nothing, so its idle is next. */
static void pace(void) {
    uint32_t sc = game_screen();
    double fps = g_fps > 0 ? g_fps : present_refresh_hz();   /* the default: no frame is drawn that is never shown */
    if (!(g_fps > 0)) fps = fps < 30 ? 30 : fps > 1000 ? 1000 : fps;
    double period = 1000.0 / fps;
    DWORD now = timeGetTime();
    if (!sc) { g_seen = 0; wait_ms(1); return; }   /* menus: draw on change only; just yield */
    uint32_t last = MEM32(sc + SCREEN_LAST);
    if (!g_seen) g_due = last + period;
    else if (last != g_seen) {                       /* a pass ran: the next one is a period on */
        g_due += period;
        if (g_due < (double)now - period) g_due = now;   /* fell behind: no catching up */
    }
    g_seen = last;
    DWORD due = (DWORD)ceil(g_due);
    MEM32(sc + SCREEN_GATE) = (int32_t)(due - last) > 0 ? due - last : 0;
    if ((int32_t)(due - now) > 0) wait_ms(due - now);
}

/* PeekMessageA: the game's loop (0x00425655) calls it with PM_REMOVE and runs
 * its idle when it returns FALSE. With the machine let go, as host.c's window
 * calls: it can deliver sent messages to the game's window procedure. */
void framerate_PeekMessageA(void) {
    MSG* msg = (MSG*)(uintptr_t)ARG(0);
    HWND w = (HWND)(uintptr_t)ARG(1);
    UINT lo = ARG(2), hi = ARG(3), rm = ARG(4);
    uint32_t esp = g_esp;
    BOOL r;
    mach_leave();
    r = PeekMessageA(msg, w, lo, hi, rm);
    DWORD err = GetLastError();
    mach_enter();
    g_esp = esp;
    if (!r) {
        if (g_log) watch_world();
        if (g_pace) pace();
    }
    SetLastError(err);
    g_eax = r;
    g_esp = esp + 4 + 5 * 4;
}

void framerate_timeGetTime(void) {
    if (g_log) count(C_TGT);
    g_eax = timeGetTime();
    g_esp += 4;
}

/* ---- the log ---------------------------------------------------------------- */
static int by_n(const void* a, const void* b) {
    long x = ((const slot_t*)a)->n, y = ((const slot_t*)b)->n;
    return (x < y) - (x > y);
}

static DWORD WINAPI log_thread(LPVOID unused) {
    (void)unused;
    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    LARGE_INTEGER last = t0;
    long last_frames = softdd_frames;
    uint32_t last_wt = 0;
    uint64_t last_cpu = 0;   /* process CPU time, 100 ns: cpu is % of one core */
    for (;;) {
        Sleep(1000);
        slot_t by[C_N][SLOTS];
        long total[C_N];
        EnterCriticalSection(&g_cs);
        memcpy(by, g_by, sizeof by);
        memcpy(total, g_total, sizeof total);
        memset(g_by, 0, sizeof g_by);
        memset(g_total, 0, sizeof g_total);
        LeaveCriticalSection(&g_cs);
        QueryPerformanceCounter(&t);
        double dt = (double)(t.QuadPart - last.QuadPart) / f.QuadPart;
        double at = (double)(t.QuadPart - t0.QuadPart) / f.QuadPart;
        last = t;
        long fr = softdd_frames;
        FILETIME c_, e_, k, u;
        GetProcessTimes(GetCurrentProcess(), &c_, &e_, &k, &u);
        uint64_t cpu = ((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime);
        char line[2048];
        int n = _snprintf(line, sizeof line, "[fps] %6.1fs mode %u cpu %3.0f%% presents %.0f/s changed %.0f/s",
                          at, mode_of(), last_cpu ? (cpu - last_cpu) / 1e5 / dt : 0.0,
                          (fr - last_frames) / dt, InterlockedExchange(&g_changed, 0) / dt);
        last_cpu = cpu;
        last_frames = fr;
        uint32_t w = world_of(), wt = w ? peek(w + WORLD_TIME) : 0, sc = game_screen();
        if (w) {
            uint32_t sp = peek(w + WORLD_SPEED);
            LONG st = InterlockedExchange(&g_steps, 0), ms = InterlockedExchange(&g_step_ms, 0);
            LONG mx = InterlockedExchange(&g_step_max, 0);
            n += _snprintf(line + n, sizeof line - n, " | world %u ms +%.0f/s speed %.2f",
                           wt, last_wt && wt >= last_wt ? (wt - last_wt) / dt : 0.0, *(float*)&sp);
            if (st) n += _snprintf(line + n, sizeof line - n, " steps %.1f/s of %.1f ms (max %ld)",
                                   st / dt, (double)ms / st, mx);
        }
        if (sc) n += _snprintf(line + n, sizeof line - n, " | gate %u ms", peek(sc + SCREEN_GATE));
        last_wt = wt;
        for (int c = 0; c < C_N; c++) {
            if (!total[c]) continue;
            qsort(by[c], SLOTS, sizeof by[c][0], by_n);
            n += _snprintf(line + n, sizeof line - n, " | %s %.0f/s", c_name[c], total[c] / dt);
            for (int k = 0; k < 4 && by[c][k].n; k++)
                n += _snprintf(line + n, sizeof line - n, " %08X:%ld", by[c][k].va, by[c][k].n);
        }
        fprintf(stderr, "%s\n", line);
    }
    return 0;
}

int framerate_arg(int argc, char** argv, int i) {
    if (!strcmp(argv[i], "--fps-log")) { g_log = 1; return 1; }
    if (!strcmp(argv[i], "--fps") && i + 1 < argc) {
        if (!_stricmp(argv[i + 1], "off")) { g_pace = 0; g_fps = 0; return 2; }
        g_fps = atof(argv[i + 1]);
        if (g_fps < 1 || g_fps > 1000) { fprintf(stderr, "--fps: 1 to 1000, or off\n"); exit(1); }
        return 2;
    }
    return 0;
}

void framerate_start(void) {
    InitializeCriticalSection(&g_cs);
    if (g_pace) {
        timeBeginPeriod(1);   /* waits to the millisecond */
        if (g_fps > 0) printf("  frame rate: %.0f a second in game (--fps)\n", g_fps);
        else printf("  frame rate: the display's refresh in game (--fps N to choose, --fps off for the game's own)\n");
    }
    if (g_log) {
        stack_init();
        CloseHandle(CreateThread(NULL, 0, log_thread, NULL, 0, NULL));
    }
}
