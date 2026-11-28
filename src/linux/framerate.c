/*
 * framerate.c - --fps N and --fps-log on the win32hle host (Linux, macOS).
 *
 * The same pacing as the Windows host's src/runtime/framerate.c, which has
 * the engine's side of it (docs/framerate.md): in game the screen's idle
 * draws a frame when timeGetTime() - [screen+LAST] >= [screen+GATE], and the
 * simulation steps by the wall clock whatever the frame rate. When the game's
 * PeekMessageA finds nothing (its idle is next), the gate is set so passes
 * start N times a second, and until the next one is due the thread sleeps
 * with the machine let go instead of spinning in the message loop. In the
 * menus (which draw on change only) it yields a millisecond. Without --fps
 * the rate is the display's refresh (60 headless); --fps off leaves the game
 * to its own rate and its spin.
 *
 * --fps-log prints a line a second in the Windows host's format, which
 * tools/test.py's perf check and tools/soak.py read:
 *   [fps] T s mode M cpu C% presents P/s changed X/s | world ... | gate G ms | timeGetTime N/s ...
 * presents are changes to the primary (main.c counts them); changed, those
 * whose pixels differ from the last. The Windows host also counts Blts,
 * Flips, Locks and DCs on its own DirectDraw; win32hle's are not wrapped.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <SDL.h>

#include "win32hle.h"
#include "ddraw_int.h"
#include "guest.h"
#include "guest_addrs.h"
#include "swgb_linux.h"

static int    g_log;             /* --fps-log */
static double g_fps;             /* --fps N; 0: the display's refresh */
static int    g_pace = 1;        /* --fps off: 0 */

/* ---- the engine's state, read from guest memory (framerate.c's offsets) ---- */
#define WORLD_TIME    0x10       /* game ms */
#define WORLD_SPEED   0x24       /* float: 1.5 is Normal */

static uint32_t world_of(void) { uint32_t g = guest_read32(G_GAME); return g ? guest_read32(g + G_GAME_WORLD) : 0; }
static uint32_t mode_of(void)  { uint32_t g = guest_read32(G_GAME); return g ? guest_read32(g + G_GAME_MODE) : 0; }
static uint32_t game_screen(void) {
    uint32_t g = guest_read32(G_GAME), m = g ? guest_read32(g + G_GAME_MODE) : 0, s = g ? guest_read32(g + G_GAME_SCREEN) : 0;
    return (m == 4 || m == 5) && s && guest_read32(s) == G_SCREEN_VTABLE ? s : 0;
}

/* ---- counters ------------------------------------------------------------- */
#define SLOTS 64
typedef struct { uint32_t va; long n; } slot_t;
static slot_t g_tgt_by[SLOTS];
static long   g_tgt;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile long g_changed;

static void count_tgt(void) {
    uint32_t va = g_cur_func;
    pthread_mutex_lock(&g_lock);
    g_tgt++;
    for (uint32_t i = (va >> 4) % SLOTS, k = 0; k < SLOTS; k++, i = (i + 1) % SLOTS)
        if (g_tgt_by[i].va == va || !g_tgt_by[i].va) { g_tgt_by[i].va = va; g_tgt_by[i].n++; break; }
    pthread_mutex_unlock(&g_lock);
}

/* A present (main.c, holding the machine): does it differ from the last? */
void framerate_on_present(void) {
    static uint8_t* last;
    static size_t cap, size;
    if (!g_log) return;
    const uint8_t* px;
    const uint32_t* pal;
    int w, h, pitch, bpp;
    unsigned seq;
    const uint8_t* (*hook)(surf_t*) = hle_dd_frame_hook;   /* the primary's own pixels, not a composed copy */
    hle_dd_frame_hook = NULL;
    int ok = hle_dd_frame(&px, &w, &h, &pitch, &bpp, &pal, &seq);
    hle_dd_frame_hook = hook;
    if (!ok || h <= 0 || pitch <= 0) return;
    size_t n = (size_t)h * (size_t)pitch;
    if (n != size || !last || memcmp(px, last, n)) {
        if (cap < n) { free(last); last = (uint8_t*)malloc(n); cap = last ? n : 0; }
        if (last) memcpy(last, px, n);
        size = last ? n : 0;
        __sync_add_and_fetch(&g_changed, 1);
    }
}

/* ---- the world clock: a change between two passes is a simulation step ---- */
static volatile long g_steps, g_step_ms, g_step_max;
static void watch_world(void) {
    static uint32_t last;
    uint32_t w = world_of(), wt = w ? guest_read32(w + WORLD_TIME) : 0;
    if (wt == last) return;
    if (last && wt > last) {
        long d = (long)(wt - last);
        g_steps++, g_step_ms += d;
        if (d > g_step_max) g_step_max = d;
    }
    last = wt;
}

/* ---- --fps N ------------------------------------------------------------------ */
static double g_due;          /* ms when the next pass may start */
static uint32_t g_seen;       /* the screen's last pass, as the host last saw it */
static int g_refresh = 60;    /* the display's, once the window is open */

static void wait_ms(uint32_t ms) {
    if (!ms) return;
    hle_block_begin();        /* the machine let go: the window's thread pumps meanwhile */
    usleep(ms * 1000u);
    hle_block_end();
}

static void pace(void) {
    uint32_t sc = game_screen();
    double fps = g_fps > 0 ? g_fps : g_refresh < 30 ? 30 : g_refresh > 1000 ? 1000 : g_refresh;
    double period = 1000.0 / fps;
    uint32_t now = hle_ticks_ms();
    if (!sc) { g_seen = 0; wait_ms(1); return; }  /* menus: draw on change only; just yield */
    uint32_t last = MEM32(sc + G_SCREEN_LAST);
    if (!g_seen) g_due = last + period;
    else if (last != g_seen) {                    /* a pass ran: the next one is a period on */
        g_due += period;
        if (g_due < (double)now - period) g_due = now;   /* fell behind: no catching up */
    }
    g_seen = last;
    uint32_t due = (uint32_t)ceil(g_due);
    MEM32(sc + G_SCREEN_GATE) = (int32_t)(due - last) > 0 ? due - last : 0;
    if ((int32_t)(due - now) > 0) wait_ms(due - now);
}

static recomp_func_t g_PeekMessageA, g_timeGetTime;

static void s_PeekMessageA(void) {                /* (msg, hwnd, min, max, remove) */
    g_PeekMessageA();
    if (!g_eax) {
        uint32_t eax = g_eax, esp = g_esp;
        if (g_log) watch_world();
        if (g_pace) pace();
        g_eax = eax, g_esp = esp;
    }
}

static void s_timeGetTime(void) {
    if (g_log) count_tgt();
    g_timeGetTime();
}

/* ---- the log ---------------------------------------------------------------- */
static int by_n(const void* a, const void* b) {
    long x = ((const slot_t*)a)->n, y = ((const slot_t*)b)->n;
    return (x < y) - (x > y);
}

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static double cpu_s(void) {
    struct rusage u;
    getrusage(RUSAGE_SELF, &u);
    return u.ru_utime.tv_sec + u.ru_utime.tv_usec / 1e6 + u.ru_stime.tv_sec + u.ru_stime.tv_usec / 1e6;
}

static void* log_thread(void* unused) {
    (void)unused;
    double t0 = now_s(), last = t0, last_cpu = cpu_s();
    long last_frames = swgb_frames;
    uint32_t last_wt = 0;
    for (;;) {
        sleep(1);
        slot_t by[SLOTS];
        long tgt;
        pthread_mutex_lock(&g_lock);
        memcpy(by, g_tgt_by, sizeof by);
        tgt = g_tgt;
        memset(g_tgt_by, 0, sizeof g_tgt_by);
        g_tgt = 0;
        pthread_mutex_unlock(&g_lock);
        double t = now_s(), dt = t - last, cpu = cpu_s();
        mach_enter();                                 /* the guest's memory, between two of its steps */
        last = t;
        long fr = swgb_frames;
        char line[2048];
        int n = snprintf(line, sizeof line, "[fps] %6.1fs mode %u cpu %3.0f%% presents %.0f/s changed %.0f/s",
                         t - t0, mode_of(), (cpu - last_cpu) * 100.0 / dt, (fr - last_frames) / dt,
                         __sync_lock_test_and_set(&g_changed, 0) / dt);
        last_cpu = cpu;
        last_frames = fr;
        uint32_t w = world_of(), wt = w ? guest_read32(w + WORLD_TIME) : 0, sc = game_screen();
        if (w) {
            uint32_t sp = guest_read32(w + WORLD_SPEED);
            float speed;
            memcpy(&speed, &sp, sizeof speed);
            long st = __sync_lock_test_and_set(&g_steps, 0), ms = __sync_lock_test_and_set(&g_step_ms, 0);
            long mx = __sync_lock_test_and_set(&g_step_max, 0);
            n += snprintf(line + n, sizeof line - n, " | world %u ms +%.0f/s speed %.2f",
                          wt, last_wt && wt >= last_wt ? (wt - last_wt) / dt : 0.0, speed);
            if (st) n += snprintf(line + n, sizeof line - n, " steps %.1f/s of %.1f ms (max %ld)", st / dt, (double)ms / st, mx);
        }
        if (sc) n += snprintf(line + n, sizeof line - n, " | gate %u ms", guest_read32(sc + G_SCREEN_GATE));
        last_wt = wt;
        mach_leave();
        if (tgt) {
            qsort(by, SLOTS, sizeof by[0], by_n);
            n += snprintf(line + n, sizeof line - n, " | timeGetTime %.0f/s", tgt / dt);
            for (int k = 0; k < 4 && by[k].n && n < (int)sizeof line - 24; k++)
                n += snprintf(line + n, sizeof line - n, " %08X:%ld", by[k].va, by[k].n);
        }
        fprintf(stderr, "%s\n", line);
    }
    return NULL;
}

/* ---- the host's side ---------------------------------------------------------- */
int framerate_arg(int argc, char** argv, int i) {
    if (!strcmp(argv[i], "--fps-log")) { g_log = 1; return 1; }
    if (!strcmp(argv[i], "--fps") && i + 1 < argc) {
        if (!strcasecmp(argv[i + 1], "off")) { g_pace = 0; g_fps = 0; return 2; }
        g_fps = atof(argv[i + 1]);
        if (g_fps < 1 || g_fps > 1000) { fprintf(stderr, "--fps: 1 to 1000, or off\n"); exit(1); }
        return 2;
    }
    return 0;
}

/* The wrappers, ahead of win32hle's (before swgb_register_shims' table). */
void framerate_register(void) {
    static const win32hle_shim shims[] = {
        { "PeekMessageA", s_PeekMessageA },
        { "timeGetTime", s_timeGetTime },
        { 0, 0 },
    };
    for (const win32hle_shim* t = win32hle_user32; t->name; t++) if (!strcmp(t->name, "PeekMessageA")) g_PeekMessageA = t->fn;
    for (const win32hle_shim* t = win32hle_winmm; t->name; t++) if (!strcmp(t->name, "timeGetTime")) g_timeGetTime = t->fn;
    if (!g_PeekMessageA || !g_timeGetTime) { fprintf(stderr, "[fps] win32hle has no PeekMessageA or timeGetTime\n"); return; }
    win32hle_register(shims);
}

/* After hle_screen_open: the display's refresh is known. */
void framerate_start(int headless) {
    if (!headless) {
        SDL_DisplayMode dm;
        if (SDL_GetCurrentDisplayMode(0, &dm) == 0 && dm.refresh_rate > 0) g_refresh = dm.refresh_rate;
    }
    if (g_pace) {
        if (g_fps > 0) printf("  frame rate: %.0f a second in game (--fps)\n", g_fps);
        else printf("  frame rate: the display's refresh (%d Hz) in game (--fps N to choose, --fps off for the game's own)\n", g_refresh);
    }
    if (g_log) {
        pthread_t t;
        pthread_create(&t, NULL, log_thread, NULL);
        pthread_detach(t);
    }
}
