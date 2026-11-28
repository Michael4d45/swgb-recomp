/*
 * --smooth: units drawn between simulation steps.
 *
 * The world steps about 20 times a second (docs/framerate.md), and a unit
 * moves only when it steps: at --fps 120 it stands still for six frames and
 * jumps on the seventh. With --smooth every object is drawn where it was
 * between its last two simulated positions, by how far the wall clock is
 * into the current step. Nothing the simulation reads is written: only the
 * screen position the draw call is given.
 *
 * Where an object is drawn (Battlegrounds.exe v1.1; docs/framerate.md,
 * "--smooth: drawn between steps"; guest_addrs.h has Clone Campaigns'):
 *   the map view's tile pass (0x0060A8DF) calls, for each visible tile,
 *   sub_00496470(tile, area, x, y, ...)  the object pass: for each object on
 *        the tile that the player can see, its virtual draw (vtable +0x70),
 *   sub_0054AD10(area, x, y, colors)    RGE_Static_Object's draw, every
 *        object class but one (sub_00556040, which ends in a call to it).
 *        It draws the object's sprite at x + [obj+0x34], y + [obj+0x36]
 *        (and its shadow at +0x38, +0x3A): the tile's place on screen plus
 *        the object's offset in the tile, which the simulation sets when the
 *        object moves (sub_0054DB30, new_location).
 * The draw is a virtual call, so the host owns it (recomp_lookup_manual):
 * when the caller is the object pass, x and y are moved by the screen
 * distance between where the object is and where it is drawn, and put back
 * after.
 *
 * Positions: [obj+0x48], +0x4C, +0x50, floats, in tiles (x, y, height). A
 * tile is [map+0xBF40] pixels wide by half and [map+0xBF3E] high by half
 * (48 and 24 in SWGB): a step of dx, dy, dz moves the sprite by
 *     sx = hw * (dx + dy),   sy = hh * (dy - dx - dz)
 * (new_location's own arithmetic). The selection frame and hit-point bar are
 * drawn inside the same draw, from the same x, y: they move with the sprite.
 *
 * The step: [world+0x20] is the wall time (timeGetTime) of the world's last
 * step. Between two steps at t0 and t1 = t0 + T, a frame at t is drawn with
 * f = (t - t1) / T, 0 to 1, and an object at p1 that was at p0 a step before
 * is drawn at p0 + f (p1 - p0): a step behind the simulation, so it never
 * overshoots where a unit stops, turns or dies. An object that was not
 * drawn the step before (it came into view, was made, or its slot was
 * reused), or that moved more than MAX_JUMP tiles in a step (a teleport),
 * is drawn where it is.
 */
#ifndef SWGB_WIN32HLE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest.h"
#include "guest_addrs.h"
#include "smooth.h"

#define ARG(n) MEM32(g_esp + 4 + 4 * (n))

int smooth_on;
int smooth_hooked;

void G_OBJ_DRAW(void);
void G_OBJ_DRAW2(void);

static uint32_t now_ms(void) {
#ifdef SWGB_WIN32HLE
    return hle_ticks_ms();       /* the guest's timeGetTime */
#else
    return timeGetTime();
#endif
}

static FILE* g_log;              /* SWGB_SMOOTH_LOG */

/* ---- the world's steps ------------------------------------------------------ */
static uint32_t g_world, g_step_t, g_step_n, g_interval = 50;

static void track_step(void) {
    uint32_t g = MEM32(G_GAME);
    uint32_t w = g ? MEM32(g + G_GAME_WORLD) : 0;
    if (!w) return;
    uint32_t t = MEM32(w + G_WORLD_STEP_T);
    if (w != g_world) { g_world = w; g_step_t = t; g_step_n += 2; return; }   /* a new game: no history */
    if (t != g_step_t) {
        uint32_t d = t - g_step_t;
        g_interval = d >= 10 && d <= 500 ? d : 50;
        g_step_t = t;
        g_step_n++;
        if (g_log) fflush(g_log);
    }
}

/* ---- each drawn object's last two positions --------------------------------- */
#define SLOTS 8192                   /* a power of two; a few hundred objects are drawn a frame */
#define MAX_JUMP 3.0f                /* tiles a step: more is a teleport, not a walk */
typedef struct { uint32_t obj, id, step; float x, y, z, px, py, pz; } ent_t;
static ent_t g_ent[SLOTS];

static ent_t* entry(uint32_t obj, uint32_t id) {
    uint32_t h = (obj >> 3) * 0x9E3779B1u >> 19;
    ent_t* free_e = NULL;
    for (int k = 0; k < 16; k++) {
        ent_t* e = &g_ent[(h + k) & (SLOTS - 1)];
        if (e->obj == obj) {
            if (e->id != id) { e->id = id; e->step = 0; }
            return e;
        }
        if (!free_e && (!e->obj || g_step_n - e->step > 2)) free_e = e;   /* empty, or not drawn for two steps */
    }
    if (!free_e) free_e = &g_ent[h & (SLOTS - 1)];
    free_e->obj = obj;
    free_e->id = id;
    free_e->step = 0;
    return free_e;
}

/* The screen offset (pixels) at which obj is drawn this frame, from where it is. */
static void offset_for(uint32_t obj, int* ox, int* oy) {
    *ox = *oy = 0;
    float x = *(float*)GUEST_PTR(obj + 0x48), y = *(float*)GUEST_PTR(obj + 0x4C), z = *(float*)GUEST_PTR(obj + 0x50);
    ent_t* e = entry(obj, MEM32(obj + G_OBJ_ID));
    if (e->step != g_step_n) {
        if (e->step && e->step + 1 == g_step_n) { e->px = e->x; e->py = e->y; e->pz = e->z; }
        else { e->px = x; e->py = y; e->pz = z; }
        e->x = x; e->y = y; e->z = z;
        e->step = g_step_n;
    } else if (e->x != x || e->y != y || e->z != z) {
        e->x = x; e->y = y; e->z = z;     /* moved between steps (not by the world): no history */
        e->px = x; e->py = y; e->pz = z;
    }
    float dx = e->x - e->px, dy = e->y - e->py, dz = e->z - e->pz;
    if (dx == 0 && dy == 0 && dz == 0) return;
    if (fabsf(dx) > MAX_JUMP || fabsf(dy) > MAX_JUMP || fabsf(dz) > MAX_JUMP) return;
    uint32_t el = now_ms() - g_step_t;
    float f = el >= g_interval ? 1.0f : (float)el / (float)g_interval;
    float back = f - 1.0f;           /* -1 at the step, 0 a step later */
    uint32_t map = MEM32(g_world + 0x34);
    int hw = (int16_t)MEM16(map + G_MAP_HALF_W), hh = (int16_t)MEM16(map + G_MAP_HALF_H);
    *ox = (int)lrintf(back * hw * (dx + dy));
    *oy = (int)lrintf(back * hh * (dy - dx - dz));
}

/* ---- the hooked draw ------------------------------------------------------------ */
static void log_draw(uint32_t obj, int x, int y, int ox, int oy) {
    if (!g_log) return;
    float px = *(float*)GUEST_PTR(obj + 0x48), py = *(float*)GUEST_PTR(obj + 0x4C);
    fprintf(g_log, "%u %u %u %08X %u %.4f %.4f %d %d %d %d\n", now_ms(), g_step_n, g_step_t, obj,
            MEM32(obj + G_OBJ_ID), px, py, x + (int16_t)MEM16(obj + 0x34), y + (int16_t)MEM16(obj + 0x36), ox, oy);
}

static int g_announced;
static void draw(recomp_func_t fn) {
    if (g_cur_func != G_OBJ_PASS) { fn(); return; }
    uint32_t obj = g_ecx, at = g_esp;
    uint32_t x = ARG(1), y = ARG(2);
    int ox = 0, oy = 0;
    track_step();
    if (g_world) offset_for(obj, &ox, &oy);
    if (!smooth_on) ox = oy = 0;
    if ((ox || oy) && !g_announced) {
        g_announced = 1;
        printf("[smooth] first object drawn between steps (%+d, %+d px)\n", ox, oy);
        fflush(stdout);
    }
    log_draw(obj, (int16_t)x, (int16_t)y, ox, oy);
    MEM32(at + 8) = (uint32_t)((int)x + ox);
    MEM32(at + 12) = (uint32_t)((int)y + oy);
    fn();
    MEM32(at + 8) = x;               /* the caller's argument slots, as they were */
    MEM32(at + 12) = y;
}
static void hook_draw(void)  { draw(G_OBJ_DRAW); }
static void hook_draw2(void) { draw(G_OBJ_DRAW2); }

void smooth_init(void) {
    const char* log = getenv("SWGB_SMOOTH_LOG");
    if (log && *log) {
        g_log = fopen(log, "w");
        if (g_log) printf("[smooth] logging object draws to %s\n", log);
    }
    smooth_hooked = smooth_on || g_log;
    if (smooth_on) printf("  --smooth: units drawn between simulation steps\n");
}

recomp_func_t smooth_lookup(uint32_t va) {
    if (!smooth_on && !g_log) return NULL;
    if (va == G_OBJ_DRAW_VA) return hook_draw;
    if (va == G_OBJ_DRAW2_VA) return hook_draw2;
    return NULL;
}
