/*
 * scaler: the game's frame -> the 0x00RRGGBB picture present.c draws, and
 * which rows of it changed since the last one.
 *
 * What it costs matters: it runs on the game thread for every shown frame,
 * up to the display's refresh (180 a second here), and the old one-pass
 * bilinear took 3 ms a frame at 1080p and 11 ms at 4K. So:
 *
 *  - Only what changed. The frame is compared with the last one, row by row
 *    (with the palette, the sizes and the filter); unchanged rows are not
 *    converted, scaled or drawn again. In game, 9 to 30 frames a second differ
 *    from the one before, and often only where the cursor moved.
 *  - Separable. Each source row is scaled across once, into a row cache;
 *    each picture row is then a copy of one cached row (the "sharp" filter
 *    keeps most rows whole) or a blend of two (SSE2, four pixels at once).
 *  - 1:1 (the game at the window's size: --res desktop fullscreen) is the
 *    converted frame itself.
 *
 * The result is bit for bit the old scaler's (tools/present_bench.c checks
 * it): the same axis tables, the same 8-bit weights and rounding.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <emmintrin.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "present.h"
#include "scaler.h"

static void* grow(void* p, size_t* cap, size_t n) {
    if (*cap >= n) return p;
    free(p);
    p = malloc(n);
    *cap = p ? n : 0;
    return p;
}

/* One axis: for each destination pixel, the two source pixels and the weight
 * (0..256) of the second. The same arithmetic as the old present.c's. */
static void axis_build(scaler_axis_t* a, int dn, int sn, int mode) {
    if (a->cap < dn) {
        free(a->i0); free(a->i1); free(a->w);
        a->i0 = (int*)malloc(dn * sizeof(int)); a->i1 = (int*)malloc(dn * sizeof(int)); a->w = (int*)malloc(dn * sizeof(int));
        a->cap = a->i0 && a->i1 && a->w ? dn : 0;
        if (!a->cap) return;
    }
    a->n = dn;
    float sharp = (float)dn / sn;   /* texel edges stay one destination pixel wide */
    for (int d = 0; d < dn; d++) {
        if (mode == PRES_NEAREST || mode == PRES_INTEGER) {
            int s = (int)(((int64_t)(2 * d + 1) * sn) / (2 * (int64_t)dn));
            a->i0[d] = a->i1[d] = s < sn - 1 ? s : sn - 1;
            a->w[d] = 0;
            continue;
        }
        float sx = ((float)d + 0.5f) * sn / dn - 0.5f;
        int x0 = (int)floorf(sx);
        float fr = sx - (float)x0;
        if (x0 < 0) { x0 = 0; fr = 0; }
        int x1 = x0 + 1;
        if (x1 >= sn) { x0 = x1 = sn - 1; fr = 0; }
        if (mode == PRES_SHARP && sharp > 1.0f) {
            fr = (fr - 0.5f) * sharp + 0.5f;
            fr = fr < 0 ? 0 : fr > 1 ? 1 : fr;
        }
        a->i0[d] = x0; a->i1[d] = x1; a->w[d] = (int)(fr * 256.0f + 0.5f);
    }
}

static inline uint32_t lerp(uint32_t a, uint32_t b, uint32_t w) {   /* w 0..256 */
    uint32_t rb = (((a & 0xFF00FFu) * (256 - w) + (b & 0xFF00FFu) * w) >> 8) & 0xFF00FFu;
    uint32_t g = (((a & 0x00FF00u) * (256 - w) + (b & 0x00FF00u) * w) >> 8) & 0x00FF00u;
    return rb | g;
}

/* o = lerp(a, b, w) for n pixels, 0 < w < 256. Per channel the same as lerp():
 * (a*(256-w) + b*w) >> 8, which fits 16 bits (255*256). */
static void blend_row(uint32_t* o, const uint32_t* a, const uint32_t* b, int n, uint32_t w) {
    int x = 0;
    __m128i wa = _mm_set1_epi16((short)(256 - w)), wb = _mm_set1_epi16((short)w), z = _mm_setzero_si128();
    __m128i keep = _mm_set1_epi32(0x00FFFFFF);
    for (; x + 4 <= n; x += 4) {
        __m128i va = _mm_loadu_si128((const __m128i*)(a + x)), vb = _mm_loadu_si128((const __m128i*)(b + x));
        __m128i lo = _mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(va, z), wa), _mm_mullo_epi16(_mm_unpacklo_epi8(vb, z), wb));
        __m128i hi = _mm_add_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(va, z), wa), _mm_mullo_epi16(_mm_unpackhi_epi8(vb, z), wb));
        __m128i r = _mm_packus_epi16(_mm_srli_epi16(lo, 8), _mm_srli_epi16(hi, 8));
        _mm_storeu_si128((__m128i*)(o + x), _mm_and_si128(r, keep));
    }
    for (; x < n; x++) o[x] = lerp(a[x], b[x], w);
}

/* One source row across: d (dw pixels) from s. */
static void scale_row(uint32_t* d, const uint32_t* s, const scaler_axis_t* ax) {
    const int* i0 = ax->i0, *i1 = ax->i1, *w = ax->w;
    for (int x = 0, n = ax->n; x < n; x++) {
        uint32_t wx = (uint32_t)w[x];
        d[x] = !wx ? s[i0[x]] : wx == 256 ? s[i1[x]] : lerp(s[i0[x]], s[i1[x]], wx);
    }
}

/* Source rows [y0, y1) of the frame -> sc->src (0x00RRGGBB). */
static void convert(scaler_t* sc, const uint8_t* bits, int w, int pitch, int bpp, int y0, int y1) {
    for (int y = y0; y < y1; y++) {
        const uint8_t* s = bits + (size_t)y * pitch;
        uint32_t* o = sc->src + (size_t)y * w;
        if (bpp == 8) for (int x = 0; x < w; x++) o[x] = sc->lut[s[x]];
        else if (bpp == 32) memcpy(o, s, (size_t)w * 4);
        else for (int x = 0; x < w; x++) {
            uint32_t v = ((const uint16_t*)s)[x], r = v >> 11, g = (v >> 5) & 63, b = v & 31;
            o[x] = ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
        }
    }
}

int scaler_run(scaler_t* sc, const uint8_t* bits, int w, int h, int pitch, int bpp, const PALETTEENTRY* pal,
               int dw, int dh, int mode, int force, int* out_y0, int* out_y1) {
    *out_y0 = *out_y1 = 0;
    if ((bpp != 8 && bpp != 16 && bpp != 32) || w <= 0 || h <= 0 || dw <= 0 || dh <= 0) return 0;
    size_t row = (size_t)w * (bpp / 8);
    int fresh = force || w != sc->w || h != sc->h || bpp != sc->bpp || dw != sc->dw || dh != sc->dh || mode != sc->mode;

    /* the palette: a new one is a new picture */
    uint32_t lut[256];
    if (bpp == 8) {
        for (int i = 0; i < 256; i++)
            lut[i] = pal ? ((uint32_t)pal[i].peRed << 16) | ((uint32_t)pal[i].peGreen << 8) | pal[i].peBlue
                         : (uint32_t)i * 0x010101u;
        if (memcmp(lut, sc->lut, sizeof lut)) { memcpy(sc->lut, lut, sizeof lut); fresh = 1; }
    }
    if (fresh) {
        sc->src = (uint32_t*)grow(sc->src, &sc->src_cap, (size_t)w * h * 4);
        sc->prev = (uint8_t*)grow(sc->prev, &sc->prev_cap, row * h);
        if (!sc->src || !sc->prev) { sc->w = 0; return 0; }
        sc->w = w; sc->h = h; sc->bpp = bpp; sc->dw = dw; sc->dh = dh; sc->mode = mode;
        sc->one = dw == w && dh == h;
        if (!sc->one) {
            axis_build(&sc->ax, dw, w, mode);
            axis_build(&sc->ay, dh, h, mode);
            sc->rows = (uint32_t*)grow(sc->rows, &sc->rows_cap, (size_t)dw * h * 4);
            sc->img = (uint32_t*)grow(sc->img, &sc->img_cap, (size_t)dw * dh * 4);
            if (!sc->ax.cap || !sc->ay.cap || !sc->rows || !sc->img) { sc->w = 0; return 0; }
        }
    }

    /* the changed source rows: [y0, y1) */
    int y0 = h, y1 = 0;
    for (int y = 0; y < h; y++) {
        const uint8_t* s = bits + (size_t)y * pitch;
        uint8_t* p = sc->prev + (size_t)y * row;
        if (!fresh && !memcmp(s, p, row)) continue;
        memcpy(p, s, row);
        if (y < y0) y0 = y;
        y1 = y + 1;
    }
    if (y0 >= y1) return 1;   /* nothing new */
    convert(sc, bits, w, pitch, bpp, y0, y1);
    if (sc->one) { *out_y0 = y0; *out_y1 = y1; return 1; }

    /* across, the changed source rows */
    for (int y = y0; y < y1; y++) scale_row(sc->rows + (size_t)y * dw, sc->src + (size_t)y * w, &sc->ax);
    /* down: the picture rows that read any of them (i0 and i1 rise with the row) */
    int d0 = 0, d1 = dh;
    while (d0 < dh && sc->ay.i1[d0] < y0) d0++;
    while (d1 > d0 && sc->ay.i0[d1 - 1] >= y1) d1--;
    for (int y = d0; y < d1; y++) {
        uint32_t wy = (uint32_t)sc->ay.w[y];
        const uint32_t* r0 = sc->rows + (size_t)sc->ay.i0[y] * dw;
        const uint32_t* r1 = sc->rows + (size_t)sc->ay.i1[y] * dw;
        uint32_t* o = sc->img + (size_t)y * dw;
        if (!wy) memcpy(o, r0, (size_t)dw * 4);
        else if (wy == 256) memcpy(o, r1, (size_t)dw * 4);
        else blend_row(o, r0, r1, dw, wy);
    }
    *out_y0 = d0;
    *out_y1 = d1;
    return 1;
}

const uint32_t* scaler_picture(const scaler_t* sc) { return sc->one ? sc->src : sc->img; }
