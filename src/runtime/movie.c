/*
 * movie: the game's films (its logos, the intro, the campaigns' films).
 *
 * The game plays them through an MCIWnd (MSVFW32's MCIWndCreateA) over its
 * window, after asking ICInfo whether Indeo 5 (IV50) is installed; without
 * it, it plays none. Windows 10 and 11 ship no Indeo 5, and even with it an
 * MCIWnd paints the screen, which under softdd nothing but the presented
 * frame reaches. So the host makes the window itself: a child of the game
 * window that answers the few MCIWnd messages the game sends (0x005E43F0
 * plays a film, 0x005E4950 stops one, 0x005EB8AF polls it once a second):
 *
 *   MCIWNDM_GETPALETTE  the palette to show the film in (the game installs it)
 *   MCI_PLAY, MCI_STOP, MCI_PAUSE, MCI_RESUME
 *   MCIWNDM_GETMODEA    "playing", "paused" or "stopped": stopped ends the film
 *   MCIWNDM_REALIZE     nothing to do
 *   WM_CLOSE            the game is done with it
 *
 * A child ffmpeg process decodes the video to raw BGRA frames on a pipe
 * (no new build dependency; softdd's --record does the same the other way),
 * and another decodes the sound to 16-bit PCM for waveOut. The sound's
 * position is the clock: each frame is shown when it is due, late frames
 * are dropped. With no sound (no audio device, or a film without any) the
 * clock is the wall clock.
 *
 * The frame reaches the picture as every child window does: softdd's
 * compose() prints the visible children into the frame it presents
 * (WM_PRINT), so the window draws the film there, ordered-dithered into the
 * 8-bit palette in use, as DrawDib did on an 8-bit screen. Each new frame
 * asks softdd for a present. Nothing is drawn on the screen directly, so
 * --headless and --record show the film the same way.
 *
 * Skipping is the game's: a click on the window, or Enter, Space or Esc.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shlwapi.h>

#include "movie.h"
#include "softdd.h"

const char* movie_dir;

#define MCIWNDM_GETMODEA   (WM_USER + 106)
#define MCIWNDM_REALIZE    (WM_USER + 118)
#define MCIWNDM_GETPALETTE (WM_USER + 126)
#define WM_MOVIE_FRAME     (WM_APP + 0x3D1)

#define AUDIO_RATE 44100
#define AUDIO_BUFS 8
#define AUDIO_BUF  (AUDIO_RATE / 10 * 4)   /* 100 ms of 16-bit stereo */

/* ---- FFmpeg -------------------------------------------------------------- */

static char g_ffmpeg[MAX_PATH];

static int ffmpeg_found(void) {
    static int known = -1;
    if (known < 0) {
        char* part;
        known = SearchPathA(NULL, "ffmpeg.exe", NULL, MAX_PATH, g_ffmpeg, &part) != 0;
        if (!known) fprintf(stderr, "[movie] no ffmpeg on PATH: the films are skipped\n");
    }
    return known;
}

/* ffmpeg ARGS with its standard output on a pipe; only that pipe is
 * inherited (not, say, softdd's --record pipe, which would then never see
 * its end). */
static HANDLE spawn(const char* args, HANDLE* proc) {
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd, wr, nul;
    char cmd[2048];
    if (!CreatePipe(&rd, &wr, &sa, 1 << 20)) return NULL;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                      OPEN_EXISTING, 0, NULL);
    _snprintf(cmd, sizeof cmd - 1, "\"%s\" %s", g_ffmpeg, args);
    cmd[sizeof cmd - 1] = 0;

    SIZE_T n = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &n);
    LPPROC_THREAD_ATTRIBUTE_LIST al = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(n);
    HANDLE inherit[2] = { wr, nul };
    InitializeProcThreadAttributeList(al, 1, 0, &n);
    UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, NULL, NULL);
    STARTUPINFOEXA si = { { sizeof si } };
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = wr;
    si.StartupInfo.hStdError = nul;   /* -loglevel error: nothing, unless the film is broken */
    si.lpAttributeList = al;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                             NULL, NULL, &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(al);
    free(al);
    CloseHandle(wr);
    CloseHandle(nul);
    if (!ok) { CloseHandle(rd); return NULL; }
    CloseHandle(pi.hThread);
    *proc = pi.hProcess;
    return rd;
}

static int read_all(HANDLE h, void* buf, DWORD n) {
    DWORD got = 0, k;
    while (got < n) {
        if (!ReadFile(h, (char*)buf + got, n - got, &k, NULL) || !k) return (int)got;
        got += k;
    }
    return (int)got;
}

/* ---- the film's size and rate, from its AVI header ------------------------ */

typedef struct { int w, h; DWORD rate, scale; int has_audio; } avi_info;

static int avi_probe(const char* path, avi_info* ai) {
    uint8_t b[8192];
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(b, 1, sizeof b, f);
    fclose(f);
    if (n < 64 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "AVI ", 4)) return 0;
    memset(ai, 0, sizeof *ai);
    DWORD us = 0;
    for (size_t i = 12; i + 64 <= n; i++) {
        if (!memcmp(b + i, "avih", 4)) {
            us = *(DWORD*)(b + i + 8);
            ai->w = *(int*)(b + i + 8 + 32);
            ai->h = *(int*)(b + i + 8 + 36);
        } else if (!memcmp(b + i, "strh", 4)) {
            if (!memcmp(b + i + 8, "vids", 4) && !ai->rate) {
                ai->scale = *(DWORD*)(b + i + 8 + 20);
                ai->rate = *(DWORD*)(b + i + 8 + 24);
            } else if (!memcmp(b + i + 8, "auds", 4)) ai->has_audio = 1;
        }
    }
    if (!ai->rate || !ai->scale) { ai->rate = 1000000; ai->scale = us ? us : 66667; }
    return ai->w > 0 && ai->h > 0 && ai->w <= 4096 && ai->h <= 4096;
}

/* ---- one film ----------------------------------------------------------- */

typedef struct {
    HWND hwnd;
    char path[MAX_PATH];
    avi_info ai;

    CRITICAL_SECTION cs;          /* the frame and the clock */
    uint32_t* frame;              /* BGRA, ai.w x ai.h; the newest shown */
    int have_frame;
    volatile LONG posted;         /* a WM_MOVIE_FRAME is on its way */

    HANDLE vproc, vpipe, vthread;
    HANDLE aproc, apipe, athread, aready;
    volatile LONG stop;
    int playing, paused, done_video, done_audio;

    /* the clock: the sound's position while it plays, else the wall clock
     * from base_ms at base_tick (less the time paused) */
    HWAVEOUT wo;
    int audio_on;                 /* the sound is the clock */
    double base_ms;
    DWORD base_tick, pause_tick, pause_total;
} Movie;

static double wave_ms(Movie* m) {
    MMTIME t = { TIME_SAMPLES };
    if (waveOutGetPosition(m->wo, &t, sizeof t) != MMSYSERR_NOERROR) return m->base_ms;
    if (t.wType == TIME_BYTES) return t.u.cb / 4 * 1000.0 / AUDIO_RATE;
    if (t.wType == TIME_MS) return t.u.ms;
    return t.u.sample * 1000.0 / AUDIO_RATE;
}

/* In ms from the film's start. */
static double clock_ms(Movie* m) {
    EnterCriticalSection(&m->cs);
    double t;
    if (m->audio_on) t = wave_ms(m);
    else {
        DWORD now = m->paused ? m->pause_tick : GetTickCount();
        t = m->base_ms + (double)(now - m->base_tick - m->pause_total);
    }
    LeaveCriticalSection(&m->cs);
    return t;
}

/* The sound has run out: the wall clock carries on from where it stopped. */
static void clock_to_wall(Movie* m) {
    EnterCriticalSection(&m->cs);
    if (m->audio_on) m->base_ms = wave_ms(m);
    m->audio_on = 0;
    m->base_tick = m->paused ? m->pause_tick : GetTickCount();
    m->pause_total = 0;
    LeaveCriticalSection(&m->cs);
}

static DWORD WINAPI audio_thread(LPVOID arg) {
    Movie* m = (Movie*)arg;
    static WAVEHDR hdr[AUDIO_BUFS];   /* one film at a time */
    static char buf[AUDIO_BUFS][AUDIO_BUF];
    WAVEFORMATEX wf = { WAVE_FORMAT_PCM, 2, AUDIO_RATE, AUDIO_RATE * 4, 4, 16, 0 };
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    HWAVEOUT wo;
    int queued = 0, eof = 0;
    /* m->wo changes only under the lock: movie_pause uses it from the game's thread */
    if (waveOutOpen(&wo, WAVE_MAPPER, &wf, (DWORD_PTR)ev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        fprintf(stderr, "[movie] no audio device: the wall clock times the film\n");
        goto none;
    }
    EnterCriticalSection(&m->cs);
    m->wo = wo;
    LeaveCriticalSection(&m->cs);
    {
        char args[MAX_PATH + 256];
        _snprintf(args, sizeof args - 1, "-hide_banner -loglevel error -nostdin -i \"%s\" -map 0:a:0 -vn "
                  "-f s16le -acodec pcm_s16le -ac 2 -ar %d -", m->path, AUDIO_RATE);
        args[sizeof args - 1] = 0;
        m->apipe = spawn(args, &m->aproc);
    }
    if (!m->apipe) goto none;
    waveOutPause(wo);   /* until the buffers are queued */
    for (int i = 0; i < AUDIO_BUFS && !eof; i++) {
        int n = read_all(m->apipe, buf[i], AUDIO_BUF);
        if (n < AUDIO_BUF) eof = 1;
        if (n <= 0) break;
        hdr[i].lpData = buf[i];
        hdr[i].dwBufferLength = (DWORD)n & ~3u;
        hdr[i].dwFlags = 0;
        waveOutPrepareHeader(wo, &hdr[i], sizeof hdr[i]);
        waveOutWrite(wo, &hdr[i], sizeof hdr[i]);
        queued++;
    }
    if (!queued) goto none;
    EnterCriticalSection(&m->cs);
    m->audio_on = 1;
    if (!m->paused) waveOutRestart(wo);
    LeaveCriticalSection(&m->cs);
    SetEvent(m->aready);
    while (!m->stop && queued) {
        WaitForSingleObject(ev, 50);
        for (int i = 0; i < AUDIO_BUFS; i++) {
            if (!hdr[i].lpData || !(hdr[i].dwFlags & WHDR_DONE)) continue;
            waveOutUnprepareHeader(wo, &hdr[i], sizeof hdr[i]);
            queued--;
            int n = eof || m->stop ? 0 : read_all(m->apipe, buf[i], AUDIO_BUF);
            if (n < AUDIO_BUF) eof = 1;
            if (n <= 0) { hdr[i].lpData = NULL; continue; }
            hdr[i].dwBufferLength = (DWORD)n & ~3u;
            hdr[i].dwFlags = 0;
            waveOutPrepareHeader(wo, &hdr[i], sizeof hdr[i]);
            waveOutWrite(wo, &hdr[i], sizeof hdr[i]);
            queued++;
        }
    }
    clock_to_wall(m);
    EnterCriticalSection(&m->cs);
    m->wo = NULL;
    LeaveCriticalSection(&m->cs);
    waveOutReset(wo);
    for (int i = 0; i < AUDIO_BUFS; i++)
        if (hdr[i].lpData) { waveOutUnprepareHeader(wo, &hdr[i], sizeof hdr[i]); hdr[i].lpData = NULL; }
    waveOutClose(wo);
    m->done_audio = 1;
    CloseHandle(ev);
    return 0;
none:
    EnterCriticalSection(&m->cs);
    wo = m->wo;
    m->wo = NULL;
    LeaveCriticalSection(&m->cs);
    if (wo) waveOutClose(wo);
    m->done_audio = 1;
    SetEvent(m->aready);
    CloseHandle(ev);
    return 0;
}

static DWORD WINAPI video_thread(LPVOID arg) {
    Movie* m = (Movie*)arg;
    size_t bytes = (size_t)m->ai.w * m->ai.h * 4;
    uint32_t* next = (uint32_t*)malloc(bytes);
    double frame_ms = 1000.0 * m->ai.scale / m->ai.rate;
    DWORD last_shown = GetTickCount();
    long shown = 0, dropped = 0;
    WaitForSingleObject(m->aready, 3000);   /* the sound starts the clock */
    EnterCriticalSection(&m->cs);
    if (!m->audio_on) { m->base_ms = 0; m->base_tick = GetTickCount(); m->pause_total = 0; }
    int by_sound = m->audio_on;
    LeaveCriticalSection(&m->cs);
    DWORD t0 = GetTickCount();
    long k;
    for (k = 0; !m->stop; k++) {
        if (read_all(m->vpipe, next, (DWORD)bytes) < (int)bytes) break;
        double due = k * frame_ms, now;
        while (!m->stop && (now = clock_ms(m)) < due) {
            double wait = due - now;
            Sleep(wait > 10 ? 10 : (DWORD)wait + 1);
        }
        if (m->stop) break;
        /* Late by a frame or more: drop it, but show one at least every
         * 100 ms however slow the machine. */
        if (now >= due + frame_ms && GetTickCount() - last_shown < 100) { dropped++; continue; }
        EnterCriticalSection(&m->cs);
        memcpy(m->frame, next, bytes);
        m->have_frame = 1;
        LeaveCriticalSection(&m->cs);
        last_shown = GetTickCount();
        shown++;
        if (!InterlockedExchange(&m->posted, 1)) PostMessageA(m->hwnd, WM_MOVIE_FRAME, 0, 0);
    }
    free(next);
    if (!m->stop && k)   /* the last frame's time against the wall clock: how well the timing held */
        fprintf(stderr, "[movie] %s: %ld frames shown, %ld dropped; the last due at %.2f s, shown at %.2f s"
                " (clock: %s)\n", PathFindFileNameA(m->path), shown, dropped, (k - 1) * frame_ms / 1000,
                (last_shown - t0) / 1000.0, by_sound ? "the sound" : "the wall");
    m->done_video = 1;
    return 0;
}

static void movie_stop(Movie* m) {
    if (!m->playing) return;
    InterlockedExchange(&m->stop, 1);
    if (m->vproc) TerminateProcess(m->vproc, 0);
    if (m->aproc) TerminateProcess(m->aproc, 0);
    HANDLE th[2];
    int n = 0;
    if (m->vthread) th[n++] = m->vthread;
    if (m->athread) th[n++] = m->athread;
    if (n) WaitForMultipleObjects(n, th, TRUE, 5000);
    HANDLE* hs[] = { &m->vthread, &m->athread, &m->vproc, &m->aproc, &m->vpipe, &m->apipe, &m->aready };
    for (int i = 0; i < (int)(sizeof hs / sizeof hs[0]); i++)
        if (*hs[i]) { CloseHandle(*hs[i]); *hs[i] = NULL; }
    m->playing = 0;
    m->done_video = m->done_audio = 1;
}

static void movie_pause(Movie* m, int pause);

static void movie_play(Movie* m) {
    if (m->playing) {   /* MCI_PLAY again: carry on */
        movie_pause(m, 0);
        return;
    }
    char args[MAX_PATH + 256];
    _snprintf(args, sizeof args - 1, "-hide_banner -loglevel error -nostdin -i \"%s\" -map 0:v:0 -an "
              "-fps_mode cfr -f rawvideo -pix_fmt bgra -", m->path);
    args[sizeof args - 1] = 0;
    m->stop = 0;
    m->done_video = m->done_audio = 0;
    m->vpipe = spawn(args, &m->vproc);
    if (!m->vpipe) {
        fprintf(stderr, "[movie] cannot start ffmpeg for %s\n", m->path);
        m->done_video = m->done_audio = 1;
        return;
    }
    m->aready = CreateEventA(NULL, TRUE, FALSE, NULL);
    m->playing = 1;
    if (m->ai.has_audio) m->athread = CreateThread(NULL, 0, audio_thread, m, 0, NULL);
    else { m->done_audio = 1; SetEvent(m->aready); }
    m->vthread = CreateThread(NULL, 0, video_thread, m, 0, NULL);
    fprintf(stderr, "[movie] playing %s (%dx%d, %.2f fps%s)\n", m->path, m->ai.w, m->ai.h,
            (double)m->ai.rate / m->ai.scale, m->ai.has_audio ? ", with sound" : "");
}

static void movie_pause(Movie* m, int pause) {
    EnterCriticalSection(&m->cs);
    if (m->playing && pause != m->paused) {
        if (pause) m->pause_tick = GetTickCount();
        else m->pause_total += GetTickCount() - m->pause_tick;
        m->paused = pause;
        if (m->wo) { if (pause) waveOutPause(m->wo); else waveOutRestart(m->wo); }
    }
    LeaveCriticalSection(&m->cs);
}

static const char* movie_mode(Movie* m) {
    if (m->playing && !(m->done_video && m->done_audio)) return m->paused ? "paused" : "playing";
    return "stopped";
}

/* ---- the picture ---------------------------------------------------------- */

/* The palette the film is shown in: black, a 6x6x6 colour cube, greys,
 * white. The game installs it (and keeps entries 0 and 255 black and white). */
static RGBQUAD g_film_q[256];   /* the same, as a DIB colour table */

static HPALETTE film_palette(void) {
    static HPALETTE pal;
    if (pal) return pal;
    struct { WORD ver, n; PALETTEENTRY e[256]; } lp = { 0x300, 256 };
    for (int i = 0; i < 216; i++) {
        PALETTEENTRY c = { (BYTE)(i / 36 * 51), (BYTE)(i / 6 % 6 * 51), (BYTE)(i % 6 * 51), 0 };
        lp.e[1 + i] = c;
    }
    for (int i = 217; i < 255; i++) {   /* the greys between the cube's */
        BYTE g = (BYTE)((i - 216) * 255 / 39);
        PALETTEENTRY c = { g, g, g, 0 };
        lp.e[i] = c;
    }
    PALETTEENTRY white = { 255, 255, 255, 0 };
    lp.e[255] = white;
    for (int i = 0; i < 256; i++)
        g_film_q[i].rgbRed = lp.e[i].peRed, g_film_q[i].rgbGreen = lp.e[i].peGreen, g_film_q[i].rgbBlue = lp.e[i].peBlue;
    pal = CreatePalette((LOGPALETTE*)&lp);
    return pal;
}

/* RGB555 -> the nearest entry of a palette that is not the film's. */
static uint8_t g_lut[32768];
static RGBQUAD g_lut_pal[256];
static int g_lut_ok;

static void lut_for(const RGBQUAD* q) {
    if (g_lut_ok && !memcmp(q, g_lut_pal, sizeof g_lut_pal)) return;
    memcpy(g_lut_pal, q, sizeof g_lut_pal);
    g_lut_ok = 1;
    for (int c = 0; c < 32768; c++) {
        int r = (c >> 10) * 255 / 31, g = (c >> 5 & 31) * 255 / 31, b = (c & 31) * 255 / 31;
        int best = 0, bd = 1 << 30;
        for (int i = 0; i < 256; i++) {
            int dr = r - q[i].rgbRed, dg = g - q[i].rgbGreen, db = b - q[i].rgbBlue;
            int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
            if (d < bd) bd = d, best = i;
        }
        g_lut[c] = (uint8_t)best;
    }
}

static const int8_t g_bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

/* v (0..255) as one of n + 1 evenly spaced levels, ordered-dithered by t
 * (0..15): flat black and white stay flat. */
static int dither(int v, int n, int t) {
    int l = (v * n * 32 + (2 * t + 1) * 255) / (255 * 32);
    return l > n ? n : l;
}

/* A pixel as an entry of the film palette: near-greys on its 40 greys,
 * the rest on its 6x6x6 cube. */
static uint8_t film_index(int r, int g, int b, int t) {
    int hi = r > g ? (r > b ? r : b) : (g > b ? g : b), lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
    if (hi - lo <= 12) {
        int l = dither((r + g + b) / 3, 39, t);
        return (uint8_t)(l == 0 ? 0 : l == 39 ? 255 : 216 + l);
    }
    return (uint8_t)(1 + dither(r, 5, t) * 36 + dither(g, 5, t) * 6 + dither(b, 5, t));
}

/* The film, scaled to the window, through GDI (which knows which way up the
 * DC's bitmap is). Onto an 8-bit DC (softdd's composed frame in an 8-bit
 * mode) it is ordered-dithered into that DC's own colour table first and
 * drawn 1:1, so GDI's colour matching finds each entry exactly; any other
 * depth takes the 32-bit frame as it is. */
static void movie_draw(Movie* m, HDC dc) {
    RECT cr;
    GetClientRect(m->hwnd, &cr);
    int cw = cr.right, ch = cr.bottom, vw = m->ai.w, vh = m->ai.h;
    if (cw <= 0 || ch <= 0) return;
    BITMAP bm;
    HBITMAP hb = (HBITMAP)GetCurrentObject(dc, OBJ_BITMAP);
    int bpp = hb && GetObject(hb, sizeof bm, &bm) ? bm.bmBitsPixel : GetDeviceCaps(dc, BITSPIXEL);
    EnterCriticalSection(&m->cs);
    if (!m->have_frame) {
        PatBlt(dc, 0, 0, cw, ch, BLACKNESS);
    } else if (bpp != 8) {
        BITMAPINFOHEADER bi = { sizeof bi, vw, -vh, 1, 32, BI_RGB };
        int old = SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, 0, 0, cw, ch, 0, 0, vw, vh, m->frame, (BITMAPINFO*)&bi, DIB_RGB_COLORS, SRCCOPY);
        SetStretchBltMode(dc, old);
    } else {
        static struct { BITMAPINFOHEADER h; RGBQUAD c[256]; } bi;
        static uint8_t* buf;
        static size_t cap;
        int pitch = (cw + 3) & ~3;
        if (cap < (size_t)pitch * ch) { free(buf); cap = (size_t)pitch * ch; buf = (uint8_t*)malloc(cap); }
        memset(bi.c, 0, sizeof bi.c);
        if (!GetDIBColorTable(dc, 0, 256, bi.c)) {   /* not a DIB: the palette selected into dc */
            PALETTEENTRY e[256] = { 0 };
            GetPaletteEntries((HPALETTE)GetCurrentObject(dc, OBJ_PAL), 0, 256, e);
            for (int i = 0; i < 256; i++)
                bi.c[i].rgbRed = e[i].peRed, bi.c[i].rgbGreen = e[i].peGreen, bi.c[i].rgbBlue = e[i].peBlue;
        }
        film_palette();
        int ours = !memcmp(bi.c, g_film_q, sizeof g_film_q);   /* the game installed it */
        if (!ours) lut_for(bi.c);
        for (int y = 0; y < ch; y++) {
            uint8_t* row = buf + (size_t)y * pitch;
            const uint32_t* src = m->frame + (size_t)(y * vh / ch) * vw;
            for (int x = 0; x < cw; x++) {
                uint32_t p = src[x * vw / cw];
                int r = p >> 16 & 255, g = p >> 8 & 255, b = p & 255;
                row[x] = ours ? film_index(r, g, b, g_bayer[y & 3][x & 3])
                              : g_lut[(r >> 3) << 10 | (g >> 3) << 5 | b >> 3];   /* some other palette: nearest */
            }
        }
        BITMAPINFOHEADER h = { sizeof h, cw, -ch, 1, 8, BI_RGB, 0, 0, 0, 256 };
        bi.h = h;
        SetDIBitsToDevice(dc, 0, 0, cw, ch, 0, 0, 0, ch, buf, (BITMAPINFO*)&bi, DIB_RGB_COLORS);
    }
    LeaveCriticalSection(&m->cs);
}

/* ---- the window ----------------------------------------------------------- */

static LRESULT CALLBACK movie_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    Movie* m = (Movie*)GetWindowLongPtrA(h, GWLP_USERDATA);
    if (!m) return DefWindowProcA(h, msg, w, l);
    switch (msg) {
    case MCI_PLAY: movie_play(m); return 0;
    case MCI_STOP: movie_stop(m); return 0;
    case MCI_PAUSE: movie_pause(m, 1); return 0;
    case MCI_RESUME: movie_pause(m, 0); return 0;
    case MCIWNDM_GETMODEA: {
        const char* s = movie_mode(m);
        if (l && w) lstrcpynA((char*)l, s, (int)w);
        return !strcmp(s, "playing") ? MCI_MODE_PLAY : !strcmp(s, "paused") ? MCI_MODE_PAUSE : MCI_MODE_STOP;
    }
    case MCIWNDM_GETPALETTE: return (LRESULT)film_palette();
    case MCIWNDM_REALIZE: return 0;
    case WM_MOVIE_FRAME:
        InterlockedExchange(&m->posted, 0);
        softdd_refresh();
        return 0;
    case WM_PRINT: case WM_PRINTCLIENT: movie_draw(m, (HDC)w); return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        movie_draw(m, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SETFOCUS:   /* the keys that skip (Enter, Space, Esc) are the game window's */
        SetFocus(GetParent(h));
        return 0;
    case WM_DESTROY:
        movie_stop(m);
        SetWindowLongPtrA(h, GWLP_USERDATA, 0);
        DeleteCriticalSection(&m->cs);
        free(m->frame);
        free(m);
        softdd_refresh();   /* the film is gone from the picture */
        return 0;
    }
    return DefWindowProcA(h, msg, w, l);
}

int movie_find(const char* name, char* out, int size) {
    char base[MAX_PATH], p[MAX_PATH];
    if (!movie_dir || !name || _stricmp(PathFindExtensionA(name), ".avi")) return 0;
    lstrcpynA(base, PathFindFileNameA(name), MAX_PATH);
    for (int x = 0; x < 2; x++) {   /* NAME.avi, then xNAME.avi (the Saga's) */
        _snprintf(p, MAX_PATH - 1, "%s\\%s%s", movie_dir, x ? "x" : "", base);
        p[MAX_PATH - 1] = 0;
        if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) {
            GetFullPathNameA(p, size, out, NULL);
            return 1;
        }
    }
    return 0;
}

HWND movie_create(HWND parent, DWORD style, const char* file) {
    static ATOM cls;
    Movie* m;
    char path[MAX_PATH];
    if (!file || !ffmpeg_found()) return NULL;
    if (GetFileAttributesA(file) == INVALID_FILE_ATTRIBUTES && movie_find(file, path, MAX_PATH)) file = path;
    m = (Movie*)calloc(1, sizeof *m);
    GetFullPathNameA(file, MAX_PATH, m->path, NULL);
    if (!avi_probe(m->path, &m->ai)) {
        fprintf(stderr, "[movie] %s: missing or not an AVI, skipped\n", m->path);
        free(m);
        return NULL;
    }
    if (!cls) {
        WNDCLASSA wc = { 0 };
        wc.lpfnWndProc = movie_proc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.lpszClassName = "swgb.movie";
        cls = RegisterClassA(&wc);
    }
    m->frame = (uint32_t*)calloc((size_t)m->ai.w * m->ai.h, 4);
    InitializeCriticalSection(&m->cs);
    /* Only WS_CHILD and WS_VISIBLE mean anything here: the MCIWNDF_ bits
     * (no menu, no play bar, notify...) are what this window is anyway. */
    m->hwnd = CreateWindowExA(0, "swgb.movie", "", WS_CHILD | (style & WS_VISIBLE) | WS_CLIPSIBLINGS,
                              0, 0, m->ai.w, m->ai.h, parent, NULL, GetModuleHandleA(NULL), NULL);
    if (!m->hwnd) {
        DeleteCriticalSection(&m->cs);
        free(m->frame);
        free(m);
        return NULL;
    }
    SetWindowLongPtrA(m->hwnd, GWLP_USERDATA, (LONG_PTR)m);
    return m->hwnd;
}

BOOL movie_icinfo(DWORD type, DWORD handler, void* icinfo) {
    typedef BOOL (WINAPI *icinfo_t)(DWORD, DWORD, void*);
    static icinfo_t real;
    if (!real) real = (icinfo_t)GetProcAddress(LoadLibraryA("msvfw32.dll"), "ICInfo");
    /* FFmpeg decodes it whether Windows has it or not: say so. */
    if (type == mmioFOURCC('v', 'i', 'd', 'c') && (handler | 0x20202020u) == mmioFOURCC('i', 'v', '5', '0')
        && ffmpeg_found()) {
        if (icinfo) {
            DWORD* d = (DWORD*)icinfo;   /* ICINFO: dwSize, fccType, fccHandler, dwFlags, ... */
            d[1] = type, d[2] = handler, d[3] = d[4] = d[5] = 0;
        }
        return TRUE;
    }
    return real ? real(type, handler, icinfo) : FALSE;
}
