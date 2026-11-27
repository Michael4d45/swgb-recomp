/* src/runtime/smooth.c (--smooth) without the game: the file itself is compiled
 * in here, with a fake clock (timeGetTime), against a game object, world, map and
 * objects at the engine's own offsets. The world steps every 50 ms, as
 * sub_0061AB70 does from 20 frames a second up; a frame is drawn every 5 ms by
 * calling the hooked draw as the object pass does (the caller sub_00496470, the
 * object in ecx, area, x, y, colors on the stack).
 *
 *   a walking object: drawn a little further each frame, never more than a
 *     frame's share of a step at once (and the plain draw: a whole step at once)
 *   it stops: drawn up to where it stopped, never past it
 *   a teleport (more than 3 tiles in a step), a new object in a slot, a new
 *     game: drawn where it is
 *   the draw called from anywhere but the object pass: untouched
 *   the caller's argument slots and the object: as they were (nothing written)
 *   no --smooth: no hook
 *
 * Built twice by tools/test.py: the base game's addresses and SWGB_CC's.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t g_now = 1000000;   /* the clock, ms */
#define timeGetTime() (g_now)

#include "../../src/runtime/smooth.c"

/* What smooth.c uses from the rest of the host. */
uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp, g_cur_func;

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t g_guest[0x380000];   /* 0x0042xxxx..0x007Axxxx: the engine's globals' addresses */
static uint32_t t_game, t_world, t_map;
static uint8_t g_stack[4096];

/* The objects' draw, as the engine's: where the sprite lands (x + [obj+0x34], ...). */
static int g_drawn_x, g_drawn_y, g_draws;
static uint32_t g_drawn_obj;
static void fake_draw(void) {
    g_drawn_obj = g_ecx;
    g_drawn_x = (int16_t)ARG(1) + (int16_t)MEM16(g_ecx + 0x34);
    g_drawn_y = (int16_t)ARG(2) + (int16_t)MEM16(g_ecx + 0x36);
    g_draws++;
    g_esp += 4 + 16;   /* ret 0x10 */
}
void G_OBJ_DRAW(void) { fake_draw(); }
void G_OBJ_DRAW2(void) { fake_draw(); }

#define HW 48
#define HH 24
static uint32_t new_obj(uint32_t id, float x, float y) {
    uint32_t o = (uint32_t)(uintptr_t)calloc(1, 0x100);
    MEM32(o + G_OBJ_ID) = id;
    *(float*)(uintptr_t)(o + 0x48) = x;
    *(float*)(uintptr_t)(o + 0x4C) = y;
    return o;
}
static void place(uint32_t o, float x, float y) {
    *(float*)(uintptr_t)(o + 0x48) = x;
    *(float*)(uintptr_t)(o + 0x4C) = y;
}
/* The world steps: the game's clock of the step. */
static void step(void) { MEM32(t_world + G_WORLD_STEP_T) = g_now; }

/* One frame's draw of o as the object pass makes it; the place it is drawn, in pixels.
 * The tile's place is the object's own here (its offset in the tile is 0). */
static int frame(uint32_t o, uint32_t caller, uint32_t va, int* dy) {
    float x = *(float*)(uintptr_t)(o + 0x48), y = *(float*)(uintptr_t)(o + 0x4C);
    int sx = (int)lrintf(HW * (x + y)), sy = (int)lrintf(HH * (y - x));
    g_esp = (uint32_t)(uintptr_t)(g_stack + 2048);
    MEM32(g_esp) = 0;                     /* the return address */
    MEM32(g_esp + 4) = 0x1234;            /* the area */
    MEM32(g_esp + 8) = (uint32_t)sx;
    MEM32(g_esp + 12) = (uint32_t)sy;
    MEM32(g_esp + 16) = 0x5678;           /* the colors */
    uint32_t at = g_esp;
    g_ecx = o;
    g_cur_func = caller;
    recomp_func_t f = smooth_lookup(va);
    g_drawn_x = g_drawn_y = -99999;
    if (f) f(); else fake_draw();
    CHECK(MEM32(at + 8) == (uint32_t)sx && MEM32(at + 12) == (uint32_t)sy && MEM32(at + 4) == 0x1234 && MEM32(at + 16) == 0x5678,
          "the caller's argument slots were not put back");
    if (dy) *dy = g_drawn_y;
    return g_drawn_x;
}
static int frame_pass(uint32_t o, int* dy) { return frame(o, G_OBJ_PASS, G_OBJ_DRAW_VA, dy); }

/* A walk of `steps` steps of (vx, vy) tiles, a frame every frame_ms; the largest change
 * from one frame to the next of the drawn x, the number of frames it did not move, and
 * the drawn x at the end. */
typedef struct { int max_jump, still, frames, last_x, overshoot; } walk_t;
static walk_t walk(uint32_t o, int steps, float vx, float vy, int frame_ms, int step_ms, float stop_after) {
    walk_t w = { 0 };
    int prev = frame_pass(o, NULL);
    float x = *(float*)(uintptr_t)(o + 0x48), y = *(float*)(uintptr_t)(o + 0x4C);
    int travelled = 0;
    for (int s = 0; s < steps; s++) {
        for (int t = 0; t < step_ms; t += frame_ms) {
            g_now += frame_ms;
            if (t + frame_ms >= step_ms) {      /* the world steps at the start of this frame */
                if (travelled < stop_after) { x += vx; y += vy; travelled++; }
                place(o, x, y);
                step();
            }
            int d = frame_pass(o, NULL);
            int j = abs(d - prev);
            if (j > w.max_jump) w.max_jump = j;
            if (j == 0) w.still++;
            w.frames++;
            int true_x = (int)lrintf(HW * (x + y));
            if ((vx + vy > 0 && d > true_x) || (vx + vy < 0 && d < true_x)) w.overshoot++;
            prev = d;
        }
    }
    w.last_x = prev;
    return w;
}

int main(void) {
    if (G_GAME < (uint32_t)(uintptr_t)g_guest || G_GAME + 4 > (uint32_t)(uintptr_t)g_guest + sizeof g_guest) {
        printf("FAIL: 0x%08X is not in the test's .bss (%p..+0x%X): link it at /BASE:0x00400000 /FIXED\n",
               G_GAME, (void*)g_guest, (unsigned)sizeof g_guest);
        return 1;
    }
    t_game = (uint32_t)(uintptr_t)calloc(1, 0x2000);
    t_world = (uint32_t)(uintptr_t)calloc(1, 0x100);
    t_map = (uint32_t)(uintptr_t)calloc(1, 0xC000);
    MEM32(G_GAME) = t_game;
    MEM32(t_game + 0x420) = t_world;
    MEM32(t_world + 0x34) = t_map;
    MEM16(t_map + G_MAP_HALF_W) = HW;
    MEM16(t_map + G_MAP_HALF_H) = HH;
    step();

    /* no --smooth (and no log): the draw is not the host's */
    smooth_on = 0;
    CHECK(smooth_lookup(G_OBJ_DRAW_VA) == NULL && smooth_lookup(G_OBJ_DRAW2_VA) == NULL, "no --smooth: hooked");
    {   /* the plain draw: a walking object moves a whole step at once, then stands */
        uint32_t o = new_obj(7, 10, 10);
        walk_t w = walk(o, 20, 0.1f, 0.05f, 5, 50, 1e9f);
        printf("  plain      largest jump %2d px, %3d of %3d frames still\n", w.max_jump, w.still, w.frames);
        CHECK(w.max_jump >= 7 && w.still >= w.frames * 8 / 10, "the plain draw: jump %d px, %d still", w.max_jump, w.still);
    }

    smooth_on = 1;
    CHECK(smooth_lookup(G_OBJ_DRAW_VA) == hook_draw && smooth_lookup(G_OBJ_DRAW2_VA) == hook_draw2, "--smooth: not hooked");
    CHECK(smooth_lookup(0x00401000u) == NULL, "--smooth hooks another function");

    /* walking: 0.15 tiles a step along x + y is 7.2 px; at 5 ms frames and 50 ms steps a
     * frame moves 0.72 px: at most 1, and 2 where two roundings meet */
    {
        uint32_t o = new_obj(8, 20, 20);
        uint8_t before[0x100];
        walk(o, 2, 0.1f, 0.05f, 5, 50, 1e9f);   /* history: two steps */
        memcpy(before, (void*)(uintptr_t)o, sizeof before);
        int x0 = frame_pass(o, NULL);
        CHECK(!memcmp(before, (void*)(uintptr_t)o, sizeof before), "the object was written");
        (void)x0;
        walk_t w = walk(o, 20, 0.1f, 0.05f, 5, 50, 1e9f);
        printf("  --smooth   largest jump %2d px, %3d of %3d frames still\n", w.max_jump, w.still, w.frames);
        CHECK(w.max_jump <= 2, "--smooth: a jump of %d px", w.max_jump);
        CHECK(w.still <= w.frames / 2, "--smooth: %d of %d frames still", w.still, w.frames);
        CHECK(!w.overshoot, "--smooth: drawn ahead of the object %d times", w.overshoot);
        /* at 144 frames a second (7 ms) and 120 (8 ms): the same */
        walk_t w7 = walk(o, 20, 0.1f, 0.05f, 7, 49, 1e9f), w8 = walk(o, 20, 0.1f, 0.05f, 8, 56, 1e9f);
        CHECK(w7.max_jump <= 2 && w8.max_jump <= 2, "--smooth at 7 and 8 ms frames: jumps %d, %d px", w7.max_jump, w8.max_jump);
    }

    /* it stops after 3 steps: drawn up to where it stopped, never past, and then there */
    {
        uint32_t o = new_obj(9, 30, 30);
        walk(o, 2, -0.1f, -0.05f, 5, 50, 1e9f);
        float x = *(float*)(uintptr_t)(o + 0x48), y = *(float*)(uintptr_t)(o + 0x4C);
        walk_t w = walk(o, 8, -0.1f, -0.05f, 5, 50, 3);
        int stop_x = (int)lrintf(HW * (x - 0.3f + y - 0.15f));
        printf("  stopping   largest jump %2d px, ends at %d (stopped at %d)\n", w.max_jump, w.last_x, stop_x);
        CHECK(!w.overshoot, "stopping: drawn past the object %d times", w.overshoot);
        CHECK(abs(w.last_x - stop_x) <= 1, "stopping: drawn at %d, it stopped at %d", w.last_x, stop_x);
        CHECK(w.max_jump <= 2, "stopping: a jump of %d px", w.max_jump);
    }

    /* a teleport: 5 tiles in a step is drawn where it is at once */
    {
        uint32_t o = new_obj(10, 40, 40);
        walk(o, 2, 0.1f, 0.0f, 5, 50, 1e9f);
        g_now += 50;
        place(o, 45, 40);
        step();
        int d = frame_pass(o, NULL);
        CHECK(d == HW * 85, "a teleport: drawn at %d, it is at %d", d, HW * 85);
    }

    /* the draw from anywhere but the object pass: the plain place */
    {
        uint32_t o = new_obj(11, 50, 50);
        walk(o, 2, 0.1f, 0.0f, 5, 50, 1e9f);
        g_now += 50;
        place(o, 50.3f, 50);
        step();
        int dy, d = frame(o, 0x00401000u, G_OBJ_DRAW_VA, &dy);
        CHECK(d == (int)lrintf(HW * (50.3f + 50)), "another caller: drawn at %d", d);
        /* and from the object pass, the same frame: a step behind */
        d = frame_pass(o, NULL);
        CHECK(abs(d - (int)lrintf(HW * 100.2f)) <= 1, "the object pass at the step: drawn at %d, want %d", d, (int)lrintf(HW * 100.2f));
        /* the other draw (vtable 0x006600E0's): the same */
        d = frame(o, G_OBJ_PASS, G_OBJ_DRAW2_VA, NULL);
        CHECK(abs(d - (int)lrintf(HW * 100.2f)) <= 1, "the second draw: drawn at %d", d);
    }

    /* another object in the slot (a new id): no history */
    {
        uint32_t o = new_obj(12, 60, 60);
        walk(o, 3, 0.1f, 0.0f, 5, 50, 1e9f);
        g_now += 50;
        MEM32(o + G_OBJ_ID) = 13;
        place(o, 61, 60);
        step();
        int d = frame_pass(o, NULL);
        CHECK(d == HW * 121, "a new object in the slot: drawn at %d, it is at %d", d, HW * 121);
    }

    /* a new game (another world): no history */
    {
        uint32_t o = new_obj(14, 70, 70);
        walk(o, 3, 0.1f, 0.0f, 5, 50, 1e9f);
        uint32_t w2 = (uint32_t)(uintptr_t)calloc(1, 0x100);
        MEM32(w2 + 0x34) = t_map;
        MEM32(t_game + 0x420) = w2;
        t_world = w2;
        g_now += 50;
        place(o, 70.6f, 70);
        step();
        int d = frame_pass(o, NULL);
        CHECK(d == (int)lrintf(HW * 140.6f), "a new game: drawn at %d", d);
    }

    /* a paused game (no steps): drawn where it is, still */
    {
        uint32_t o = new_obj(15, 80, 80);
        walk(o, 3, 0.1f, 0.0f, 5, 50, 1e9f);
        g_now += 400;
        int a = frame_pass(o, NULL);
        g_now += 400;
        int b = frame_pass(o, NULL);
        float x = *(float*)(uintptr_t)(o + 0x48);
        CHECK(a == b && a == (int)lrintf(HW * (x + 80)), "paused: drawn at %d then %d, it is at %d", a, b, (int)lrintf(HW * (x + 80)));
    }

    printf(fails ? "%d FAILED\n" : "OK\n", fails);
    return fails != 0;
}
