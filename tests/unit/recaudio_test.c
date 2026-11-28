/* src/runtime/recaudio.c without the game or a sound card: its DirectSound
 * proxy over a fake DirectSound here (buffers whose play cursor moves with
 * the clock, as a real one's does), the stream tap, and the WAV it writes.
 * The clock is simulated (recaudio_test_clock_ms) and the test ticks the
 * mixer itself every 10 ms of it, as its thread does: no case waits on the
 * real clock, so a busy machine cannot move a sound.
 *
 *   one-shot   a tone played 200 ms in is in the WAV from there, for its length
 *   gains      volume -10000 is silent; pan +10000 leaves only the right channel
 *   rewrite    a looping buffer rewritten (Lock, Unlock) while it plays: the WAV
 *              follows, as a game streaming its music into one does
 *   stream     a tap chunk at 22050 Hz mono heard 300 ms from now is there,
 *              resampled, at that time; a pause holds it
 *   drift      a stream whose player's device runs 3% fast is kept where the
 *              player says it is heard
 *   effects    the sound slider's gain: on the real buffer's volume (the game
 *              still reads its own), on buffers made after it, in the WAV;
 *              its bottom is silence; every buffer can take it
 *   length     the sound is cut (or padded) to exactly the video's frames
 *
 *   py -3 tools\test.py --level unit        (builds and runs every unit)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "recaudio.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the simulated clock ------------------------------------------------------- */
static double g_now = 1000.0;     /* ms; not 0, as a real clock's would not be */
static double t0;                 /* the recording's start */
static double sim_clock(void) { return g_now; }
/* Time passes to t0 + ms, the mixer ticking every 10 ms of it. */
static void run_to(double ms) {
    while (g_now < t0 + ms) {
        g_now = g_now + 10 < t0 + ms ? g_now + 10 : t0 + ms;
        recaudio_tick();
    }
}
static double at(void) { return (g_now - t0) / 1000; }   /* now, in seconds of the recording */

/* ---- a fake DirectSound ---------------------------------------------------------- */
typedef struct {
    const IDirectSoundBufferVtbl* lpVtbl;
    LONG refs;
    WAVEFORMATEX wf;
    DWORD bytes, flags;
    uint8_t* mem;
    int playing, looping;
    LONG volume;                  /* what the proxy set on the real buffer */
    double t_play;                /* recaudio_clock_ms at Play, less the start position's time */
} Fake;
#define F ((Fake*)This)

static DWORD cursor(Fake* f) {
    double frames = (recaudio_clock_ms() - f->t_play) * f->wf.nSamplesPerSec / 1000.0;
    DWORD total = f->bytes / f->wf.nBlockAlign;
    if (!f->playing) return 0;
    if (f->looping) return ((DWORD)frames % total) * f->wf.nBlockAlign;
    return frames >= total ? 0 : (DWORD)frames * f->wf.nBlockAlign;
}
static HRESULT STDMETHODCALLTYPE f_QI(IDirectSoundBuffer* This, REFIID r, LPVOID* o) { (void)r; *o = This; F->refs++; return S_OK; }
static ULONG STDMETHODCALLTYPE f_AddRef(IDirectSoundBuffer* This) { return (ULONG)++F->refs; }
static ULONG STDMETHODCALLTYPE f_Release(IDirectSoundBuffer* This) {
    LONG n = --F->refs;
    if (!n) { free(F->mem); free(F); }
    return (ULONG)n;
}
static HRESULT STDMETHODCALLTYPE f_GetCaps(IDirectSoundBuffer* This, LPDSBCAPS c) { c->dwFlags = F->flags; c->dwBufferBytes = F->bytes; return S_OK; }
static HRESULT STDMETHODCALLTYPE f_GetCurrentPosition(IDirectSoundBuffer* This, LPDWORD p, LPDWORD w) {
    DWORD c = cursor(F);
    if (p) *p = c;
    if (w) *w = (c + 4410 * F->wf.nBlockAlign / 10) % F->bytes;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE f_GetFormat(IDirectSoundBuffer* This, LPWAVEFORMATEX f, DWORD n, LPDWORD got) {
    memcpy(f, &F->wf, n < sizeof F->wf ? n : sizeof F->wf);
    if (got) *got = sizeof F->wf;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE f_GetStatus(IDirectSoundBuffer* This, LPDWORD s) {
    if (F->playing && !F->looping &&
        (recaudio_clock_ms() - F->t_play) * F->wf.nSamplesPerSec / 1000.0 >= F->bytes / F->wf.nBlockAlign) F->playing = 0;
    *s = F->playing ? DSBSTATUS_PLAYING | (F->looping ? DSBSTATUS_LOOPING : 0) : 0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE f_Lock(IDirectSoundBuffer* This, DWORD off, DWORD n, LPVOID* p1, LPDWORD n1, LPVOID* p2, LPDWORD n2, DWORD flags) {
    if (flags & DSBLOCK_ENTIREBUFFER) off = 0, n = F->bytes;
    *p1 = F->mem + off;
    *n1 = off + n <= F->bytes ? n : F->bytes - off;
    if (p2) *p2 = off + n > F->bytes ? F->mem : NULL;
    if (n2) *n2 = off + n > F->bytes ? off + n - F->bytes : 0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE f_Unlock(IDirectSoundBuffer* This, LPVOID p1, DWORD n1, LPVOID p2, DWORD n2) { (void)This, (void)p1, (void)n1, (void)p2, (void)n2; return S_OK; }
static HRESULT STDMETHODCALLTYPE f_Play(IDirectSoundBuffer* This, DWORD a, DWORD b, DWORD flags) {
    (void)a, (void)b;
    F->playing = 1, F->looping = !!(flags & DSBPLAY_LOOPING), F->t_play = recaudio_clock_ms();
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE f_Stop(IDirectSoundBuffer* This) { F->playing = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE f_SetVolume(IDirectSoundBuffer* This, LONG v) {
    if (!(F->flags & DSBCAPS_CTRLVOLUME)) return DSERR_CONTROLUNAVAIL;
    if (v < DSBVOLUME_MIN || v > DSBVOLUME_MAX) return DSERR_INVALIDPARAM;
    F->volume = v;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE f_GetVolume(IDirectSoundBuffer* This, LPLONG v) { *v = F->volume; return S_OK; }
static HRESULT STDMETHODCALLTYPE f_ok1(IDirectSoundBuffer* This, LONG v) { (void)This, (void)v; return S_OK; }
static HRESULT STDMETHODCALLTYPE f_okd(IDirectSoundBuffer* This, DWORD v) { (void)This, (void)v; return S_OK; }
static HRESULT STDMETHODCALLTYPE f_notimpl(IDirectSoundBuffer* This) { (void)This; return E_NOTIMPL; }
#undef F

static IDirectSoundBufferVtbl g_fvt;
static void fake_vtbl(void) {
    void** v = (void**)&g_fvt;
    for (int i = 0; i < (int)(sizeof g_fvt / sizeof(void*)); i++) v[i] = (void*)f_notimpl;
    g_fvt.QueryInterface = f_QI, g_fvt.AddRef = f_AddRef, g_fvt.Release = f_Release, g_fvt.GetCaps = f_GetCaps;
    g_fvt.GetCurrentPosition = f_GetCurrentPosition, g_fvt.GetFormat = f_GetFormat, g_fvt.GetStatus = f_GetStatus;
    g_fvt.Lock = f_Lock, g_fvt.Unlock = f_Unlock, g_fvt.Play = f_Play, g_fvt.Stop = f_Stop;
    g_fvt.SetVolume = f_SetVolume, g_fvt.GetVolume = f_GetVolume;
    g_fvt.SetPan = f_ok1, g_fvt.SetFrequency = f_okd, g_fvt.SetCurrentPosition = f_okd;
}

typedef struct { const IDirectSoundVtbl* lpVtbl; LONG refs; } FakeDS;
static Fake* g_last_real;         /* the real buffer the last CreateSoundBuffer made */
static HRESULT STDMETHODCALLTYPE ds_Create(IDirectSound* This, LPCDSBUFFERDESC d, LPDIRECTSOUNDBUFFER* out, LPUNKNOWN u) {
    (void)This, (void)u;
    Fake* f = (Fake*)calloc(1, sizeof *f);
    f->lpVtbl = &g_fvt, f->refs = 1;
    f->wf = *d->lpwfxFormat;
    f->bytes = d->dwBufferBytes;
    f->flags = d->dwFlags;
    f->mem = (uint8_t*)calloc(1, f->bytes);
    *out = (IDirectSoundBuffer*)f;
    g_last_real = f;
    return S_OK;
}
static ULONG STDMETHODCALLTYPE ds_AddRef(IDirectSound* This) { return (ULONG)++((FakeDS*)This)->refs; }
static ULONG STDMETHODCALLTYPE ds_Release(IDirectSound* This) { return (ULONG)--((FakeDS*)This)->refs; }
static HRESULT STDMETHODCALLTYPE ds_notimpl(IDirectSound* This) { (void)This; return E_NOTIMPL; }
static IDirectSoundVtbl g_dsvt;

/* ---- helpers ----------------------------------------------------------------------- */
static IDirectSound* g_ds;
static IDirectSoundBuffer* make_buffer(int rate, int channels, double secs, double hz, double amp, DWORD flags) {
    WAVEFORMATEX wf = { WAVE_FORMAT_PCM, (WORD)channels, (DWORD)rate, (DWORD)(rate * 2 * channels), (WORD)(2 * channels), 16, 0 };
    DSBUFFERDESC d = { sizeof d, flags, (DWORD)(rate * secs) * wf.nBlockAlign, 0, &wf };
    IDirectSoundBuffer* b = NULL;
    void *p1, *p2;
    DWORD n1, n2;
    IDirectSound_CreateSoundBuffer(g_ds, &d, &b, NULL);
    IDirectSoundBuffer_Lock(b, 0, 0, &p1, &n1, &p2, &n2, DSBLOCK_ENTIREBUFFER);
    for (DWORD i = 0; i < n1 / 2; i++) ((int16_t*)p1)[i] = (int16_t)(amp * 32767 * sin(2 * 3.14159265 * hz * (i / channels) / rate));
    IDirectSoundBuffer_Unlock(b, p1, n1, p2, n2);
    return b;
}
static IDirectSoundBuffer* tone(int rate, int channels, double secs, double hz, double amp) {
    return make_buffer(rate, channels, secs, hz, amp, DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN);
}

static int16_t* g_pcm;
static long g_frames;
static int read_wav(const char* path) {
    FILE* f = fopen(path, "rb");
    uint8_t h[44];
    if (!f) return 0;
    if (fread(h, 1, 44, f) != 44 || memcmp(h, "RIFF", 4) || memcmp(h + 36, "data", 4)) { fclose(f); return 0; }
    uint32_t n;
    memcpy(&n, h + 40, 4);
    g_frames = (long)(n / 4);
    free(g_pcm);
    g_pcm = (int16_t*)malloc(n + 4);
    long got = (long)fread(g_pcm, 4, (size_t)g_frames, f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    return got == g_frames && size == 44 + (long)n;
}
/* rms of a channel (0, 1, or 2 for both) over [a, b) seconds */
static double rms(double a, double b, int ch) {
    long i0 = (long)(a * 44100), i1 = (long)(b * 44100);
    double s = 0;
    long n = 0;
    if (i0 < 0) i0 = 0;
    if (i1 > g_frames) i1 = g_frames;
    for (long i = i0; i < i1; i++)
        for (int c = 0; c < 2; c++)
            if (ch == 2 || ch == c) { s += (double)g_pcm[2 * i + c] * g_pcm[2 * i + c]; n++; }
    return n ? sqrt(s / n) / 32768.0 : 0;
}
/* the first sample in [a, b) seconds louder than thr */
static double onset(double a, double b, double thr) {
    for (long i = (long)(a * 44100); i < (long)(b * 44100) && i < g_frames; i++)
        if (fabs(g_pcm[2 * i] / 32768.0) > thr || fabs(g_pcm[2 * i + 1] / 32768.0) > thr) return i / 44100.0;
    return -1;
}
/* zero crossings a second, left channel, over [a, b) */
static double pitch(double a, double b) {
    long n = 0;
    for (long i = (long)(a * 44100) + 1; i < (long)(b * 44100) && i < g_frames; i++)
        if ((g_pcm[2 * i - 2] < 0) != (g_pcm[2 * i] < 0)) n++;
    return n / 2.0 / (b - a);
}
#define ONSET_TOL 0.002           /* s: a tone's first sample over the threshold, past its zero crossing */

/* ---- the cases: each plays its part of the recording, then checks the WAV ---- */
static double t_a;
static void run_oneshot(void) {   /* 200 .. 500 ms */
    IDirectSoundBuffer* a = tone(22050, 1, 0.30, 441, 0.5);
    run_to(200);
    t_a = at();
    IDirectSoundBuffer_Play(a, 0, 0, 0);
    run_to(700);
    IDirectSoundBuffer_Release(a);
}
static void check_oneshot(void) {
    double on = onset(0, 0.6, 0.05);
    printf("  one-shot: played at %.3f s, heard from %.3f s; rms %.3f, %.0f Hz\n", t_a, on, rms(t_a + 0.02, t_a + 0.28, 2), pitch(t_a + 0.02, t_a + 0.28));
    CHECK(on >= t_a && on - t_a < ONSET_TOL, "the one-shot is heard at %.3f s, played at %.3f s", on, t_a);
    CHECK(rms(t_a + 0.02, t_a + 0.28, 2) > 0.3, "the one-shot is quiet");
    CHECK(fabs(pitch(t_a + 0.02, t_a + 0.28) - 441) < 10, "the one-shot is at %.0f Hz, not 441", pitch(t_a + 0.02, t_a + 0.28));
    CHECK(rms(t_a + 0.31, 0.7, 2) < 0.001, "sound after the one-shot ended");
}

static double t_q, t_p;
static void run_gains(void) {     /* 700 .. 900 silent, 1000 .. 1200 right */
    IDirectSoundBuffer* q = tone(44100, 2, 0.20, 882, 0.5);
    IDirectSoundBuffer* p = tone(44100, 1, 0.20, 441, 0.5);
    IDirectSoundBuffer_SetVolume(q, -10000);
    IDirectSoundBuffer_SetPan(p, 10000);
    t_q = at();
    IDirectSoundBuffer_Play(q, 0, 0, 0);
    run_to(1000);
    t_p = at();
    IDirectSoundBuffer_Play(p, 0, 0, 0);
    run_to(1300);
    IDirectSoundBuffer_Release(q), IDirectSoundBuffer_Release(p);
}
static void check_gains(void) {
    CHECK(rms(t_q, t_q + 0.2, 2) < 0.001, "volume -10000 is heard");
    CHECK(rms(t_p + 0.02, t_p + 0.18, 1) > 0.3 && rms(t_p + 0.02, t_p + 0.18, 0) < 0.001,
          "pan +10000: left %.3f, right %.3f", rms(t_p + 0.02, t_p + 0.18, 0), rms(t_p + 0.02, t_p + 0.18, 1));
}

static double t_lp, t_rw;
static void run_rewrite(void) {   /* 1300 .. 1800 tone, then silence to 2100 */
    IDirectSoundBuffer* lp = tone(11025, 1, 0.25, 220.5, 0.5);
    t_lp = at();
    IDirectSoundBuffer_Play(lp, 0, 0, DSBPLAY_LOOPING);
    run_to(1800);
    void *p1, *p2;
    DWORD n1, n2;
    IDirectSoundBuffer_Lock(lp, 0, 0, &p1, &n1, &p2, &n2, DSBLOCK_ENTIREBUFFER);
    memset(p1, 0, n1);
    IDirectSoundBuffer_Unlock(lp, p1, n1, p2, n2);
    t_rw = at();
    run_to(2100);
    IDirectSoundBuffer_Stop(lp);
    IDirectSoundBuffer_Release(lp);
}
static void check_rewrite(void) {
    printf("  looping: rms %.3f before the rewrite, %.4f after\n", rms(t_lp + 0.02, t_rw - 0.02, 2), rms(t_rw + 0.03, 2.05, 2));
    CHECK(rms(t_lp + 0.02, t_rw - 0.02, 2) > 0.3, "the looping buffer is quiet (it loops past its 250 ms)");
    CHECK(rms(t_rw + 0.03, 2.05, 2) < 0.001, "the rewritten (silent) looping buffer is heard");
}

static double t_s, t_pause, t_resume;
static void run_stream(void) {    /* 200 ms of 22050 Hz mono heard from 2.4 s, paused 2.45 .. 2.75 */
    int s = recaudio_stream_open(22050, 1);
    CHECK(s >= 0, "no stream");
    static int16_t chunk[22050 / 5];
    for (int i = 0; i < 22050 / 5; i++) chunk[i] = (int16_t)(0.5 * 32767 * sin(2 * 3.14159265 * 1102.5 * i / 22050));
    t_s = at() + 0.3;
    recaudio_stream_write(s, chunk, 22050 / 5, 300);
    run_to(2450);
    t_pause = at();
    recaudio_stream_pause(s, 1);
    run_to(2750);
    t_resume = at();
    recaudio_stream_pause(s, 0);
    run_to(3200);
    recaudio_stream_close(s);
}
static void check_stream(void) {
    double son = onset(2.2, 3.3, 0.05), left = 0.2 - (t_pause - t_s);
    printf("  stream: due at %.3f s, heard from %.3f s; %.0f Hz\n", t_s, son, pitch(t_resume + 0.01, t_resume + left - 0.01));
    CHECK(son >= t_s && son - t_s < ONSET_TOL, "the stream is heard at %.3f s, due at %.3f s", son, t_s);
    CHECK(fabs(pitch(t_resume + 0.01, t_resume + left - 0.01) - 1102.5) < 25, "the stream is not at 1102.5 Hz after the pause");
    CHECK(rms(t_pause + 0.01, t_resume - 0.01, 2) < 0.001, "the paused stream is heard");
    CHECK(rms(t_resume + 0.01, t_resume + left - 0.01, 2) > 0.3, "the stream does not carry on after the pause");
    CHECK(rms(t_resume + left + 0.01, 3.33, 2) < 0.001, "sound after the stream ended");
}

/* A player whose device runs 3% fast, kept 200 ms ahead of it, for 4 s
 * (3.4 .. 7.4 s); its last chunk is a tone. Appended blindly the stream would
 * be heard 120 ms late by then: it is kept within 60 ms of when the player
 * says. */
static double t_beep;
static void run_drift(void) {
    int d = recaudio_stream_open(44100, 2);
    static int16_t quiet[2 * 1102], beep[2 * 1102];
    for (int i = 0; i < 1102; i++) beep[2 * i] = beep[2 * i + 1] = (int16_t)(0.5 * 32767 * sin(2 * 3.14159265 * 1000 * i / 44100.0));
    run_to(3400);
    double tw0 = g_now;
    int64_t sent = 0;
    for (int last = 0; !last; ) {
        run_to(g_now - t0 + 10);
        double played = (g_now - tw0) * 44.1 * 1.03;
        last = at() > 7.4;
        while (sent - played < 8820 || last) {
            double in_ms = (sent - played) / 44.1;
            if (last) t_beep = at() + in_ms / 1000;
            recaudio_stream_write(d, last ? beep : quiet, 1102, in_ms);
            sent += 1102;
            if (last) break;
        }
    }
    run_to(7700);
    recaudio_stream_close(d);
}
static void check_drift(void) {
    double bon = onset(7.3, 7.8, 0.05);
    printf("  drift: the last chunk due at %.3f s, heard from %.3f s (3%% over 4 s: 120 ms uncorrected)\n", t_beep, bon);
    CHECK(bon >= 0 && fabs(bon - t_beep) < 0.06 + ONSET_TOL, "the drifting stream's last chunk is heard at %.3f s, due at %.3f s", bon, t_beep);
    CHECK(rms(3.4, 7.3, 2) < 0.001, "the drifting stream's silence is heard");
}

/* The sound slider: a looping tone at full from 7.8 s, the effects' gain
 * at 1/8 from 8.2 s, at the slider's bottom from 8.6 s, stopped at 9.0 s. */
static LONG v_eighth, v_bottom, v_game, v_new, v_back;
static DWORD new_flags;
static void run_effects(void) {
    IDirectSoundBuffer* e = tone(44100, 2, 0.5, 1000, 0.5);
    IDirectSoundBuffer_SetVolume(e, -600);                    /* the game's own: -6 dB */
    Fake* real = g_last_real;
    run_to(7800);
    IDirectSoundBuffer_Play(e, 0, 0, DSBPLAY_LOOPING);
    run_to(8200);
    recaudio_set_effects_gain(0.125f);
    v_eighth = real->volume;
    IDirectSoundBuffer_GetVolume(e, &v_game);
    IDirectSoundBuffer* late = make_buffer(22050, 1, 0.1, 441, 0.5, 0);   /* no DSBCAPS_CTRLVOLUME asked */
    v_new = g_last_real->volume, new_flags = g_last_real->flags;
    run_to(8600);
    recaudio_set_effects_gain(0.01f * 0.01f * 0.01f);        /* the bottom step: 1%, cubed */
    v_bottom = real->volume;
    run_to(9000);
    IDirectSoundBuffer_Stop(e);
    recaudio_set_effects_gain(1.0f);
    v_back = real->volume;
    IDirectSoundBuffer_Release(e), IDirectSoundBuffer_Release(late);
}
static void check_effects(void) {
    double full = rms(7.85, 8.15, 2), eighth = rms(8.25, 8.55, 2), bottom = rms(8.65, 8.95, 2);
    printf("  effects: rms %.4f at full, %.4f at 1/8 (%.1f dB), %.6f at the bottom\n", full, eighth, 20 * log10(eighth / full), bottom);
    CHECK(fabs(full - 0.5 / sqrt(2) * pow(10, -0.3)) < 0.01, "the -6 dB tone at full is %.4f", full);
    CHECK(fabs(20 * log10(eighth / full) - 20 * log10(0.125)) < 0.2, "at 1/8 the tone is %.1f dB, not -18.1", 20 * log10(eighth / full));
    CHECK(bottom == 0, "the slider's bottom is heard: rms %g", bottom);
    CHECK(v_eighth == -600 - 1806, "the real buffer at 1/8 is at %ld, not -2406", v_eighth);
    CHECK(v_game == -600, "the game reads back %ld, not its own -600", v_game);
    CHECK(v_bottom == DSBVOLUME_MIN, "the real buffer at the bottom is at %ld, not -10000", v_bottom);
    CHECK(v_back == -600, "back at full the real buffer is at %ld, not -600", v_back);
    CHECK((new_flags & DSBCAPS_CTRLVOLUME) && v_new == -1806, "a buffer made at 1/8: flags %lX, volume %ld", new_flags, v_new);
}

/* Even without --record (before recaudio_init), the proxy still exists to
 * carry the sound slider's effects gain on secondary buffers. */
static void test_effects_without_record(void) {
    static const WAVEFORMATEX wf = { WAVE_FORMAT_PCM, 2, 44100, 44100 * 4, 4, 16, 0 };
    FakeDS real = { &g_dsvt, 1 };
    IDirectSound* ds = NULL;
    IDirectSoundBuffer* b = NULL;
    DSBUFFERDESC d = { sizeof d, 0, 4410, 0, (LPWAVEFORMATEX)&wf };
    LONG v_game = 0;
    CHECK(!recaudio_on(), "expected recaudio_on() == 0 before recaudio_init");
    CHECK(recaudio_wrap(&real, (void**)&ds) == S_OK && ds, "wrap without --record");
    CHECK(IDirectSound_CreateSoundBuffer(ds, &d, &b, NULL) == S_OK && b, "create buffer without --record");
    Fake* fb = g_last_real;
    CHECK((fb->flags & DSBCAPS_CTRLVOLUME) != 0, "buffer without --record keeps DSBCAPS_CTRLVOLUME");
    IDirectSoundBuffer_SetVolume(b, -600);
    recaudio_set_effects_gain(0.125f);
    CHECK(fb->volume == -600 - 1806, "without --record at 1/8: real volume %ld, not -2406", fb->volume);
    CHECK(IDirectSoundBuffer_GetVolume(b, &v_game) == S_OK && v_game == -600,
          "without --record GetVolume returned %ld, not -600", v_game);
    recaudio_set_effects_gain(0.01f * 0.01f * 0.01f);
    CHECK(fb->volume == DSBVOLUME_MIN, "without --record at bottom: real volume %ld, not %ld", fb->volume, (long)DSBVOLUME_MIN);
    recaudio_set_effects_gain(1.0f);
    CHECK(fb->volume == -600, "without --record back at full: real volume %ld, not -600", fb->volume);
    IDirectSoundBuffer_Release(b);
    IDirectSound_Release(ds);
}

int main(void) {
    char dir[MAX_PATH], mp4[MAX_PATH], wav[MAX_PATH + 8];
    GetTempPathA(MAX_PATH, dir);
    _snprintf(mp4, MAX_PATH, "%srecaudio_test_%lu.mp4", dir, GetCurrentProcessId());
    _snprintf(wav, sizeof wav, "%s.wav", mp4);
    fake_vtbl();
    {
        void** v = (void**)&g_dsvt;
        for (int i = 0; i < (int)(sizeof g_dsvt / sizeof(void*)); i++) v[i] = (void*)ds_notimpl;
        g_dsvt.AddRef = ds_AddRef, g_dsvt.Release = ds_Release, g_dsvt.CreateSoundBuffer = ds_Create;
    }
    static FakeDS real = { &g_dsvt, 1 };

    CHECK(recaudio_stream_open(44100, 2) == -1, "a stream opens without --record");
    test_effects_without_record();
    recaudio_test_clock_ms = sim_clock;
    recaudio_init();
    CHECK(recaudio_wrap(&real, (void**)&g_ds) == S_OK && g_ds && g_ds != (IDirectSound*)&real, "no proxy");

    t0 = g_now;
    recaudio_start(t0, mp4);
    run_oneshot();
    run_gains();
    run_rewrite();
    run_stream();
    run_drift();
    run_effects();
    run_to(10000);
    recaudio_finish(300, 30);                                     /* 300 frames: 10 s exactly */

    CHECK(read_wav(wav), "no WAV at %s, or its sizes are wrong", wav);
    CHECK(g_frames == 441000, "%ld frames, not 300 video frames' 441000", g_frames);
    check_oneshot();
    check_gains();
    check_rewrite();
    check_stream();
    check_drift();
    check_effects();
    DeleteFileA(wav);
    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
