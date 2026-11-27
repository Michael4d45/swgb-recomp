/* src/runtime/recaudio.c without the game or a sound card: its DirectSound
 * proxy over a fake DirectSound here (buffers whose play cursor moves with
 * the clock, as a real one's does), the stream tap, and the WAV it writes.
 *
 *   one-shot   a tone played 200 ms in is in the WAV from there, for its length
 *   gains      volume -10000 is silent; pan +10000 leaves only the right channel
 *   rewrite    a looping buffer rewritten (Lock, Unlock) while it plays: the WAV
 *              follows, as a game streaming its music into one does
 *   stream     a tap chunk at 22050 Hz mono heard 300 ms from now is there,
 *              resampled, at that time; a pause holds it
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

/* ---- a fake DirectSound ---------------------------------------------------------- */
typedef struct {
    const IDirectSoundBufferVtbl* lpVtbl;
    LONG refs;
    WAVEFORMATEX wf;
    DWORD bytes;
    uint8_t* mem;
    int playing, looping;
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
static HRESULT STDMETHODCALLTYPE f_GetCaps(IDirectSoundBuffer* This, LPDSBCAPS c) { c->dwFlags = 0; c->dwBufferBytes = F->bytes; return S_OK; }
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
    g_fvt.SetVolume = f_ok1, g_fvt.SetPan = f_ok1, g_fvt.SetFrequency = f_okd, g_fvt.SetCurrentPosition = f_okd;
}

typedef struct { const IDirectSoundVtbl* lpVtbl; LONG refs; } FakeDS;
static HRESULT STDMETHODCALLTYPE ds_Create(IDirectSound* This, LPCDSBUFFERDESC d, LPDIRECTSOUNDBUFFER* out, LPUNKNOWN u) {
    (void)This, (void)u;
    Fake* f = (Fake*)calloc(1, sizeof *f);
    f->lpVtbl = &g_fvt, f->refs = 1;
    f->wf = *d->lpwfxFormat;
    f->bytes = d->dwBufferBytes;
    f->mem = (uint8_t*)calloc(1, f->bytes);
    *out = (IDirectSoundBuffer*)f;
    return S_OK;
}
static ULONG STDMETHODCALLTYPE ds_AddRef(IDirectSound* This) { return (ULONG)++((FakeDS*)This)->refs; }
static ULONG STDMETHODCALLTYPE ds_Release(IDirectSound* This) { return (ULONG)--((FakeDS*)This)->refs; }
static HRESULT STDMETHODCALLTYPE ds_notimpl(IDirectSound* This) { (void)This; return E_NOTIMPL; }
static IDirectSoundVtbl g_dsvt;

/* ---- helpers ----------------------------------------------------------------------- */
static IDirectSound* g_ds;
static IDirectSoundBuffer* tone(int rate, int channels, double secs, double hz, double amp) {
    WAVEFORMATEX wf = { WAVE_FORMAT_PCM, (WORD)channels, (DWORD)rate, (DWORD)(rate * 2 * channels), (WORD)(2 * channels), 16, 0 };
    DSBUFFERDESC d = { sizeof d, DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN, (DWORD)(rate * secs) * wf.nBlockAlign, 0, &wf };
    IDirectSoundBuffer* b = NULL;
    void *p1, *p2;
    DWORD n1, n2;
    IDirectSound_CreateSoundBuffer(g_ds, &d, &b, NULL);
    IDirectSoundBuffer_Lock(b, 0, 0, &p1, &n1, &p2, &n2, DSBLOCK_ENTIREBUFFER);
    for (DWORD i = 0; i < n1 / 2; i++) ((int16_t*)p1)[i] = (int16_t)(amp * 32767 * sin(2 * 3.14159265 * hz * (i / channels) / rate));
    IDirectSoundBuffer_Unlock(b, p1, n1, p2, n2);
    return b;
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
static void wait_until(double t0, double ms) { while (recaudio_clock_ms() - t0 < ms) Sleep(1); }

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
    recaudio_init();
    CHECK(recaudio_wrap(&real, (void**)&g_ds) == S_OK && g_ds && g_ds != (IDirectSound*)&real, "no proxy");

    /* the buffers are made (their samples copied at Unlock) before the recording starts */
    IDirectSoundBuffer* a = tone(22050, 1, 0.30, 441, 0.5);      /* one shot */
    IDirectSoundBuffer* q = tone(44100, 2, 0.20, 882, 0.5);      /* silent: volume */
    IDirectSoundBuffer* p = tone(44100, 1, 0.20, 441, 0.5);      /* right only: pan */
    IDirectSoundBuffer* lp = tone(11025, 1, 0.25, 220.5, 0.5);   /* looping, then rewritten */
    IDirectSoundBuffer_SetVolume(q, -10000);
    IDirectSoundBuffer_SetPan(p, 10000);

    double t0 = recaudio_clock_ms();
    recaudio_start(t0, mp4);
    wait_until(t0, 200);
    double t_a = recaudio_clock_ms() - t0;
    IDirectSoundBuffer_Play(a, 0, 0, 0);                          /* 200 .. 500 ms */
    wait_until(t0, 700);
    double t_q = recaudio_clock_ms() - t0;
    IDirectSoundBuffer_Play(q, 0, 0, 0);                          /* 700 .. 900: silent */
    wait_until(t0, 1000);
    double t_p = recaudio_clock_ms() - t0;
    IDirectSoundBuffer_Play(p, 0, 0, 0);                          /* 1000 .. 1200: right */
    wait_until(t0, 1300);
    double t_lp = recaudio_clock_ms() - t0;
    IDirectSoundBuffer_Play(lp, 0, 0, DSBPLAY_LOOPING);           /* 1300 .. 1800 tone, then silence */
    wait_until(t0, 1800);
    {
        void *p1, *p2;
        DWORD n1, n2;
        IDirectSoundBuffer_Lock(lp, 0, 0, &p1, &n1, &p2, &n2, DSBLOCK_ENTIREBUFFER);
        memset(p1, 0, n1);
        IDirectSoundBuffer_Unlock(lp, p1, n1, p2, n2);
    }
    double t_rw = recaudio_clock_ms() - t0;
    wait_until(t0, 2100);
    IDirectSoundBuffer_Stop(lp);

    /* the tap: 200 ms of 22050 Hz mono, heard 300 ms from now (2.4 .. 2.6 s) */
    int s = recaudio_stream_open(22050, 1);
    CHECK(s >= 0, "no stream");
    static int16_t chunk[22050 / 5];
    for (int i = 0; i < 22050 / 5; i++) chunk[i] = (int16_t)(0.5 * 32767 * sin(2 * 3.14159265 * 1102.5 * i / 22050));
    double t_s = recaudio_clock_ms() - t0 + 300;
    recaudio_stream_write(s, chunk, 22050 / 5, 300);
    wait_until(t0, 2450);
    double t_pause = recaudio_clock_ms() - t0;
    recaudio_stream_pause(s, 1);                                  /* held from 2.45 to 2.75 */
    wait_until(t0, 2750);
    double t_resume = recaudio_clock_ms() - t0;
    recaudio_stream_pause(s, 0);
    wait_until(t0, 3200);
    recaudio_stream_close(s);

    IDirectSoundBuffer_Release(a), IDirectSoundBuffer_Release(q), IDirectSoundBuffer_Release(p), IDirectSoundBuffer_Release(lp);

    /* drift: a player whose device runs 3% fast, kept 200 ms ahead of it, for 4 s
     * (3.4 .. 7.4 s); its last chunk is a tone. Appended blindly the stream would
     * be heard 120 ms late by then: it is kept within 60 ms of when the player
     * says. */
    int d = recaudio_stream_open(44100, 2);
    static int16_t quiet[2 * 1102], beep[2 * 1102];
    for (int i = 0; i < 1102; i++) beep[2 * i] = beep[2 * i + 1] = (int16_t)(0.5 * 32767 * sin(2 * 3.14159265 * 1000 * i / 44100.0));
    wait_until(t0, 3400);
    double tw0 = recaudio_clock_ms(), t_beep = 0;
    int64_t sent = 0;
    for (int last = 0; !last; ) {
        Sleep(10);
        double played = (recaudio_clock_ms() - tw0) * 44.1 * 1.03;
        last = recaudio_clock_ms() - t0 > 7400;
        while (sent - played < 8820 || last) {
            double in_ms = (sent - played) / 44.1;
            if (last) t_beep = recaudio_clock_ms() - t0 + in_ms;
            recaudio_stream_write(d, last ? beep : quiet, 1102, in_ms);
            sent += 1102;
            if (last) break;
        }
    }
    wait_until(t0, 8000);
    recaudio_finish(250, 30);                                     /* 250 frames: 8.333 s exactly */

    CHECK(read_wav(wav), "no WAV at %s, or its sizes are wrong", wav);
    CHECK(g_frames == 367500, "%ld frames, not 250 video frames' 367500", g_frames);
    double bon = onset(7.3, 8.3, 0.05);
    printf("  drift: the last chunk due at %.3f s, heard from %.3f s (3%% over 4 s: 120 ms uncorrected)\n", t_beep / 1000, bon);
    CHECK(bon >= 0 && fabs(bon - t_beep / 1000) < 0.07, "the drifting stream's last chunk is heard at %.3f s, due at %.3f s", bon, t_beep / 1000);
    CHECK(rms(3.4, 7.3, 2) < 0.001, "the drifting stream's silence is heard");
    double on = onset(0, 0.6, 0.05);
    printf("  one-shot: played at %.3f s, heard from %.3f s; rms %.3f, %.0f Hz\n", t_a / 1000, on, rms(t_a / 1000 + 0.02, t_a / 1000 + 0.28, 2), pitch(0.25, 0.45));
    CHECK(on >= 0 && fabs(on - t_a / 1000) < 0.025, "the one-shot is heard at %.3f s, played at %.3f s", on, t_a / 1000);
    CHECK(rms(t_a / 1000 + 0.02, t_a / 1000 + 0.28, 2) > 0.3, "the one-shot is quiet");
    CHECK(fabs(pitch(t_a / 1000 + 0.02, t_a / 1000 + 0.28) - 441) < 10, "the one-shot is at %.0f Hz, not 441", pitch(t_a / 1000 + 0.02, t_a / 1000 + 0.28));
    CHECK(rms(t_a / 1000 + 0.33, t_q / 1000 - 0.02, 2) < 0.001, "sound after the one-shot ended");
    CHECK(rms(t_q / 1000, t_q / 1000 + 0.2, 2) < 0.001, "volume -10000 is heard");
    CHECK(rms(t_p / 1000 + 0.02, t_p / 1000 + 0.18, 1) > 0.3 && rms(t_p / 1000 + 0.02, t_p / 1000 + 0.18, 0) < 0.001,
          "pan +10000: left %.3f, right %.3f", rms(t_p / 1000 + 0.02, t_p / 1000 + 0.18, 0), rms(t_p / 1000 + 0.02, t_p / 1000 + 0.18, 1));
    printf("  looping: rms %.3f before the rewrite, %.4f after\n", rms(t_lp / 1000 + 0.02, t_rw / 1000 - 0.02, 2), rms(t_rw / 1000 + 0.03, 2.05, 2));
    CHECK(rms(t_lp / 1000 + 0.02, t_rw / 1000 - 0.02, 2) > 0.3, "the looping buffer is quiet (it loops past its 250 ms)");
    CHECK(rms(t_rw / 1000 + 0.03, 2.05, 2) < 0.001, "the rewritten (silent) looping buffer is heard");
    double son = onset(2.2, 3.3, 0.05);
    double left = 0.2 - (t_pause - t_s) / 1000, pw0 = t_resume / 1000 + 0.01, pw1 = pw0 + (left > 0.09 ? left - 0.04 : 0.05);
    printf("  stream: due at %.3f s, heard from %.3f s; %.0f Hz\n", t_s / 1000, son, pitch(pw0, pw1));
    CHECK(son >= 0 && fabs(son - t_s / 1000) < 0.025, "the stream is heard at %.3f s, due at %.3f s", son, t_s / 1000);
    CHECK(fabs(pitch(pw0, pw1) - 1102.5) < 25, "the stream is at %.0f Hz, not 1102.5", pitch(pw0, pw1));
    /* by the times the calls were made: a loaded machine wakes this thread late */
    printf("  pause: at %.3f s, resumed at %.3f s, %.3f s left to play\n", t_pause / 1000, t_resume / 1000, left);
    CHECK(rms(t_pause / 1000 + 0.01, t_resume / 1000 - 0.01, 2) < 0.001, "the paused stream is heard");
    CHECK(left < 0.05 || rms(t_resume / 1000 + 0.01, t_resume / 1000 + left - 0.02, 2) > 0.3, "the stream does not carry on after the pause");
    CHECK(rms(t_resume / 1000 + left + 0.02, 3.33, 2) < 0.001, "sound after the stream ended");
    DeleteFileA(wav);
    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
