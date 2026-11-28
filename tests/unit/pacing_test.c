/* src/runtime/framerate.c's --fps pacing without the game: the file itself is
 * compiled in here, with a fake clock (timeGetTime) and a fake wait
 * (MsgWaitForMultipleObjects), against a game object and game screen at the
 * engine's own addresses. The game's idle runs a pass when
 * timeGetTime() - [screen+LAST] >= [screen+GATE], as sub_004F6EA0 does.
 *
 *   --fps 60 and 30: that many passes a second, the gate 16 and 17 ms in turn
 *   the default: the display's refresh, held to 30..1000
 *   a long pass (a hitch): no burst of passes to catch up
 *   the menus (no game screen): the gate is left alone, the wait is 1 ms
 *   --fps, --fps off, --fps-log: what framerate_arg takes
 *
 * Built twice by tools/test.py: the base game's addresses and SWGB_CC's.
 */
#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#include <windows.h>
#include <mmsystem.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* The clock, in microseconds; the real one is milliseconds. */
static uint64_t g_now_us = 1000000;
static DWORD fake_time(void) { return (DWORD)(g_now_us / 1000); }
static int g_waits, g_wait_ms_total, g_last_wait;
static DWORD fake_wait(DWORD ms) { g_now_us += (uint64_t)ms * 1000; g_waits++; g_wait_ms_total += ms; g_last_wait = ms; return WAIT_TIMEOUT; }
#define timeGetTime() fake_time()
#define MsgWaitForMultipleObjects(n, h, all, ms, mask) fake_wait(ms)

#include "../../src/runtime/framerate.c"

/* What framerate.c uses from the rest of the host. */
uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp, g_cur_func;
volatile long softdd_frames;
void (*softdd_on_present)(const uint8_t* bits, int w, int h, int pitch, long frame);
Surf* softdd_surf(void* iface) { (void)iface; return NULL; }
static double g_refresh = 60;
double present_refresh_hz(void) { return g_refresh; }
void mach_enter(void) {}
void mach_leave(void) {}
static void f0(void) {}
const recomp_dispatch_entry_t recomp_dispatch_table[] = { { 0x00401000u, f0 } };
const uint32_t recomp_dispatch_count = 1;

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t g_game, g_screen;
static uint8_t g_guest[0x380000];   /* 0x0042xxxx..0x007Axxxx: the engine's globals' addresses */

static void reset(double fps, int mode) {
    g_fps = fps;
    g_pace = 1;
    g_due = 0;
    g_seen = 0;
    MEM32(g_game + G_GAME_MODE) = mode;
    MEM32(g_screen + SCREEN_LAST) = fake_time();
    MEM32(g_screen + SCREEN_GATE) = 1;               /* the game's own: 1 ms */
}

typedef struct { int passes, min_gap_ms, max_gap_ms; int gates[64]; int ngates; } run_t;

/* The game's loop for `seconds` of fake time: PeekMessage finds nothing (pace), then the
 * idle runs a pass if the gate allows it; a pass costs work_us, the one at hitch_at_s
 * hitch_ms more. */
static run_t loop(double seconds, int work_us, double hitch_at_s, int hitch_ms) {
    run_t r = { 0, 1 << 30, 0 };
    uint64_t end = g_now_us + (uint64_t)(seconds * 1e6), start = g_now_us, last_pass = 0;
    int hitched = 0;
    while (g_now_us < end) {
        pace();
        g_now_us += 20;                               /* the loop itself */
        uint32_t sc = g_screen, now = fake_time();
        if (now - MEM32(sc + SCREEN_LAST) >= MEM32(sc + SCREEN_GATE)) {
            MEM32(sc + SCREEN_LAST) = now;
            if (last_pass) {
                int gap = (int)((g_now_us - last_pass) / 1000);
                if (gap < r.min_gap_ms) r.min_gap_ms = gap;
                if (gap > r.max_gap_ms) r.max_gap_ms = gap;
            }
            last_pass = g_now_us;
            r.passes++;
            if (r.ngates < 64) r.gates[r.ngates++] = (int)MEM32(sc + SCREEN_GATE);
            g_now_us += work_us;
            if (!hitched && hitch_ms && g_now_us - start >= (uint64_t)(hitch_at_s * 1e6)) {
                hitched = 1;
                g_now_us += (uint64_t)hitch_ms * 1000;
            }
        }
    }
    return r;
}

int main(void) {
    /* The engine's game object pointer is at a fixed address. The process's low memory is
     * full of mapped files by the time main runs, so this exe is linked at 0x00400000
     * (tools/test.py: /BASE /FIXED) and its .bss spans the address. */
    if (GAME < (uint32_t)(uintptr_t)g_guest || GAME + 4 > (uint32_t)(uintptr_t)g_guest + sizeof g_guest) {
        printf("FAIL: 0x%08X is not in the test's .bss (%p..+0x%X): link it at /BASE:0x00400000 /FIXED\n",
               GAME, (void*)g_guest, (unsigned)sizeof g_guest);
        return 1;
    }
    uint8_t* game = (uint8_t*)calloc(1, 0x2000), *screen = (uint8_t*)calloc(1, 0x2000);
    g_game = (uint32_t)(uintptr_t)game;
    g_screen = (uint32_t)(uintptr_t)screen;
    MEM32(GAME) = g_game;
    MEM32(g_game + G_GAME_SCREEN) = g_screen;
    MEM32(g_screen) = SCREEN_VTABLE;

    /* --fps 60 and 30: that many passes a second; frames start every 16.67 ms (16 and 17) */
    static const double rates[] = { 60, 30, 144, 1000 };
    for (int i = 0; i < 4; i++) {
        double fps = rates[i];
        reset(fps, 4);
        int work = fps >= 1000 ? 300 : 2000;
        run_t r = loop(10, work, 0, 0);
        double got = r.passes / 10.0;
        CHECK(got >= fps * 0.98 && got <= fps * 1.02 + 0.2, "--fps %.0f: %.1f passes a second", fps, got);
        printf("  --fps %-4.0f %7.1f passes/s, gaps %d..%d ms\n", fps, got, r.min_gap_ms, r.max_gap_ms);
        if (fps == 60) {
            int odd = 0;
            for (int k = 1; k < r.ngates; k++) odd += r.gates[k] != 16 && r.gates[k] != 17;
            CHECK(r.min_gap_ms >= 15 && r.max_gap_ms <= 18, "--fps 60: passes %d..%d ms apart (want 16..17)", r.min_gap_ms, r.max_gap_ms);
            CHECK(!odd, "--fps 60: %d gates other than 16 or 17 ms", odd);
        }
    }

    /* the default (no --fps): the display's refresh, held to 30..1000 */
    static const double refresh[][2] = { { 60, 60 }, { 144, 144 }, { 10, 30 }, { 5000, 1000 } };
    for (int i = 0; i < 4; i++) {
        g_refresh = refresh[i][0];
        reset(0, 4);
        run_t r = loop(5, refresh[i][1] >= 1000 ? 300 : 1000, 0, 0);
        double got = r.passes / 5.0, want = refresh[i][1];
        CHECK(got >= want * 0.97 && got <= want * 1.03 + 0.2, "refresh %.0f Hz: %.1f passes a second (want %.0f)", refresh[i][0], got, want);
        printf("  refresh %-5.0f %7.1f passes/s (want %.0f)\n", refresh[i][0], got, want);
    }
    g_refresh = 60;

    /* a hitch: one pass of 250 ms at --fps 60. The next pass comes at once, then the rate
     * goes on from there: no burst of short passes to catch up the time lost. */
    {
        reset(60, 4);
        run_t before = loop(2, 2000, 0, 0);
        run_t r = loop(2, 2000, 0.5, 250);       /* the pacing state carries on across it */
        /* 2 s at 60 is 120 passes; 250 ms of them are lost, and not made up */
        CHECK(r.passes >= 120 - 16 - 2 && r.passes <= 120 - 15 + 2, "hitch: %d passes in 2 s (want about 105)", r.passes);
        CHECK(r.max_gap_ms >= 250, "hitch: the longest gap %d ms", r.max_gap_ms);
        printf("  hitch      %d passes in 2 s (%d without), shortest gap %d ms\n", r.passes, before.passes, r.min_gap_ms);
    }

    /* the menus: no game screen (mode 0): the gate is not touched, the host only yields 1 ms */
    {
        reset(60, 0);
        MEM32(g_screen + SCREEN_GATE) = 1;
        g_waits = 0;
        g_last_wait = 0;
        for (int i = 0; i < 100; i++) pace();
        CHECK(MEM32(g_screen + SCREEN_GATE) == 1, "menus: the gate was changed to %u", MEM32(g_screen + SCREEN_GATE));
        CHECK(g_waits == 100 && g_last_wait == 1, "menus: %d waits, the last %d ms", g_waits, g_last_wait);
        /* a game screen that is not one (another vtable): the same */
        reset(60, 4);
        MEM32(g_screen) = SCREEN_VTABLE + 4;
        g_waits = 0;
        for (int i = 0; i < 10; i++) pace();
        CHECK(MEM32(g_screen + SCREEN_GATE) == 1 && g_waits == 10, "not a game screen: gate %u", MEM32(g_screen + SCREEN_GATE));
        MEM32(g_screen) = SCREEN_VTABLE;
    }

    /* the command line */
    {
        char* a1[] = { "x", "--fps", "75" };
        char* a2[] = { "x", "--fps", "off" };
        char* a3[] = { "x", "--fps-log" };
        char* a4[] = { "x", "--fpsx" };
        g_fps = 0; g_pace = 1; g_log = 0;
        CHECK(framerate_arg(3, a1, 1) == 2 && g_fps == 75 && g_pace == 1, "--fps 75");
        CHECK(framerate_arg(3, a2, 1) == 2 && g_fps == 0 && g_pace == 0, "--fps off");
        CHECK(framerate_arg(2, a3, 1) == 1 && g_log == 1, "--fps-log");
        CHECK(framerate_arg(2, a4, 1) == 0, "--fpsx is not ours");
        CHECK(framerate_arg(2, a1, 1) == 0, "--fps with no value is not taken");
    }

    printf(fails ? "%d FAILED\n" : "OK\n", fails);
    return fails != 0;
}
