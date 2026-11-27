/*
 * cdaudio.c - MCI cdaudio over the Steam release's Music\TrackNN.ogg, and the
 * mixer's CD line as the music's volume (cdaudio.h; docs/music.md).
 *
 * What the game does with it (Battlegrounds.exe, the music player at
 * 0x004ACC00-0x004AE2F1):
 *
 *   start-up   MCI_OPEN (MCI_OPEN_TYPE_ID, MCI_DEVTYPE_CD_AUDIO, shareable,
 *              wait), MCI_SET MCI_FORMAT_TMSF, MCI_STATUS
 *              MCI_STATUS_NUMBER_OF_TRACKS. Fewer than 2 tracks, or an open
 *              that fails, and it tries MIDI files, then streamed WAV/MP3
 *              files, neither of which a Steam install has: no music.
 *              Then the mixer: the first device with a
 *              MIXERLINE_COMPONENTTYPE_SRC_COMPACTDISC line, its volume
 *              control, and that control's value (what it restores at exit).
 *   play       MCI_STOP of what plays, then MCI_PLAY with MCI_NOTIFY,
 *              MCI_FROM track (TMSF) and MCI_TO the track after the last one
 *              it wants (no MCI_TO when that is past the disc's end), or a
 *              MCI_SEEK to a saved position and a play from there.
 *   the end    MM_MCINOTIFY to its window: it counts its plays with notify
 *              and their notifications (aborted and superseded ones too),
 *              and when none is outstanding it plays the next track, or the
 *              range again.
 *   volume     its music slider (0-100) -> -100*v -> the CD line's volume
 *              control, linearly over the control's range.
 *   pause      on WM_ACTIVATEAPP: MCI_PAUSE, and MCI_PLAY with no flags to
 *              go on (to the disc's end, no notify).
 *   save       MCI_STATUS MCI_STATUS_POSITION, kept with the game.
 *
 * The disc: track 1 is data (2 s of silence), tracks 2..N the Ogg files,
 * N the highest TrackNN.ogg there is. Positions are samples at 44.1 kHz
 * from the disc's start (588 to a CD frame); a track at another rate is
 * resampled. A player thread decodes and feeds the output; the commands
 * only change what it should play (g_gen), so nothing on the game's thread
 * waits for it.
 */
#ifndef _WIN32
#include "winport.h"
#endif
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(CDAUDIO_NO_SHIMS)
#ifdef SWGB_WIN32HLE
#include "win32hle.h"
#define CD_ARG(n) A32(n)
#define CD_RET(v, n) RET(v, n)
#define CD_PTR(va) ((uint32_t*)hle_ptr((uint32_t)(va)))
#else
#include "native32.h"
#define CD_ARG(n) MEM32(g_esp + 4 + 4 * (n))
#define CD_RET(v, n) do { g_eax = (uint32_t)(v); g_esp += 4 + 4 * (n); return; } while (0)
#define CD_PTR(va) ((uint32_t*)(uintptr_t)(uint32_t)(va))
#endif
#else
#define CD_PTR(va) ((uint32_t*)(uintptr_t)(va))   /* the unit test: host memory, 32-bit */
#endif

#include "cdaudio.h"

/* stb_vorbis, the one file of it (third_party/stb_vorbis, public domain or MIT). */
#if defined(_MSC_VER)
#pragma warning(push, 1)
#pragma warning(disable: 4244 4245 4456 4457 4701 4703)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-value"
#endif
#define STB_VORBIS_NO_PUSHDATA_API
#include "../../third_party/stb_vorbis/stb_vorbis.c"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#undef L
#undef C
#undef R
#undef CHECK

/* ---- MCI, as mmsystem.h has it ---- */
#define MM_MCINOTIFY_ 0x3B9u
#define NOTIFY_SUCCESSFUL 1u
#define NOTIFY_SUPERSEDED 2u
#define NOTIFY_ABORTED 4u
#define C_OPEN 0x803u
#define C_CLOSE 0x804u
#define C_PLAY 0x806u
#define C_SEEK 0x807u
#define C_STOP 0x808u
#define C_PAUSE 0x809u
#define C_INFO 0x80Au
#define C_GETDEVCAPS 0x80Bu
#define C_SET 0x80Du
#define C_STATUS 0x814u
#define C_RESUME 0x855u
#define F_NOTIFY 0x1u
#define F_WAIT 0x2u
#define F_FROM 0x4u
#define F_TO 0x8u
#define F_TRACK 0x10u
#define F_OPEN_ELEMENT 0x200u
#define F_OPEN_TYPE_ID 0x1000u
#define F_OPEN_TYPE 0x2000u
#define F_SEEK_START 0x100u
#define F_SEEK_END 0x200u
#define F_SET_DOOR_OPEN 0x100u
#define F_SET_DOOR_CLOSED 0x200u
#define F_SET_TIME_FORMAT 0x400u
#define F_SET_AUDIO 0x800u
#define F_ITEM 0x100u            /* MCI_STATUS_ITEM, MCI_GETDEVCAPS_ITEM */
#define F_STATUS_START 0x200u
#define DEVTYPE_CD_AUDIO 516u
#define FMT_MS 0u
#define FMT_MSF 2u
#define FMT_TMSF 10u
#define MODE_STOP 525u
#define MODE_PLAY 526u
#define MODE_PAUSE 529u
#define TRACK_AUDIO 1088u        /* MCI_CDA_TRACK_AUDIO */
#define TRACK_OTHER 1089u
#define E_INVALID_DEVICE_ID 257u
#define E_UNRECOGNIZED_COMMAND 261u
#define E_INVALID_DEVICE_NAME 263u
#define E_MISSING_PARAMETER 273u
#define E_UNSUPPORTED_FUNCTION 274u
#define E_OUTOFRANGE 282u
#define E_BAD_TIME_FORMAT 293u
#define CD_DEVICE_ID 0x7CDAu     /* ours; WINMM's own ids are small */

#define RATE 44100
#define FRAME 588                /* samples to a CD frame (1/75 s) */
#define DATA_LEN (2 * RATE)      /* track 1, the data track */
#define MAX_TRACKS 99
#define CHUNK 2048               /* frames decoded at a time */
#define QUEUE (RATE / 2)         /* frames queued on the output at most: 0.5 s */

typedef struct {
    char path[1024];
    int64_t start, len;          /* on the disc, in 44.1 kHz samples */
    int rate, audio;             /* audio: an Ogg file is there */
} cd_track;

static cd_track g_tr[MAX_TRACKS + 2];
static int g_ntracks;            /* the last track's number (0: no disc) */
static int64_t g_disc_len;
static char g_dir[1024];         /* --music DIR */
static int g_off, g_native;      /* --music off / native */
static int g_open;               /* MCI_OPEN'd (and how many times: shareable) */
static uint32_t g_format = FMT_MSF;
static float g_gain = 1.0f;
static uint32_t g_t0;            /* the first MCI_OPEN: the log's clock */

/* What plays, under g_lock. The player thread plays g_pos..g_end while
 * g_mode is PLAY and g_gen is the job it took. */
#ifdef _WIN32
static CRITICAL_SECTION g_lock;
#else
static CRITICAL_SECTION g_lock;
#endif
static int g_lock_ok;
static HANDLE g_wake, g_thread;
static volatile uint32_t g_gen;
static volatile int g_quit;
static uint32_t g_mode = MODE_STOP;
static int64_t g_pos, g_end;
static uint32_t g_notify_hwnd;   /* a play with MCI_NOTIFY outstanding: its callback */

int cdaudio_test_instant;
void (*cdaudio_pcm_hook)(const int16_t* pcm, int frames);
void (*cdaudio_notify_hook)(uint32_t hwnd, uint32_t code, uint32_t dev);

void cdaudio_set_dir(const char* dir) { snprintf(g_dir, sizeof g_dir, "%s", dir ? dir : ""); }
void cdaudio_set_gain(float gain) { g_gain = gain < 0 ? 0 : gain > 1 ? 1 : gain; }
float cdaudio_gain(void) { return g_gain; }

int cdaudio_arg(int argc, char** argv, int i) {
    if (strcmp(argv[i], "--music") || i + 1 >= argc) return 0;
    const char* v = argv[i + 1];
    if (!strcmp(v, "off")) g_off = 1;
    else if (!strcmp(v, "native")) g_native = 1;
    else {
#ifdef _WIN32
        GetFullPathNameA(v, sizeof g_dir, g_dir, NULL);   /* the game runs in its own folder */
#else
        if (v[0] == '/' || !getcwd(g_dir, sizeof g_dir)) snprintf(g_dir, sizeof g_dir, "%s", v);
        else { size_t n = strlen(g_dir); snprintf(g_dir + n, sizeof g_dir - n, "/%s", v); }
#endif
    }
    return 2;
}

static void lock(void) {
    if (!g_lock_ok) { InitializeCriticalSection(&g_lock); g_lock_ok = 1; }
    EnterCriticalSection(&g_lock);
}
static void unlock(void) { LeaveCriticalSection(&g_lock); }

/* ---- the notification ----
 * Posted to the play's callback window, as WINMM does: wParam the outcome,
 * lParam the device. From the player thread too: PostMessageA is any
 * thread's on Windows; on win32hle the queue is the machine's. */
static void post(uint32_t hwnd, uint32_t code) {
    static const char* what[] = { "", "successful", "superseded", "", "aborted" };
    printf("[cdaudio] notify %s\n", code < 5 ? what[code] : "?");
    if (cdaudio_notify_hook) { cdaudio_notify_hook(hwnd, code, CD_DEVICE_ID); return; }
#if defined(CDAUDIO_NO_SHIMS)
    (void)hwnd;
#elif defined(_WIN32)
    PostMessageA((HWND)(uintptr_t)hwnd, MM_MCINOTIFY_, code, CD_DEVICE_ID);
#else
    mach_enter();
    hle_post_message(hwnd, MM_MCINOTIFY_, code, CD_DEVICE_ID);
    mach_leave();
#endif
}

/* A command that ends a play's wait: the play's notification, aborted (or
 * superseded by a new play with MCI_NOTIFY). Under g_lock; posted after. */
static uint32_t take_notify(void) {
    uint32_t h = g_notify_hwnd;
    g_notify_hwnd = 0;
    return h;
}

/* ---- the tracks ---- */
static int track_file(int t, char* out, size_t n) {
    char name[32];
    snprintf(name, sizeof name, "Track%02d.ogg", t);
    if (g_dir[0]) {
        FILE* f;
        snprintf(out, n, "%s/%s", g_dir, name);
        if ((f = fopen(out, "rb"))) { fclose(f); return 1; }
        snprintf(out, n, "%s/track%02d.ogg", g_dir, t);
        if ((f = fopen(out, "rb"))) { fclose(f); return 1; }
        return 0;
    }
#if defined(SWGB_WIN32HLE) && !defined(CDAUDIO_NO_SHIMS)
    char guest[64];
    snprintf(guest, sizeof guest, "C:\\Music\\%s", name);  /* C: is the game's folder */
    return hle_host_path(guest, out, n);
#else
    FILE* f;
    snprintf(out, n, "Music/%s", name);                    /* the game runs in its folder */
    if ((f = fopen(out, "rb"))) { fclose(f); return 1; }
    return 0;
#endif
}

/* The disc: every TrackNN.ogg's length, from its last page (stb_vorbis reads
 * the headers and seeks to the end: about a millisecond a file). */
static void scan(void) {
    uint32_t t0 = GetTickCount();
    if (!g_t0) g_t0 = t0;
    int64_t at = 0;
    g_ntracks = 0;
    memset(g_tr, 0, sizeof g_tr);
    for (int t = 1; t <= MAX_TRACKS; t++) {
        cd_track* k = &g_tr[t];
        int err = 0;
        if (!track_file(t, k->path, sizeof k->path)) continue;
        stb_vorbis* v = stb_vorbis_open_filename(k->path, &err, NULL);
        if (!v) { printf("[cdaudio] %s: not Ogg Vorbis (error %d), left out\n", k->path, err); continue; }
        stb_vorbis_info in = stb_vorbis_get_info(v);
        unsigned n = stb_vorbis_stream_length_in_samples(v);
        stb_vorbis_close(v);
        if (!in.sample_rate || !n) continue;
        k->rate = (int)in.sample_rate;
        k->len = (int64_t)n * RATE / k->rate;
        k->audio = 1;
        g_ntracks = t;
    }
    for (int t = 1; t <= g_ntracks; t++) {   /* the data track, and any gap, as silence */
        if (!g_tr[t].audio) g_tr[t].len = t == 1 ? DATA_LEN : 0;
        g_tr[t].start = at;
        at += g_tr[t].len;
    }
    g_disc_len = at;
    if (g_ntracks)
        printf("[cdaudio] disc: tracks 1-%d (%d.%01d min of audio) from %s, %u ms\n", g_ntracks,
               (int)(at / RATE / 60), (int)(at / RATE % 60 / 6), g_dir[0] ? g_dir : "the game's Music folder",
               (unsigned)(GetTickCount() - t0));
    else
        printf("[cdaudio] no Music\\TrackNN.ogg: no CD music\n");
}

static int track_at(int64_t pos) {          /* the track pos is in: the last at the disc's end */
    for (int t = g_ntracks; t >= 1; t--)
        if (pos >= g_tr[t].start && g_tr[t].len) return t;
    return 1;
}

/* ---- time formats ---- */
static uint32_t msf(int64_t pos) {
    int64_t f = pos / FRAME;
    return (uint32_t)(f / 4500) | (uint32_t)(f / 75 % 60) << 8 | (uint32_t)(f % 75) << 16;
}
/* A disc position for the log: "track 5 1:02" (or "the end"). */
static const char* where(int64_t pos, char* out) {
    if (pos >= g_disc_len) return "the end";
    int t = track_at(pos);
    int64_t s = (pos - g_tr[t].start) / RATE;
    snprintf(out, 32, "track %d %d:%02d", t, (int)(s / 60), (int)(s % 60));
    return out;
}
static uint32_t to_format(int64_t pos) {
    if (g_format == FMT_MS) return (uint32_t)(pos * 1000 / RATE);
    if (g_format == FMT_MSF) return msf(pos);
    int t = track_at(pos);
    int64_t f = (pos - g_tr[t].start) / FRAME;
    return (uint32_t)t | (uint32_t)(f / 4500 & 0xFF) << 8 | (uint32_t)(f / 75 % 60) << 16 | (uint32_t)(f % 75) << 24;
}
static int from_format(uint32_t v, int64_t* pos) {   /* 0: out of range */
    int64_t p;
    if (g_format == FMT_MS) p = (int64_t)v * RATE / 1000;
    else if (g_format == FMT_MSF) p = ((int64_t)(v & 0xFF) * 4500 + (v >> 8 & 0xFF) * 75 + (v >> 16 & 0xFF)) * FRAME;
    else {
        int t = (int)(v & 0xFF);
        if (t < 1 || t > g_ntracks) return 0;
        p = g_tr[t].start + ((int64_t)(v >> 8 & 0xFF) * 4500 + (v >> 16 & 0xFF) * 75 + (v >> 24)) * FRAME;
    }
    if (p > g_disc_len) return 0;
    *pos = p;
    return 1;
}

/* ---- the output: waveOut, win32hle's stream, a clock, or as fast as it comes ----
 * All of it 44.1 kHz stereo s16. played() is how much of what was written
 * has been heard. */
static int64_t g_written, g_clock0;
static int g_out_kind;           /* 0: clock, 1: waveOut, 2: win32hle stream, 3: instant */
#ifdef _WIN32
#define NBUF 8
#define BUF_FRAMES 2205          /* 50 ms */
static HWAVEOUT g_wo;
static WAVEHDR g_hdr[NBUF];
static int16_t g_pcm[NBUF][BUF_FRAMES * 2];
static HANDLE g_wo_event;
#elif !defined(CDAUDIO_NO_SHIMS)
static int g_stream = -1;
#endif

static uint32_t now_ms(void) { return GetTickCount(); }

static void out_open(void) {
    if (cdaudio_test_instant) { g_out_kind = 3; return; }
#ifdef _WIN32
    WAVEFORMATEX wf = { WAVE_FORMAT_PCM, 2, RATE, RATE * 4, 4, 16, 0 };
    g_wo_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (waveOutOpen(&g_wo, WAVE_MAPPER, &wf, (DWORD_PTR)g_wo_event, 0, CALLBACK_EVENT) == MMSYSERR_NOERROR) {
        for (int i = 0; i < NBUF; i++) {
            g_hdr[i].lpData = (LPSTR)g_pcm[i];
            g_hdr[i].dwBufferLength = sizeof g_pcm[i];
            waveOutPrepareHeader(g_wo, &g_hdr[i], sizeof g_hdr[i]);
            g_hdr[i].dwFlags |= WHDR_DONE;
        }
        g_out_kind = 1;
        printf("[cdaudio] output: waveOut, 44100 Hz stereo\n");
        return;
    }
    g_wo = NULL;
#elif !defined(CDAUDIO_NO_SHIMS)
    if ((g_stream = hle_audio_stream_open(RATE, 2)) >= 0) {
        g_out_kind = 2;
        printf("[cdaudio] output: win32hle's audio stream, 44100 Hz stereo\n");
        return;
    }
#endif
    g_out_kind = 0;
    printf("[cdaudio] no audio device: the tracks keep time on the clock\n");
}

static int64_t played(void) {
    switch (g_out_kind) {
#ifdef _WIN32
    case 1: {
        int64_t q = 0;
        for (int i = 0; i < NBUF; i++) if (!(g_hdr[i].dwFlags & WHDR_DONE)) q += g_hdr[i].dwBufferLength / 4;
        return g_written - q;
    }
#elif !defined(CDAUDIO_NO_SHIMS)
    case 2: return g_written - hle_audio_stream_queued(g_stream);
#endif
    case 3: return g_written;
    default: {
        int64_t c = (int64_t)(uint32_t)(now_ms() - (uint32_t)g_clock0) * RATE / 1000;
        return c < g_written ? c : g_written;
    }
    }
}

static void out_reset(void) {                /* drop what is queued */
#ifdef _WIN32
    if (g_out_kind == 1) waveOutReset(g_wo);
#elif !defined(CDAUDIO_NO_SHIMS)
    if (g_out_kind == 2) { hle_audio_stream_close(g_stream); g_stream = hle_audio_stream_open(RATE, 2); if (g_stream < 0) g_out_kind = 0; }
#endif
    g_written = 0;
    g_clock0 = now_ms();
}

/* Write frames, waiting for room; 0 when the job changed meanwhile. */
static int out_write(const int16_t* pcm, int frames, uint32_t gen) {
    while (frames > 0) {
        if (g_gen != gen || g_quit) return 0;
        if (g_written - played() >= QUEUE) {
#ifdef _WIN32
            if (g_out_kind == 1) { WaitForSingleObject(g_wo_event, 20); continue; }
#endif
            Sleep(10);
            continue;
        }
        int n = frames;
#ifdef _WIN32
        if (g_out_kind == 1) {
            int i = 0;
            while (i < NBUF && !(g_hdr[i].dwFlags & WHDR_DONE)) i++;
            if (i == NBUF) { WaitForSingleObject(g_wo_event, 20); continue; }
            if (n > BUF_FRAMES) n = BUF_FRAMES;
            memcpy(g_pcm[i], pcm, (size_t)n * 4);
            g_hdr[i].dwBufferLength = (DWORD)n * 4;
            g_hdr[i].dwFlags &= ~WHDR_DONE;
            waveOutWrite(g_wo, &g_hdr[i], sizeof g_hdr[i]);
        }
#elif !defined(CDAUDIO_NO_SHIMS)
        if (g_out_kind == 2) hle_audio_stream_push(g_stream, pcm, n);
#endif
        if (g_out_kind == 3 && cdaudio_pcm_hook) cdaudio_pcm_hook(pcm, n);
        g_written += n;
        pcm += 2 * n;
        frames -= n;
    }
    return 1;
}

/* ---- the player thread ---- */
static stb_vorbis* g_dec;
static int g_dec_track;
static int64_t g_dec_at;         /* the track's sample (its own rate) the decoder is at */
static double g_rs_pos;          /* resampling: where in the decoded frames the next output frame is */

static void set_pos(int64_t p, uint32_t gen) { lock(); if (g_gen == gen) g_pos = p; unlock(); }

/* Decode up to n frames of track t from disc position pos into out (44.1 kHz
 * stereo); the frames made, 0 at the track's end. */
static int decode(int t, int64_t pos, int16_t* out, int n) {
    cd_track* k = &g_tr[t];
    int64_t want = (pos - k->start) * k->rate / RATE;   /* the track's own sample */
    if (g_dec_track != t || !g_dec) {
        if (g_dec) stb_vorbis_close(g_dec);
        g_dec = stb_vorbis_open_filename(k->path, NULL, NULL);
        g_dec_track = g_dec ? t : 0;
        g_dec_at = 0;
        if (!g_dec) return 0;
    }
    if (want != g_dec_at) {
        if (!stb_vorbis_seek(g_dec, (unsigned)want)) return 0;
        g_dec_at = want;
        g_rs_pos = 0;
    }
    if (k->rate == RATE) {
        int got = stb_vorbis_get_samples_short_interleaved(g_dec, 2, out, n * 2);
        g_dec_at += got;
        return got;
    }
    /* Another rate: linear, from frames decoded at the track's rate. */
    static int16_t in[CHUNK * 4 + 4];
    double step = (double)k->rate / RATE;
    int need = (int)(n * step) + 2;
    if (need > CHUNK * 2) need = CHUNK * 2, n = (int)((need - 2) / step);
    int got = stb_vorbis_get_samples_short_interleaved(g_dec, 2, in, need * 2);
    if (got < 2) { g_dec_at += got; return 0; }
    int m = 0;
    for (; m < n; m++) {
        double x = m * step;
        int i = (int)x;
        if (i + 1 >= got) break;
        double f = x - i;
        out[2 * m] = (int16_t)(in[2 * i] + (in[2 * i + 2] - in[2 * i]) * f);
        out[2 * m + 1] = (int16_t)(in[2 * i + 1] + (in[2 * i + 3] - in[2 * i + 1]) * f);
    }
    int used = (int)(m * step);
    g_dec_at += used;
    if (used < got) stb_vorbis_seek(g_dec, (unsigned)g_dec_at);   /* give back what was read ahead */
    return m;
}

static void play_job(uint32_t gen, int64_t pos, int64_t end) {
    static int16_t buf[CHUNK * 2];
    if (cdaudio_test_instant) g_out_kind = 3;            /* the unit test switches between the two */
    else if (g_out_kind == 3) g_out_kind = 0;
    out_reset();
    int64_t base = pos;
    int last = 0;
    while (pos < end) {
        int t = track_at(pos);
        int64_t tend = g_tr[t].start + g_tr[t].len;
        int n = CHUNK;
        if (pos + n > end) n = (int)(end - pos);
        if (pos + n > tend) n = (int)(tend - pos);
        if (n <= 0) { pos = tend > pos ? tend : pos + 1; continue; }
        if (t != last) {
            printf("[cdaudio] track %d (%s) at %.1f s\n", t, g_tr[t].audio ? "audio" : "data: silence",
                   (uint32_t)(GetTickCount() - g_t0) / 1000.0);
            last = t;
        }
        int got = 0;
        if (g_tr[t].audio) got = decode(t, pos, buf, n);
        if (!g_tr[t].audio || got <= 0) {    /* data, or the file ended before its length: silence */
            memset(buf, 0, (size_t)n * 4);
            got = n;
        }
        float gn = g_gain;
        if (gn < 0.999f)
            for (int i = 0; i < got * 2; i++) buf[i] = (int16_t)(buf[i] * gn);
        if (!out_write(buf, got, gen)) { out_reset(); return; }
        pos += got;
        set_pos(base + played(), gen);
    }
    /* Played to the end: wait until it has been heard, then notify. */
    while (played() < g_written) {
        if (g_gen != gen || g_quit) { out_reset(); return; }
        set_pos(base + played(), gen);
        Sleep(10);
    }
    uint32_t h = 0;
    lock();
    if (g_gen == gen) {
        g_pos = end;
        g_mode = MODE_STOP;
        h = take_notify();
    }
    unlock();
    char w[32];
    printf("[cdaudio] played to %s at %.1f s\n", where(end, w), (uint32_t)(GetTickCount() - g_t0) / 1000.0);
    if (h) post(h, NOTIFY_SUCCESSFUL);
}

static DWORD WINAPI player(LPVOID unused) {
    (void)unused;
    out_open();
    for (;;) {
        WaitForSingleObject(g_wake, 100);
        lock();
        if (g_quit) { unlock(); break; }
        uint32_t gen = g_gen;
        int go = g_mode == MODE_PLAY;
        int64_t pos = g_pos, end = g_end;
        unlock();
        if (go) play_job(gen, pos, end);
        else if (g_written) out_reset();
    }
    out_reset();
#ifdef _WIN32
    if (g_out_kind == 1) {
        for (int i = 0; i < NBUF; i++) waveOutUnprepareHeader(g_wo, &g_hdr[i], sizeof g_hdr[i]);
        waveOutClose(g_wo);
        g_wo = NULL;
    }
#elif !defined(CDAUDIO_NO_SHIMS)
    if (g_out_kind == 2) hle_audio_stream_close(g_stream), g_stream = -1;
#endif
    if (g_dec) stb_vorbis_close(g_dec), g_dec = NULL, g_dec_track = 0;
    return 0;
}

/* A new job (play) or none (stop, pause, seek): the player drops the old one. */
static void kick(void) {
    g_gen++;
    if (g_wake) SetEvent(g_wake);
}

/* ---- the commands ---- */
static const char* cmd_name(uint32_t m) {
    switch (m) {
    case C_OPEN: return "open"; case C_CLOSE: return "close"; case C_PLAY: return "play";
    case C_SEEK: return "seek"; case C_STOP: return "stop"; case C_PAUSE: return "pause";
    case C_SET: return "set"; case C_STATUS: return "status"; case C_GETDEVCAPS: return "getdevcaps";
    case C_INFO: return "info"; case C_RESUME: return "resume";
    }
    return "?";
}

static uint32_t c_open(uint32_t flags, uint32_t* p, const char* type_name) {
    int cd = (flags & F_OPEN_TYPE_ID) ? (p[2] & 0xFFFF) == DEVTYPE_CD_AUDIO
           : (flags & F_OPEN_TYPE) && type_name && !strncmp(type_name, "cdaudio", 7);
    if (!cd || (flags & F_OPEN_ELEMENT)) return E_INVALID_DEVICE_NAME;
    if (g_off) { printf("[cdaudio] open: --music off\n"); return E_INVALID_DEVICE_NAME; }
    if (!g_open) {
        /* once per folder: the game closes and opens the CD at every game's start */
        static char scanned[sizeof g_dir] = "\001";
        if (strcmp(scanned, g_dir)) { scan(); memcpy(scanned, g_dir, sizeof g_dir); }
        if (!g_ntracks) return E_INVALID_DEVICE_NAME;   /* no disc: the game goes on without music */
        g_format = FMT_MSF;
        g_mode = MODE_STOP;
        g_pos = 0;
        if (!g_wake) g_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
        g_quit = 0;
        if (!g_thread) g_thread = CreateThread(NULL, 0, player, NULL, 0, NULL);
    }
    g_open++;
    p[1] = CD_DEVICE_ID;
    return 0;
}

static uint32_t c_close(void) {
    uint32_t h;
    lock();
    g_mode = MODE_STOP;
    h = take_notify();
    kick();
    unlock();
    if (h) post(h, NOTIFY_ABORTED);
    if (g_open) g_open--;
    return 0;
}

static uint32_t c_play(uint32_t flags, uint32_t* p) {
    int64_t from, to;
    uint32_t old;
    lock();
    from = g_pos, to = g_disc_len;
    if ((flags & F_FROM) && !from_format(p[1], &from)) { unlock(); return E_OUTOFRANGE; }
    if ((flags & F_TO) && !from_format(p[2], &to)) { unlock(); return E_OUTOFRANGE; }
    if (from > to) { unlock(); return E_OUTOFRANGE; }
    old = take_notify();
    g_pos = from, g_end = to;
    g_mode = MODE_PLAY;
    if (flags & F_NOTIFY) g_notify_hwnd = p[0];
    kick();
    unlock();
    char a[32], b[32];
    printf("[cdaudio] play %s %s to %s%s at %.1f s\n", flags & F_FROM ? "from" : "on at", where(from, a), where(to, b),
           flags & F_NOTIFY ? ", notify" : "", (uint32_t)(GetTickCount() - g_t0) / 1000.0);
    if (old) post(old, flags & F_NOTIFY ? NOTIFY_SUPERSEDED : NOTIFY_ABORTED);
    return 0;
}

/* Stop, pause and seek: the play ends where it is (seek: at its target). */
static uint32_t c_halt(uint32_t msg, uint32_t flags, uint32_t* p) {
    int64_t to = -1;
    if (msg == C_SEEK) {
        if (flags & F_SEEK_START) to = 0;
        else if (flags & F_SEEK_END) to = g_disc_len;
        else if (!(flags & F_TO)) return E_MISSING_PARAMETER;
        else if (!from_format(p[1], &to)) return E_OUTOFRANGE;
    }
    uint32_t h;
    lock();
    int was = g_mode == MODE_PLAY;
    g_mode = msg == C_PAUSE && was ? MODE_PAUSE : MODE_STOP;
    if (to >= 0) g_pos = to;
    h = take_notify();
    kick();
    unlock();
    if (h) post(h, NOTIFY_ABORTED);
    return 0;
}

static uint32_t c_set(uint32_t flags, uint32_t* p) {
    if (flags & F_SET_TIME_FORMAT) {
        if (p[1] != FMT_MS && p[1] != FMT_MSF && p[1] != FMT_TMSF) return E_BAD_TIME_FORMAT;
        g_format = p[1];
    }
    return 0;   /* the door, the audio channels: nothing to do */
}

static uint32_t c_status(uint32_t flags, uint32_t* p) {
    if (!(flags & F_ITEM)) return E_MISSING_PARAMETER;
    int t = (int)p[3];
    int has_t = (flags & F_TRACK) != 0;
    if (has_t && (t < 1 || t > g_ntracks)) return E_OUTOFRANGE;
    switch (p[2]) {
    case 1:                                                /* MCI_STATUS_LENGTH */
        if (g_format == FMT_MS) p[1] = (uint32_t)((has_t ? g_tr[t].len : g_disc_len) * 1000 / RATE);
        else p[1] = msf(has_t ? g_tr[t].len : g_disc_len);   /* MSF, in TMSF too */
        break;
    case 2: {                                              /* MCI_STATUS_POSITION */
        int64_t at;
        lock();
        at = g_pos;
        unlock();
        p[1] = to_format(flags & F_STATUS_START ? 0 : has_t ? g_tr[t].start : at);
        break;
    }
    case 3: p[1] = (uint32_t)g_ntracks; break;            /* MCI_STATUS_NUMBER_OF_TRACKS */
    case 4: lock(); p[1] = g_mode; unlock(); break;       /* MCI_STATUS_MODE */
    case 5: p[1] = 1; break;                              /* MCI_STATUS_MEDIA_PRESENT */
    case 6: p[1] = g_format; break;                       /* MCI_STATUS_TIME_FORMAT */
    case 7: p[1] = 1; break;                              /* MCI_STATUS_READY */
    case 8: lock(); p[1] = (uint32_t)track_at(g_pos); unlock(); break;   /* MCI_STATUS_CURRENT_TRACK */
    case 0x4001:                                          /* MCI_CDA_STATUS_TYPE_TRACK */
        if (!has_t) return E_MISSING_PARAMETER;
        p[1] = g_tr[t].audio ? TRACK_AUDIO : TRACK_OTHER;
        break;
    default: return E_UNSUPPORTED_FUNCTION;
    }
    return 0;
}

static uint32_t c_getdevcaps(uint32_t flags, uint32_t* p) {
    if (!(flags & F_ITEM)) return E_MISSING_PARAMETER;
    switch (p[2]) {
    case 2: case 7: case 8: p[1] = 1; break;             /* has audio, can eject, can play */
    case 4: p[1] = DEVTYPE_CD_AUDIO; break;
    case 1: case 3: case 5: case 6: case 9: p[1] = 0; break;
    default: return E_UNSUPPORTED_FUNCTION;
    }
    return 0;
}

uint32_t cdaudio_command(uint32_t dev, uint32_t msg, uint32_t flags, uint32_t* p, const char* type_name) {
    static uint32_t zero[8];
    uint32_t rc;
    if (!p) p = zero;
    if (msg == C_OPEN) rc = c_open(flags, p, type_name);
    else if (dev != CD_DEVICE_ID || !g_open) rc = E_INVALID_DEVICE_ID;
    else switch (msg) {
    case C_CLOSE: rc = c_close(); break;
    case C_PLAY: rc = c_play(flags, p); break;
    case C_SEEK: case C_STOP: case C_PAUSE: rc = c_halt(msg, flags, p); break;
    case C_RESUME: rc = c_play(0, p); break;
    case C_SET: rc = c_set(flags, p); break;
    case C_STATUS: rc = c_status(flags, p); break;
    case C_GETDEVCAPS: rc = c_getdevcaps(flags, p); break;
    default: rc = E_UNRECOGNIZED_COMMAND; break;
    }
    if (msg != C_STATUS && msg != C_PLAY)
        printf("[cdaudio] %s flags 0x%X%s\n", cmd_name(msg), (unsigned)flags, rc ? " failed" : "");
    else if (msg == C_STATUS && !rc)
        printf("[cdaudio] status item %u%s -> 0x%X\n", (unsigned)p[2], flags & F_TRACK ? " of a track" : "", (unsigned)p[1]);
    /* A command with MCI_NOTIFY that is done at once (a play waits for its end). */
    if (!rc && (flags & F_NOTIFY) && msg != C_PLAY && msg != C_OPEN && p[0]) post(p[0], NOTIFY_SUCCESSFUL);
    if (rc && msg != C_OPEN) printf("[cdaudio] %s: MCI error %u\n", cmd_name(msg), (unsigned)rc);
    return rc;
}

void cdaudio_shutdown(void) {
    if (!g_thread) return;
    lock();
    g_mode = MODE_STOP;
    g_notify_hwnd = 0;
    g_quit = 1;
    kick();
    unlock();
    WaitForSingleObject(g_thread, 2000);
    CloseHandle(g_thread);
    g_thread = NULL;
    g_open = 0;
}

/* ---- the mixer's CD line ----
 * The music player finds the first mixer device with a
 * MIXERLINE_COMPONENTTYPE_SRC_COMPACTDISC line, that line's volume control by
 * type, and sets the control's value: that is the music's gain here. Only
 * that line is ours. The game's other mixer user, the sound effects' volume
 * on the wave-out line (0x00542E00), still gets WINMM's mixer on Windows,
 * where Vista and later give a program's wave line its own session volume;
 * the system's CD line, where a driver still has one, is never touched. On
 * win32hle there is no other mixer: one device, with this line alone. */
#define MX_HANDLE 0x0000CDA0u    /* a device of ours (no WINMM mixer at all, or win32hle) */
#define LINE_CD 0xCDA0CDA0u
#define CTRL_CD 0xCDA00101u
#define SRC_CD 0x1005u
#define VOLUME 0x50030001u       /* MIXERCONTROL_CONTROLTYPE_VOLUME */
#define MX_NOT_OURS 0xFFFFFFFFu  /* forward it (Windows), or answer as win32hle does */

static void put_str(uint32_t* at, const char* s, int n) {
    size_t k = strlen(s) < (size_t)n ? strlen(s) : (size_t)n - 1;
    memset(at, 0, (size_t)n);
    memcpy(at, s, k);
}

static uint32_t mixer_line(uint32_t* l, uint32_t how) {   /* MIXERLINEA, 168 bytes */
    int ours = ((how & 0xF) == 3 && l[6] == SRC_CD) || ((how & 0xF) == 2 && l[3] == LINE_CD);
    if (!ours) return MX_NOT_OURS;
    memset(l + 1, 0, 168 - 4);
    l[2] = 0x1000;               /* dwSource: one no real device numbers */
    l[3] = LINE_CD;              /* dwLineID */
    l[4] = 0x1 | 0x80000000u;    /* MIXERLINE_LINEF_ACTIVE | _SOURCE */
    l[6] = SRC_CD;
    l[7] = 2;                    /* cChannels */
    l[9] = 1;                    /* cControls */
    put_str(l + 10, "CD Audio", 16);
    put_str(l + 14, "CD Audio", 64);
    l[30] = 5;                   /* Target.dwType: MIXERLINE_TARGETTYPE_AUX */
    put_str(l + 34, "Music (TrackNN.ogg)", 32);
    return 0;
}

static uint32_t mixer_controls(uint32_t* c, uint32_t how) {   /* MIXERLINECONTROLSA */
    uint32_t q = how & 0xF;
    if (q == 1 ? c[2] != CTRL_CD : c[1] != LINE_CD) return MX_NOT_OURS;   /* by id, or the line's */
    if (q == 2 && c[2] != VOLUME) return 1025;                              /* MIXERR_INVALCONTROL */
    if (c[4] < 148 || !c[5]) return 11;                                     /* MMSYSERR_INVALPARAM */
    uint32_t* m = CD_PTR(c[5]);
    memset(m, 0, 148);
    m[0] = 148;
    m[1] = CTRL_CD;
    m[2] = VOLUME;
    m[3] = 0x1;                  /* MIXERCONTROL_CONTROLF_UNIFORM */
    put_str(m + 5, "Volume", 16);
    put_str(m + 9, "CD Audio Volume", 64);
    m[25] = 0;                   /* Bounds.dwMinimum */
    m[26] = 0xFFFF;              /* Bounds.dwMaximum */
    m[31] = 0xFFFF;              /* Metrics.cSteps */
    c[1] = LINE_CD;
    c[3] = 1;
    return 0;
}

static uint32_t mixer_details(uint32_t* d, int set) {   /* MIXERCONTROLDETAILS, one unsigned per channel */
    if (d[1] != CTRL_CD) return MX_NOT_OURS;
    if (d[4] < 4 || !d[5]) return 11;
    uint32_t* v = CD_PTR(d[5]);
    uint32_t n = d[2] ? d[2] : 1;
    if (n > 2) n = 2;
    if (!set) {
        for (uint32_t i = 0; i < n; i++) v[i] = (uint32_t)(g_gain * 0xFFFF + 0.5f);
        return 0;
    }
    uint32_t val = v[0];
    if (n == 2 && v[1] > val) val = v[1];
    if (val > 0xFFFF) val = 0xFFFF;
    float g = val / 65535.0f;
    if (g != g_gain) printf("[cdaudio] volume %d%%\n", (int)(g * 100 + 0.5f));
    cdaudio_set_gain(g);
    return 0;
}

/* ---- the shims ---- */
#if !defined(CDAUDIO_NO_SHIMS)

#ifdef _WIN32
static FARPROC real_import(const char* name) {
#ifdef SWGB_CC
    /* --music native: Steam's winmm wrapper beside the game (the Ogg tracks
     * through libvorbisfile); the mixer is WINMM's either way */
    HMODULE m = LoadLibraryA(strncmp(name, "mci", 3) ? "winmm.dll" : "WIN32.dll");
#else
    HMODULE m = LoadLibraryA("winmm.dll");
#endif
    FARPROC f = m ? GetProcAddress(m, name) : NULL;
    if (!f) printf("[cdaudio] no %s to call\n", name);
    return f;
}
typedef uint32_t (WINAPI* fn0)(void);
typedef uint32_t (WINAPI* fn1)(uint32_t);
typedef uint32_t (WINAPI* fn3)(uint32_t, uint32_t, uint32_t);
typedef uint32_t (WINAPI* fn4)(uint32_t, uint32_t, uint32_t, uint32_t);
typedef uint32_t (WINAPI* fn5)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
/* The real import with the guest's arguments (n of them). */
static uint32_t real_call(const char* name, int n) {
    FARPROC f = real_import(name);
    if (!f) return 11;
    switch (n) {
    case 0: return ((fn0)(void*)f)();
    case 1: return ((fn1)(void*)f)(CD_ARG(0));
    case 3: return ((fn3)(void*)f)(CD_ARG(0), CD_ARG(1), CD_ARG(2));
    case 4: return ((fn4)(void*)f)(CD_ARG(0), CD_ARG(1), CD_ARG(2), CD_ARG(3));
    default: return ((fn5)(void*)f)(CD_ARG(0), CD_ARG(1), CD_ARG(2), CD_ARG(3), CD_ARG(4));
    }
}
static int g_real_mixers = -1;   /* WINMM's mixer devices; none: MX_HANDLE is device 0 */
static int real_mixers(void) {
    if (g_real_mixers < 0) {
        FARPROC f = real_import("mixerGetNumDevs");
        g_real_mixers = f ? (int)((fn0)(void*)f)() : 0;
    }
    return g_real_mixers;
}
#define NOT_OURS(name, n, err) CD_RET(real_call(name, n), n)
#else
static int real_mixers(void) { return 0; }
#define NOT_OURS(name, n, err) CD_RET(err, n)
#endif

static int is_cd_open(uint32_t flags, const uint32_t* p, const char* type) {
    if (flags & F_OPEN_TYPE_ID) return (p[2] & 0xFFFF) == DEVTYPE_CD_AUDIO;
    return type && !strncmp(type, "cdaudio", 7);
}

void cdaudio_mciSendCommandA(void) {           /* (device, msg, flags, params) */
    uint32_t dev = CD_ARG(0), msg = CD_ARG(1), flags = CD_ARG(2), pv = CD_ARG(3);
    uint32_t* p = pv ? CD_PTR(pv) : NULL;
    const char* type = NULL;
    if (msg == C_OPEN && p && (flags & F_OPEN_TYPE) && !(flags & F_OPEN_TYPE_ID) && p[2])
        type = (const char*)CD_PTR(p[2]);
    int ours = msg == C_OPEN ? p && is_cd_open(flags, p, type) : dev == CD_DEVICE_ID;
#ifdef _WIN32
    if (g_native || !ours) {                   /* WINMM's: --music native, or not the CD (MIDI, a wave file) */
        uint32_t rc = real_call("mciSendCommandA", 4);
        if (g_native) {
            printf("[cdaudio] native %s dev %u flags 0x%X -> %u\n", cmd_name(msg), (unsigned)dev, (unsigned)flags, (unsigned)rc);
            if (msg == C_STATUS && !rc && p) printf("[cdaudio] native status item %u -> 0x%X\n", (unsigned)p[2], (unsigned)p[1]);
            if (msg == C_PLAY && p) printf("[cdaudio] native play from 0x%08X to 0x%08X\n", (unsigned)p[1], (unsigned)p[2]);
        }
        CD_RET(rc, 4);
    }
#else
    if (!ours) CD_RET(msg == C_OPEN ? E_INVALID_DEVICE_NAME : E_INVALID_DEVICE_ID, 4);   /* no other MCI device here */
#endif
    CD_RET(cdaudio_command(dev, msg, flags, p, type), 4);
}

void cdaudio_mciGetErrorStringA(void) {        /* (error, buffer, length) */
    uint32_t e = CD_ARG(0), n = CD_ARG(2);
    char* b = CD_ARG(1) ? (char*)CD_PTR(CD_ARG(1)) : NULL;
    const char* s = e == E_INVALID_DEVICE_NAME ? "The specified device is not open or is not recognized by MCI."
                  : e == E_OUTOFRANGE ? "The specified parameter is out of range for the specified command."
                  : e == E_INVALID_DEVICE_ID ? "Invalid device ID. Use the ID given to the device when the device was opened."
                  : NULL;
#ifdef _WIN32
    if (g_native || !s) CD_RET(real_call("mciGetErrorStringA", 3), 3);
#endif
    if (!b || !n) CD_RET(0, 3);
    snprintf(b, n, "%s", s ? s : "Unknown MCI error.");
    CD_RET(1, 3);
}

void cdaudio_mixerGetNumDevs(void) { int n = real_mixers(); CD_RET(n ? n : 1, 0); }
void cdaudio_mixerOpen(void) {                 /* (&hmx, id, callback, instance, flags) */
    if (real_mixers()) NOT_OURS("mixerOpen", 5, 0u);
    uint32_t ph = CD_ARG(0), id = CD_ARG(1);
    if (id != 0) CD_RET(2, 5);                 /* MMSYSERR_BADDEVICEID */
    if (ph) *CD_PTR(ph) = MX_HANDLE;
    CD_RET(0, 5);
}
void cdaudio_mixerClose(void) {
    if (CD_ARG(0) == MX_HANDLE && !real_mixers()) CD_RET(0, 1);
    NOT_OURS("mixerClose", 1, 5u);             /* MMSYSERR_INVALHANDLE */
}
void cdaudio_mixerGetLineInfoA(void) {         /* (h, &line, flags) */
    uint32_t rc = CD_ARG(1) ? mixer_line(CD_PTR(CD_ARG(1)), CD_ARG(2)) : MX_NOT_OURS;
    if (rc != MX_NOT_OURS) CD_RET(rc, 3);
    NOT_OURS("mixerGetLineInfoA", 3, 1024u);   /* MIXERR_INVALLINE */
}
void cdaudio_mixerGetLineControlsA(void) {     /* (h, &controls, flags) */
    uint32_t rc = CD_ARG(1) ? mixer_controls(CD_PTR(CD_ARG(1)), CD_ARG(2)) : MX_NOT_OURS;
    if (rc != MX_NOT_OURS) CD_RET(rc, 3);
    NOT_OURS("mixerGetLineControlsA", 3, 1024u);
}
void cdaudio_mixerGetControlDetailsA(void) {   /* (h, &details, flags) */
    uint32_t rc = CD_ARG(1) ? mixer_details(CD_PTR(CD_ARG(1)), 0) : MX_NOT_OURS;
    if (rc != MX_NOT_OURS) CD_RET(rc, 3);
    NOT_OURS("mixerGetControlDetailsA", 3, 1025u);   /* MIXERR_INVALCONTROL */
}
void cdaudio_mixerSetControlDetails(void) {    /* (h, &details, flags) */
    uint32_t rc = CD_ARG(1) ? mixer_details(CD_PTR(CD_ARG(1)), 1) : MX_NOT_OURS;
    if (rc != MX_NOT_OURS) CD_RET(rc, 3);
    NOT_OURS("mixerSetControlDetails", 3, 1025u);
}
#endif
