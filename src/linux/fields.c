/*
 * fields.c - compose the game's EDIT children into the DirectDraw primary.
 *
 * Under real DirectDraw, Windows paints EDIT controls over the fullscreen
 * surface. Here only the primary reaches the screen (--shot / SDL present),
 * so each frame is a copy of the primary with visible children WM_PRINTed
 * in (softdd's compose). The controls keep focus and keys. A film's window
 * (movie.c) is drawn here too.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "win32hle.h"
#include "ddraw_int.h"
#include "swgb_linux.h"

#define GW_HWNDNEXT 2u
#define GW_HWNDPREV 3u
#define GW_CHILD    5u
static uint8_t *g_comp;
static size_t   g_comp_n;
static int      g_cw, g_ch, g_cpitch, g_cbpp;

static uint32_t game_hwnd(void) {
    uint32_t f = hle_focus_window();
    while (f && hle_window_parent(f)) f = hle_window_parent(f);
    if (f) return f;
    return hle_first_hwnd();
}

static int child_shown(uint32_t c, int *l, int *t, int *r, int *b) {
    if (!hle_window_visible(c)) return 0;
    if (!hle_window_screen_rect(c, l, t, r, b)) return 0;
    return *r > *l + 2 && *b > *t + 2;
}

static int has_children(uint32_t root) {
    int l, t, r, b;
    if (!root) return 0;
    for (uint32_t c = hle_window_get(root, GW_CHILD); c; c = hle_window_get(c, GW_HWNDNEXT))
        if (child_shown(c, &l, &t, &r, &b)) return 1;
    return 0;
}

static const uint32_t *primary_palette(surf_t *s) {
    /* hle_dd_frame would re-enter this hook; clear it for the palette only. */
    const uint8_t *(*saved)(surf_t *) = hle_dd_frame_hook;
    const uint8_t *px;
    const uint32_t *pe = NULL;
    int w, h, pitch, bpp;
    unsigned seq;
    hle_dd_frame_hook = NULL;
    hle_dd_frame(&px, &w, &h, &pitch, &bpp, &pe, &seq);
    hle_dd_frame_hook = saved;
    (void)s; (void)px; (void)w; (void)h; (void)pitch; (void)bpp; (void)seq;
    return pe;
}

static const uint8_t *compose(surf_t *s) {
    uint32_t root = game_hwnd();
    if ((s->bpp != 8 && s->bpp != 16) || !has_children(root)) return NULL;
    size_t need = (size_t)s->pitch * (size_t)s->h;
    if (!g_comp || g_comp_n < need || g_cw != s->w || g_ch != s->h ||
        g_cbpp != s->bpp || g_cpitch != s->pitch) {
        free(g_comp);
        g_comp = (uint8_t *)malloc(need);
        g_comp_n = g_comp ? need : 0;
        g_cw = s->w, g_ch = s->h, g_cpitch = s->pitch, g_cbpp = s->bpp;
        if (!g_comp) return NULL;
    }
    memcpy(g_comp, s->px, need);
    const uint32_t *pe = s->bpp == 8 ? primary_palette(s) : NULL;
    uint32_t hdc = hle_gdi_surface_dc_pal(g_comp, s->w, s->h, s->pitch, s->bpp, pe);
    if (!hdc) return g_comp;

    /* Bottom of the z-order first (softdd): last sibling, then PREV. */
    uint32_t first = hle_window_get(root, GW_CHILD), last = first;
    if (last)
        for (uint32_t n = hle_window_get(last, GW_HWNDNEXT); n; n = hle_window_get(n, GW_HWNDNEXT))
            last = n;
    for (uint32_t c = last; c; c = hle_window_get(c, GW_HWNDPREV)) {
        int l, t, r, b;
        const char *cls;
        if (!child_shown(c, &l, &t, &r, &b)) continue;
        cls = hle_window_class(c);
        if (cls && !strcmp(cls, "swgb.movie")) {         /* a film (movie.c): its frame, scaled into the window */
            if (r > s->w) r = s->w;
            if (b > s->h) b = s->h;
            swgb_movie_draw(c, g_comp, s->pitch, s->bpp, l < 0 ? 0 : l, t < 0 ? 0 : t, r, b, pe);
            continue;
        }
        if (!cls || (strcasecmp(cls, "Edit") && strcasecmp(cls, "EDIT"))) continue;
        /* softdd SetWindowRgn(empty): the control keeps focus/keys but game-
         * drawn OK/Cancel under/near it receive the clicks. */
        hle_window_set_no_hit(c, 1);
        if (l < 0) l = 0;
        if (t < 0) t = 0;
        if (r > s->w) r = s->w;
        if (b > s->h) b = s->h;
        hle_gdi_viewport(hdc, l, t);
        hle_gdi_clip(hdc, 1, l, t, r, b);
        /* Bypass the guest EDIT subclass: it SelectObject's the WM_PRINT DC
         * and clears px before CallWindowProc reaches win32hle's paint. */
        hle_paint_edit(c, hdc);
        hle_gdi_clip(hdc, 0, 0, 0, 0, 0);
        hle_gdi_viewport(hdc, 0, 0);
    }
    hle_gdi_release_dc(hdc);
    return g_comp;
}

static const uint8_t *frame_hook(surf_t *primary) {
    return compose(primary);
}

void swgb_fields_init(void) {
    hle_dd_frame_hook = frame_hook;
}
