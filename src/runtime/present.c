/*
 * present: how softdd's frame reaches the window.
 *
 * The game draws a fixed-size frame (800x600 and up, 4:3). The window can be
 * any shape, so the picture is fitted into the client area with its aspect
 * kept and black bars around it: present_layout is the one rectangle that
 * both drawing and the mouse go through, so a click lands where it is drawn.
 *
 * Filters (the modes pcrecomp's win32hle/screen.c offers for SDL2, done on
 * the CPU here, so GDI is all it needs):
 *   sharp    integer-ish prescale look: nearest inside a texel, a one-pixel
 *            bilinear blend at its edges (the default: no shimmer, no blur)
 *   smooth   plain bilinear
 *   nearest  nearest neighbour at any size (uneven pixels at odd scales)
 *   integer  nearest, in whole multiples of the frame only (bars around)
 *
 * Fullscreen is the same window made borderless and sized to its monitor:
 * the desktop's display mode is never changed.
 *
 * Pacing: the game presents on every Unlock/ReleaseDC of a primary without a
 * back buffer, hundreds of times a second in the menus. A frame is shown when
 * a refresh interval has passed; otherwise it is remembered and a one-shot
 * timer shows the latest one, so the last frame before the game goes idle is
 * never lost.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "present.h"
#include "scaler.h"
#include "present_d3d.h"

int present_filter = PRES_SHARP;
int present_fullscreen_start;
int present_refresh;
int present_log;
int present_noclip;

int present_headless_paint(void) {
    static int v = -1;
    char b[8];
    if (v < 0) v = GetEnvironmentVariableA("SWGB_PAINT_HEADLESS", b, sizeof b) > 0;
    return v;
}

static const char* const k_names[PRES_MODES] = { "sharp", "smooth", "nearest", "integer" };

int present_filter_by_name(const char* n) {
    for (int i = 0; i < PRES_MODES; i++)
        if (!_stricmp(n, k_names[i])) return i;
    return -1;
}
const char* present_filter_name(int m) { return m >= 0 && m < PRES_MODES ? k_names[m] : "?"; }

/* ------------------------------------------------------------- layout */

void present_layout(int mode, int cw, int ch, int sw, int sh, RECT* r) {
    int dw, dh;
    if (cw <= 0 || ch <= 0 || sw <= 0 || sh <= 0) { RECT z = { 0, 0, cw > 0 ? cw : 0, ch > 0 ? ch : 0 }; *r = z; return; }
    int whole = 0;
    if (mode == PRES_INTEGER) {
        int k = cw / sw < ch / sh ? cw / sw : ch / sh;
        if (k >= 1) { dw = sw * k; dh = sh * k; whole = 1; }
    }
    if (!whole) {
        if ((int64_t)cw * sh <= (int64_t)ch * sw) { dw = cw; dh = (int)((int64_t)cw * sh / sw); }
        else { dh = ch; dw = (int)((int64_t)ch * sw / sh); }
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
    }
    r->left = (cw - dw) / 2;
    r->top = (ch - dh) / 2;
    r->right = r->left + dw;
    r->bottom = r->top + dh;
}

void present_to_game(HWND wnd, int gw, int gh, POINT* p) {
    RECT rc, r;
    if (!wnd || gw <= 0 || gh <= 0 || !GetClientRect(wnd, &rc) || !rc.right || !rc.bottom) return;
    present_layout(present_filter, rc.right, rc.bottom, gw, gh, &r);
    int rw = r.right - r.left, rh = r.bottom - r.top;
    int x = p->x < r.left ? r.left : p->x >= r.right ? r.right - 1 : p->x;
    int y = p->y < r.top ? r.top : p->y >= r.bottom ? r.bottom - 1 : p->y;
    /* through the pixel's centre: the last pixel of a smaller picture still reaches the last game pixel */
    p->x = (int)((int64_t)(2 * (x - r.left) + 1) * gw / (2 * (int64_t)rw));
    p->y = (int)((int64_t)(2 * (y - r.top) + 1) * gh / (2 * (int64_t)rh));
}

void present_to_client(HWND wnd, int gw, int gh, POINT* p) {
    RECT rc, r;
    if (!wnd || gw <= 0 || gh <= 0 || !GetClientRect(wnd, &rc) || !rc.right || !rc.bottom) return;
    present_layout(present_filter, rc.right, rc.bottom, gw, gh, &r);
    int rw = r.right - r.left, rh = r.bottom - r.top;
    int cx = r.left + (int)((int64_t)(2 * p->x + 1) * rw / (2 * (int64_t)gw));
    int cy = r.top + (int)((int64_t)(2 * p->y + 1) * rh / (2 * (int64_t)gh));
    /* the pixel that maps back to the game pixel asked for (a picture smaller than the frame cannot reach all) */
    static const int tries[] = { 0, -1, 1, -2, 2 };
    for (int i = 0; i < 5; i++) {
        POINT t = { cx + tries[i], 0 };
        present_to_game(wnd, gw, gh, &t);
        if (t.x == p->x) { cx += tries[i]; break; }
    }
    for (int i = 0; i < 5; i++) {
        POINT t = { 0, cy + tries[i] };
        present_to_game(wnd, gw, gh, &t);
        if (t.y == p->y) { cy += tries[i]; break; }
    }
    p->x = cx;
    p->y = cy;
}

/* ------------------------------------------------------------- scaling */

/* scaler.c: the frame -> the picture, only the rows that changed. One state
 * for the window and one for wshot (a shot must not use up the window's
 * "changed"). */
static scaler_t g_sc_win, g_sc_shot;

static BITMAPINFO* bmi32(int w, int h) {
    static BITMAPINFO b;
    memset(&b, 0, sizeof b);
    b.bmiHeader.biSize = sizeof b.bmiHeader;
    b.bmiHeader.biWidth = w;
    b.bmiHeader.biHeight = -h;
    b.bmiHeader.biPlanes = 1;
    b.bmiHeader.biBitCount = 32;
    return &b;
}

/* ------------------------------------------------------------- painting */

static RECT g_bars_client, g_bars_rect;   /* the layout the bars were last drawn for */
static int  g_bars_dirty = 1;
static int  g_full = 1;          /* draw the whole picture next time (resized, a new mode...) */
static DWORD g_full_at;          /* and once a second anyway, in case the window lost it */

static void paint_bars(HDC dc, const RECT* client, const RECT* pic) {
    if (!g_bars_dirty && !memcmp(&g_bars_client, client, sizeof *client) && !memcmp(&g_bars_rect, pic, sizeof *pic)) return;
    HBRUSH k = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RECT a = { client->left, client->top, client->right, pic->top };
    RECT b = { client->left, pic->bottom, client->right, client->bottom };
    RECT c = { client->left, pic->top, pic->left, pic->bottom };
    RECT d = { pic->right, pic->top, client->right, pic->bottom };
    if (a.bottom > a.top) FillRect(dc, &a, k);
    if (b.bottom > b.top) FillRect(dc, &b, k);
    if (c.right > c.left) FillRect(dc, &c, k);
    if (d.right > d.left) FillRect(dc, &d, k);
    g_bars_client = *client;
    g_bars_rect = *pic;
    g_bars_dirty = 0;
}

/* Only what changed since the last frame is drawn: the window keeps the rest
 * (DWM keeps every window's picture; a resize or a new mode draws it all). */
void present_paint(HWND wnd, const uint8_t* bits, int w, int h, int pitch, int bpp, const PALETTEENTRY* pal) {
    RECT rc, r;
    int y0, y1;
    if (!wnd || !GetClientRect(wnd, &rc) || rc.right <= 0 || rc.bottom <= 0) return;
    present_layout(present_filter, rc.right, rc.bottom, w, h, &r);
    int dw = r.right - r.left, dh = r.bottom - r.top;
    int gdi = present_filter == PRES_NEAREST || present_filter == PRES_INTEGER;   /* GDI scales these: cheap at any size */
    int bars = g_bars_dirty || memcmp(&g_bars_client, &rc, sizeof rc) || memcmp(&g_bars_rect, &r, sizeof r);
    int full = g_full || bars || GetTickCount() - g_full_at >= 1000;
    if (present_d3d_ready(wnd)) {   /* present_d3d.c: the GPU scales; only changed rows are uploaded */
        if (!scaler_run(&g_sc_win, bits, w, h, pitch, bpp, pal, w, h, present_filter, full, &y0, &y1)) return;
        if (y0 >= y1 && !bars && !full) return;   /* the same picture */
        if (present_d3d_draw(wnd, scaler_picture(&g_sc_win), w, h, y0, y1, rc.right, rc.bottom, &r, present_filter, full)) {
            if (full) { g_full = 0; g_full_at = GetTickCount(); }
            g_bars_client = rc; g_bars_rect = r; g_bars_dirty = 0;
            return;
        }
        g_full = g_bars_dirty = full = bars = 1;   /* GDI from here: all of it */
    }
    if (!scaler_run(&g_sc_win, bits, w, h, pitch, bpp, pal, gdi ? w : dw, gdi ? h : dh, present_filter, full, &y0, &y1))
        return;
    if (full) { g_full = 0; g_full_at = GetTickCount(); }
    if (y0 >= y1 && !bars) return;   /* the same picture */
    HDC dc = GetDC(wnd);
    paint_bars(dc, &rc, &r);
    const uint32_t* pic = scaler_picture(&g_sc_win);
    if (y0 < y1 && gdi) {
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, r.left, r.top, dw, dh, 0, 0, w, h, pic, bmi32(w, h), DIB_RGB_COLORS, SRCCOPY);
    } else if (y0 < y1)
        SetDIBitsToDevice(dc, r.left, r.top + y0, dw, y1 - y0, 0, 0, 0, y1 - y0, pic + (size_t)y0 * dw,
                          bmi32(dw, y1 - y0), DIB_RGB_COLORS);
    ReleaseDC(wnd, dc);
}

/* ------------------------------------------------------------- wshot */

static volatile LONG g_wshot_due;
static char g_wshot_path[MAX_PATH];

void present_wshot_request(const char* path) {
    lstrcpynA(g_wshot_path, path, MAX_PATH);
    InterlockedExchange(&g_wshot_due, 1);
}
int present_wshot_pending(void) { return g_wshot_due != 0; }

void present_wshot_write(HWND wnd, const uint8_t* bits, int w, int h, int pitch, int bpp, const PALETTEENTRY* pal) {
    RECT rc, r;
    InterlockedExchange(&g_wshot_due, 0);
    int y0, y1;
    if (!wnd || !GetClientRect(wnd, &rc) || rc.right <= 0 || rc.bottom <= 0) return;
    present_layout(present_filter, rc.right, rc.bottom, w, h, &r);
    int dw = r.right - r.left, dh = r.bottom - r.top, cw = rc.right, ch = rc.bottom;
    /* the same scaler for every filter: the shot is what the code computes */
    if (!scaler_run(&g_sc_shot, bits, w, h, pitch, bpp, pal, dw, dh, present_filter, 1, &y0, &y1)) return;
    const uint32_t* g_img = scaler_picture(&g_sc_shot);
    uint32_t* win = (uint32_t*)calloc((size_t)cw * ch, 4);
    FILE* f = fopen(g_wshot_path, "wb");
    if (!win || !f) { free(win); if (f) fclose(f); return; }
    for (int y = 0; y < dh; y++) memcpy(win + (size_t)(r.top + y) * cw + r.left, g_img + (size_t)y * dw, (size_t)dw * 4);
    BITMAPFILEHEADER fh = { 0x4D42, (DWORD)(sizeof fh + sizeof(BITMAPINFOHEADER) + (size_t)cw * ch * 4), 0, 0,
                            sizeof fh + sizeof(BITMAPINFOHEADER) };
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&bmi32(cw, ch)->bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
    fwrite(win, 4, (size_t)cw * ch, f);
    fclose(f);
    free(win);
    fprintf(stderr, "[wshot] window %dx%d, picture %d,%d %dx%d (%s) -> %s\n", cw, ch, (int)r.left, (int)r.top, dw, dh,
            present_filter_name(present_filter), g_wshot_path);
}

/* ------------------------------------------------------------- fullscreen */

static int   g_fs;
static LONG  g_fs_style, g_fs_ex;
static RECT  g_fs_rect;
static char  g_title[128];

int present_is_fullscreen(void) { return g_fs; }

static void show_state(HWND wnd) {
    if (!g_title[0]) GetWindowTextA(wnd, g_title, sizeof g_title - 32);
    char t[192];
    _snprintf(t, sizeof t - 1, "%s  [%s]", g_title, present_filter_name(present_filter));
    t[sizeof t - 1] = 0;
    SetWindowTextA(wnd, t);
}

void present_set_fullscreen(HWND wnd, int on) {
    if (!wnd) return;
    if (on < 0) on = !g_fs;
    if (on == g_fs) return;
    g_fs = on;   /* before the moves below: present_fs_keep leaves our own alone (the rect is the same) */
    if (on) {
        MONITORINFO mi = { sizeof mi };
        if (IsZoomed(wnd)) ShowWindow(wnd, SW_RESTORE);
        g_fs_style = GetWindowLongA(wnd, GWL_STYLE);
        g_fs_ex = GetWindowLongA(wnd, GWL_EXSTYLE);
        GetWindowRect(wnd, &g_fs_rect);
        if (!GetMonitorInfoA(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), &mi)) { g_fs = 0; return; }
        /* WS_VISIBLE as it is: a window kept off the screen stays off it */
        SetWindowLongA(wnd, GWL_STYLE, WS_POPUP | (g_fs_style & WS_VISIBLE));
        SetWindowLongA(wnd, GWL_EXSTYLE, 0);
        SetWindowPos(wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
    } else {
        SetWindowLongA(wnd, GWL_STYLE, g_fs_style);
        SetWindowLongA(wnd, GWL_EXSTYLE, g_fs_ex);
        SetWindowPos(wnd, HWND_NOTOPMOST, g_fs_rect.left, g_fs_rect.top, g_fs_rect.right - g_fs_rect.left,
                     g_fs_rect.bottom - g_fs_rect.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
    }
    present_cursor_clip(wnd, on);
    RECT rc;
    GetClientRect(wnd, &rc);
    fprintf(stderr, "[present] %s: client %ldx%ld\n", on ? "fullscreen (borderless)" : "windowed", rc.right, rc.bottom);
    present_invalidate(wnd);
}

/* The cursor cannot leave a fullscreen window for another monitor (the game
 * scrolls the map by the cursor's place at the edge), and is let go whenever
 * the window is not the active one. */
static int g_clip_on;
static RECT g_clip_rect;
int present_clip_get(RECT* out) { if (g_clip_on) *out = g_clip_rect; return g_clip_on; }

void present_cursor_clip(HWND wnd, int active) {
    if (!wnd) return;
    g_clip_on = active && g_fs;
    if (g_clip_on) {
        GetClientRect(wnd, &g_clip_rect);
        MapWindowPoints(wnd, NULL, (POINT*)&g_clip_rect, 2);
    }
    if (present_noclip) return;   /* the window station's cursor is the user's: headless never clips it */
    ClipCursor(g_clip_on ? &g_clip_rect : NULL);
}

void present_fs_keep(HWND wnd, UINT msg, WPARAM w, LPARAM l) {
    if (!g_fs || !wnd) return;
    if (msg == WM_WINDOWPOSCHANGING) {
        WINDOWPOS* wp = (WINDOWPOS*)l;
        MONITORINFO mi = { sizeof mi };
        if ((wp->flags & SWP_NOMOVE) && (wp->flags & SWP_NOSIZE)) return;   /* z-order, show, hide */
        if (!GetMonitorInfoA(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), &mi)) return;
        int x = mi.rcMonitor.left, y = mi.rcMonitor.top, cx = mi.rcMonitor.right - x, cy = mi.rcMonitor.bottom - y;
        if ((!(wp->flags & SWP_NOMOVE) && (wp->x != x || wp->y != y)) || (!(wp->flags & SWP_NOSIZE) && (wp->cx != cx || wp->cy != cy)))
            fprintf(stderr, "[present] fullscreen kept against a move to %d,%d %dx%d\n", wp->x, wp->y, wp->cx, wp->cy);
        wp->flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
        wp->x = x; wp->y = y; wp->cx = cx; wp->cy = cy;
    } else if (msg == WM_STYLECHANGING && (int)w == GWL_STYLE) {
        STYLESTRUCT* ss = (STYLESTRUCT*)l;
        DWORD keep = WS_POPUP | (ss->styleNew & WS_VISIBLE);
        if (ss->styleNew != keep) fprintf(stderr, "[present] fullscreen kept against style %08lX\n", ss->styleNew);
        ss->styleNew = keep;
    }
}

void present_cycle_filter(HWND wnd) {
    present_filter = (present_filter + 1) % PRES_MODES;
    fprintf(stderr, "[present] filter: %s\n", present_filter_name(present_filter));
    if (wnd && !g_fs) show_state(wnd);
    g_bars_dirty = 1;
    present_invalidate(wnd);
}

void present_set_client_size(HWND wnd, int w, int h) {
    RECT r = { 0, 0, w, h };
    if (!wnd || g_fs) return;
    AdjustWindowRectEx(&r, GetWindowLongA(wnd, GWL_STYLE), FALSE, GetWindowLongA(wnd, GWL_EXSTYLE));
    SetWindowPos(wnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    fprintf(stderr, "[present] client size set to %dx%d\n", w, h);
    present_invalidate(wnd);
}

/* ------------------------------------------------------------- pacing */

static int64_t g_qpf, g_next_us, g_interval_us = 16667;   /* g_next_us: when the next frame is due */
static int     g_hz_known, g_dirty, g_armed;

long long (*present_test_clock_us)(void);

static int64_t now_us(void) {
    LARGE_INTEGER t;
    if (present_test_clock_us) return present_test_clock_us();
    if (!g_qpf) { QueryPerformanceFrequency(&t); g_qpf = t.QuadPart; }
    QueryPerformanceCounter(&t);
    /* split: seconds * 1e6 would overflow after days of uptime at a 10 MHz counter */
    return (t.QuadPart / g_qpf) * 1000000 + (t.QuadPart % g_qpf) * 1000000 / g_qpf;
}

static int monitor_hz(HWND wnd) {
    int hz = 60;
    if (present_refresh > 0) hz = present_refresh;
    else {
        MONITORINFOEXA mi;
        DEVMODEA dm;
        memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
        memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
        HMONITOR m = wnd ? MonitorFromWindow(wnd, MONITOR_DEFAULTTOPRIMARY) : NULL;
        if (m && GetMonitorInfoA(m, (MONITORINFO*)&mi) &&
            EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
            hz = (int)dm.dmDisplayFrequency;
    }
    return hz < 5 ? 5 : hz > 2000 ? 2000 : hz;   /* a real display tops out far below; --refresh is for tests */
}

static void learn_hz(HWND wnd) {
    static int shown_hz;
    int hz = monitor_hz(wnd);
    g_interval_us = 1000000 / hz;
    if (hz != shown_hz) fprintf(stderr, "[present] pacing to %d Hz\n", hz);
    shown_hz = hz;
    g_hz_known = GetTickCount() | 1;
}

/* The refresh shown frames are paced to: framerate.c's default frame rate. */
double present_refresh_hz(void) {
    if (!g_hz_known) return monitor_hz(NULL);
    return 1e6 / (double)g_interval_us;
}

int present_due(HWND wnd) {
    if (!g_hz_known || GetTickCount() - (DWORD)g_hz_known > 2000) learn_hz(wnd);   /* moved to another monitor */
    return now_us() >= g_next_us - g_interval_us / 8;   /* a little early is fine: jitter must not skip a refresh */
}

/* Slots, not "an interval since the last one": presents arriving a little faster
 * than the refresh (210 a second at 180 Hz) would then be shown every other one.
 * Each shown frame takes the next slot of a steady beat. */
void present_painted(void) {
    int64_t now = now_us();
    g_next_us += g_interval_us;
    if (g_next_us < now) g_next_us = now + g_interval_us / 2;   /* idle, or far behind: start a new beat */
    g_dirty = 0;
}
int  present_dirty(void) { return g_dirty; }

void present_defer(HWND wnd) {
    g_dirty = 1;
    if (g_armed || !wnd) return;
    int64_t left = g_next_us - now_us();
    UINT ms = left > 0 ? (UINT)((left + 999) / 1000) : 1;
    if (SetTimer(wnd, SOFTDD_PACE, ms, NULL)) g_armed = 1;   /* the 100 ms timer is the fallback */
}

void present_disarm(HWND wnd) {
    if (g_armed && wnd) KillTimer(wnd, SOFTDD_PACE);
    g_armed = 0;
}

static int g_damaged;   /* the window itself changed (not just a frame held back) */
int  present_damaged(void) { int d = g_damaged; g_damaged = 0; return d; }

/* WM_PAINT: the game draws its frames through its own WM_PAINT (UpdateWindow,
 * hundreds a second in game), so a paint message is not damage: DWM keeps
 * the window's picture. It only asks that the current frame be shown again
 * (the 100 ms timer does it if the game draws nothing new). */
void present_repaint(HWND wnd) {
    (void)wnd;
    g_dirty = 1;
}

void present_invalidate(HWND wnd) {
    g_damaged = 1;
    g_full = 1;
    g_bars_dirty = 1;
    g_hz_known = 0;   /* a new monitor, maybe */
    present_disarm(wnd);
    present_defer(wnd);
}

static long g_why[4];
void present_why(int why) { if (why >= 1 && why <= 3) g_why[why]++; }

void present_note(int presented, int shown) {
    static int64_t t0;
    static long np, ns;
    int64_t t = now_us();
    if (!t0) t0 = t;
    np += presented;
    ns += shown;
    if (present_log && t - t0 >= 1000000) {
        fprintf(stderr, "[present] %.1f s: %ld presents, %ld shown (%d Hz, %s); by the timer: %ld held, %ld window damage, %ld text field\n",
                (double)(t - t0) / 1e6, np, ns, (int)(1000000 / g_interval_us), present_filter_name(present_filter),
                g_why[1], g_why[2], g_why[3]);
    }
    if (t - t0 >= 1000000) { t0 = t; np = ns = 0; g_why[1] = g_why[2] = g_why[3] = 0; }
}
