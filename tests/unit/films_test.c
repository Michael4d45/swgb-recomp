/* The films' helpers without the game or FFmpeg. On both hosts, movie_core.c,
 * which src/runtime/movie.c and src/linux/movie.c share:
 *
 *   avi_probe     the film's size, rate and sound from its AVI header
 *   film_name     --films DIR's names: NAME.avi, then the Saga's xNAME.avi
 *   the palette   the 6x6x6 cube, the greys between, black and white
 *   film_index    a pixel as an entry of it, ordered-dithered: flat colours stay flat
 *   film_lut      any other palette: the nearest entry
 *   the clock     the wall clock, a pause, the sound running out
 *
 * On Windows also src/runtime/movie.c itself (compiled in here): movie_find
 * in a folder, the GDI palette, and movie_draw into an 8-bit DC.
 *
 *   py -3 tools\test.py --level unit        (builds and runs every unit)
 */
#ifdef _WIN32
#include "../../src/runtime/movie.c"

void softdd_refresh(void) {}
/* not recording (recaudio.c has its own test) */
int recaudio_stream_open(int rate, int channels) { (void)rate, (void)channels; return -1; }
void recaudio_stream_write(int id, const int16_t* pcm, int frames, double plays_in_ms) { (void)id, (void)pcm, (void)frames, (void)plays_in_ms; }
void recaudio_stream_pause(int id, int paused) { (void)id, (void)paused; }
void recaudio_stream_close(int id) { (void)id; }
#define SEP "\\"
#else
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "movie_core.h"
#define SEP "/"
#endif

#include <math.h>

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

static void test_avi_probe(const char* dir) {
    char p[1024];
    uint8_t b[1024];
    avi_info ai;
    snprintf(p, sizeof p, "%s" SEP "a.avi", dir);
    write_file(p, b, make_avi(b, 640, 480, 66667, 1, 15, 1, 1));
    CHECK(avi_probe(p, &ai) && ai.w == 640 && ai.h == 480 && ai.scale == 1 && ai.rate == 15 && ai.has_audio,
          "a.avi: %dx%d %u/%u audio %d", ai.w, ai.h, (unsigned)ai.rate, (unsigned)ai.scale, ai.has_audio);
    write_file(p, b, make_avi(b, 320, 240, 40000, 0, 0, 0, 0));   /* no stream header: avih's rate */
    CHECK(avi_probe(p, &ai) && ai.rate == 1000000 && ai.scale == 40000 && !ai.has_audio, "no strh: %u/%u",
          (unsigned)ai.rate, (unsigned)ai.scale);
    write_file(p, b, make_avi(b, 5000, 480, 66667, 1, 15, 0, 1));
    CHECK(!avi_probe(p, &ai), "5000 wide is refused");
    size_t n = make_avi(b, 640, 480, 66667, 1, 15, 0, 1);
    memcpy(b + 8, "WAVE", 4);
    write_file(p, b, n);
    CHECK(!avi_probe(p, &ai), "not an AVI");
    remove(p);
    snprintf(p, sizeof p, "%s" SEP "missing.avi", dir);
    CHECK(!avi_probe(p, &ai), "a missing file");
}

static void test_film_name(void) {
    char out[64];
    CHECK(film_name("avi\\logo1.avi", 0, out, sizeof out) && !strcmp(out, "logo1.avi"), "a Windows path: %s", out);
    CHECK(film_name("avi\\logo1.avi", 1, out, sizeof out) && !strcmp(out, "xlogo1.avi"), "the Saga's name: %s", out);
    CHECK(film_name("C:\\x/y\\INTRO.AVI", 0, out, sizeof out) && !strcmp(out, "INTRO.AVI"), "either slash, any case: %s", out);
    CHECK(film_name("D:intro.avi", 0, out, sizeof out) && !strcmp(out, "intro.avi"), "a drive with no slash: %s", out);
    CHECK(!film_name("sound\\logo1.mp3", 0, out, sizeof out), "not an .avi");
    CHECK(!film_name("avi.d\\logo1", 0, out, sizeof out), "a dot in the folder only");
    CHECK(!film_name(NULL, 0, out, sizeof out), "NULL");
}

static void test_palette(void) {
    const uint32_t* e = film_palette_entries();
    CHECK(e[0] == 0, "0 is not black");
    CHECK(e[255] == 0xFFFFFFu, "255 is not white");
    int cube = 1;
    for (int r = 0; r < 6; r++) for (int g = 0; g < 6; g++) for (int bl = 0; bl < 6; bl++)
        cube &= e[1 + r * 36 + g * 6 + bl] == ((uint32_t)(r * 51) | (uint32_t)(g * 51) << 8 | (uint32_t)(bl * 51) << 16);
    CHECK(cube, "the 6x6x6 cube (r, g, b as a PALETTEENTRY)");
    int greys = 1;
    for (int i = 217; i < 255; i++) {
        uint32_t c = e[i], v = c & 255;
        greys &= c == (v | v << 8 | v << 16) && (i == 217 || v > (e[i - 1] & 255));
    }
    CHECK(greys, "the greys rise");

    uint32_t pe[256];
    memcpy(pe, e, sizeof pe);
    pe[0] = 0x00123456u, pe[255] = 0x01000000u;   /* the game's own 0 and 255, a flags byte */
    for (int i = 1; i < 255; i++) pe[i] |= 0x04000000u;
    CHECK(film_palette_is(pe), "the film palette with other ends and flags");
    pe[100] ^= 0x10;
    CHECK(!film_palette_is(pe), "one colour changed");
}

static void test_index(void) {
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
    int grey = 1;   /* greys stay grey (never a cube colour that is not grey) */
    for (int v = 0; v < 256; v += 5)
        for (int t = 0; t < 16; t++) {
            int i = film_index(v, v, v, t);
            grey &= i == 0 || i == 255 || (i >= 217 && i <= 254);
        }
    CHECK(grey, "a grey pixel is a grey entry");
    int avg_ok = 1;   /* the dither keeps the level on average: 16 thresholds of a 4x4 Bayer matrix */
    for (int v = 0; v < 256; v += 17) {
        double sum = 0;
        for (int t = 0; t < 16; t++) sum += (film_index(v, 0, 255, t) - 1) / 36 * 51;   /* red's level x 51 */
        double got = sum / 16;
        if (fabs(got - v) > 6.5) { avg_ok = 0; printf("  red %d dithers to %.1f on average\n", v, got); }
    }
    CHECK(avg_ok, "the dithered level is the colour on average");
}

static void test_rows(void) {
    uint32_t src[8], grey[256];
    uint8_t row[16];
    for (int i = 0; i < 8; i++) src[i] = i < 4 ? 0x000000FFu : 0x00FF0000u;   /* BGRA: blue, then red */
    film_row8(row, 16, src, 8, 0, NULL);
    int ok = 1;
    for (int x = 0; x < 16; x++) ok &= row[x] == (x < 8 ? 1 + 5 : 1 + 5 * 36);
    CHECK(ok, "8 pixels to 16, dithered into the film palette: blue then red");
    for (int i = 0; i < 256; i++) grey[i] = (uint32_t)i | (uint32_t)i << 8 | (uint32_t)i << 16;
    const uint8_t* lut = film_lut(grey);
    CHECK(film_lut(grey) == lut, "the same palette: the table is kept");
    for (int i = 0; i < 8; i++) src[i] = 0x00C8C8C8u;   /* grey 200 */
    film_row8(row, 16, src, 8, 1, lut);
    int near_ok = abs(row[0] - 200) <= 8;
    for (int x = 1; x < 16; x++) near_ok &= row[x] == row[0];
    CHECK(near_ok, "a grey palette: grey 200 -> %d everywhere", row[0]);
    CHECK(lut[0x7FFF] == 255 && lut[0] == 0, "white and black are the palette's ends");
}

static void test_clock(void) {
    film_clock c;
    memset(&c, 0, sizeof c);
    film_clock_start(&c, 1000);
    CHECK(film_wall_ms(&c, 1250) == 250, "the wall clock: %.0f", film_wall_ms(&c, 1250));
    CHECK(film_clock_pause(&c, 1, 1300) && !film_clock_pause(&c, 1, 1350), "a second pause changes nothing");
    CHECK(film_wall_ms(&c, 1800) == 300, "paused: held at 300, got %.0f", film_wall_ms(&c, 1800));
    CHECK(film_clock_pause(&c, 0, 1800), "resumed");
    CHECK(film_wall_ms(&c, 1900) == 400, "less the 500 ms paused: %.0f", film_wall_ms(&c, 1900));

    memset(&c, 0, sizeof c);
    c.audio_on = 1;
    film_clock_to_wall(&c, 5000, 2345.0);   /* the sound ran out at 2.345 s */
    CHECK(!c.audio_on && film_wall_ms(&c, 5100) == 2445, "on from the sound's end: %.0f", film_wall_ms(&c, 5100));
    memset(&c, 0, sizeof c);
    film_clock_start(&c, 0);
    film_clock_pause(&c, 1, 700);
    film_clock_to_wall(&c, 900, 0);         /* no sound: the wall clock as it was, still paused */
    CHECK(film_wall_ms(&c, 2000) == 700, "paused through it: %.0f", film_wall_ms(&c, 2000));

    CHECK(!strcmp(film_mode(1, 0, 0), "playing") && !strcmp(film_mode(1, 1, 0), "paused") &&
          !strcmp(film_mode(1, 0, 1), "stopped") && !strcmp(film_mode(0, 0, 0), "stopped"), "the modes");
}

#ifdef _WIN32
static void test_movie_find(const char* dir) {
    char p[MAX_PATH], out[MAX_PATH];
    uint8_t b[1024];
    size_t n = make_avi(b, 640, 480, 66667, 1, 15, 0, 1);
    static const char* names[] = { "xlogo1.avi", "intro.avi", "xintro.avi" };
    for (int i = 0; i < 3; i++) { _snprintf(p, MAX_PATH, "%s\\%s", dir, names[i]); write_file(p, b, n); }
    movie_dir = NULL;
    CHECK(!movie_find("avi\\logo1.avi", out, MAX_PATH), "no --films: nothing");
    movie_dir = dir;
    CHECK(movie_find("avi\\logo1.avi", out, MAX_PATH) && strstr(out, "\\xlogo1.avi"), "logo1 -> xlogo1: %s", out);
    CHECK(movie_find("C:\\x\\y\\INTRO.AVI", out, MAX_PATH) && strstr(out, "\\INTRO.AVI") && !strstr(out, "xINTRO"),
          "intro.avi itself first: %s", out);
    CHECK(!movie_find("avi\\nothere.avi", out, MAX_PATH), "a film not there");
    CHECK(!movie_find("sound\\logo1.mp3", out, MAX_PATH), "not an .avi");
    CHECK(!movie_find(NULL, out, MAX_PATH), "NULL");
    for (int i = 0; i < 3; i++) { _snprintf(p, MAX_PATH, "%s\\%s", dir, names[i]); DeleteFileA(p); }
}

static void test_gdi(void) {
    HPALETTE pal = film_palette();
    CHECK(pal != NULL, "no palette");
    CHECK(film_palette() == pal, "made twice");
    PALETTEENTRY e[256];
    GetPaletteEntries(pal, 0, 256, e);
    CHECK(!memcmp(e, film_palette_entries(), sizeof e), "the GDI palette is the film palette");

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
    for (int i = 0; i < 256; i++)
        bi.c[i].rgbRed = e[i].peRed, bi.c[i].rgbGreen = e[i].peGreen, bi.c[i].rgbBlue = e[i].peBlue;
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
#endif

int main(void) {
    char dir[1024];
#ifdef _WIN32
    GetTempPathA(MAX_PATH, dir);
    _snprintf(dir + strlen(dir), 64, "swgb_films_test_%lu", GetCurrentProcessId());
    CreateDirectoryA(dir, NULL);
#else
    const char* tmp = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/swgb_films_test_XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(dir)) { printf("FAIL: no scratch folder %s\n", dir); return 1; }
#endif
    test_avi_probe(dir);
    test_film_name();
    test_palette();
    test_index();
    test_rows();
    test_clock();
#ifdef _WIN32
    test_movie_find(dir);
    test_gdi();
    RemoveDirectoryA(dir);
#else
    rmdir(dir);
#endif
    printf(fails ? "%d FAILED\n" : "OK\n", fails);
    return fails != 0;
}
