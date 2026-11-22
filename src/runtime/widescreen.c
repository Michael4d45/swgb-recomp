/*
 * Any resolution: --res WxH (or --res desktop) makes the game's 1280x1024
 * Screen Size mean WxH.
 *
 * How the game picks its mode (Battlegrounds.exe v1.1):
 *   prog_info = [[0x006A0FD4] + 0x24] (the game object's program info)
 *   +0x8F4/+0x8FC  the in-game width and height. Set from the registry's
 *                  "Screen Size" (sub_00424290: 640/800 -> 800x600, 1024 ->
 *                  1024x768, 1280 -> 1280x1024, anything else ignored), the
 *                  command-line switches 800/1024/1280 (sub_004256D0), and
 *                  the Options dialog (sub_00459FC0 -> sub_0042C100).
 *   +0x8EC/+0x8F0  the menus' size, 800x600.
 *   +0x8E4/+0x8E8  the mode DirectDraw starts in (sub_00425E20).
 * Entering a game, sub_0042C0C0 calls the game's virtual set_screen_size
 * (vtable 0x00665A20 slot 0x68, sub_005ED2D0; its base is sub_0042C170)
 * with +0x8F4/+0x8FC, which changes the mode through sub_00470AF0
 * (IDirectDraw::SetDisplayMode) and lays the game screen out again.
 * Leaving it, sub_0042C0E0 calls the same with +0x8EC/+0x8F0.
 *
 * That call is indirect, so the host owns it (recomp_lookup_manual): a
 * request for 1280x1024 becomes WxH. +0x8F4 stays 1280, so the registry,
 * the Options dialog and every "which art" choice keyed off it still see
 * the 1280 entry and use the 1280-wide interface art.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native32.h"
#include "widescreen.h"

#define ARG(n) MEM32(g_esp + 4 + 4 * (n))

int widescreen_w, widescreen_h;
static int g_base_w, g_base_h;
static uint32_t g_screen;   /* the game screen laid out at WxH */
static int g_map_h = 774;   /* the map view's height in the base layout */

void sub_005ED2D0(void);
void sub_004F5210(void);
void sub_004FEDB0(void);
void sub_005D5D00(void);
void sub_004F6A70(void);
void sub_004F46B0(void);

static FILE* g_trace;
static int g_trace_on;

int widescreen_parse(const char* s) {
    int w = 0, h = 0;
    if (!_stricmp(s, "desktop")) {
        w = GetSystemMetrics(SM_CXSCREEN);
        h = GetSystemMetrics(SM_CYSCREEN);
    } else if (sscanf(s, "%dx%d", &w, &h) != 2) {
        return 0;
    }
    if (w < 800 || h < 600 || w > 4096 || h > 4096) {
        fprintf(stderr, "--res %s: from 800x600 to 4096x4096\n", s);
        return 0;
    }
    widescreen_w = w & ~3;   /* the game's spans and pitches are dword-aligned */
    widescreen_h = h;
    g_base_w = w >= 1280 && h >= 720 ? 1280 : w >= 1024 && h >= 700 ? 1024 : 800;
    g_base_h = g_base_w == 1280 ? 1024 : g_base_w == 1024 ? 768 : 600;
    return 1;
}

/* vtable 0x00665A20 + 0x68: set_screen_size(w, h), thiscall. */
static void hook_set_screen_size(void) {
    if (ARG(0) == 1280 && ARG(1) == 1024) {
        printf("[res] screen size 1280x1024 -> %dx%d\n", widescreen_w, widescreen_h);
        ARG(0) = (uint32_t)widescreen_w;
        ARG(1) = (uint32_t)widescreen_h;
    }
    if (!g_trace && getenv("SWGB_RES_TRACE")) g_trace = fopen(getenv("SWGB_RES_TRACE"), "w");
    if (g_trace) { fprintf(g_trace, "---- set_screen_size %d %d\n", (int)ARG(0), (int)ARG(1)); fflush(g_trace); g_trace_on = 1; }
    sub_005ED2D0();
}

/* SWGB_RES_TRACE=FILE: every new (target, caller) pair of indirect calls
 * after the first mode change, with ecx's vtable and four arguments. The
 * registers are only out when the callee runs, so a trampoline logs them. */
static uint32_t g_trace_va;
static recomp_func_t g_trace_fn;
static void trace_tramp(void) {
    uint32_t va = g_trace_va;
    recomp_func_t fn = g_trace_fn;
    uint32_t vt = g_ecx > 0x10000 && g_ecx < 0x7FFF0000 ? MEM32(g_ecx) : 0;
    fprintf(g_trace, "%08X from %08X  ecx %08X vt %08X  args %d %d %d %d\n", va, g_cur_func, g_ecx, vt,
            (int)ARG(0), (int)ARG(1), (int)ARG(2), (int)ARG(3));
    fn();
}
static recomp_func_t trace_icall(uint32_t va, recomp_func_t fn) {
    static uint64_t seen[1 << 16];
    uint64_t key = ((uint64_t)va << 32) | g_cur_func;
    uint32_t h = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 48);
    while (seen[h] && seen[h] != key) h = (h + 1) & 0xFFFF;
    if (seen[h] || !fn) return fn;
    seen[h] = key;
    g_trace_va = va;
    g_trace_fn = fn;
    return trace_tramp;
}

/* ---- panels (the engine's TPanel) -----------------------------------------
 * +0x00 vtable (slot 0x0C: set_rect(x, y, w, h), screen pixels)
 * +0x0C x  +0x10 y  +0x14 w  +0x18 h  +0x20 the draw area
 * +0x24 the clip rect (l, t, r, b, inclusive; within the parent's)
 * +0x40 the parent  +0x58 the first child's node {panel, prev, next} */
#define P_X(p)   MEM32((p) + 0x0C)
#define P_Y(p)   MEM32((p) + 0x10)
#define P_W(p)   MEM32((p) + 0x14)
#define P_H(p)   MEM32((p) + 0x18)
#define P_KIDS(p) MEM32((p) + 0x58)

/* A thiscall into lifted code from a host function that runs in the lifted
 * model (a hook): the arguments and a dummy return address go on the guest
 * stack, and the callee's `ret n` takes them off. */
static void guest_thiscall(uint32_t va, uint32_t self, int n, const uint32_t* args) {
    recomp_func_t f = recomp_resolve(va);
    if (!f) { fprintf(stderr, "[res] no function at 0x%08X\n", va); return; }
    for (int i = n - 1; i >= 0; i--) { g_esp -= 4; MEM32(g_esp) = args[i]; }
    g_esp -= 4;
    MEM32(g_esp) = 0;
    g_ecx = self;
    uint32_t caller = g_cur_func;
    f();
    g_cur_func = caller;
}

static void set_rect(uint32_t p, int x, int y, int w, int h) {
    uint32_t a[4] = { (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h };
    guest_thiscall(MEM32(MEM32(p) + 0x0C), p, 4, a);
}

static void dump_tree(FILE* f, uint32_t p, int depth) {
    fprintf(f, "%*s%08X vt %08X  %d,%d %dx%d\n", depth * 2, "", p, MEM32(p),
            (int)P_X(p), (int)P_Y(p), (int)P_W(p), (int)P_H(p));
    for (uint32_t n = P_KIDS(p); n; n = MEM32(n + 8)) dump_tree(f, MEM32(n), depth + 1);
}

/* Moves p and everything under it (positions are screen pixels, not the
 * parent's): the parent first, so a child's clip rect lands inside it. */
static void move_tree(uint32_t p, int dx, int dy) {
    set_rect(p, (int)P_X(p) + dx, (int)P_Y(p) + dy, (int)P_W(p), (int)P_H(p));
    for (uint32_t n = P_KIDS(p); n; n = MEM32(n + 8)) move_tree(MEM32(n), dx, dy);
}

/* A panel over the map keeps its place relative to the nearest edge (or the
 * middle) of the map area, which grew by grow_w, grow_h from base_w x map_h.
 * One that spans the map's width (the chat and message lines) widens. */
static void anchor_tree(uint32_t p, int base_w, int map_h, int grow_w, int grow_h) {
    int x = (int)P_X(p), y = (int)P_Y(p), w = (int)P_W(p), h = (int)P_H(p);
    int cx = x + w / 2, cy = y + h / 2, mx, my;
    if (w <= 0 || h <= 0) return;   /* placed when it is shown */
    mx = cx < base_w / 3 ? 0 : cx > base_w * 2 / 3 ? grow_w : grow_w / 2;
    my = cy < map_h / 3 ? 0 : cy > map_h * 2 / 3 ? grow_h : grow_h / 2;
    if (w >= base_w * 9 / 10) {
        set_rect(p, x, y + my, w + grow_w, h);
        for (uint32_t n = P_KIDS(p); n; n = MEM32(n + 8)) move_tree(MEM32(n), 0, my);
    } else {
        move_tree(p, mx, my);
    }
}


/* vtable 0x0065CE24 + 0x50: the game screen's handle_size(w, h), sub_004F5210.
 * It places every panel from screen_coords.txt (DRS 'bina' 50006), choosing
 * the column by an exact match of its width (1024, 1280, else 800). At WxH it
 * lays out at the base size instead; then the screen takes WxH, the map
 * view (the child at 0,0) grows to fill everything above the interface, and
 * the interface panels go to the bottom, centred. */
static void hook_game_screen_size(void) {
    uint32_t screen = g_ecx;
    int W = widescreen_w, H = widescreen_h;
    if ((int)ARG(0) != W || (int)ARG(1) != H) {
        if (screen == g_screen) g_screen = 0;   /* another size: the game's own layout */
        sub_004F5210();
        return;
    }
    ARG(0) = (uint32_t)g_base_w;
    ARG(1) = (uint32_t)g_base_h;
    uint32_t esp = g_esp;
    sub_004F5210();
    uint32_t ret = g_eax, esp_after = g_esp;
    g_esp = esp;   /* below the caller's arguments: room for our own calls */
    g_esp -= 64;

    int dx = (W - g_base_w) / 2, dy = H - g_base_h;
    uint32_t a[4] = { 0, 0, (uint32_t)W, (uint32_t)H };
    guest_thiscall(0x004B5660u, screen, 4, a);   /* TPanel::set_rect, not the screen's own */
    if (!MEM32(screen + 0x10A4)) {   /* its own early way out: no interface yet */
        g_esp = esp_after;
        g_eax = ret;
        return;
    }
    g_screen = screen;
    uint32_t map = 0;
    for (uint32_t n = P_KIDS(screen); n; n = MEM32(n + 8)) {
        uint32_t c = MEM32(n);
        if (P_X(c) == 0 && P_Y(c) == 0 && (int)P_W(c) == g_base_w && (int)P_H(c) < g_base_h) { map = c; break; }
    }
    if (map) {
        /* The map view: everything above the interface. Its own children (the
         * chat, messages, rollover help) keep to their edges of it. */
        g_map_h = (int)P_H(map);
        set_rect(map, 0, 0, W, g_map_h + dy);
        for (uint32_t n = P_KIDS(map); n; n = MEM32(n + 8))
            anchor_tree(MEM32(n), g_base_w, g_map_h, W - g_base_w, dy);
    }
    for (uint32_t n = P_KIDS(screen); n; n = MEM32(n + 8)) {
        uint32_t c = MEM32(n);
        if (c == map) continue;
        if ((int)P_Y(c) + (int)P_H(c) > g_map_h + 8)   /* the interface: bottom, centred */
            move_tree(c, dx, dy);
        else                                           /* over the map */
            anchor_tree(c, g_base_w, g_map_h, W - g_base_w, dy);
    }
    if (getenv("SWGB_RES_DUMP")) {
        FILE* f = fopen(getenv("SWGB_RES_DUMP"), "w");
        if (f) { dump_tree(f, screen, 0); fclose(f); }
    }
    g_esp = esp_after;
    g_eax = ret;
}

/* ---- the interface art ------------------------------------------------------
 * The game screen's draw (vtable 0x0065CE24 + 0x3C, sub_004FEDB0) locks its
 * draw area and draws, with direct calls to the shape blitter sub_005415D0
 * (shape; area, x, y, frame, flags), the interface background at 0,0 (the
 * shape at +0x100C: one picture the size of the screen, transparent where the
 * map shows) and two overlays at +0x108C and +0x10A0, at places it picks by an
 * exact match of its width with 1280.
 *
 * A shape (TShape) keeps its SLP at +0x0C and the SLP's size at +0x04
 * (32-byte header, then a 32-byte record per frame with the hotspot at
 * +0x18/+0x1C), and the blitter puts a frame at x - hotspot x, y - hotspot y.
 * The SLPs are in a read-only view of the .drs file, so a shape that has to
 * land dx, dy further gets, for the call, a copy of its SLP with the hotspots
 * moved by -dx, -dy. */
#define AREA_BITS(a) MEM32((a) + 0x08)

static uint32_t shifted_slp(uint32_t shape, int dx, int dy) {
    static struct { uint32_t slp; int dx, dy; uint32_t copy; } cache[16];
    static int n;
    uint32_t slp = MEM32(shape + 0x0C), size = MEM32(shape + 0x04);
    for (int i = 0; i < n; i++)
        if (cache[i].slp == slp && cache[i].dx == dx && cache[i].dy == dy) return cache[i].copy;
    uint32_t frames = MEM32(slp + 4);
    if (n == 16 || frames == 0 || size < 32 + frames * 32 || size > (64u << 20)) return slp;
    uint8_t* c = (uint8_t*)malloc(size);
    if (!c) return slp;
    uint32_t copy = (uint32_t)(uintptr_t)c;
    memcpy(c, (void*)(uintptr_t)slp, size);
    for (uint32_t f = 0; f < frames; f++) {
        MEM32(copy + 32 + f * 32 + 0x18) -= (uint32_t)dx;
        MEM32(copy + 32 + f * 32 + 0x1C) -= (uint32_t)dy;
    }
    cache[n].slp = slp; cache[n].dx = dx; cache[n].dy = dy; cache[n].copy = copy; n++;
    return copy;
}

static void fill(uint32_t area, int x1, int y1, int x2, int y2, int color) {
    if (x2 < x1 || y2 < y1) return;
    uint32_t a[5] = { (uint32_t)x1, (uint32_t)y1, (uint32_t)x2, (uint32_t)y2, (uint32_t)color };
    guest_thiscall(0x004736C0u, area, 5, a);
}

/* The screen's art shapes, moved for as long as the screen paints: its own
 * draw and its panels' (the unit-info and minimap panels put the piece of
 * the background under them back before they draw). */
static const uint32_t g_art_at[] = { 0x100C, 0x108C, 0x10A0 };
static uint32_t g_art_shape[3], g_art_slp[3];
static int g_art_depth;

static void art_shift(uint32_t screen) {
    if (g_art_depth++) return;
    int dx = (widescreen_w - g_base_w) / 2, dy = widescreen_h - g_base_h;
    for (int i = 0; i < 3; i++) {
        uint32_t sh = MEM32(screen + g_art_at[i]);
        g_art_shape[i] = sh;
        g_art_slp[i] = sh && MEM8(sh + 0x14) ? MEM32(sh + 0x0C) : 0;
        if (g_art_slp[i]) {
            uint32_t c = shifted_slp(sh, dx, dy);
            MEM32(sh + 0x0C) = c;
            MEM32(sh + 0x10) = c + 32;
        }
    }
}

static void art_restore(void) {
    if (--g_art_depth) return;
    for (int i = 0; i < 3; i++)
        if (g_art_slp[i]) {
            MEM32(g_art_shape[i] + 0x0C) = g_art_slp[i];
            MEM32(g_art_shape[i] + 0x10) = g_art_slp[i] + 32;
        }
}

static int laid_out(uint32_t screen) {
    return screen && screen == g_screen && (int)MEM32(screen + 0x2C) == widescreen_w - 1;
}

/* The screen reports WxH: dialogs centre on it (TDialogPanel::setup,
 * sub_004C6380, uses its parent's width and height). The game's own checks
 * of it for exactly 1280 want the base size: while they run, it has it. */
static int g_base_depth;
static void base_size_on(void) {
    if (g_base_depth++ == 0) { P_W(g_screen) = (uint32_t)g_base_w; P_H(g_screen) = (uint32_t)g_base_h; }
}
static void base_size_off(void) {
    if (--g_base_depth == 0) { P_W(g_screen) = (uint32_t)widescreen_w; P_H(g_screen) = (uint32_t)widescreen_h; }
}
#define AT_BASE_SIZE(name, fn) \
    void fn(void); \
    static void name(void) { \
        if (!laid_out(g_screen)) { fn(); return; } \
        base_size_on(); fn(); base_size_off(); \
    }
/* The resource counters (vtable 0x00664E58 + 0x3C): their spacing by their
 * parent's width. */
AT_BASE_SIZE(hook_resources_draw, sub_005D4930)
/* The game's vtable 0x00665A20 + 0x14: (re)loads the interface art
 * (sub_004FC220: 1600x1200, 1280x1024, 1024x768 or 800x600 by the screen's
 * size). */
AT_BASE_SIZE(hook_game_interface, sub_005E2C50)

/* vtable 0x0065CE24 + 0x60, sub_004F6A70: the screen paints itself and its
 * panels. */
static void hook_game_screen_paint(void) {
    uint32_t screen = g_ecx;
    if (!laid_out(screen)) { sub_004F6A70(); return; }
    art_shift(screen);
    sub_004F6A70();
    art_restore();
}

static void hook_game_screen_draw(void) {
    uint32_t screen = g_ecx;
    int W = widescreen_w, H = widescreen_h;
    if (!laid_out(screen)) { sub_004FEDB0(); return; }
    int dx = (W - g_base_w) / 2;
    art_shift(screen);
    base_size_on();   /* the overlays' 1280 places */
    uint32_t esp = g_esp;
    sub_004FEDB0();
    uint32_t esp_after = g_esp;
    base_size_off();
    art_restore();

    /* Beside the interface: black, under the map view. */
    uint32_t area = MEM32(screen + 0x20);
    int top = H - (g_base_h - g_map_h);
    if (area && dx > 0 && top < H) {
        static const char name[] = "widescreen";
        uint32_t lk[2] = { (uint32_t)(uintptr_t)name, 1 };
        g_esp = esp - 64;
        guest_thiscall(0x00471FD0u, area, 2, lk);
        if (g_eax && AREA_BITS(area)) {
            fill(area, 0, top, dx - 1, H - 1, 0);
            fill(area, dx + g_base_w, top, W - 1, H - 1, 0);
            guest_thiscall(0x00472080u, area, 1, lk);
        }
        g_esp = esp_after;
    }
}

/* vtable 0x00665088 + 0x3C, sub_005D5D00: the unit-info panel's draw. It
 * picks its 800/1024/1280 variant (+0xF8, read throughout the drawing) from
 * the width of the draw area (+0x18) on entry: on the laid-out screen, the
 * base width. */
static void hook_info_draw(void) {
    uint32_t area = MEM32(g_ecx + 0x20);
    if (!g_screen || !area || (int)MEM32(area + 0x18) != widescreen_w || MEM32(g_ecx + 0x40) != g_screen) { sub_005D5D00(); return; }
    MEM32(area + 0x18) = (uint32_t)g_base_w;
    sub_005D5D00();
    MEM32(area + 0x18) = (uint32_t)widescreen_w;
}

/* LoadStringA: the Options dialog's "1280 x 1024" (its button and its help
 * line) names the size it now gives. */
void widescreen_LoadStringA(void) {
    HINSTANCE inst = (HINSTANCE)(uintptr_t)ARG(0);
    UINT id = ARG(1);
    char* buf = (char*)(uintptr_t)ARG(2);
    int max = (int)ARG(3);
    int n = LoadStringA(inst, id, buf, max);
    char* at;
    if (n > 0 && max > 0 && widescreen_w && (at = strstr(buf, "1280 x 1024")) != NULL) {
        char size[32], rest[1024];
        _snprintf(size, sizeof size, "%d x %d", widescreen_w, widescreen_h);
        size[sizeof size - 1] = 0;
        lstrcpynA(rest, at + 11, sizeof rest);
        int room = max - (int)(at - buf);
        if (room > 0) _snprintf(at, room, "%s%s", size, rest);
        buf[max - 1] = 0;
        n = (int)strlen(buf);
    }
    g_eax = (uint32_t)n;
    g_esp += 4 + 4 * 4;
}

/* vtable 0x0065CE24 + 0x00: the game screen's destructor. */
static void hook_game_screen_delete(void) {
    if (g_ecx == g_screen) g_screen = 0;
    sub_004F46B0();
}

static recomp_func_t hooked(uint32_t va) {
    if (!widescreen_w) return NULL;
    switch (va) {
    case 0x005ED2D0u: return hook_set_screen_size;
    case 0x004F5210u: return hook_game_screen_size;
    case 0x004FEDB0u: return hook_game_screen_draw;
    case 0x005D5D00u: return hook_info_draw;
    case 0x004F6A70u: return hook_game_screen_paint;
    case 0x005D4930u: return hook_resources_draw;
    case 0x005E2C50u: return hook_game_interface;
    case 0x004F46B0u: return hook_game_screen_delete;
    }
    return NULL;
}

recomp_func_t widescreen_lookup(uint32_t va) {
    recomp_func_t f = hooked(va);
    if (g_trace_on && va < 0x0062E000u)   /* game code: the CRT keeps x87 values live across calls */
        return trace_icall(va, f ? f : recomp_lookup(va));
    return f;
}
