/*
 * Star Wars: Galactic Battlegrounds - the native host, for Linux and macOS.
 *
 * The same lifted C as the Windows build (src/recomp/gen), on pcrecomp's
 * runtime/win32hle instead of runtime/native32: every Win32 import the game
 * makes is answered by win32hle's own implementation (files, windows and
 * their controls, DirectDraw and DirectSound on SDL2, the registry, Winsock),
 * so no Windows and no Wine. softdd is not here: win32hle's DirectDraw is the
 * virtual display, and screen.c shows it (docs/linux-macos.md, "DirectDraw").
 *
 * What is here is what is specific to this game, as on Windows (host.c):
 * where the image goes, the command line, the play disc, scripted input,
 * --shot and --record, muting, and the fault report. Modelled on Red Alert 2's
 * and Civilization III's src/linux.
 *
 * 32-bit (Linux, -m32) it is linked non-PIE at 0x08048000, so the guest's
 * 0x00400000..0x00A5A000 is free; 64-bit (macOS, or Linux x86-64/arm64) the
 * guest's memory is one block of the host's and the image goes at its own
 * address in it.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <unistd.h>
#include <limits.h>
#include <pthread.h>
#include <sys/stat.h>
#include <dirent.h>
#include <SDL.h>

#include "win32hle.h"
#include "pe_loader.h"
#include "host.h"
#include "keyscript.h"
#include "coverage.h"
#include "widescreen.h"
#include "smooth.h"
#include "dplay.h"
#include "cdaudio.h"
#include "profile.h"
#include "swgb_linux.h"

extern const uint32_t swgb_entry_va;   /* recomp_dispatch.c */

#define IMAGE_BASE 0x00400000u
/* SWGB_CC: Clone Campaigns' host, swgbrecomp_cc (cmake/native.cmake), as
 * swgbrecomp_cc.exe is on Windows: its exe, registry key and disc. */
#ifdef SWGB_CC
#define GUEST_EXE  "battlegrounds_x1.exe"
#define EXE_STAMP  0x3CF57142u         /* battlegrounds_x1.exe (2002-05-29), unwrapped */
#define REG_GAME   "Software\\LucasArts Entertainment Company LLC\\Star Wars Galactic Battlegrounds: Clone Campaigns\\1.0"
#define CD_PROBE   "gamedata/game/battlegrounds_x1.exe"
#define CD_NAME    "the Clone Campaigns disc"
#define GAME_TITLE "Star Wars Galactic Battlegrounds: Clone Campaigns"
#else
#define GUEST_EXE  "Battlegrounds.exe"
#define EXE_STAMP  0x3C03FD69u         /* Battlegrounds.exe v1.1 (run_lift.py refuses others) */
#define REG_GAME   "Software\\LucasArts Entertainment Company LLC\\Star Wars Galactic Battlegrounds\\1.0"
#define CD_PROBE   "Game"
#define CD_NAME    "disc 2"
#define GAME_TITLE "Star Wars Galactic Battlegrounds"
#endif

char swgb_cd_letter;
char swgb_temp_drive;
static char g_temp_dir[PATH_MAX];

/* The game's temp folder: a folder of this run's own (two --multi games at
 * once write the same temNNNN.TMP names), drive T:, removed at exit. */
static void temp_make(void) {
    const char* base = getenv("TMPDIR");
    snprintf(g_temp_dir, sizeof g_temp_dir, "%s/swgbrecomp-XXXXXX", base && *base ? base : "/tmp");
    if (!mkdtemp(g_temp_dir)) { g_temp_dir[0] = 0; return; }
    swgb_temp_drive = 'T';
    hle_set_drive('T', g_temp_dir);
}
static void temp_remove(void) {
    if (!g_temp_dir[0]) return;
    DIR* d = opendir(g_temp_dir);
    struct dirent* e;
    while (d && (e = readdir(d))) {
        char p[PATH_MAX + 260];
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "%s/%s", g_temp_dir, e->d_name);
        unlink(p);
    }
    if (d) closedir(d);
    rmdir(g_temp_dir);
    g_temp_dir[0] = 0;
}
int  swgb_test_run;
int swgb_log_files;
static unsigned g_watchdog_s;

/* Unset: Genie draws mcursor into the primary (screen.c hides the SDL
 * pointer). A hook that returns nonzero would force the Win32/SDL cursor
 * instead — useful only while that sprite is missing. */

/* ---- what the lift calls into ---------------------------------------------- */
void recomp_not_lifted(uint32_t va) {
    fprintf(stderr, "\n[not-lifted] sub_%08X  (called from 0x%08X)\n", va, g_cur_func);
    fflush(stderr);
    _exit(2);
}

/* ---- file opens: --files, and on:NAME in --keys ----------------------------- */
static pthread_mutex_t g_seen_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_seen_cond = PTHREAD_COND_INITIALIZER;
static char g_wait_file[64];
static int  g_file_seen;

void swgb_file_seen(const char* name, int ok) {
    if (swgb_log_files) printf("[host] open %s -> %s\n", name, ok ? "ok" : "missing");
    pthread_mutex_lock(&g_seen_lock);
    if (g_wait_file[0] && strcasestr(name, g_wait_file)) {
        g_wait_file[0] = 0;
        g_file_seen = 1;
        pthread_cond_signal(&g_seen_cond);
    }
    pthread_mutex_unlock(&g_seen_lock);
}

/* ---- the frame: --shot and --record ------------------------------------------
 * The primary as win32hle's DirectDraw holds it (8-bit through its palette,
 * or RGB565), as 24-bit RGB rows, top-down, into the caller's buffer (grown
 * as needed: --shot and --record each have their own). Read holding the
 * machine: a mode change can free the primary under us. */
typedef struct { uint8_t* rgb; size_t n; } frame_buf;

static int grab(frame_buf* fb, int* w_out, int* h_out) {
    const uint8_t* px;
    const uint32_t* pal;
    int w, h, pitch, bpp, ok;
    mach_enter();
    ok = hle_dd_frame(&px, &w, &h, &pitch, &bpp, &pal, NULL) && (bpp == 8 || bpp == 16) && w > 0 && w <= 4096 && h > 0;
    if (ok && fb->n < (size_t)w * h * 3) {
        free(fb->rgb);
        fb->n = (size_t)w * h * 3;
        fb->rgb = (uint8_t*)malloc(fb->n);
        if (!fb->rgb) fb->n = 0, ok = 0;
    }
    for (int y = 0; ok && y < h; y++) {
        const uint8_t* s = px + (size_t)y * pitch;
        uint8_t* d = fb->rgb + (size_t)y * w * 3;
        for (int x = 0; x < w; x++, d += 3) {
            if (bpp == 8) {
                uint32_t e = pal ? pal[s[x]] : s[x] * 0x010101u;   /* PALETTEENTRY: r, g, b */
                d[0] = (uint8_t)e, d[1] = (uint8_t)(e >> 8), d[2] = (uint8_t)(e >> 16);
            } else {
                uint16_t p = ((const uint16_t*)s)[x];
                d[0] = (uint8_t)((p >> 11) << 3), d[1] = (uint8_t)((p >> 5 & 63) << 2), d[2] = (uint8_t)((p & 31) << 3);
            }
        }
    }
    mach_leave();
    if (ok) *w_out = w, *h_out = h;
    return ok;
}

/* From the --shot thread and the --keys thread: one at a time. */
static void write_bmp(const char* path) {
    static frame_buf fb;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    int w, h;
    pthread_mutex_lock(&lock);
    FILE* f = NULL;
    if (!grab(&fb, &w, &h)) fprintf(stderr, "[shot] no frame yet for %s\n", path);
    else if (!(f = fopen(path, "wb"))) fprintf(stderr, "[shot] cannot write %s\n", path);
    if (!f) { pthread_mutex_unlock(&lock); return; }
    uint32_t row = (uint32_t)(w * 3 + 3) & ~3u, size = 54 + row * (uint32_t)h;
    uint8_t hdr[54] = { 'B', 'M' };
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54, hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1, hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    uint8_t line[4096 * 3 + 4] = { 0 };
    for (int y = h - 1; y >= 0; y--) {                    /* BMP rows are bottom-up, pixels BGR */
        const uint8_t* s = fb.rgb + (size_t)y * w * 3;
        for (int x = 0; x < w; x++) line[3 * x] = s[3 * x + 2], line[3 * x + 1] = s[3 * x + 1], line[3 * x + 2] = s[3 * x];
        fwrite(line, 1, row, f);
    }
    fclose(f);
    pthread_mutex_unlock(&lock);
    fprintf(stderr, "[shot] %dx%d -> %s\n", w, h, path);
}

static char g_shot_file[16][PATH_MAX];
static unsigned g_shot_s[16];
static int g_shots;
static void* shot_thread(void* unused) {
    (void)unused;
    uint32_t t0 = hle_ticks_ms();
    for (int i = 0; i < g_shots; i++) {
        int32_t wait = (int32_t)(t0 + g_shot_s[i] * 1000u - hle_ticks_ms());
        if (wait > 0) usleep((useconds_t)wait * 1000u);
        write_bmp(g_shot_file[i]);
    }
    return NULL;
}

/* --record out.mp4: 30 frames a second against the clock, piped to ffmpeg,
 * so the mp4 plays at the game's real speed (as softdd records on Windows).
 * --frames N closes it after N and ends the run. */
static const char* g_record;
static long g_record_frames;
static FILE* g_ffmpeg;
static pthread_mutex_t g_rec_lock = PTHREAD_MUTEX_INITIALIZER;

static void record_close(void) {
    pthread_mutex_lock(&g_rec_lock);
    if (g_ffmpeg) { pclose(g_ffmpeg); g_ffmpeg = NULL; fprintf(stderr, "[record] closed %s\n", g_record); }
    pthread_mutex_unlock(&g_rec_lock);
}
static void on_exit_close(int code) { (void)code; dplay_finish(); profile_finish(); record_close(); temp_remove(); }

static void* record_thread(void* unused) {
    (void)unused;
    int rec_w = 0, rec_h = 0;
    long n = 0;
    frame_buf fb = { NULL, 0 };
    uint32_t next = hle_ticks_ms();
    for (;;) {
        next += 33;
        int32_t wait = (int32_t)(next - hle_ticks_ms());
        if (wait > 0) usleep((useconds_t)wait * 1000u);
        int w, h;
        if (!grab(&fb, &w, &h)) continue;
        pthread_mutex_lock(&g_rec_lock);
        if (!g_ffmpeg && n) { pthread_mutex_unlock(&g_rec_lock); return NULL; }   /* closed */
        if (!g_ffmpeg) {
            char cmd[PATH_MAX + 256];
            rec_w = w, rec_h = h;                         /* a later mode is cropped or padded to the first */
            snprintf(cmd, sizeof cmd, "ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgb24 -s %dx%d -r 30 -i - "
                     "-c:v libx264 -preset veryfast -pix_fmt yuv420p '%s'", rec_w, rec_h, g_record);
            g_ffmpeg = popen(cmd, "w");
            if (!g_ffmpeg) { fprintf(stderr, "[record] cannot start ffmpeg\n"); pthread_mutex_unlock(&g_rec_lock); return NULL; }
            fprintf(stderr, "[record] %dx%d at 30 fps -> %s\n", rec_w, rec_h, g_record);
        }
        static uint8_t pad[4096 * 3];
        for (int y = 0; y < rec_h; y++) {
            if (y < h && w >= rec_w) fwrite(fb.rgb + (size_t)y * w * 3, 3, (size_t)rec_w, g_ffmpeg);
            else if (y < h) { fwrite(fb.rgb + (size_t)y * w * 3, 3, (size_t)w, g_ffmpeg); fwrite(pad, 3, (size_t)(rec_w - w), g_ffmpeg); }
            else fwrite(pad, 3, (size_t)rec_w, g_ffmpeg);
        }
        int bad = ferror(g_ffmpeg);
        pthread_mutex_unlock(&g_rec_lock);
        if (bad) { fprintf(stderr, "[record] ffmpeg stopped taking frames\n"); record_close(); return NULL; }
        if (++n == g_record_frames) {
            fprintf(stderr, "[record] %ld frames: stopping (--frames)\n", n);
            record_close();
            hle_exit(0);
        }
    }
}

/* ---- scripted input (--keys, --keys-live) ------------------------------------
 * The grammar is keyscript.c's, the same parser as the Windows host's
 * (tests/unit/keys_test.c). Input goes in as a mouse and keyboard would give
 * it (hle_input_*): win32hle hit-tests the click and sends keys to the focus,
 * so a text field gets its own, and GetKeyState sees the modifiers and the
 * buttons held. Headless the picture is the whole "window" at the screen's
 * origin, so window places (wmove, wclick) are game pixels; with a window they
 * are its client points, the picture fitted to it as the presenter does. */
#define MAX_STEPS 16384   /* tools/soak.py: a 30-minute monkey run is a few thousand steps */
static step_t g_steps[MAX_STEPS];
static int g_nsteps;
static int g_headless;
static char g_live_file[PATH_MAX];
static uint32_t g_keys_t0;

static void mouse(uint32_t msg, int x, int y) { mach_enter(); hle_input_mouse(msg, x, y); mach_leave(); }
/* The key's scan code, as a keyboard sends it (US set 1) and the Windows
 * host's --keys does (MapVirtualKeyA): bits 16-23 of the key messages. */
static uint32_t scan_of_vk(int vk) {
    static const uint8_t scan[256] = {
        [0x08] = 0x0E, [0x09] = 0x0F, [0x0D] = 0x1C, [0x10] = 0x2A, [0x11] = 0x1D, [0x12] = 0x38, [0x1B] = 0x01, [0x20] = 0x39,
        ['1'] = 2, ['2'] = 3, ['3'] = 4, ['4'] = 5, ['5'] = 6, ['6'] = 7, ['7'] = 8, ['8'] = 9, ['9'] = 10, ['0'] = 11,
        ['Q'] = 0x10, ['W'] = 0x11, ['E'] = 0x12, ['R'] = 0x13, ['T'] = 0x14, ['Y'] = 0x15, ['U'] = 0x16, ['I'] = 0x17,
        ['O'] = 0x18, ['P'] = 0x19, ['A'] = 0x1E, ['S'] = 0x1F, ['D'] = 0x20, ['F'] = 0x21, ['G'] = 0x22, ['H'] = 0x23,
        ['J'] = 0x24, ['K'] = 0x25, ['L'] = 0x26, ['Z'] = 0x2C, ['X'] = 0x2D, ['C'] = 0x2E, ['V'] = 0x2F, ['B'] = 0x30,
        ['N'] = 0x31, ['M'] = 0x32, [0x70] = 0x3B, [0x71] = 0x3C, [0x72] = 0x3D, [0x73] = 0x3E, [0x74] = 0x3F,
        [0x75] = 0x40, [0x76] = 0x41, [0x77] = 0x42, [0x78] = 0x43, [0x79] = 0x44, [0x7A] = 0x57, [0x7B] = 0x58,
        [0x25] = 0x4B, [0x26] = 0x48, [0x27] = 0x4D, [0x28] = 0x50, [0x2D] = 0x52, [0x2E] = 0x53, [0x24] = 0x47,
        [0x23] = 0x4F, [0x21] = 0x49, [0x22] = 0x51 };
    return scan[vk & 0xFF];
}
static void key(int down, int vk) { mach_enter(); hle_input_key(down, (uint32_t)vk, scan_of_vk(vk)); mach_leave(); }

static void wait_for_file(const char* name) {
    pthread_mutex_lock(&g_seen_lock);
    if (!g_wait_file[0] && !g_file_seen) snprintf(g_wait_file, sizeof g_wait_file, "%s", name);
    while (!g_file_seen) pthread_cond_wait(&g_seen_cond, &g_seen_lock);
    g_file_seen = 0;
    pthread_mutex_unlock(&g_seen_lock);
}

/* The window's client size and the picture in it (game pixels when headless). */
static void client_and_picture(int* cw, int* ch, int pic[4]) {
    int gw = 800, gh = 600, bpp;
    hle_dd_mode(&gw, &gh, &bpp);
    if (gw <= 0 || gh <= 0) gw = 800, gh = 600;
    *cw = gw, *ch = gh;
    struct SDL_Window* w = g_headless ? NULL : hle_screen_sdl_window();
    if (w) SDL_GetWindowSize(w, cw, ch);
    if ((long long)*cw * gh > (long long)*ch * gw) {      /* bars left and right */
        int pw = (int)((long long)*ch * gw / gh);
        pic[0] = (*cw - pw) / 2, pic[1] = 0, pic[2] = pic[0] + pw, pic[3] = *ch;
    } else {
        int ph = (int)((long long)*cw * gh / gw);
        pic[0] = 0, pic[1] = (*ch - ph) / 2, pic[2] = *cw, pic[3] = pic[1] + ph;
    }
}

/* "R-3", "C", "PL+10", "120": a place on one axis of the client area (keyscript.h). */
static int place(const char* t, int axis, int cw, int ch, const int pic[4]) {
    char name[8] = "";
    int n = 0, base;
    const char* q = t;
    while (((*q | 0x20) >= 'a' && (*q | 0x20) <= 'z') && n < 7) name[n++] = *q++;
    name[n] = 0;
    if (!n) return atoi(t);
    if (axis == 0) {
        if (!strcasecmp(name, "L")) base = 0;
        else if (!strcasecmp(name, "R")) base = cw - 1;
        else if (!strcasecmp(name, "C")) base = cw / 2;
        else if (!strcasecmp(name, "PL")) base = pic[0];
        else if (!strcasecmp(name, "PR")) base = pic[2] - 1;
        else if (!strcasecmp(name, "BL")) base = pic[0] / 2;
        else if (!strcasecmp(name, "BR")) base = (pic[2] + cw) / 2;
        else { fprintf(stderr, "--keys: no place %s on x\n", name); exit(1); }
    } else {
        if (!strcasecmp(name, "T")) base = 0;
        else if (!strcasecmp(name, "B")) base = ch - 1;
        else if (!strcasecmp(name, "C")) base = ch / 2;
        else if (!strcasecmp(name, "PT")) base = pic[1];
        else if (!strcasecmp(name, "PB")) base = pic[3] - 1;
        else if (!strcasecmp(name, "BT")) base = pic[1] / 2;
        else if (!strcasecmp(name, "BB")) base = (pic[3] + ch) / 2;
        else { fprintf(stderr, "--keys: no place %s on y\n", name); exit(1); }
    }
    return base + (*q ? atoi(q) : 0);
}

/* One step, now: a shot, a size, the cursor, a click or a key. */
static void key_run(step_t* st) {
    if (st->shot[0]) { write_bmp(st->shot); return; }   /* wshot too: no bars to show here */
    if (st->size) {
        struct SDL_Window* w = g_headless ? NULL : hle_screen_sdl_window();
        if (w) SDL_SetWindowSize(w, st->x, st->y);
        printf("[keys] %.1fs size %dx%d%s\n", st->at / 1000.0, st->x, st->y, w ? "" : " (headless: no window)");
        return;
    }
    if (st->setcursor) {   /* a Windows presenter check (WM_SETCURSOR on re-entry); SDL owns the pointer here */
        printf("[cursor] re-entry: kept (SDL shows the pointer the game set)\n");
        return;
    }
    if (st->move) {
        int gx = st->x, gy = st->y, gw = 800, gh = 600, bpp;
        hle_dd_mode(&gw, &gh, &bpp);
        if (st->win) {                                    /* a window place -> the game pixel under it */
            int cw, ch, pic[4];
            client_and_picture(&cw, &ch, pic);
            int cx = place(st->xs, 0, cw, ch, pic), cy = place(st->ys, 1, cw, ch, pic);
            gx = pic[2] > pic[0] ? (int)((long long)(cx - pic[0]) * gw / (pic[2] - pic[0])) : cx;
            gy = pic[3] > pic[1] ? (int)((long long)(cy - pic[1]) * gh / (pic[3] - pic[1])) : cy;
            printf("[keys] window %d,%d -> game %d,%d\n", cx, cy, gx, gy);
        }
        if (gw > 0 && gh > 0) {                           /* the cursor stays on the screen */
            if (gx < 0) gx = 0; else if (gx >= gw) gx = gw - 1;
            if (gy < 0) gy = 0; else if (gy >= gh) gy = gh - 1;
        }
        st->x = gx, st->y = gy;
        mouse(0x0200u, gx, gy);                           /* WM_MOUSEMOVE (MK_LBUTTON while a drag holds it) */
        if (st->click == 3) {                             /* ldown: held until lup */
            usleep(100000);
            mouse(0x0201u, gx, gy);
        } else if (st->click == 4) {
            usleep(100000);
            mouse(0x0202u, gx, gy);
        } else if (st->click) {
            int r = st->click == 2;
            usleep(100000);
            mouse(r ? 0x0204u : 0x0201u, gx, gy);         /* WM_RBUTTONDOWN / WM_LBUTTONDOWN */
            usleep(150000);
            mouse(r ? 0x0205u : 0x0202u, gx, gy);
        }
        printf("[keys] %.1fs %s %d,%d\n", st->at / 1000.0, st->click == 2 ? "rclick" : st->click == 3 ? "ldown" :
               st->click == 4 ? "lup" : st->click ? "click" : "move", gx, gy);
        return;
    }
    /* Alt+, Ctrl+, Shift+: the modifier is down around the key, as GetKeyState
     * reports it; with Alt down win32hle sends WM_SYSKEYDOWN, as Windows does. */
    int mod = st->alt == 1 ? 0x12 : st->alt == 2 ? 0x11 : st->alt == 3 ? 0x10 : 0;   /* VK_MENU, VK_CONTROL, VK_SHIFT */
    if (mod) { key(1, mod); usleep(50000); }
    key(1, st->vk);                                       /* TranslateMessage makes the WM_CHAR */
    usleep(150000);
    key(0, st->vk);
    if (mod) { usleep(50000); key(0, mod); }
    printf("[keys] %.1fs %skey %02X\n", st->at / 1000.0,
           mod == 0x12 ? "alt+" : mod == 0x11 ? "ctrl+" : mod ? "shift+" : "", st->vk);
}

static void* keys_thread(void* unused) {
    (void)unused;
    uint32_t t0 = hle_ticks_ms();
    for (int i = 0; i < g_nsteps; i++) {
        step_t* st = &g_steps[i];
        if (st->on[0]) {
            wait_for_file(st->on);
            t0 = hle_ticks_ms();
            printf("[keys] %s opened: times count from here\n", st->on);
            continue;
        }
        for (int j = i + 1; j < g_nsteps; j++)            /* the next wait listens from now */
            if (g_steps[j].on[0]) {
                pthread_mutex_lock(&g_seen_lock);
                if (!g_wait_file[0] && !g_file_seen) snprintf(g_wait_file, sizeof g_wait_file, "%s", g_steps[j].on);
                pthread_mutex_unlock(&g_seen_lock);
                break;
            }
        int32_t wait = (int32_t)(t0 + (uint32_t)st->at - hle_ticks_ms());
        if (wait > 0) usleep((useconds_t)wait * 1000u);
        key_run(st);
    }
    return NULL;
}

/* --keys-live FILE: steps appended to FILE while the game runs ("0:what", a
 * line or a comma apart) run as they are read, so a controller can look at a
 * shot and answer it (tools/soak.py --mode fight). */
static void* keys_live_thread(void* unused) {
    (void)unused;
    long pos = 0;
    char line[512];
    size_t n = 0;
    for (;;) {
        FILE* f = fopen(g_live_file, "rb");
        if (f) {
            int c;
            fseek(f, pos, SEEK_SET);
            while ((c = fgetc(f)) != EOF) {
                pos++;
                if (c == '\n' || c == '\r' || c == ',') {
                    step_t st;
                    line[n] = 0;
                    if (n && key_parse(line, &st, 0) && !st.on[0]) {
                        st.at = hle_ticks_ms() - g_keys_t0;
                        key_run(&st);
                    }
                    n = 0;
                } else if (n < sizeof line - 1) line[n++] = (char)c;
            }
            fclose(f);
        }
        usleep(200000);
    }
    return NULL;
}

/* ---- presents: the count the Windows host's softdd prints -------------------
 * A change to the primary (a Blt, an Unlock, a Flip) is what a present is on
 * Windows, where the primary is the screen. tools/conformance.py waits for
 * "frame 100 presented". */
static void (*g_prev_primary_hook)(void);
volatile long swgb_frames;
static void on_primary(void) {
    long n = __sync_add_and_fetch(&swgb_frames, 1);
    framerate_on_present();
    if (n == 1 || n == 10 || n == 100 || n % 1000 == 0) fprintf(stderr, "[host] frame %ld presented\n", n);
    if (g_prev_primary_hook) g_prev_primary_hook();
}

/* ---- SWGB_MSG_TRACE=1: the focus, activation, key and show messages the
 * windows are given (a debugging aid for input and focus). */
static void msg_trace(uint32_t h, uint32_t m, uint32_t w, uint32_t l) {
    static const struct { uint32_t m; const char* n; } k[] = {
        { 0x0006, "WM_ACTIVATE" }, { 0x0007, "WM_SETFOCUS" }, { 0x0008, "WM_KILLFOCUS" }, { 0x0018, "WM_SHOWWINDOW" },
        { 0x001C, "WM_ACTIVATEAPP" }, { 0x0086, "WM_NCACTIVATE" }, { 0x0100, "WM_KEYDOWN" }, { 0x0101, "WM_KEYUP" },
        { 0x0102, "WM_CHAR" }, { 0x0104, "WM_SYSKEYDOWN" }, { 0x0111, "WM_COMMAND" }, { 0x0087, "WM_GETDLGCODE" },
        { 0x0046, "WM_WINDOWPOSCHANGING" }, { 0x0047, "WM_WINDOWPOSCHANGED" }, { 0x000C, "WM_SETTEXT" }, { 0x000E, "WM_GETTEXTLENGTH" },
        { 0x000D, "WM_GETTEXT" }, { 0x0010, "WM_CLOSE" }, { 0x0002, "WM_DESTROY" }, { 0x000A, "WM_ENABLE" }, { 0x000F, "WM_PAINT" }, { 0x0014, "WM_ERASEBKGND" } };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (k[i].m == m) {
            const char* cls = hle_window_class(h);
            fprintf(stderr, "[msg] %s %08X (%s) w=%08X l=%08X\n", k[i].n, h, cls ? cls : "?", w, l);
            return;
        }
}

/* ---- faults and the watchdog ------------------------------------------------- */
static void report(const char* why) {
    fprintf(stderr, "\n=== %s ===\n  in lifted sub_%08X, last import %s\n", why, g_cur_func,
            g_cur_import ? g_cur_import : "(none)");
    fprintf(stderr, "  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
    fprintf(stderr, "last indirect calls (newest first):\n");
    for (int i = 1; i <= 12 && i <= (int)g_icall_trace_idx; i++) {
        uint32_t k = (g_icall_trace_idx - i) & (ICALL_TRACE_SIZE - 1);
        const char* nm = hle_name(g_icall_trace[k]);
        fprintf(stderr, "  0x%08X  from 0x%08X  %s\n", g_icall_trace[k], g_icall_from[k], nm ? nm : "");
    }
}

static void on_fault(int sig, siginfo_t* si, void* uc) {
    static volatile int once;
    (void)uc;
    if (__sync_lock_test_and_set(&once, 1)) _exit(3);
    char why[96];
    snprintf(why, sizeof why, "fault: signal %d, address %p", sig, si->si_addr);
    report(why);
    record_close();
    fflush(stderr);
    _exit(3);
}

static void* watchdog(void* unused) {
    (void)unused;
    sleep(g_watchdog_s);
    fprintf(stderr, "\n[watchdog] %u s: in sub_%08X, last import %s, %u indirect calls, %ld frames\n",
            g_watchdog_s, g_cur_func, g_cur_import ? g_cur_import : "(none)", g_icall_count, swgb_frames);
    fflush(stdout);
    hle_exit(4);
    return NULL;
}

static void spawn(void* (*fn)(void*)) {
    pthread_t t;
    pthread_create(&t, NULL, fn, NULL);
    pthread_detach(t);
}

/* ---- startup ------------------------------------------------------------------ */
static uint32_t exe_stamp(const char* path) {
    uint8_t head[4096] = { 0 };
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(head, 1, sizeof head, f);
    fclose(f);
    uint32_t nt = n >= 0x40 ? *(const uint32_t*)(head + 0x3C) : 0;
    return nt && nt < sizeof head - 12 ? *(const uint32_t*)(head + nt + 8) : 0;
}

/* An absolute path for a file named on the command line: the run changes
 * into the game folder before it writes anything. */
static void absolute(const char* p, char* out, size_t n) {
    char cwd[PATH_MAX];
    if (p[0] == '/') snprintf(out, n, "%s", p);
    else snprintf(out, n, "%s/%s", getcwd(cwd, sizeof cwd) ? cwd : ".", p);
}

/* The play disc (disc 2), mounted or copied: a folder with Game/Avi in it,
 * from --cd DIR or $SWGB_CD. It becomes drive D:, which the game finds as a
 * CD-ROM (shims.c) through CDPath, the letter it checks after its first run.
 * Without --cd, try the Steam Saga install (parent of Game/). Without a disc
 * the game's own “insert a game CD” panel blocks Single Player. */
/* dir/rel exists, each name matched without regard to case (a disc's
 * names are upper case, a copy's often not). */
static int ci_exists(const char* dir, const char* rel) {
    char path[PATH_MAX], name[256];
    snprintf(path, sizeof path, "%s", dir);
    while (*rel) {
        size_t k = strcspn(rel, "/");
        snprintf(name, sizeof name, "%.*s", (int)k, rel);
        rel += k + (rel[k] == '/');
        DIR* d = opendir(path);
        struct dirent* e;
        int found = 0;
        while (d && (e = readdir(d)))
            if (!strcasecmp(e->d_name, name)) {
                size_t l = strlen(path);
                snprintf(path + l, sizeof path - l, "/%s", e->d_name);
                found = 1;
                break;
            }
        if (d) closedir(d);
        if (!found) return 0;
    }
    return 1;
}

static int cd_try(const char* dir, char* full, size_t n) {
    char probe[PATH_MAX + 16];
    struct stat st;
    if (!dir || !*dir || !realpath(dir, full)) return 0;
    if (!ci_exists(full, CD_PROBE)) return 0;
#ifndef SWGB_CC
    snprintf(probe, sizeof probe, "%s/Game/Avi", full);
    if (stat(probe, &st) != 0) mkdir(probe, 0755);       /* Steam keeps .avi in Game/; the check wants Avi/ */
#else
    (void)probe, (void)st;
#endif
    return 1;
}
static void cd_pick(const char* dir) {
    char full[PATH_MAX], home_buf[PATH_MAX], local_buf[PATH_MAX];
    const char* home = getenv("HOME");
    if (!dir || !*dir) dir = getenv("SWGB_CD");
    if (home && *home) {
        snprintf(home_buf, sizeof home_buf,
                 "%s/.steam/steam/steamapps/common/STAR WARS - Galactic Battlegrounds Saga", home);
        snprintf(local_buf, sizeof local_buf,
                 "%s/.local/share/Steam/steamapps/common/STAR WARS - Galactic Battlegrounds Saga", home);
    } else {
        home_buf[0] = local_buf[0] = 0;
    }
    if ((!dir || !*dir || !cd_try(dir, full, sizeof full)) &&
        !(home_buf[0] && cd_try(home_buf, full, sizeof full)) &&
        !(local_buf[0] && cd_try(local_buf, full, sizeof full))) {
        printf("  no play disc: mount " CD_NAME " and pass --cd DIR (or set SWGB_CD)\n");
        printf("  without it, Single Player stops on \"insert a game CD\"\n");
        return;
    }
    swgb_cd_letter = 'D';
    hle_set_drive('D', full);
    hle_reg_set_string(0x80000002u, REG_GAME, "CDPath", "D:\\");   /* HKEY_LOCAL_MACHINE */
    printf("  play disc: D: = %s\n", full);
}

/* The host's own indirect-call targets: --res (widescreen.c), then --smooth (smooth.c). */
static recomp_func_t lookup_hooks(uint32_t va) {
    recomp_func_t f = widescreen_w ? widescreen_lookup(va) : NULL;
    return f ? f : smooth_lookup(va);
}

static void usage(void) {
    printf("usage: swgbrecomp [--run] [--headless] [--record out.mp4] [--frames N] [--sound]\n"
           "                  [--shot S file.bmp] [--keys SCRIPT|@FILE] [--keys-live FILE] [--watchdog S] [--files] [--multi]\n"
           "                  [--exe work/" GUEST_EXE "] [--game game] [--cd DIR] [--films DIR]\n"
           "                  [--args \"<game switches>\"] [--fullscreen] [--scale sharp|smooth|crt|nearest|integer]\n"
           "                  [--res WxH|desktop] [--hud-sides fill|black] [--fps N|off] [--fps-log] [--smooth]\n"
           "                  [--mp-port N] [--mp-join HOST[,HOST]] [--mp-log] [--music off|DIR]\n"
           "                  [--profile FILE] [--profile-window A:B] [--profile-hz N]\n"
           "                  [--trace]\n");
}

int main(int argc, char** argv) {
    const char* exe = "work/" GUEST_EXE;
    const char* game = "game";
    const char* cd = NULL;
    const char* scale = NULL;
    const char* res = NULL;
    char guest_args[512] = "";
    int run = 0, sound = 0, headless = 0, fullscreen = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--run")) run = 1;
        else if (!strcmp(argv[i], "--sound")) sound = 1;
        else if (!strcmp(argv[i], "--headless")) headless = 1;
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = argv[++i];
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) {
            static char rec[PATH_MAX];
            absolute(argv[++i], rec, sizeof rec);
            g_record = rec;
        }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) g_record_frames = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--shot") && i + 2 < argc && g_shots < 16) {
            g_shot_s[g_shots] = (unsigned)strtoul(argv[++i], NULL, 0);
            absolute(argv[++i], g_shot_file[g_shots++], PATH_MAX);
        }
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc)    /* shot= paths come back absolute */
            g_nsteps += keys_parse_all(argv[++i], g_steps + g_nsteps, MAX_STEPS - g_nsteps);
        else if (!strcmp(argv[i], "--keys-live") && i + 1 < argc) absolute(argv[++i], g_live_file, sizeof g_live_file);
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) g_watchdog_s = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--files")) swgb_log_files = 1;
        else if (!strcmp(argv[i], "--multi")) {}           /* named objects are this process's own here */
        else if (!strcmp(argv[i], "--exe") && i + 1 < argc) exe = argv[++i];
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--cd") && i + 1 < argc) cd = argv[++i];
        else if (!strcmp(argv[i], "--args") && i + 1 < argc) snprintf(guest_args, sizeof guest_args, " %s", argv[++i]);
        else if (!strcmp(argv[i], "--trace")) win32hle_trace = 1;
        else if (!strcmp(argv[i], "--smooth")) smooth_on = 1;
        else if (framerate_arg(argc, argv, i)) i += framerate_arg(argc, argv, i) - 1;
        else if (dplay_arg(argc, argv, i)) i += dplay_arg(argc, argv, i) - 1;
        else if (cdaudio_arg(argc, argv, i)) i += cdaudio_arg(argc, argv, i) - 1;
        else if (profile_arg(argc, argv, i)) i += profile_arg(argc, argv, i) - 1;
        else if (!strcmp(argv[i], "--films") && i + 1 < argc) {
            static char films[PATH_MAX];
            if (!realpath(argv[++i], films)) { fprintf(stderr, "--films: no folder %s\n", argv[i]); return 1; }
            swgb_films_dir = films;
        }
        else if (!strcmp(argv[i], "--res") && i + 1 < argc) {   /* desktop: once the window is open */
            res = argv[++i];
            if (strcasecmp(res, "desktop") && !widescreen_parse(res)) return 1;
        }
        else if (!strcmp(argv[i], "--hud-sides") && i + 1 < argc) { if (!widescreen_sides_parse(argv[++i])) return 1; }
        else { usage(); return !strcmp(argv[i], "--help") || !strcmp(argv[i], "-h") ? 0 : 1; }
    }
    swgb_test_run = headless || g_nsteps || g_live_file[0] || g_watchdog_s;
    g_headless = headless;

    char game_full[PATH_MAX], exe_full[PATH_MAX];
    if (!realpath(game, game_full)) { fprintf(stderr, "no game folder at %s\n", game); return 1; }
    if (!realpath(exe, exe_full)) {                       /* no work/ copy: the game folder's own */
        snprintf(exe_full, sizeof exe_full, "%s/" GUEST_EXE, game_full);
        exe = exe_full;
    }
    uint32_t stamp = exe_stamp(exe_full);
    if (stamp != EXE_STAMP) {
        fprintf(stderr, "%s is not the build this host was lifted from (PE timestamp 0x%08X, wanted 0x%08X)\n",
                exe_full, stamp, EXE_STAMP);
        return 1;
    }
    hle_set_drive('C', game_full);                        /* the game's folder is C:\ */
    temp_make();

    signal(SIGPIPE, SIG_IGN);                             /* a recorder whose ffmpeg went away gets an error */
    {   /* faults report on their own stack: a guest stack overflow is one */
        static uint8_t alt[1 << 16];
        stack_t ss = { .ss_sp = alt, .ss_size = sizeof alt };
        sigaltstack(&ss, NULL);
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_fault;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
        sigaction(SIGFPE, &sa, NULL);
        sigaction(SIGILL, &sa, NULL);
    }

    framerate_register();                                 /* first: the game's own answers win */
    dplay_init();                                         /* DirectPlay: the host's own (dplay.c) */
    swgb_movie_register();                                /* the films (movie.c) */
    swgb_register_shims();
    recomp_host_init();
    coverage_init();                                      /* an unresolved indirect call stops the run there (exit 5) */
#ifdef __APPLE__
    printf("Galactic Battlegrounds recomp host (native macOS)\n  lifted functions in dispatch: %u\n", recomp_dispatch_count);
#else
    printf("Galactic Battlegrounds recomp host (native Linux)\n  lifted functions in dispatch: %u\n", recomp_dispatch_count);
#endif
    cd_pick(cd);

    pe_image img;
    if (recomp_pe_map(exe_full, &img) != 0 || img.base != IMAGE_BASE) {
        fprintf(stderr, "cannot map %s at 0x%08X\n", exe_full, IMAGE_BASE);
        return 1;
    }
    hle_module_add("C:\\" GUEST_EXE, img.base, img.resource_rva, img.resource_size, 1);
    coverage_image_end = img.base + img.span;
    int unresolved = recomp_pe_bind(&img, hle_resolve_or_stub);
    printf("  mapped %s: 0x%08X-0x%08X, %d imports not implemented yet\n", exe, img.base, img.base + img.span, unresolved);
    char cmdline[600];
    snprintf(cmdline, sizeof cmdline, "\"C:\\" GUEST_EXE "\"%s", guest_args);
    hle_set_command_line(cmdline);
    printf("  guest command line %s\n", cmdline);
    if (!run) {
        printf("\n(dry run: image mapped and bound; --run enters 0x%08X)\n", swgb_entry_va);
        return 0;
    }
    if (chdir(game_full) != 0) { fprintf(stderr, "cannot enter %s\n", game_full); return 1; }

    /* Test runs are silent unless --sound, as on Windows; headless, SDL's
     * dummy drivers keep the run off the screen and the speakers. */
    if (!swgb_test_run) sound = 1;
    if (!sound) {
        hle_dsound_set_master(0.0f);
        if (headless) setenv("SDL_AUDIODRIVER", "dummy", 0);
    }
    printf("  audio: %s\n", sound ? "on" : "muted (a test run; --sound to hear it)");
    if (scale && !hle_screen_scale(scale)) fprintf(stderr, "[host] no scaling \"%s\": sharp, smooth, crt, nearest or integer\n", scale);
    swgb_fields_init();                                   /* EDIT fields into the primary (New Player, …) */
    if (hle_screen_open(GAME_TITLE, fullscreen, headless) != 0) return 1;
    if (res && !strcasecmp(res, "desktop") && !widescreen_parse(res)) return 1;
    framerate_start(headless);
    if (widescreen_w) printf("  --res: the 1280 x 1024 screen size is %dx%d\n", widescreen_w, widescreen_h);
    smooth_init();                                        /* --smooth: objects drawn between steps (smooth.c) */
    if (widescreen_w || smooth_hooked) hle_lookup_manual_hook = lookup_hooks;
    hle_on_exit = on_exit_close;
    if (getenv("SWGB_MSG_TRACE")) hle_proc_hook = msg_trace;
    g_prev_primary_hook = hle_dd_primary_hook;
    hle_dd_primary_hook = on_primary;
    if (g_record) spawn(record_thread);
    if (g_shots) spawn(shot_thread);
    g_keys_t0 = hle_ticks_ms();
    if (g_nsteps) spawn(keys_thread);
    if (g_live_file[0]) spawn(keys_live_thread);
    if (g_watchdog_s) spawn(watchdog);
    profile_start();                                      /* --profile: samples this thread */
    printf("  entering 0x%08X\n\n", swgb_entry_va);
    uint32_t rc = hle_call_guest(swgb_entry_va, 0, NULL);
    printf("\nentry returned eax=%08X\n", rc);
    hle_exit((int)rc);
    return 0;
}
