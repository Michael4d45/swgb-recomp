/*
 * movie.c - the game's films on the win32hle host (Linux, macOS), as the
 * Windows host's src/runtime/movie.c plays them (its header has the game's
 * side: MCIWndCreateA, ICInfo for Indeo 5, the messages it sends).
 *
 * MCIWndCreateA makes a child window of the game's ("swgb.movie") whose
 * window procedure is a shim of ours, and answers the messages the game
 * sends: MCI_PLAY, MCI_STOP, MCI_PAUSE, MCI_RESUME, MCIWNDM_GETMODEA ("playing",
 * "paused", "stopped": stopped ends the film), MCIWNDM_GETPALETTE (the film's
 * palette, which the game installs) and MCIWNDM_REALIZE.
 *
 * Two ffmpeg processes decode the film: the video to raw BGRA frames, the
 * sound to 16-bit stereo PCM, which goes to win32hle's mixer
 * (hle_audio_stream_*). The sound's position is the clock (the wall clock
 * without sound); a late frame is dropped. The frame reaches the picture as
 * the text fields do (fields.c): the primary as shown is a copy with the
 * visible children drawn into it, and this window is drawn there, ordered-
 * dithered into the film palette (or the nearest entry of another). Each new
 * frame tells the presenter the primary changed. --headless, --shot and
 * --record show it the same way.
 *
 * --films DIR: a film the game cannot open (the Steam install keeps them
 * beside the exe, the game looks for Avi\NAME on the disc) is looked for in
 * DIR as NAME.avi or xNAME.avi (the Saga's names).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "win32hle.h"
#include "movie_core.h"
#include "swgb_linux.h"

#define MCI_PLAY            0x0806u
#define MCI_STOP            0x0808u
#define MCI_PAUSE           0x0809u
#define MCI_RESUME          0x0855u
#define MCI_MODE_STOP       525u
#define MCI_MODE_PLAY       526u
#define MCI_MODE_PAUSE      529u
#define MCIWNDM_GETMODEA    (0x0400u + 106)
#define MCIWNDM_REALIZE     (0x0400u + 118)
#define MCIWNDM_GETPALETTE  (0x0400u + 126)
#define WM_DESTROY_         0x0002u
#define WM_SETFOCUS_        0x0007u
#define WM_PAINT_           0x000Fu
#define WM_ERASEBKGND_      0x0014u

#define AUDIO_RATE 44100
#define AUDIO_CHUNK (AUDIO_RATE / 10)          /* frames pushed at a time: 100 ms */

const char* swgb_films_dir;

static uint32_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u);
}

/* ---- ffmpeg ------------------------------------------------------------------ */
static int ffmpeg_found(void) {
    static int known = -1;
    if (known < 0) {
        known = system("ffmpeg -version >/dev/null 2>&1") == 0;
        if (!known) fprintf(stderr, "[movie] no ffmpeg on PATH: the films are skipped\n");
    }
    return known;
}

/* ffmpeg with argv, its standard output on a pipe (read end returned), the rest to /dev/null. */
static int spawn(char* const argv[], pid_t* pid) {
    int fd[2];
    if (pipe2(fd, O_CLOEXEC)) return -1;
    pid_t p = fork();
    if (p < 0) { close(fd[0]); close(fd[1]); return -1; }
    if (!p) {
        int nul = open("/dev/null", O_RDWR);
        dup2(nul, 0), dup2(fd[1], 1), dup2(nul, 2);
        execvp("ffmpeg", argv);
        _exit(127);
    }
    close(fd[1]);
    *pid = p;
    return fd[0];
}

static int read_all(int fd, void* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t k = read(fd, (char*)buf + got, n - got);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        got += (size_t)k;
    }
    return (int)got;
}

/* ---- one film ---------------------------------------------------------------- */
typedef struct {
    uint32_t hwnd;
    char path[PATH_MAX];
    avi_info ai;
    pthread_mutex_t lock;          /* the frame and the clock */
    uint32_t* frame;               /* BGRA, w x h: the newest shown */
    int have_frame;
    pid_t vpid, apid;
    int vfd, afd;
    pthread_t vthread, athread;
    int has_v, has_a;
    volatile int stop;
    int playing, done_video, done_audio;
    int stream;                    /* win32hle's mixer, or -1 */
    long pushed;                   /* frames given to it */
    film_clock clk;                /* under lock; while clk.audio_on, the clock is what the mixer played */
    int audio_ready;
    pthread_cond_t ready;
} movie_t;

#define MAX_MOVIES 4
static movie_t* g_movie[MAX_MOVIES];
static pthread_mutex_t g_movies = PTHREAD_MUTEX_INITIALIZER;

static movie_t* movie_of(uint32_t hwnd) {
    movie_t* m = NULL;
    pthread_mutex_lock(&g_movies);
    for (int i = 0; i < MAX_MOVIES; i++) if (g_movie[i] && g_movie[i]->hwnd == hwnd) m = g_movie[i];
    pthread_mutex_unlock(&g_movies);
    return m;
}

static double sound_ms(movie_t* m) {
    long played = m->pushed - hle_audio_stream_queued(m->stream);
    return played * 1000.0 / AUDIO_RATE;
}

static double clock_ms(movie_t* m) {
    pthread_mutex_lock(&m->lock);
    double t = m->clk.audio_on ? sound_ms(m) : film_wall_ms(&m->clk, now_ms());
    pthread_mutex_unlock(&m->lock);
    return t;
}

static void clock_to_wall(movie_t* m) {
    pthread_mutex_lock(&m->lock);
    film_clock_to_wall(&m->clk, now_ms(), m->clk.audio_on ? sound_ms(m) : 0);
    pthread_mutex_unlock(&m->lock);
}

static void audio_started(movie_t* m) {
    pthread_mutex_lock(&m->lock);
    m->audio_ready = 1;
    pthread_cond_broadcast(&m->ready);
    pthread_mutex_unlock(&m->lock);
}

static void* audio_thread(void* arg) {
    movie_t* m = (movie_t*)arg;
    static int16_t buf[AUDIO_CHUNK * 2];
    char rate[16];
    snprintf(rate, sizeof rate, "%d", AUDIO_RATE);
    char* argv[] = { "ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin", "-i", m->path, "-map", "0:a:0", "-vn",
                     "-f", "s16le", "-acodec", "pcm_s16le", "-ac", "2", "-ar", rate, "-", NULL };
    m->stream = hle_audio_stream_open(AUDIO_RATE, 2);
    if (m->stream < 0) fprintf(stderr, "[movie] no audio: the wall clock times the film\n");
    else m->afd = spawn(argv, &m->apid);
    if (m->stream < 0 || m->afd < 0) goto none;
    int first = 1;
    while (!m->stop) {
        if (m->clk.paused || hle_audio_stream_queued(m->stream) > AUDIO_CHUNK * 3) { usleep(10000); continue; }
        int n = read_all(m->afd, buf, sizeof buf) / 4;
        if (n <= 0) break;
        hle_audio_stream_push(m->stream, buf, n);
        pthread_mutex_lock(&m->lock);
        m->pushed += n;
        if (first) m->clk.audio_on = 1;
        pthread_mutex_unlock(&m->lock);
        if (first) { first = 0; audio_started(m); }
    }
    while (!m->stop && hle_audio_stream_queued(m->stream) > 0) usleep(10000);   /* the sound's last 300 ms */
    clock_to_wall(m);
    hle_audio_stream_close(m->stream);
    m->stream = -1;
    m->done_audio = 1;
    if (first) audio_started(m);
    return NULL;
none:
    if (m->stream >= 0) hle_audio_stream_close(m->stream);
    m->stream = -1;
    m->done_audio = 1;
    audio_started(m);
    return NULL;
}

/* The presenter: the primary changed (the film's frame is drawn into it as shown). */
static void refresh(void) {
    mach_enter();
    if (hle_dd_primary_hook) hle_dd_primary_hook();
    mach_leave();
}

static void* video_thread(void* arg) {
    movie_t* m = (movie_t*)arg;
    size_t bytes = (size_t)m->ai.w * m->ai.h * 4;
    uint32_t* next = (uint32_t*)malloc(bytes);
    double frame_ms = 1000.0 * m->ai.scale / m->ai.rate;
    uint32_t last_shown = now_ms();
    long shown = 0, dropped = 0;
    pthread_mutex_lock(&m->lock);                         /* the sound starts the clock (3 s at most) */
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 3;
    while (!m->audio_ready && pthread_cond_timedwait(&m->ready, &m->lock, &until) != ETIMEDOUT) {}
    if (!m->clk.audio_on) film_clock_start(&m->clk, now_ms());
    int by_sound = m->clk.audio_on;
    pthread_mutex_unlock(&m->lock);
    uint32_t t0 = now_ms();
    long k;
    double now = 0;
    for (k = 0; !m->stop; k++) {
        if (read_all(m->vfd, next, bytes) < (int)bytes) break;
        double due = k * frame_ms;
        while (!m->stop && (now = clock_ms(m)) < due) {
            double wait = due - now;
            usleep((useconds_t)((wait > 10 ? 10 : wait + 1) * 1000));
        }
        if (m->stop) break;
        if (now >= due + frame_ms && now_ms() - last_shown < 100) { dropped++; continue; }   /* late: dropped */
        pthread_mutex_lock(&m->lock);
        memcpy(m->frame, next, bytes);
        m->have_frame = 1;
        pthread_mutex_unlock(&m->lock);
        last_shown = now_ms();
        shown++;
        refresh();
    }
    free(next);
    if (!m->stop && k) {
        const char* base = strrchr(m->path, '/');
        fprintf(stderr, "[movie] %s: %ld frames shown, %ld dropped; the last due at %.2f s, shown at %.2f s"
                " (clock: %s)\n", base ? base + 1 : m->path, shown, dropped, (k - 1) * frame_ms / 1000,
                (last_shown - t0) / 1000.0, by_sound ? "the sound" : "the wall");
    }
    m->done_video = 1;
    return NULL;
}

/* From the window procedure (holding the machine): the video thread may be waiting for it. */
static void movie_stop(movie_t* m) {
    if (!m->playing) return;
    m->stop = 1;
    if (m->vpid > 0) kill(m->vpid, SIGKILL);
    if (m->apid > 0) kill(m->apid, SIGKILL);
    hle_block_begin();
    if (m->has_v) pthread_join(m->vthread, NULL);
    if (m->has_a) pthread_join(m->athread, NULL);
    hle_block_end();
    if (m->vpid > 0) waitpid(m->vpid, NULL, 0);
    if (m->apid > 0) waitpid(m->apid, NULL, 0);
    if (m->vfd >= 0) close(m->vfd);
    if (m->afd >= 0) close(m->afd);
    m->vpid = m->apid = 0, m->vfd = m->afd = -1, m->has_v = m->has_a = 0;
    m->playing = 0;
    m->done_video = m->done_audio = 1;
}

static void movie_pause(movie_t* m, int pause) {
    pthread_mutex_lock(&m->lock);
    if (m->playing && film_clock_pause(&m->clk, pause, now_ms()) && m->stream >= 0)
        hle_audio_stream_gain(m->stream, pause ? 0.0f : 1.0f);
    pthread_mutex_unlock(&m->lock);
}

static void movie_play(movie_t* m) {
    if (m->playing) { movie_pause(m, 0); return; }
    char* argv[] = { "ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin", "-i", m->path, "-map", "0:v:0", "-an",
                     "-fps_mode", "cfr", "-f", "rawvideo", "-pix_fmt", "bgra", "-", NULL };
    m->stop = 0;
    m->done_video = m->done_audio = 0;
    m->audio_ready = 0, m->clk.audio_on = 0, m->pushed = 0, m->stream = -1;
    m->vfd = spawn(argv, &m->vpid);
    if (m->vfd < 0) {
        fprintf(stderr, "[movie] cannot start ffmpeg for %s\n", m->path);
        m->done_video = m->done_audio = 1;
        return;
    }
    m->playing = 1;
    if (m->ai.has_audio) m->has_a = !pthread_create(&m->athread, NULL, audio_thread, m);
    if (!m->has_a) { m->done_audio = 1; audio_started(m); }
    m->has_v = !pthread_create(&m->vthread, NULL, video_thread, m);
    fprintf(stderr, "[movie] playing %s (%dx%d, %.2f fps%s)\n", m->path, m->ai.w, m->ai.h,
            (double)m->ai.rate / m->ai.scale, m->ai.has_audio ? ", with sound" : "");
}

/* ---- the picture ----------------------------------------------------------------- */
static uint32_t g_film_hpal;

/* CreatePalette, the game's GDI's: it installs the film palette (movie_core.c) from it. */
static uint32_t film_palette(void) {
    if (g_film_hpal) return g_film_hpal;
    uint8_t* lp = (uint8_t*)hle_alloc(4 + 256 * 4);
    lp[0] = 0x00, lp[1] = 0x03, lp[2] = 0x00, lp[3] = 0x01;   /* palVersion 0x300, palNumEntries 256 */
    memcpy(lp + 4, film_palette_entries(), 256 * 4);
    uint32_t a = hle_va(lp);
    g_film_hpal = hle_call_guest(hle_resolve("CreatePalette"), 1, &a);
    return g_film_hpal;
}

/* fields.c's compose: the film window at l,t..r,b of the frame (8 bits through pal, or 16). */
int swgb_movie_draw(uint32_t hwnd, uint8_t* px, int pitch, int bpp, int l, int t, int r, int b, const uint32_t* pal) {
    movie_t* m = movie_of(hwnd);
    if (!m) return 0;
    int cw = r - l, ch = b - t, vw = m->ai.w, vh = m->ai.h;
    if (cw <= 0 || ch <= 0) return 1;
    /* the film palette, which the game installed: dithered; some other palette: the nearest entry */
    const uint8_t* lut = bpp == 8 && pal && !film_palette_is(pal) ? film_lut(pal) : NULL;
    pthread_mutex_lock(&m->lock);
    for (int y = 0; y < ch; y++) {
        uint8_t* row = px + (size_t)(t + y) * pitch;
        const uint32_t* src = m->frame + (size_t)(y * vh / ch) * vw;
        if (bpp == 8 && !pal) memset(row + l, 0, (size_t)cw);
        else if (bpp == 8 && !m->have_frame) memset(row + l, lut ? lut[0] : 0, (size_t)cw);
        else if (bpp == 8) film_row8(row + l, cw, src, vw, y, lut);
        else
            for (int x = 0; x < cw; x++) {
                uint32_t p = m->have_frame ? src[x * vw / cw] : 0;
                int R = p >> 16 & 255, G = p >> 8 & 255, B = p & 255;
                ((uint16_t*)row)[l + x] = (uint16_t)((R >> 3) << 11 | (G >> 2) << 5 | B >> 3);
            }
    }
    pthread_mutex_unlock(&m->lock);
    return 1;
}

/* ---- the window ---------------------------------------------------------------------- */
static uint32_t guest4(const char* fn, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    uint32_t args[4] = { a, b, c, d };
    return hle_call_guest(hle_resolve(fn), 4, args);
}

static void movie_free(movie_t* m) {
    pthread_mutex_lock(&g_movies);
    for (int i = 0; i < MAX_MOVIES; i++) if (g_movie[i] == m) g_movie[i] = NULL;
    pthread_mutex_unlock(&g_movies);
    free(m->frame);
    free(m);
}

static void s_movie_proc(void) {                         /* (hwnd, msg, wParam, lParam) */
    uint32_t h = A32(0), msg = A32(1), w = A32(2), l = A32(3);
    movie_t* m = movie_of(h);
    if (!m) RET(guest4("DefWindowProcA", h, msg, w, l), 4);
    switch (msg) {
    case MCI_PLAY: movie_play(m); RET(0, 4);
    case MCI_STOP: movie_stop(m); RET(0, 4);
    case MCI_PAUSE: movie_pause(m, 1); RET(0, 4);
    case MCI_RESUME: movie_pause(m, 0); RET(0, 4);
    case MCIWNDM_GETMODEA: {
        const char* s = film_mode(m->playing, m->clk.paused, m->done_video && m->done_audio);
        if (l && w) snprintf((char*)hle_ptr(l), w, "%s", s);
        RET(!strcmp(s, "playing") ? MCI_MODE_PLAY : !strcmp(s, "paused") ? MCI_MODE_PAUSE : MCI_MODE_STOP, 4);
    }
    case MCIWNDM_GETPALETTE: { uint32_t p = film_palette(); RET(p, 4); }
    case MCIWNDM_REALIZE: RET(0, 4);
    case WM_PAINT_: {                                     /* drawn as the picture is composed (fields.c) */
        static uint8_t* ps;                               /* a PAINTSTRUCT: one thread paints */
        if (!ps) ps = (uint8_t*)hle_alloc(64);
        uint32_t a[2] = { h, hle_va(ps) };
        hle_call_guest(hle_resolve("BeginPaint"), 2, a);
        hle_call_guest(hle_resolve("EndPaint"), 2, a);
        RET(0, 4);
    }
    case WM_ERASEBKGND_: RET(1, 4);
    case WM_SETFOCUS_: {                                  /* the keys that skip (Enter, Space, Esc) are the game window's */
        uint32_t p = hle_window_parent(h);
        hle_call_guest(hle_resolve("SetFocus"), 1, &p);
        RET(0, 4);
    }
    case WM_DESTROY_:
        movie_stop(m);
        movie_free(m);
        refresh();                                        /* the film is gone from the picture */
        RET(0, 4);
    }
    RET(guest4("DefWindowProcA", h, msg, w, l), 4);
}

/* The film's file: the guest's path, else NAME.avi or xNAME.avi in --films DIR. */
int swgb_movie_find(const char* guest_name, char* host, size_t n) {
    char base[256];
    if (!swgb_films_dir) return 0;
    for (int x = 0; x < 2 && film_name(guest_name, x, base, sizeof base); x++) {
        struct stat st;
        snprintf(host, n, "%s/%s", swgb_films_dir, base);
        if (!stat(host, &st)) return 1;
    }
    return 0;
}

static void s_MCIWndCreateA(void) {                       /* cdecl (parent, instance, style, file) */
    uint32_t parent = A32(0), style = A32(2);
    const char* file = ASTR(3);
    char host[PATH_MAX];
    g_eax = 0;
    if (!file || !ffmpeg_found()) { g_esp += 4; return; }
    if (!hle_host_path(file, host, sizeof host) && !swgb_movie_find(file, host, sizeof host)) {
        fprintf(stderr, "[movie] %s: not found (--films DIR has the films?), skipped\n", file);
        g_esp += 4;
        return;
    }
    movie_t* m = (movie_t*)calloc(1, sizeof *m);
    if (!realpath(host, m->path)) snprintf(m->path, sizeof m->path, "%s", host);
    if (!avi_probe(m->path, &m->ai)) {
        fprintf(stderr, "[movie] %s: missing or not an AVI, skipped\n", m->path);
        free(m);
        g_esp += 4;
        return;
    }
    static uint32_t cls;
    if (!cls) {
        uint32_t* wc = (uint32_t*)hle_alloc(40);         /* WNDCLASSA */
        wc[1] = hle_resolve("swgb.movie::WndProc");
        wc[4] = 0x00400000u;
        wc[9] = hle_guest_strdup("swgb.movie");
        uint32_t a = hle_va(wc);
        cls = hle_call_guest(hle_resolve("RegisterClassA"), 1, &a);
    }
    m->frame = (uint32_t*)calloc((size_t)m->ai.w * m->ai.h, 4);
    m->vfd = m->afd = -1, m->stream = -1;
    pthread_mutex_init(&m->lock, NULL);
    pthread_cond_init(&m->ready, NULL);
    pthread_mutex_lock(&g_movies);
    int slot = -1;
    for (int i = 0; i < MAX_MOVIES && slot < 0; i++) if (!g_movie[i]) slot = i;
    if (slot >= 0) g_movie[slot] = m;
    pthread_mutex_unlock(&g_movies);
    if (slot < 0) { free(m->frame); free(m); g_esp += 4; return; }
    /* Only WS_CHILD and WS_VISIBLE mean anything here (the MCIWNDF_ bits are what this window is anyway). */
    uint32_t args[12] = { 0, hle_guest_strdup("swgb.movie"), hle_guest_strdup(""),
                          0x40000000u | (style & 0x10000000u) | 0x04000000u, 0, 0, (uint32_t)m->ai.w, (uint32_t)m->ai.h,
                          parent, 0, 0x00400000u, 0 };
    m->hwnd = 0xFFFFFFFFu;                                /* its WM_CREATE and the like go to DefWindowProcA */
    uint32_t hwnd = hle_call_guest(hle_resolve("CreateWindowExA"), 12, args);
    if (!hwnd) { movie_free(m); g_esp += 4; return; }
    m->hwnd = hwnd;
    g_eax = hwnd;
    g_esp += 4;                                           /* cdecl: the caller pops the four */
}

/* MSVFW32's ICInfo: Indeo 5 (IV50) is there when ffmpeg is: it decodes it. */
static void s_ICInfo(void) {                              /* (type, handler, &icinfo) */
    uint32_t type = A32(0), handler = A32(1) | 0x20202020u;
    if (type == 0x63646976u && handler == 0x30357669u && ffmpeg_found()) {   /* 'vidc', 'iv50' */
        if (A32(2)) { uint32_t* d = (uint32_t*)APTR(2); d[1] = type, d[2] = A32(1), d[3] = d[4] = d[5] = 0; }
        RET(1, 3);
    }
    RET(0, 3);
}

void swgb_movie_register(void) {
    static const win32hle_shim shims[] = {
        { "swgb.movie::WndProc", s_movie_proc },
        { "MCIWndCreateA", s_MCIWndCreateA },
        { "ICInfo", s_ICInfo },
        { 0, 0 },
    };
    win32hle_register(shims);
}
