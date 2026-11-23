/* src/runtime/movie.c's helpers without the game or FFmpeg (the file itself
 * is compiled in here):
 *
 *   avi_probe     the film's size, rate and sound from its AVI header
 *   movie_find    --films DIR: NAME.avi, then the Saga's xNAME.avi
 *   film_palette  the 6x6x6 cube, the greys between, black and white
 *   film_index    a pixel as an entry of it, ordered-dithered: flat colours stay flat
 *   movie_draw    a frame into an 8-bit DC: the film palette (dithered), or any
 *                 other palette (nearest)
 *
 *   py -3 tools\test.py --level unit        (builds and runs every unit)
 */
#include "../../src/runtime/movie.c"

#include <math.h>

void softdd_refresh(void) {}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void put32(uint8_t* b, size_t o, uint32_t v) { memcpy(b + o, &v, 4); }

/* A RIFF AVI header: hdrl, avih (56 bytes), a vids strh and optionally an auds one. */
static size_t make_avi(uint8_t* b, int w, int h, uint32_t us, uint32_t scale, uint32_t rate, int audio, int strh) {
    size_t o = 0;
    memset(b, 0, 1024);
    memcpy(b, "RIFF", 4); memcpy(b + 8, "AVI ", 4); o = 12;
    memcpy(b + o, "LIST", 4); memcpy(b + o + 8, "hdrl", 4); o += 12;
    memcpy(b + o, "avih", 4); put32(b, o + 4, 56);
    put32(b, o + 8, us);
    put32(b, o + 8 + 32, (uint32_t)w); put32(b, o + 8 + 36, (uint32_t)h);
    o += 8 + 56;
    if (strh) {
        memcpy(b + o, "LIST", 4); memcpy(b + o + 8, "strl", 4); o += 12;
        memcpy(b + o, "strh", 4); put32(b, o + 4, 56);
        memcpy(b + o + 8, "vids", 4); memcpy(b + o + 12, "IV50", 4);
        put32(b, o + 8 + 20, scale); put32(b, o + 8 + 24, rate);
        o += 8 + 56;
    }
    if (audio) {
        memcpy(b + o, "LIST", 4); memcpy(b + o + 8, "strl", 4); o += 12;
        memcpy(b + o, "strh", 4); put32(b, o + 4, 56);
        memcpy(b + o + 8, "auds", 4);
        o += 8 + 56;
    }
    o += 64;   /* the 'movi' list would follow */
    put32(b, 4, (uint32_t)(o - 8));
    return o;
}

static void write_file(const char* path, const void* d, size_t n) {
    FILE* f = fopen(path, "wb");
    fwrite(d, 1, n, f);
    fclose(f);
}

int main(void) {
    char dir[MAX_PATH], p[MAX_PATH], out[MAX_PATH];
    GetTempPathA(MAX_PATH, dir);
    _snprintf(dir + strlen(dir), 64, "swgb_films_test_%lu", GetCurrentProcessId());
    CreateDirectoryA(dir, NULL);

    /* ---- avi_probe ---- */
    {
        uint8_t b[1024];
        avi_info ai;
        _snprintf(p, MAX_PATH, "%s\\a.avi", dir);
        write_file(p, b, make_avi(b, 640, 480, 66667, 1, 15, 1, 1));
        CHECK(avi_probe(p, &ai) && ai.w == 640 && ai.h == 480 && ai.scale == 1 && ai.rate == 15 && ai.has_audio,
              "a.avi: %dx%d %lu/%lu audio %d", ai.w, ai.h, ai.rate, ai.scale, ai.has_audio);
        write_file(p, b, make_avi(b, 320, 240, 40000, 0, 0, 0, 0));   /* no stream header: avih's rate */
        CHECK(avi_probe(p, &ai) && ai.rate == 1000000 && ai.scale == 40000 && !ai.has_audio, "no strh: %lu/%lu", ai.rate, ai.scale);
        write_file(p, b, make_avi(b, 5000, 480, 66667, 1, 15, 0, 1));
        CHECK(!avi_probe(p, &ai), "5000 wide is refused");
        size_t n = make_avi(b, 640, 480, 66667, 1, 15, 0, 1);
        memcpy(b + 8, "WAVE", 4);
        write_file(p, b, n);
        CHECK(!avi_probe(p, &ai), "not an AVI");
        _snprintf(p, MAX_PATH, "%s\\missing.avi", dir);
        CHECK(!avi_probe(p, &ai), "a missing file");
    }

    /* ---- movie_find ---- */
    {
        uint8_t b[1024];
        size_t n = make_avi(b, 640, 480, 66667, 1, 15, 0, 1);
        _snprintf(p, MAX_PATH, "%s\\xlogo1.avi", dir); write_file(p, b, n);
        _snprintf(p, MAX_PATH, "%s\\intro.avi", dir); write_file(p, b, n);
        _snprintf(p, MAX_PATH, "%s\\xintro.avi", dir); write_file(p, b, n);
        movie_dir = NULL;
        CHECK(!movie_find("avi\\logo1.avi", out, MAX_PATH), "no --films: nothing");
        movie_dir = dir;
        CHECK(movie_find("avi\\logo1.avi", out, MAX_PATH) && strstr(out, "\\xlogo1.avi"), "logo1 -> xlogo1: %s", out);
        CHECK(movie_find("C:\\x\\y\\INTRO.AVI", out, MAX_PATH) && strstr(out, "\\INTRO.AVI") && !strstr(out, "xINTRO"),
              "intro.avi itself first: %s", out);
        CHECK(!movie_find("avi\\nothere.avi", out, MAX_PATH), "a film not there");
        CHECK(!movie_find("sound\\logo1.mp3", out, MAX_PATH), "not an .avi");
        CHECK(!movie_find(NULL, out, MAX_PATH), "NULL");
    }

    /* ---- film_palette ---- */
    {
        HPALETTE pal = film_palette();
        CHECK(pal != NULL, "no palette");
        CHECK(film_palette() == pal, "made twice");
        PALETTEENTRY e[256];
        GetPaletteEntries(pal, 0, 256, e);
        CHECK(!e[0].peRed && !e[0].peGreen && !e[0].peBlue, "0 is not black");
        CHECK(e[255].peRed == 255 && e[255].peGreen == 255 && e[255].peBlue == 255, "255 is not white");
        int cube = 1;
        for (int r = 0; r < 6; r++) for (int g = 0; g < 6; g++) for (int bl = 0; bl < 6; bl++) {
            PALETTEENTRY c = e[1 + r * 36 + g * 6 + bl];
            if (c.peRed != r * 51 || c.peGreen != g * 51 || c.peBlue != bl * 51) cube = 0;
        }
        CHECK(cube, "the 6x6x6 cube");
        int greys = 1;
        for (int i = 217; i < 255; i++)
            greys &= e[i].peRed == e[i].peGreen && e[i].peGreen == e[i].peBlue && (i == 217 || e[i].peRed > e[i - 1].peRed);
        CHECK(greys, "the greys rise");
        CHECK(g_film_q[6].rgbBlue == 255 && g_film_q[6].rgbRed == 0, "the colour table matches");
    }

    /* ---- film_index ---- */
    {
        int flat = 1;
        for (int t = 0; t < 16; t++) {
            flat &= film_index(0, 0, 0, t) == 0;
            flat &= film_index(255, 255, 255, t) == 255;
            flat &= film_index(255, 0, 0, t) == 1 + 5 * 36;
            flat &= film_index(0, 0, 255, t) == 1 + 5;
            flat &= film_index(0, 255, 0, t) == 1 + 5 * 6;
            flat &= film_index(102, 204, 51, t) == 1 + 2 * 36 + 4 * 6 + 1;   /* on the cube exactly */
        }
        CHECK(flat, "black, white and the cube's own colours do not dither");
        /* greys stay grey (never a cube colour that is not grey) */
        int grey = 1;
        for (int v = 0; v < 256; v += 5)
            for (int t = 0; t < 16; t++) {
                int i = film_index(v, v, v, t);
                grey &= i == 0 || i == 255 || (i >= 217 && i <= 254);
            }
        CHECK(grey, "a grey pixel is a grey entry");
        /* the dither keeps the level on average: 16 thresholds of a 4x4 Bayer matrix */
        int avg_ok = 1;
        for (int v = 0; v < 256; v += 17) {
            double sum = 0;
            for (int t = 0; t < 16; t++) sum += (film_index(v, 0, 255, t) - 1) / 36 * 51;   /* red's level x 51 */
            double got = sum / 16;
            if (fabs(got - v) > 6.5) { avg_ok = 0; printf("  red %d dithers to %.1f on average\n", v, got); }
        }
        CHECK(avg_ok, "the dithered level is the colour on average");
    }

    /* ---- movie_draw into an 8-bit DC ---- */
    {
        HWND w = CreateWindowExA(0, "STATIC", "", WS_POPUP, 0, 0, 64, 48, NULL, NULL, NULL, NULL);
        Movie m;
        memset(&m, 0, sizeof m);
        m.hwnd = w;
        m.ai.w = 32; m.ai.h = 24;
        m.frame = (uint32_t*)malloc(32 * 24 * 4);
        InitializeCriticalSection(&m.cs);
        RECT cr;
        GetClientRect(w, &cr);
        int cw = cr.right, ch = cr.bottom;
        struct { BITMAPINFOHEADER h; RGBQUAD c[256]; } bi = { { sizeof(BITMAPINFOHEADER), cw, -ch, 1, 8, BI_RGB, 0, 0, 0, 256 } };
        void* bits;
        HDC dc = CreateCompatibleDC(NULL);
        film_palette();
        memcpy(bi.c, g_film_q, sizeof bi.c);
        HBITMAP dib = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
        HGDIOBJ old = SelectObject(dc, dib);
        int pitch = (cw + 3) & ~3;

        m.have_frame = 0;
        memset(bits, 0x55, (size_t)pitch * ch);
        movie_draw(&m, dc);
        GdiFlush();
        CHECK(((uint8_t*)bits)[0] == 0 && ((uint8_t*)bits)[pitch * (ch - 1) + cw - 1] == 0, "no frame yet: black");

        for (int i = 0; i < 32 * 24; i++) m.frame[i] = 0x000000FFu;   /* blue */
        m.have_frame = 1;
        movie_draw(&m, dc);
        GdiFlush();
        int all = 1;
        for (int y = 0; y < ch; y++) for (int x = 0; x < cw; x++) all &= ((uint8_t*)bits)[y * pitch + x] == 6;
        CHECK(all, "the film palette: blue is entry 6 everywhere (got %d)", ((uint8_t*)bits)[0]);

        /* another palette (the game's own): the nearest entry, no dither */
        for (int i = 0; i < 256; i++) bi.c[i].rgbRed = bi.c[i].rgbGreen = bi.c[i].rgbBlue = (BYTE)i;
        SetDIBColorTable(dc, 0, 256, bi.c);
        for (int i = 0; i < 32 * 24; i++) m.frame[i] = 0x00C8C8C8u;   /* grey 200 */
        movie_draw(&m, dc);
        GdiFlush();
        int near_ok = 1, got = ((uint8_t*)bits)[0];
        for (int y = 0; y < ch; y++) for (int x = 0; x < cw; x++) near_ok &= ((uint8_t*)bits)[y * pitch + x] == got;
        CHECK(near_ok && abs(got - 200) <= 8, "a grey palette: grey 200 -> %d everywhere", got);

        SelectObject(dc, old);
        DeleteObject(dib);
        DeleteDC(dc);
        DestroyWindow(w);
    }

    /* the scratch folder */
    {
        static const char* names[] = { "a.avi", "xlogo1.avi", "intro.avi", "xintro.avi" };
        for (int i = 0; i < 4; i++) { _snprintf(p, MAX_PATH, "%s\\%s", dir, names[i]); DeleteFileA(p); }
        RemoveDirectoryA(dir);
    }
    printf(fails ? "%d FAILED\n" : "OK\n", fails);
    return fails != 0;
}
