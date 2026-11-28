/*
 * movie_core: what the two hosts' film players share (movie_core.h): the AVI
 * header, --films' names, the film palette and the ordered dither into it,
 * the nearest entry of any other palette, and the A/V clock's arithmetic.
 * src/runtime/movie.c's header has how the game plays its films; this file
 * needs nothing of either platform, and tests/unit/films_test.c checks it on
 * both.
 */
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define film_strcasecmp _stricmp
#else
#include <strings.h>
#define film_strcasecmp strcasecmp
#endif

#include "movie_core.h"

int avi_probe(const char* path, avi_info* ai) {
    uint8_t b[8192];
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(b, 1, sizeof b, f);
    fclose(f);
    if (n < 64 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "AVI ", 4)) return 0;
    memset(ai, 0, sizeof *ai);
    uint32_t us = 0;
    for (size_t i = 12; i + 64 <= n; i++) {
        if (!memcmp(b + i, "avih", 4)) {
            memcpy(&us, b + i + 8, 4);
            memcpy(&ai->w, b + i + 8 + 32, 4);
            memcpy(&ai->h, b + i + 8 + 36, 4);
        } else if (!memcmp(b + i, "strh", 4)) {
            if (!memcmp(b + i + 8, "vids", 4) && !ai->rate) {
                memcpy(&ai->scale, b + i + 8 + 20, 4);
                memcpy(&ai->rate, b + i + 8 + 24, 4);
            } else if (!memcmp(b + i + 8, "auds", 4)) ai->has_audio = 1;
        }
    }
    if (!ai->rate || !ai->scale) { ai->rate = 1000000; ai->scale = us ? us : 66667; }
    return ai->w > 0 && ai->h > 0 && ai->w <= 4096 && ai->h <= 4096;
}

int film_name(const char* guest_name, int x, char* out, size_t n) {
    if (!guest_name) return 0;
    const char* base = guest_name;
    for (const char* p = guest_name; *p; p++) if (*p == '\\' || *p == '/' || *p == ':') base = p + 1;
    const char* dot = strrchr(base, '.');
    if (!dot || film_strcasecmp(dot, ".avi")) return 0;
    snprintf(out, n, "%s%s", x ? "x" : "", base);
    return 1;
}

static uint32_t g_film_pe[256];

const uint32_t* film_palette_entries(void) {
    if (g_film_pe[255]) return g_film_pe;
    for (int i = 0; i < 216; i++)
        g_film_pe[1 + i] = (uint32_t)(i / 36 * 51) | (uint32_t)(i / 6 % 6 * 51) << 8 | (uint32_t)(i % 6 * 51) << 16;
    for (int i = 217; i < 255; i++) {
        uint32_t g = (uint32_t)((i - 216) * 255 / 39);
        g_film_pe[i] = g | g << 8 | g << 16;
    }
    g_film_pe[255] = 0xFFFFFFu;
    return g_film_pe;
}

int film_palette_is(const uint32_t* pe) {
    const uint32_t* f = film_palette_entries();
    for (int i = 1; i < 255; i++)
        if ((pe[i] ^ f[i]) & 0xFFFFFFu) return 0;
    return 1;
}

static uint8_t g_lut[32768];
static uint32_t g_lut_pal[256];
static int g_lut_ok;

const uint8_t* film_lut(const uint32_t* pe) {
    if (g_lut_ok && !memcmp(pe, g_lut_pal, sizeof g_lut_pal)) return g_lut;
    memcpy(g_lut_pal, pe, sizeof g_lut_pal);
    g_lut_ok = 1;
    for (int c = 0; c < 32768; c++) {
        int r = (c >> 10) * 255 / 31, g = (c >> 5 & 31) * 255 / 31, b = (c & 31) * 255 / 31;
        int best = 0, bd = 1 << 30;
        for (int i = 0; i < 256; i++) {
            int dr = r - (int)(pe[i] & 255), dg = g - (int)(pe[i] >> 8 & 255), db = b - (int)(pe[i] >> 16 & 255);
            int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
            if (d < bd) bd = d, best = i;
        }
        g_lut[c] = (uint8_t)best;
    }
    return g_lut;
}

static const int8_t g_bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

/* v (0..255) as one of n + 1 evenly spaced levels, ordered-dithered by t
 * (0..15): flat black and white stay flat. */
static int dither(int v, int n, int t) {
    int l = (v * n * 32 + (2 * t + 1) * 255) / (255 * 32);
    return l > n ? n : l;
}

uint8_t film_index(int r, int g, int b, int t) {
    int hi = r > g ? (r > b ? r : b) : (g > b ? g : b), lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
    if (hi - lo <= 12) {
        int l = dither((r + g + b) / 3, 39, t);
        return (uint8_t)(l == 0 ? 0 : l == 39 ? 255 : 216 + l);
    }
    return (uint8_t)(1 + dither(r, 5, t) * 36 + dither(g, 5, t) * 6 + dither(b, 5, t));
}

void film_row8(uint8_t* row, int cw, const uint32_t* src, int vw, int y, const uint8_t* lut) {
    for (int x = 0; x < cw; x++) {
        uint32_t p = src[(long long)x * vw / cw];
        int r = p >> 16 & 255, g = p >> 8 & 255, b = p & 255;
        row[x] = lut ? lut[(r >> 3) << 10 | (g >> 3) << 5 | b >> 3] : film_index(r, g, b, g_bayer[y & 3][x & 3]);
    }
}

double film_wall_ms(const film_clock* c, uint32_t now) {
    return c->base_ms + (double)((c->paused ? c->pause_tick : now) - c->base_tick - c->pause_total);
}

void film_clock_start(film_clock* c, uint32_t now) {
    c->base_ms = 0;
    c->base_tick = now;
    c->pause_total = 0;
}

void film_clock_to_wall(film_clock* c, uint32_t now, double sound_ms) {
    if (!c->audio_on) return;   /* the wall clock already: it runs on as it is */
    c->base_ms = sound_ms;
    c->audio_on = 0;
    c->base_tick = c->paused ? c->pause_tick : now;
    c->pause_total = 0;
}

int film_clock_pause(film_clock* c, int pause, uint32_t now) {
    if (pause == c->paused) return 0;
    if (pause) c->pause_tick = now;
    else c->pause_total += now - c->pause_tick;
    c->paused = pause;
    return 1;
}

const char* film_mode(int playing, int paused, int done) {
    if (playing && !done) return paused ? "paused" : "playing";
    return "stopped";
}
