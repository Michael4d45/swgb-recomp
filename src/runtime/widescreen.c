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
#ifndef SWGB_WIN32HLE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest.h"   /* native32 on Windows; win32hle on Linux and macOS (src/linux) */
#include "guest_addrs.h"
#include "widescreen.h"

#define ARG(n) MEM32(g_esp + 4 + 4 * (n))
#ifdef SWGB_WIN32HLE
#include <strings.h>
#define STRICMP strcasecmp
#define GUEST_ALLOC(n) hle_heap_alloc((uint32_t)(n), 0)
#else
#define STRICMP _stricmp
#define GUEST_ALLOC(n) malloc(n)
#endif

int widescreen_w, widescreen_h;
int widescreen_sides = WIDESCREEN_SIDES_FILL;
static int g_base_w, g_base_h;
static int g_fit_w, g_fit_h;   /* the largest of the game's sizes within WxH: full screens */
static uint32_t g_screen;   /* the game screen laid out at WxH */
static int g_map_h = 774;   /* the map view's height in the base layout */

void G_SET_SCREEN_SIZE(void);
void G_SCREEN_SIZE(void);
void G_SCREEN_DRAW(void);
void G_INFO_DRAW(void);
void G_SCREEN_PAINT(void);
void G_SCREEN_DELETE(void);
void G_PANEL_SIZE(void);
void G_PANEL_IDEAL(void);

static FILE* g_trace;
static int g_trace_on;

int widescreen_parse(const char* s) {
    int w = 0, h = 0;
    if (!STRICMP(s, "desktop")) {
#ifdef SWGB_WIN32HLE
        /* the display the window opened on: main.c parses --res after hle_screen_open */
        if (!hle_screen_desktop(&w, &h)) { fprintf(stderr, "--res desktop: no display (headless)\n"); return 0; }
#else
        w = GetSystemMetrics(SM_CXSCREEN);
        h = GetSystemMetrics(SM_CYSCREEN);
#endif
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
    g_fit_w = w >= 1280 && h >= 1024 ? 1280 : w >= 1024 && h >= 768 ? 1024 : 800;
    g_fit_h = g_fit_w == 1280 ? 1024 : g_fit_w == 1024 ? 768 : 600;
    return 1;
}

int widescreen_sides_parse(const char* s) {
    if (!STRICMP(s, "fill")) widescreen_sides = WIDESCREEN_SIDES_FILL;
    else if (!STRICMP(s, "black")) widescreen_sides = WIDESCREEN_SIDES_BLACK;
    else { fprintf(stderr, "--hud-sides %s: black or fill\n", s); return 0; }
    return 1;
}

static void lay_out_skipped(uint32_t game);

/* vtable 0x00665A20 + 0x68: set_screen_size(w, h), thiscall. */
static void hook_set_screen_size(void) {
    uint32_t game = g_ecx;
    int wide = ARG(0) == 1280 && ARG(1) == 1024;
    if (wide) {
        printf("[res] screen size 1280x1024 -> %dx%d\n", widescreen_w, widescreen_h);
        ARG(0) = (uint32_t)widescreen_w;
        ARG(1) = (uint32_t)widescreen_h;
    }
    if (!g_trace && getenv("SWGB_RES_TRACE")) g_trace = fopen(getenv("SWGB_RES_TRACE"), "w");
    if (g_trace) { fprintf(g_trace, "---- set_screen_size %d %d\n", (int)ARG(0), (int)ARG(1)); fflush(g_trace); g_trace_on = 1; }
    G_SET_SCREEN_SIZE();
    if (wide) lay_out_skipped(game);
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
    fflush(g_trace);   /* a run ended by --watchdog keeps its last lines */
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
        G_SCREEN_SIZE();
        return;
    }
    ARG(0) = (uint32_t)g_base_w;
    ARG(1) = (uint32_t)g_base_h;
    uint32_t esp = g_esp;
    G_SCREEN_SIZE();
    uint32_t ret = g_eax, esp_after = g_esp;
    g_esp = esp;   /* below the caller's arguments: room for our own calls */
    g_esp -= 64;

    /* Its own early way out, no interface yet: TPanel::setup sizing a screen
     * made while the mode is already WxH (a game loaded or restarted in a
     * game). It stays at the base size, so the interface its constructor
     * makes next picks the base size's column and art; hook_set_screen_size
     * widens it. */
    if (!MEM32(screen + G_SCREEN_IFACE)) {
        g_esp = esp_after;
        g_eax = ret;
        return;
    }
    int dx = (W - g_base_w) / 2, dy = H - g_base_h;
    uint32_t a[4] = { 0, 0, (uint32_t)W, (uint32_t)H };
    guest_thiscall(G_PANEL_SET_RECT, screen, 4, a);   /* TPanel::set_rect, not the screen's own */
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
    uint8_t* c = (uint8_t*)GUEST_ALLOC(size);   /* the game reads it: guest memory */
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
    guest_thiscall(G_AREA_FILL, area, 5, a);
}

/* The screen's art shapes, moved for as long as the screen paints: its own
 * draw and its panels' (the unit-info and minimap panels put the piece of
 * the background under them back before they draw). */
static const uint32_t g_art_at[] = { G_SCREEN_ART0, G_SCREEN_ART1, G_SCREEN_ART2 };
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

/* set_screen_size (sub_005ED2D0) calls the game screen's handle_size only
 * when the draw area changes size. A game loaded or restarted in a game makes
 * its screen while the mode is already WxH, so the call that lays it out never
 * comes: it is made here, as the game would have made it. */
static void lay_out_skipped(uint32_t game) {
    uint32_t screen = MEM32(game + 0x17B4);
    if (!screen || MEM32(screen) != G_SCREEN_VTABLE || !MEM32(screen + G_SCREEN_IFACE) || laid_out(screen))
        return;
    printf("[res] game screen %08X made at %dx%d: laid out for it\n", screen, widescreen_w, widescreen_h);
    uint32_t eax = g_eax, a[2] = { (uint32_t)widescreen_w, (uint32_t)widescreen_h };
    guest_thiscall(G_SCREEN_SIZE_VA, screen, 2, a);
    g_eax = eax;
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
AT_BASE_SIZE(hook_resources_draw, G_RESOURCES_DRAW)
/* The game's vtable 0x00665A20 + 0x14: (re)loads the interface art
 * (sub_004FC220: 1600x1200, 1280x1024, 1024x768 or 800x600 by the screen's
 * size). */
AT_BASE_SIZE(hook_game_interface, G_GAME_INTERFACE)

/* ---- the sides beside the interface (--hud-sides fill) -----------------------
 * The interface art is base_w wide (one SLP per civilization and size,
 * INTERFAC.DRS 51101-51146 and interfac_x1.drs 51107-51148), and no two
 * civilizations' art is alike, so the sides are made from the art itself:
 * each row continues with the art's outermost band columns (12, 10 or 8 at
 * 1280, 1024 or 800), mirrored back and forth: a fold at the art's edge,
 * another every band columns.
 * The rims along the top and the bottom run on across the screen, the frame
 * becomes a ribbed plate in the civilization's own colours, and no button or
 * screen of the art is copied. Every seam is a mirror fold, so nothing jumps.
 *
 * It copies palette indices, after the screen has drawn its art and before
 * its panels draw over it: the sides are the screen's alone (no panel
 * reaches them), and the map's clickable area and the minimap are not moved.
 * A row of the art whose edge is transparent (Clone Campaigns' 1024 art has
 * 131 clear rows on top; they are above the interface band) stays black:
 * the SLP's outline table (left and right margins per row) says which.
 *
 * The draw area (TDrawArea): +0x08 the bits, +0x18 width, +0x1C height,
 * +0x20 the pitch, +0x24 1 when the rows run top-down (else bottom-up), as
 * the area's fill (sub_004736C0; Clone Campaigns' sub_00474160) reads them. */
#define AREA_W(a)     MEM32((a) + 0x18)
#define AREA_H(a)     MEM32((a) + 0x1C)
#define AREA_PITCH(a) MEM32((a) + 0x20)
#define AREA_TOPDOWN(a) (MEM32((a) + 0x24) == 1)

static uint8_t* area_row(uint32_t area, int y) {
    int h = (int)AREA_H(area);
    int row = AREA_TOPDOWN(area) ? y : h - 1 - y;
    return (uint8_t*)GUEST_PTR(AREA_BITS(area) + (uint32_t)row * AREA_PITCH(area));
}

/* The column of the art (0 = its edge, counting inwards) shown at distance d
 * (0 = the column next to the art) out from it: a fold every band columns. */
int widescreen_side_column(int d, int band) {
    int k = d % (2 * band);
    return k < band ? k : 2 * band - 1 - k;
}

static void sides_fill(uint32_t area, uint32_t shape, int top) {
    int W = widescreen_w, H = widescreen_h;
    if (!shape || !MEM8(shape + 0x14)) return;
    /* The SLP the game loaded: inside the screen's paint (art_shift still on)
     * the shape holds the copy with moved hotspots. */
    uint32_t slp = g_art_depth && g_art_shape[0] == shape && g_art_slp[0] ? g_art_slp[0] : MEM32(shape + 0x0C);
    if (!slp || MEM32(slp + 4) == 0 || (int)AREA_W(area) < W || (int)AREA_H(area) < H) return;
    uint32_t fr = slp + 32;   /* frame 0 */
    uint32_t outline = slp + MEM32(fr + 0x04);
    int fw = (int)MEM32(fr + 0x10), fh = (int)MEM32(fr + 0x14);
    int hx = (int)MEM32(fr + 0x18), hy = (int)MEM32(fr + 0x1C);
    int dx = (W - g_base_w) / 2, dy = H - g_base_h;
    int x0 = dx - hx, x1 = x0 + fw - 1;   /* the art's first and last columns on the screen */
    int band = g_base_w / 100;            /* 12, 10, 8 at 1280, 1024, 800 */
    if (fw < 2 * band || x0 < 0 || x1 >= W) return;
    for (int y = top < 0 ? 0 : top; y < H; y++) {
        int r = y - dy + hy;              /* the art's row */
        if (r < 0 || r >= fh) continue;
        int16_t l = (int16_t)MEM16(outline + (uint32_t)r * 4), rt = (int16_t)MEM16(outline + (uint32_t)r * 4 + 2);
        if (l == (int16_t)0x8000 || rt == (int16_t)0x8000) continue;   /* an empty row */
        uint8_t* p = area_row(area, y);
        if (l == 0)
            for (int d = 0; d < x0; d++) p[x0 - 1 - d] = p[x0 + widescreen_side_column(d, band)];
        if (rt == 0)
            for (int d = 0; d < W - 1 - x1; d++) p[x1 + 1 + d] = p[x1 - widescreen_side_column(d, band)];
    }
}

/* vtable 0x0065CE24 + 0x60, sub_004F6A70: the screen paints itself and its
 * panels. */
static void hook_game_screen_paint(void) {
    uint32_t screen = g_ecx;
    if (!laid_out(screen)) { G_SCREEN_PAINT(); return; }
    art_shift(screen);
    G_SCREEN_PAINT();
    art_restore();
}

/* Locks the draw area for the host's own drawing, with room on the guest
 * stack below esp (the stack as it was when the hooked call came in). */
static const char g_lock_name[] = "widescreen";
static int area_lock(uint32_t area, uint32_t esp) {
    uint32_t lk[2] = { (uint32_t)(uintptr_t)g_lock_name, 1 };
    g_esp = esp - 64;
    guest_thiscall(G_AREA_LOCK, area, 2, lk);
    return g_eax && AREA_BITS(area);
}
static void area_unlock(uint32_t area) {
    uint32_t lk[1] = { (uint32_t)(uintptr_t)g_lock_name };
    guest_thiscall(G_AREA_UNLOCK, area, 1, lk);
}

/* A screen not laid out for WxH on a WxH draw area has the game's own layout,
 * for one of its sizes: its panels and art leave the rest of the area as it
 * was, so it is cleared. The covered part is the game's size for the width
 * its panels reach (the art covers all of it, below the last panel too). */
static void clear_uncovered(uint32_t screen, uint32_t esp) {
    static const int ws[3] = { 800, 1024, 1280 }, hs[3] = { 600, 768, 1024 };
    static uint32_t told;
    uint32_t area = MEM32(screen + 0x20);
    int W = widescreen_w, H = widescreen_h, cw = 0, ch = 0, i = 0;
    if (!area || (int)AREA_W(area) != W || (int)AREA_H(area) != H) return;
    for (uint32_t n = P_KIDS(screen); n; n = MEM32(n + 8)) {
        uint32_t c = MEM32(n);
        if ((int)(P_X(c) + P_W(c)) > cw) cw = (int)(P_X(c) + P_W(c));
        if ((int)(P_Y(c) + P_H(c)) > ch) ch = (int)(P_Y(c) + P_H(c));
    }
    while (i < 3 && cw > ws[i]) i++;
    if (i < 3) { cw = ws[i]; ch = hs[i]; }   /* else wider than any of its layouts: the panels' extent */
    if (cw >= W && ch >= H) return;
    if (told != screen) {
        told = screen;
        printf("[res] game screen %08X covers %dx%d of %dx%d: the rest cleared\n", screen, cw, ch, W, H);
    }
    uint32_t esp_after = g_esp;
    if (area_lock(area, esp)) {
        fill(area, cw, 0, W - 1, H - 1, 0);
        fill(area, 0, ch, cw - 1, H - 1, 0);
        area_unlock(area);
    }
    g_esp = esp_after;
}

static void hook_game_screen_draw(void) {
    uint32_t screen = g_ecx;
    int W = widescreen_w, H = widescreen_h;
    uint32_t esp = g_esp;
    if (!laid_out(screen)) {
        G_SCREEN_DRAW();
        clear_uncovered(screen, esp);
        return;
    }
    int dx = (W - g_base_w) / 2;
    art_shift(screen);
    base_size_on();   /* the overlays' 1280 places */
    G_SCREEN_DRAW();
    uint32_t esp_after = g_esp;
    base_size_off();
    art_restore();

    /* Beside the interface: the art's own edges (--hud-sides fill) or black. */
    uint32_t area = MEM32(screen + 0x20);
    int top = H - (g_base_h - g_map_h);
    if (area && dx > 0 && top < H) {
        if (area_lock(area, esp)) {
            fill(area, 0, top, dx - 1, H - 1, 0);
            fill(area, dx + g_base_w, top, W - 1, H - 1, 0);
            if (widescreen_sides == WIDESCREEN_SIDES_FILL)
                sides_fill(area, MEM32(screen + G_SCREEN_ART0), top);
            area_unlock(area);
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
    if (!g_screen || !area || (int)MEM32(area + 0x18) != widescreen_w || MEM32(g_ecx + 0x40) != g_screen) { G_INFO_DRAW(); return; }
    MEM32(area + 0x18) = (uint32_t)g_base_w;
    G_INFO_DRAW();
    MEM32(area + 0x18) = (uint32_t)widescreen_w;
}

/* A string LoadStringA gave (n characters in a buffer of max): the Options
 * dialog's "1280 x 1024" (its button and its help line) names the size it now
 * gives. Returns the new length. */
int widescreen_string(char* buf, int max, int n) {
    char* at;
    if (n > 0 && max > 0 && widescreen_w && (at = strstr(buf, "1280 x 1024")) != NULL) {
        char size[32], rest[1024];
        snprintf(size, sizeof size, "%d x %d", widescreen_w, widescreen_h);
        snprintf(rest, sizeof rest, "%s", at + 11);
        int room = max - (int)(at - buf);
        if (room > 0) snprintf(at, room, "%s%s", size, rest);
        buf[max - 1] = 0;
        n = (int)strlen(buf);
    }
    return n;
}

#ifndef SWGB_WIN32HLE
void widescreen_LoadStringA(void) {
    HINSTANCE inst = (HINSTANCE)(uintptr_t)ARG(0);
    UINT id = ARG(1);
    char* buf = (char*)(uintptr_t)ARG(2);
    int max = (int)ARG(3);
    int n = widescreen_string(buf, max, LoadStringA(inst, id, buf, max));
    g_eax = (uint32_t)n;
    g_esp += 4 + 4 * 4;
}
#endif

/* ---- full screens shown in a game ------------------------------------------
 * A screen of its own opened in a game (Save Game, Load Game, the tech tree)
 * is a panel without a parent that TPanel::setup (sub_004B54F0) sizes to
 * the draw area: x, y, w, h at +0xA0, +0xA4, +0xB0 (and its maximum +0xB4),
 * +0xB8 (+0xBC), placed by handle_size (+0x34 == 0: x, y, w, h as given).
 * Its constructor then places its children in proportion to its width and
 * height (from a design at 800x600: sub_004BA220 and the other child makers
 * scale by +0x14/+0x18 over +0xF8/+0xFC), and its background is the art the
 * game has for 800x600, 1024x768 or 1280x1024. At WxH it is made the largest
 * of those sizes that fits, centred, with the game's black around it: it
 * looks as it does at that size. */
static int full_screen_panel(uint32_t p) {
    /* The tech tree is left full size: it lays out at any size, and draws
     * past a smaller panel (a fault at 1920x1080, garbage at 2560x1440). */
    return MEM32(p + 0x40) == 0 && MEM32(p) != G_SCREEN_VTABLE && MEM32(p) != G_TECH_TREE_VTABLE;
}

/* TPanel vtable + 0x5C: handle_size(w, h), the draw area's size. setup calls it
 * twice: with the panel at x, y, w, h (+0x34 == 0), then filling the area less
 * the insets +0xA0..+0xAC (left, top, right, bottom; +0x34 == 1). */
static void hook_panel_size(void) {
    uint32_t p = g_ecx;
    int W = widescreen_w, H = widescreen_h, dx = (W - g_fit_w) / 2, dy = (H - g_fit_h) / 2;
    if ((int)ARG(0) == W && (int)ARG(1) == H && full_screen_panel(p) && (g_fit_w != W || g_fit_h != H)) {
        int mode = (int)MEM32(p + 0x34);
        if (mode == 0 && MEM32(p + 0xA0) == 0 && MEM32(p + 0xA4) == 0 &&
            (int)MEM32(p + 0xB0) == W && (int)MEM32(p + 0xB8) == H) {
            MEM32(p + 0xA0) = (uint32_t)dx;
            MEM32(p + 0xA4) = (uint32_t)dy;
            MEM32(p + 0xB0) = MEM32(p + 0xB4) = (uint32_t)g_fit_w;
            MEM32(p + 0xB8) = MEM32(p + 0xBC) = (uint32_t)g_fit_h;
            printf("[res] full screen %08X (vtable %08X): %dx%d at %d,%d\n", p, MEM32(p), g_fit_w, g_fit_h, dx, dy);
        } else if (mode == 1 && !MEM32(p + 0xA0) && !MEM32(p + 0xA4) && !MEM32(p + 0xA8) && !MEM32(p + 0xAC)) {
            MEM32(p + 0xA0) = (uint32_t)dx;
            MEM32(p + 0xA4) = (uint32_t)dy;
            MEM32(p + 0xA8) = (uint32_t)(W - g_fit_w - dx);
            MEM32(p + 0xAC) = (uint32_t)(H - g_fit_h - dy);
        }
    }
    G_PANEL_SIZE();
}

/* TEasy_Panel vtable + 0x110, sub_004B8590: the size the panel is made for
 * (setup passes the draw area's), which picks its art. */
static void hook_panel_ideal(void) {
    uint32_t p = g_ecx;
    if ((int)ARG(0) == widescreen_w && (int)ARG(1) == widescreen_h && full_screen_panel(p) &&
        (int)P_W(p) == g_fit_w && (int)P_H(p) == g_fit_h && (g_fit_w != widescreen_w || g_fit_h != widescreen_h)) {
        ARG(0) = (uint32_t)g_fit_w;
        ARG(1) = (uint32_t)g_fit_h;
    }
    G_PANEL_IDEAL();
}

/* vtable 0x0065CE24 + 0x00: the game screen's destructor. */
static void hook_game_screen_delete(void) {
    if (g_ecx == g_screen) g_screen = 0;
    G_SCREEN_DELETE();
}

static recomp_func_t hooked(uint32_t va) {
    if (!widescreen_w) return NULL;
    switch (va) {
    case G_SET_SCREEN_SIZE_VA: return hook_set_screen_size;
    case G_SCREEN_SIZE_VA: return hook_game_screen_size;
    case G_SCREEN_DRAW_VA: return hook_game_screen_draw;
    case G_INFO_DRAW_VA: return hook_info_draw;
    case G_SCREEN_PAINT_VA: return hook_game_screen_paint;
    case G_RESOURCES_DRAW_VA: return hook_resources_draw;
    case G_GAME_INTERFACE_VA: return hook_game_interface;
    case G_SCREEN_DELETE_VA: return hook_game_screen_delete;
    case G_PANEL_SIZE_VA: return hook_panel_size;
    case G_PANEL_IDEAL_VA: return hook_panel_ideal;
    }
    return NULL;
}

recomp_func_t widescreen_lookup(uint32_t va) {
    recomp_func_t f = hooked(va);
    if (g_trace_on && va < G_CODE_END)   /* game code: the CRT keeps x87 values live across calls */
        return trace_icall(va, f ? f : recomp_lookup(va));
    return f;
}
