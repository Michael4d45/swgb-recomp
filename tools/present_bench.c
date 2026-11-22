/* The presenter's scaler: the same picture as the old one-pass scaler, and
 * what a frame costs, without the game:
 *   cl /nologo /O2 tools\present_bench.c src\runtime\scaler.c /Isrc\runtime user32.lib gdi32.lib
 *   present_bench.exe            (exit 0 and "OK" when every picture matches)
 *
 * For each frame size, picture size and filter: random 8-bit frames with a
 * palette, scaled by the old code (kept here as the reference) and by
 * scaler.c, compared pixel for pixel, also after changing only a band of
 * rows (the cursor) and a whole frame (a scroll). Then the time a frame takes:
 * the old scaler (to32 + scale), the new one with every row new, with only
 * a 40-row band new (a cursor moving), and with nothing new; and GDI's copy
 * of the picture into a DIB section of the window's size (an estimate of
 * SetDIBitsToDevice; the real window's cost is the display driver's). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "present.h"
#include "scaler.h"

/* ---- the old scaler (present.c before scaler.c), the reference ---- */
static uint32_t* o_src; static size_t o_src_n;
static uint32_t* o_img; static size_t o_img_n;
static uint32_t* o_grow(uint32_t** p, size_t* cap, size_t n) {
    if (*cap < n) { free(*p); *p = (uint32_t*)malloc(n * 4); *cap = *p ? n : 0; }
    return *p;
}
static int o_to32(const uint8_t* bits, int w, int h, int pitch, const PALETTEENTRY* pal) {
    uint32_t* d = o_grow(&o_src, &o_src_n, (size_t)w * h), lut[256];
    for (int i = 0; i < 256; i++) lut[i] = ((uint32_t)pal[i].peRed << 16) | ((uint32_t)pal[i].peGreen << 8) | pal[i].peBlue;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) d[(size_t)y * w + x] = lut[bits[(size_t)y * pitch + x]];
    return 1;
}
typedef struct { int n; int* i0; int* i1; int* w; int cap; } o_axis_t;
static void o_axis(o_axis_t* a, int dn, int sn, int mode) {
    if (a->cap < dn) { free(a->i0); free(a->i1); free(a->w); a->i0 = malloc(dn * 4); a->i1 = malloc(dn * 4); a->w = malloc(dn * 4); a->cap = dn; }
    a->n = dn;
    float sharp = (float)dn / sn;
    for (int d = 0; d < dn; d++) {
        if (mode == PRES_NEAREST || mode == PRES_INTEGER) {
            int s = (int)(((int64_t)(2 * d + 1) * sn) / (2 * (int64_t)dn));
            a->i0[d] = a->i1[d] = s < sn - 1 ? s : sn - 1; a->w[d] = 0; continue;
        }
        float sx = ((float)d + 0.5f) * sn / dn - 0.5f;
        int x0 = (int)floorf(sx); float fr = sx - (float)x0;
        if (x0 < 0) { x0 = 0; fr = 0; }
        int x1 = x0 + 1;
        if (x1 >= sn) { x0 = x1 = sn - 1; fr = 0; }
        if (mode == PRES_SHARP && sharp > 1.0f) { fr = (fr - 0.5f) * sharp + 0.5f; fr = fr < 0 ? 0 : fr > 1 ? 1 : fr; }
        a->i0[d] = x0; a->i1[d] = x1; a->w[d] = (int)(fr * 256.0f + 0.5f);
    }
}
static inline uint32_t o_lerp(uint32_t a, uint32_t b, uint32_t w) {
    uint32_t rb = (((a & 0xFF00FFu) * (256 - w) + (b & 0xFF00FFu) * w) >> 8) & 0xFF00FFu;
    uint32_t g = (((a & 0x00FF00u) * (256 - w) + (b & 0x00FF00u) * w) >> 8) & 0x00FF00u;
    return rb | g;
}
static void o_scale(int sw, int sh, int dw, int dh, int mode) {
    static o_axis_t ax, ay;
    o_grow(&o_img, &o_img_n, (size_t)dw * dh);
    o_axis(&ax, dw, sw, mode); o_axis(&ay, dh, sh, mode);
    for (int y = 0; y < dh; y++) {
        const uint32_t* r0 = o_src + (size_t)ay.i0[y] * sw, *r1 = o_src + (size_t)ay.i1[y] * sw;
        uint32_t wy = (uint32_t)ay.w[y], *o = o_img + (size_t)y * dw;
        for (int x = 0; x < dw; x++) {
            int x0 = ax.i0[x], x1 = ax.i1[x]; uint32_t wx = (uint32_t)ax.w[x];
            uint32_t top = wx ? o_lerp(r0[x0], r0[x1], wx) : r0[x0];
            if (!wy) { o[x] = top; continue; }
            o[x] = o_lerp(top, wx ? o_lerp(r1[x0], r1[x1], wx) : r1[x0], wy);
        }
    }
}

static double now_ms(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / f.QuadPart;
}

static uint32_t g_rng = 12345;
static uint32_t rnd(void) { g_rng = g_rng * 1103515245u + 12345u; return g_rng >> 8; }

static int g_fail;

int main(int argc, char** argv) {
    static const int frames[][2] = { { 800, 600 }, { 1024, 768 }, { 2560, 1440 } };
    static const int clients[][2] = { { 800, 600 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 }, { 1500, 700 } };
    static const char* fname[] = { "sharp", "smooth", "nearest", "integer" };
    int quick = argc > 1 && !strcmp(argv[1], "--quick");
    PALETTEENTRY pal[256];
    for (int i = 0; i < 256; i++) pal[i].peRed = (BYTE)rnd(), pal[i].peGreen = (BYTE)rnd(), pal[i].peBlue = (BYTE)rnd(), pal[i].peFlags = 0;
    HDC mem = CreateCompatibleDC(NULL);
    printf("%-10s %-10s %-8s %9s %9s %9s %9s %9s\n", "frame", "window", "filter", "old ms", "new all", "new band", "new same", "gdi copy");
    for (int f = 0; f < 3; f++)
        for (int c = 0; c < 5; c++) {
            int w = frames[f][0], h = frames[f][1];
            if (f == 2 && c != 2 && c != 3) continue;   /* --res: the game at the window's size, or 4K */
            RECT r;
            present_layout(PRES_SHARP, clients[c][0], clients[c][1], w, h, &r);
            int dw = r.right - r.left, dh = r.bottom - r.top;
            uint8_t* bits = (uint8_t*)malloc((size_t)w * h);
            for (size_t i = 0; i < (size_t)w * h; i++) bits[i] = (uint8_t)rnd();
            /* a DIB section the picture's size: GDI's side of a frame */
            BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), dw, -dh, 1, 32 } };
            void* dib_bits;
            HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &dib_bits, NULL, 0);
            HGDIOBJ old = SelectObject(mem, dib);
            for (int m = 0; m < 2; m++) {   /* sharp, smooth: what this scaler draws (nearest/integer: GDI's StretchDIBits) */
                scaler_t sc;
                memset(&sc, 0, sizeof sc);
                int y0, y1, n = quick ? 3 : 10;
                /* correctness: full, a band, everything */
                for (int pass = 0; pass < 3; pass++) {
                    if (pass == 1) { int b = h / 3; for (int y = b; y < b + 40; y++) for (int x = 100; x < 140; x++) bits[(size_t)y * w + x] ^= 0x5A; }
                    if (pass == 2) for (size_t i = 0; i < (size_t)w * h; i += 7) bits[i] = (uint8_t)rnd();
                    o_to32(bits, w, h, w, pal);
                    o_scale(w, h, dw, dh, m);
                    scaler_run(&sc, bits, w, h, w, 8, pal, dw, dh, m, 0, &y0, &y1);
                    if (memcmp(o_img, scaler_picture(&sc), (size_t)dw * dh * 4)) {
                        g_fail++;
                        printf("FAIL %dx%d -> %dx%d %s pass %d: the picture differs\n", w, h, dw, dh, fname[m], pass);
                    }
                }
                /* time */
                double t0 = now_ms();
                for (int i = 0; i < n; i++) { o_to32(bits, w, h, w, pal); o_scale(w, h, dw, dh, m); }
                double t_old = (now_ms() - t0) / n;
                t0 = now_ms();
                for (int i = 0; i < n; i++) scaler_run(&sc, bits, w, h, w, 8, pal, dw, dh, m, 1, &y0, &y1);
                double t_all = (now_ms() - t0) / n;
                t0 = now_ms();
                for (int i = 0; i < n; i++) {
                    int b = (h / 2 + i * 13) % (h - 40);
                    for (int y = b; y < b + 40; y++) bits[(size_t)y * w + 300] ^= 1;
                    scaler_run(&sc, bits, w, h, w, 8, pal, dw, dh, m, 0, &y0, &y1);
                }
                double t_band = (now_ms() - t0) / n;
                t0 = now_ms();
                for (int i = 0; i < n; i++) scaler_run(&sc, bits, w, h, w, 8, pal, dw, dh, m, 0, &y0, &y1);
                double t_same = (now_ms() - t0) / n;
                t0 = now_ms();
                for (int i = 0; i < n; i++)
                    SetDIBitsToDevice(mem, 0, 0, dw, dh, 0, 0, 0, dh, scaler_picture(&sc), &bi, DIB_RGB_COLORS);
                GdiFlush();
                double t_gdi = (now_ms() - t0) / n;
                char fr[16], wn[16];
                sprintf(fr, "%dx%d", w, h);
                sprintf(wn, "%dx%d", dw, dh);
                printf("%-10s %-10s %-8s %9.2f %9.2f %9.2f %9.2f %9.2f\n", fr, wn, fname[m], t_old, t_all, t_band, t_same, t_gdi);
            }
            SelectObject(mem, old);
            DeleteObject(dib);
            free(bits);
        }
    printf(g_fail ? "%d FAILED\n" : "OK\n", g_fail);
    return g_fail != 0;
}
