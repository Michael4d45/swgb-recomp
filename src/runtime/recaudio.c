/*
 * recaudio: the sound in --record's mp4.
 *
 * The game's sound is Windows' DirectSound, the films' is the host's own
 * waveOut (movie.c), and test runs mute the process's audio session, which
 * mutes a loopback capture of the process too. So nothing here listens to
 * what reaches the speakers: the mix is made again from what the game asks
 * for, on the recording's clock, whatever the speakers do.
 *
 * - DirectSound: with --record, DirectSoundCreate (DSOUND.dll#1) hands the
 *   game a proxy over the real IDirectSound, and every buffer it creates is a
 *   proxy over the real buffer. Every call still goes to the real one, so the
 *   game sounds and behaves as before. Beside it each buffer keeps a shadow:
 *   a copy of its samples (taken at each Unlock), its format, volume, pan,
 *   frequency, and whether and where it plays. The mixer plays the shadows at
 *   their own rate and follows the real play cursors (a looping buffer the
 *   game streams into, the music and the victory jingle, must be read where
 *   the real one reads it): a small rate correction keeps a shadow on its
 *   cursor, and a shadow more than 100 ms off jumps to it.
 * - Everything else the host plays itself (the films, the CD music) comes in
 *   by the stream tap, recaudio_stream_write, with when it will be heard.
 *
 * The mixer is a thread of ours, every 10 ms: 44.1 kHz 16-bit stereo, up to
 * the recording clock's now, into a WAV beside the mp4. The clock is the one
 * softdd times the video's frames by (recaudio_clock_ms), so a sample and a
 * frame for the same moment are at the same place in the file, however long
 * it runs. At the end the sound is cut or padded to the video's exact length
 * (its frames at 30 fps), and a second ffmpeg muxes the two (the video
 * copied, the sound as AAC) into the mp4.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <io.h>

#include "recaudio.h"

#define OUT_RATE 44100
#define LOG(...) do { fprintf(stderr, "[recaudio] " __VA_ARGS__); fputc('\n', stderr); } while (0)

static const GUID kIID_IUnknown = { 0x00000000, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const GUID kIID_IDirectSound = { 0x279AFA83, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static const GUID kIID_IDirectSoundBuffer = { 0x279AFA85, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };

static int g_on;
static CRITICAL_SECTION g_cs;
static double g_qpc_ms;           /* ms per QPC tick */
static LARGE_INTEGER g_qpc0;

double recaudio_clock_ms(void) {
    LARGE_INTEGER t;
    if (!g_qpc_ms) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&g_qpc0);
        g_qpc_ms = 1000.0 / (double)f.QuadPart;
    }
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - g_qpc0.QuadPart) * g_qpc_ms;
}

/* ---- the timeline ------------------------------------------------------------
 * Samples counted from the mixer's start (g_m0); the recording begins at
 * g_out0 on it. */
static double  g_m0;
static int64_t g_tl;              /* mixed up to here */
static int64_t g_out0 = -1;       /* the first frame's sample; -1 until recaudio_start */
static int64_t g_written;         /* frames in the WAV */
static FILE*   g_wav;
static char    g_wav_path[MAX_PATH], g_mp4[MAX_PATH];
static double  g_peak, g_sumsq;
static long    g_plays, g_buffers, g_resyncs, g_streams;

static int64_t now_samples(void) { return (int64_t)((recaudio_clock_ms() - g_m0) * (OUT_RATE / 1000.0)); }

/* ---- the shadows -------------------------------------------------------------- */
typedef struct { uint8_t* bytes; LONG refs; } Data;
typedef struct { DWORD off; void* p1; DWORD n1; void* p2; DWORD n2; } Locked;

typedef struct PBuf {
    const IDirectSoundBufferVtbl* lpVtbl;   /* first: the game calls through it */
    IDirectSoundBuffer* real;
    LONG refs;
    int primary, mixable;
    Data* data;
    DWORD bytes, frames;
    int channels, bits, block, rate0, rate;
    LONG volume, pan;
    float gl, gr;
    int playing, looping;
    double pos;                   /* frames */
    double err;                   /* the real cursor less ours, smoothed, in frames */
    int64_t start_at, stop_at;    /* timeline samples of the last Play and Stop (-1: none pending):
                                     the mixer runs every 10 ms, the game's calls land between */
    Locked locks[8];
    int nlocks;
} PBuf;

typedef struct PDS {
    const IDirectSoundVtbl* lpVtbl;
    IDirectSound* real;
    LONG refs;
} PDS;

#define MAX_BUFS 1024
static PBuf* g_buf[MAX_BUFS];
static int   g_nbuf;
static float g_master = 1.0f;     /* the primary buffer's volume */

static float db_gain(LONG hundredths) { return hundredths <= -10000 ? 0.0f : powf(10.0f, (float)hundredths / 2000.0f); }
static void gains(PBuf* b) {
    float v = db_gain(b->volume), l = 1.0f, r = 1.0f;
    if (b->pan < 0) r = db_gain(b->pan);
    if (b->pan > 0) l = db_gain(-b->pan);
    b->gl = v * l, b->gr = v * r;
}

static float sample(const PBuf* b, uint32_t f, int ch) {
    if (f >= b->frames) return 0.0f;
    const uint8_t* p = b->data->bytes + (size_t)f * b->block + (b->channels > 1 ? (size_t)ch * (b->bits / 8) : 0);
    return b->bits == 16 ? (float)*(const int16_t*)p / 32768.0f : ((float)*p - 128.0f) / 128.0f;
}

static void list_add(PBuf* b) {
    if (g_nbuf < MAX_BUFS) g_buf[g_nbuf++] = b;
    else b->mixable = 0;
}
static void list_remove(PBuf* b) {
    for (int i = 0; i < g_nbuf; i++)
        if (g_buf[i] == b) { g_buf[i] = g_buf[--g_nbuf]; return; }
}

static const IDirectSoundBufferVtbl g_bvt;
static PBuf* pbuf_of(IDirectSoundBuffer* p) { return p && p->lpVtbl == &g_bvt ? (PBuf*)p : NULL; }

/* The format and size from the real buffer itself. */
static PBuf* wrap_buffer(IDirectSoundBuffer* real, Data* shared) {
    PBuf* b = (PBuf*)calloc(1, sizeof *b);
    DSBCAPS caps = { sizeof caps };
    WAVEFORMATEX wf = { 0 };
    DWORD got = 0;
    b->lpVtbl = &g_bvt;
    b->real = real;
    b->refs = 1;
    b->stop_at = -1;
    if (SUCCEEDED(IDirectSoundBuffer_GetCaps(real, &caps))) {
        b->primary = !!(caps.dwFlags & DSBCAPS_PRIMARYBUFFER);
        b->bytes = caps.dwBufferBytes;
    }
    if (SUCCEEDED(IDirectSoundBuffer_GetFormat(real, &wf, sizeof wf, &got)) &&
        (wf.wFormatTag == WAVE_FORMAT_PCM || wf.wFormatTag == 0xFFFE /* WAVE_FORMAT_EXTENSIBLE */) &&
        (wf.wBitsPerSample == 8 || wf.wBitsPerSample == 16) && (wf.nChannels == 1 || wf.nChannels == 2)) {
        b->channels = wf.nChannels, b->bits = wf.wBitsPerSample;
        b->block = wf.nChannels * wf.wBitsPerSample / 8;
        b->rate0 = b->rate = (int)wf.nSamplesPerSec;
    }
    if (!b->primary && b->block && b->bytes && b->rate0) {
        b->frames = b->bytes / (DWORD)b->block;
        if (shared) { b->data = shared; InterlockedIncrement(&shared->refs); }
        else if ((b->data = (Data*)calloc(1, sizeof *b->data)) != NULL) {
            b->data->refs = 1;
            b->data->bytes = (uint8_t*)calloc(1, b->bytes);
            if (!b->data->bytes) { free(b->data); b->data = NULL; }
        }
        if (!shared && b->data && b->bits == 8) memset(b->data->bytes, 0x80, b->bytes);
        b->mixable = b->data != NULL;
    }
    gains(b);
    EnterCriticalSection(&g_cs);
    list_add(b);
    g_buffers++;
    LeaveCriticalSection(&g_cs);
    return b;
}

static void data_unref(Data* d) {
    if (d && !InterlockedDecrement(&d->refs)) { free(d->bytes); free(d); }
}

/* ---- IDirectSoundBuffer --------------------------------------------------------- */
#define B ((PBuf*)This)
#define REAL B->real

static HRESULT STDMETHODCALLTYPE b_QueryInterface(IDirectSoundBuffer* This, REFIID riid, LPVOID* out) {
    if (IsEqualGUID(riid, &kIID_IUnknown) || IsEqualGUID(riid, &kIID_IDirectSoundBuffer)) {
        InterlockedIncrement(&B->refs);
        IDirectSoundBuffer_AddRef(REAL);
        *out = This;
        return S_OK;
    }
    static int told;
    if (!told++) LOG("a buffer's QueryInterface({%08lX-...}): the real one's (not recorded)", riid->Data1);
    return IDirectSoundBuffer_QueryInterface(REAL, riid, out);
}
static ULONG STDMETHODCALLTYPE b_AddRef(IDirectSoundBuffer* This) {
    IDirectSoundBuffer_AddRef(REAL);
    return (ULONG)InterlockedIncrement(&B->refs);
}
static ULONG STDMETHODCALLTYPE b_Release(IDirectSoundBuffer* This) {
    LONG n = InterlockedDecrement(&B->refs);
    if (n > 0) { IDirectSoundBuffer_Release(REAL); return (ULONG)n; }
    EnterCriticalSection(&g_cs);
    list_remove(B);
    LeaveCriticalSection(&g_cs);
    IDirectSoundBuffer_Release(REAL);
    data_unref(B->data);
    free(B);
    return 0;
}
static HRESULT STDMETHODCALLTYPE b_GetCaps(IDirectSoundBuffer* This, LPDSBCAPS c) { return IDirectSoundBuffer_GetCaps(REAL, c); }
static HRESULT STDMETHODCALLTYPE b_GetCurrentPosition(IDirectSoundBuffer* This, LPDWORD p, LPDWORD w) { return IDirectSoundBuffer_GetCurrentPosition(REAL, p, w); }
static HRESULT STDMETHODCALLTYPE b_GetFormat(IDirectSoundBuffer* This, LPWAVEFORMATEX f, DWORD n, LPDWORD got) { return IDirectSoundBuffer_GetFormat(REAL, f, n, got); }
static HRESULT STDMETHODCALLTYPE b_GetVolume(IDirectSoundBuffer* This, LPLONG v) { return IDirectSoundBuffer_GetVolume(REAL, v); }
static HRESULT STDMETHODCALLTYPE b_GetPan(IDirectSoundBuffer* This, LPLONG v) { return IDirectSoundBuffer_GetPan(REAL, v); }
static HRESULT STDMETHODCALLTYPE b_GetFrequency(IDirectSoundBuffer* This, LPDWORD v) { return IDirectSoundBuffer_GetFrequency(REAL, v); }
static HRESULT STDMETHODCALLTYPE b_GetStatus(IDirectSoundBuffer* This, LPDWORD s) { return IDirectSoundBuffer_GetStatus(REAL, s); }
static HRESULT STDMETHODCALLTYPE b_Initialize(IDirectSoundBuffer* This, LPDIRECTSOUND ds, LPCDSBUFFERDESC d) { return IDirectSoundBuffer_Initialize(REAL, ds, d); }
static HRESULT STDMETHODCALLTYPE b_SetFormat(IDirectSoundBuffer* This, LPCWAVEFORMATEX f) { return IDirectSoundBuffer_SetFormat(REAL, f); }
static HRESULT STDMETHODCALLTYPE b_Restore(IDirectSoundBuffer* This) { return IDirectSoundBuffer_Restore(REAL); }

static HRESULT STDMETHODCALLTYPE b_Lock(IDirectSoundBuffer* This, DWORD off, DWORD n, LPVOID* p1, LPDWORD n1, LPVOID* p2, LPDWORD n2, DWORD flags) {
    if (!B->mixable) return IDirectSoundBuffer_Lock(REAL, off, n, p1, n1, p2, n2, flags);
    EnterCriticalSection(&g_cs);
    if (flags & DSBLOCK_ENTIREBUFFER) off = 0;
    else if (flags & DSBLOCK_FROMWRITECURSOR) { DWORD w = 0; IDirectSoundBuffer_GetCurrentPosition(REAL, NULL, &w); off = w; }
    HRESULT hr = IDirectSoundBuffer_Lock(REAL, off, n, p1, n1, p2, n2, flags);
    if (SUCCEEDED(hr) && p1 && B->nlocks < 8) {
        Locked* l = &B->locks[B->nlocks++];
        l->off = off % B->bytes;
        l->p1 = *p1, l->n1 = n1 ? *n1 : 0;
        l->p2 = p2 ? *p2 : NULL, l->n2 = n2 ? *n2 : 0;
    }
    LeaveCriticalSection(&g_cs);
    return hr;
}
/* What the game wrote is copied into the shadow before the real buffer has it back. */
static HRESULT STDMETHODCALLTYPE b_Unlock(IDirectSoundBuffer* This, LPVOID p1, DWORD n1, LPVOID p2, DWORD n2) {
    if (B->mixable) {
        EnterCriticalSection(&g_cs);
        for (int i = 0; i < B->nlocks; i++) {
            Locked* l = &B->locks[i];
            if (l->p1 != p1) continue;
            if (p1 && n1) {
                DWORD k = n1 < l->n1 ? n1 : l->n1;
                if (l->off + k > B->bytes) k = B->bytes - l->off;
                memcpy(B->data->bytes + l->off, p1, k);
            }
            if (p2 && n2 && p2 == l->p2) memcpy(B->data->bytes, p2, n2 < l->n2 ? n2 : l->n2);
            B->locks[i] = B->locks[--B->nlocks];
            break;
        }
        LeaveCriticalSection(&g_cs);
    }
    return IDirectSoundBuffer_Unlock(REAL, p1, n1, p2, n2);
}
static HRESULT STDMETHODCALLTYPE b_Play(IDirectSoundBuffer* This, DWORD r, DWORD prio, DWORD flags) {
    EnterCriticalSection(&g_cs);
    HRESULT hr = IDirectSoundBuffer_Play(REAL, r, prio, flags);
    if (SUCCEEDED(hr) && B->mixable) {
        DWORD pc = 0;
        if (!B->playing) g_plays++;
        B->playing = 1;
        B->looping = !!(flags & DSBPLAY_LOOPING);
        if (SUCCEEDED(IDirectSoundBuffer_GetCurrentPosition(REAL, &pc, NULL))) B->pos = (double)(pc / (DWORD)B->block);
        B->err = 0;
        B->start_at = now_samples(), B->stop_at = -1;
    }
    LeaveCriticalSection(&g_cs);
    return hr;
}
static HRESULT STDMETHODCALLTYPE b_SetCurrentPosition(IDirectSoundBuffer* This, DWORD p) {
    EnterCriticalSection(&g_cs);
    HRESULT hr = IDirectSoundBuffer_SetCurrentPosition(REAL, p);
    if (SUCCEEDED(hr) && B->mixable) B->pos = (double)((p % B->bytes) / (DWORD)B->block), B->err = 0;
    LeaveCriticalSection(&g_cs);
    return hr;
}
static HRESULT STDMETHODCALLTYPE b_SetVolume(IDirectSoundBuffer* This, LONG v) {
    EnterCriticalSection(&g_cs);
    HRESULT hr = IDirectSoundBuffer_SetVolume(REAL, v);
    if (SUCCEEDED(hr)) {
        B->volume = v;
        gains(B);
        if (B->primary) g_master = db_gain(v);
    }
    LeaveCriticalSection(&g_cs);
    return hr;
}
static HRESULT STDMETHODCALLTYPE b_SetPan(IDirectSoundBuffer* This, LONG v) {
    EnterCriticalSection(&g_cs);
    HRESULT hr = IDirectSoundBuffer_SetPan(REAL, v);
    if (SUCCEEDED(hr)) B->pan = v, gains(B);
    LeaveCriticalSection(&g_cs);
    return hr;
}
static HRESULT STDMETHODCALLTYPE b_SetFrequency(IDirectSoundBuffer* This, DWORD f) {
    EnterCriticalSection(&g_cs);
    HRESULT hr = IDirectSoundBuffer_SetFrequency(REAL, f);
    if (SUCCEEDED(hr)) B->rate = f ? (int)f : B->rate0;
    LeaveCriticalSection(&g_cs);
    return hr;
}
static HRESULT STDMETHODCALLTYPE b_Stop(IDirectSoundBuffer* This) {
    EnterCriticalSection(&g_cs);
    HRESULT hr = IDirectSoundBuffer_Stop(REAL);
    if (SUCCEEDED(hr) && B->playing) B->stop_at = now_samples();
    LeaveCriticalSection(&g_cs);
    return hr;
}
#undef B
#undef REAL

static const IDirectSoundBufferVtbl g_bvt = {
    b_QueryInterface, b_AddRef, b_Release, b_GetCaps, b_GetCurrentPosition, b_GetFormat, b_GetVolume,
    b_GetPan, b_GetFrequency, b_GetStatus, b_Initialize, b_Lock, b_Play, b_SetCurrentPosition,
    b_SetFormat, b_SetVolume, b_SetPan, b_SetFrequency, b_Stop, b_Unlock, b_Restore,
};

/* ---- IDirectSound --------------------------------------------------------------- */
#define D ((PDS*)This)
#define REAL D->real
static HRESULT STDMETHODCALLTYPE d_QueryInterface(IDirectSound* This, REFIID riid, LPVOID* out) {
    if (IsEqualGUID(riid, &kIID_IUnknown) || IsEqualGUID(riid, &kIID_IDirectSound)) {
        InterlockedIncrement(&D->refs);
        IDirectSound_AddRef(REAL);
        *out = This;
        return S_OK;
    }
    LOG("QueryInterface({%08lX-...}) on DirectSound: the real one's (its buffers are not recorded)", riid->Data1);
    return IDirectSound_QueryInterface(REAL, riid, out);
}
static ULONG STDMETHODCALLTYPE d_AddRef(IDirectSound* This) {
    IDirectSound_AddRef(REAL);
    return (ULONG)InterlockedIncrement(&D->refs);
}
static ULONG STDMETHODCALLTYPE d_Release(IDirectSound* This) {
    LONG n = InterlockedDecrement(&D->refs);
    IDirectSound_Release(REAL);
    if (!n) free(D);
    return (ULONG)(n > 0 ? n : 0);
}
static HRESULT STDMETHODCALLTYPE d_CreateSoundBuffer(IDirectSound* This, LPCDSBUFFERDESC desc, LPDIRECTSOUNDBUFFER* out, LPUNKNOWN outer) {
    IDirectSoundBuffer* real = NULL;
    HRESULT hr = IDirectSound_CreateSoundBuffer(REAL, desc, &real, outer);
    if (out) *out = SUCCEEDED(hr) && real ? (IDirectSoundBuffer*)wrap_buffer(real, NULL) : real;
    return hr;
}
static HRESULT STDMETHODCALLTYPE d_GetCaps(IDirectSound* This, LPDSCAPS c) { return IDirectSound_GetCaps(REAL, c); }
static HRESULT STDMETHODCALLTYPE d_DuplicateSoundBuffer(IDirectSound* This, LPDIRECTSOUNDBUFFER orig, LPDIRECTSOUNDBUFFER* out) {
    PBuf* o = pbuf_of(orig);
    IDirectSoundBuffer* real = NULL;
    HRESULT hr = IDirectSound_DuplicateSoundBuffer(REAL, o ? o->real : orig, &real);
    if (!out) return hr;
    *out = real;
    if (SUCCEEDED(hr) && real && o) {
        PBuf* b;
        EnterCriticalSection(&g_cs);
        Data* d = o->mixable ? o->data : NULL;
        if (d) InterlockedIncrement(&d->refs);   /* held across wrap_buffer's own reference */
        LeaveCriticalSection(&g_cs);
        b = wrap_buffer(real, d);
        data_unref(d);
        EnterCriticalSection(&g_cs);
        b->volume = o->volume, b->pan = o->pan, b->rate = o->rate;
        gains(b);
        LeaveCriticalSection(&g_cs);
        *out = (IDirectSoundBuffer*)b;
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE d_SetCooperativeLevel(IDirectSound* This, HWND w, DWORD l) { return IDirectSound_SetCooperativeLevel(REAL, w, l); }
static HRESULT STDMETHODCALLTYPE d_Compact(IDirectSound* This) { return IDirectSound_Compact(REAL); }
static HRESULT STDMETHODCALLTYPE d_GetSpeakerConfig(IDirectSound* This, LPDWORD c) { return IDirectSound_GetSpeakerConfig(REAL, c); }
static HRESULT STDMETHODCALLTYPE d_SetSpeakerConfig(IDirectSound* This, DWORD c) { return IDirectSound_SetSpeakerConfig(REAL, c); }
static HRESULT STDMETHODCALLTYPE d_Initialize(IDirectSound* This, LPCGUID g) { return IDirectSound_Initialize(REAL, g); }
#undef D
#undef REAL

static const IDirectSoundVtbl g_dvt = {
    d_QueryInterface, d_AddRef, d_Release, d_CreateSoundBuffer, d_GetCaps, d_DuplicateSoundBuffer,
    d_SetCooperativeLevel, d_Compact, d_GetSpeakerConfig, d_SetSpeakerConfig, d_Initialize,
};

long recaudio_wrap(void* real_dsound, void** out) {
    PDS* d = (PDS*)calloc(1, sizeof *d);
    if (!d) return E_OUTOFMEMORY;
    d->lpVtbl = &g_dvt;
    d->real = (IDirectSound*)real_dsound;
    d->refs = 1;
    *out = d;
    return S_OK;
}

long recaudio_DirectSoundCreate(const void* guid, void** out, void* outer) {
    typedef HRESULT (WINAPI *create_t)(LPCGUID, LPDIRECTSOUND*, LPUNKNOWN);
    static create_t create;
    if (!create) {
        HMODULE h = LoadLibraryA("dsound.dll");
        create = h ? (create_t)(void*)GetProcAddress(h, (LPCSTR)1) : NULL;   /* DirectSoundCreate */
        if (!create) return DSERR_NODRIVER;
    }
    IDirectSound* real = NULL;
    HRESULT hr = create((LPCGUID)guid, &real, (LPUNKNOWN)outer);
    if (!out) return hr;
    *out = real;
    if (g_on && SUCCEEDED(hr) && real) {
        recaudio_wrap(real, out);
        LOG("DirectSound: the game's sound is recorded (a proxy over the real one)");
    } else if (g_on) LOG("DirectSoundCreate failed (0x%08lX): the recording has no game sound", (unsigned long)hr);
    return hr;
}

/* ---- the tap's streams ------------------------------------------------------------- */
#define MAX_STREAMS 4
#define RING (1 << 19)            /* frames, 44.1 kHz stereo: 11.9 s */
#define SLACK (OUT_RATE * 6 / 100) /* 60 ms off the timeline before a stream is moved */
typedef struct {
    int used, rate, channels, paused;
    int16_t* ring;                /* resampled to OUT_RATE, stereo */
    int64_t rd, wr;               /* frames, free running */
    int64_t start;                /* the timeline sample ring[rd] is heard at; -1: placed by the next write */
    double frac;                  /* the resampler's phase */
    int16_t last[2];
} Stream;
static Stream g_st[MAX_STREAMS];

int recaudio_stream_open(int rate, int channels) {
    if (!g_on || rate <= 0 || (channels != 1 && channels != 2)) return -1;
    EnterCriticalSection(&g_cs);
    int id = -1;
    for (int i = 0; i < MAX_STREAMS && id < 0; i++)
        if (!g_st[i].used) id = i;
    if (id >= 0) {
        Stream* s = &g_st[id];
        if (!s->ring) s->ring = (int16_t*)malloc(sizeof(int16_t) * 2 * RING);
        if (!s->ring) id = -1;
        else {
            s->used = 1, s->rate = rate, s->channels = channels, s->paused = 0;
            s->rd = s->wr = 0, s->start = -1, s->frac = 0, s->last[0] = s->last[1] = 0;
            g_streams++;
        }
    }
    LeaveCriticalSection(&g_cs);
    return id;
}

static void push(Stream* s, int16_t l, int16_t r) {
    if (s->wr - s->rd >= RING) s->rd++;   /* full: the oldest goes */
    int64_t k = s->wr++ & (RING - 1);
    s->ring[2 * k] = l, s->ring[2 * k + 1] = r;
}

void recaudio_stream_write(int id, const int16_t* pcm, int frames, double plays_in_ms) {
    if (id < 0 || id >= MAX_STREAMS || frames <= 0) return;
    EnterCriticalSection(&g_cs);
    Stream* s = &g_st[id];
    if (!s->used) { LeaveCriticalSection(&g_cs); return; }
    int64_t want = now_samples() + (int64_t)(plays_in_ms * (OUT_RATE / 1000.0));
    int64_t queued = s->wr - s->rd;
    int skip = 0;   /* output frames of this chunk to drop */
    if (!queued || s->start < 0) s->start = want > g_tl ? want : g_tl;
    else if (!s->paused) {
        /* where this chunk would be heard, appended: kept there unless the
         * player's own position says otherwise by more than SLACK (a long
         * stream, or one that ran dry and came back) */
        int64_t at = (s->start > g_tl ? s->start : g_tl) + queued;
        int64_t e = want - at;
        if (e > SLACK) for (int64_t i = 0; i < e; i++) push(s, 0, 0);
        else if (e < -SLACK) skip = (int)-e;
    }
    double step = (double)s->rate / OUT_RATE;
    for (int f = 0; f < frames; f++) {
        int16_t l = pcm[f * s->channels], r = s->channels > 1 ? pcm[f * s->channels + 1] : l;
        if (s->rate == OUT_RATE) { if (skip > 0) skip--; else push(s, l, r); continue; }
        while (s->frac < 1.0) {   /* between last and this one */
            float t = (float)s->frac;
            if (skip > 0) skip--;
            else push(s, (int16_t)(s->last[0] + (l - s->last[0]) * t), (int16_t)(s->last[1] + (r - s->last[1]) * t));
            s->frac += step;
        }
        s->frac -= 1.0;
        s->last[0] = l, s->last[1] = r;
    }
    LeaveCriticalSection(&g_cs);
}

void recaudio_stream_pause(int id, int paused) {
    if (id < 0 || id >= MAX_STREAMS) return;
    EnterCriticalSection(&g_cs);
    g_st[id].paused = paused;
    LeaveCriticalSection(&g_cs);
}

void recaudio_stream_close(int id) {
    if (id < 0 || id >= MAX_STREAMS) return;
    EnterCriticalSection(&g_cs);
    g_st[id].used = 0;   /* what was not heard yet is dropped, as waveOutReset does */
    LeaveCriticalSection(&g_cs);
}

/* ---- the mixer ------------------------------------------------------------------- */
#define CHUNK 4096
static float g_acc[2 * CHUNK];

/* Keep a playing shadow on the real buffer's play cursor. The real cursor is
 * where it is now (end); ours is where the last mix left it, so it is
 * compared with where ours will be by end. */
static void follow(PBuf* b, int64_t end) {
    DWORD st = 0, pc = 0;
    if (b->stop_at >= 0) return;   /* stopped: the mixer gets there */
    double lead = (double)(end - (b->start_at > g_tl ? b->start_at : g_tl)) * b->rate / OUT_RATE;
    double proj = b->pos + (lead > 0 ? lead : 0);
    if (b->looping) proj = fmod(proj, (double)b->frames);
    if (FAILED(IDirectSoundBuffer_GetStatus(b->real, &st))) return;
    if (!(st & DSBSTATUS_PLAYING)) {
        /* it ended (or was lost): ours too, unless it is about to */
        if (b->looping || b->frames - proj > b->rate / 10.0) b->stop_at = end;
        return;
    }
    if (FAILED(IDirectSoundBuffer_GetCurrentPosition(b->real, &pc, NULL))) return;
    double e = (double)(pc / (DWORD)b->block) - proj;
    if (b->looping) {
        if (e > b->frames / 2.0) e -= b->frames;
        else if (e < -(b->frames / 2.0)) e += b->frames;
    }
    if (fabs(e) > b->rate / 10.0) {
        b->pos += e;
        if (b->looping) b->pos = fmod(b->pos + b->frames, (double)b->frames);
        else if (b->pos < 0) b->pos = 0;
        b->err = 0;
        g_resyncs++;
    } else b->err = 0.9 * b->err + 0.1 * e;
}

/* [a, a + n) of the timeline: from the Play's sample, to the Stop's. */
static void mix_buffer(PBuf* b, float* acc, int64_t a, int n) {
    int f = b->start_at > a ? (int)(b->start_at - a < n ? b->start_at - a : n) : 0;
    int last = b->stop_at >= 0 && b->stop_at < a + n ? (int)(b->stop_at > a ? b->stop_at - a : 0) : n;
    /* the cursor moves in steps of 10 ms or so: half a percent at most, over 2 s */
    double corr = b->err / (2.0 * b->rate);
    corr = corr > 0.005 ? 0.005 : corr < -0.005 ? -0.005 : corr;
    double step = (double)b->rate / OUT_RATE * (1.0 + corr);
    float gl = b->gl * g_master, gr = b->gr * g_master;   /* the primary's volume is the game's master */
    if (last < n) b->playing = 0, b->stop_at = -1;
    for (; f < last; f++) {
        if (b->pos >= b->frames) {
            if (!b->looping) { b->playing = 0; return; }
            while (b->pos >= b->frames) b->pos -= b->frames;
        }
        uint32_t i0 = (uint32_t)b->pos, i1 = i0 + 1 < b->frames ? i0 + 1 : b->looping ? 0 : i0;
        float t = (float)(b->pos - i0);
        float l0 = sample(b, i0, 0), l1 = sample(b, i1, 0);
        float l = l0 + (l1 - l0) * t, r = l;
        if (b->channels > 1) {
            float r0 = sample(b, i0, 1), r1 = sample(b, i1, 1);
            r = r0 + (r1 - r0) * t;
        }
        acc[2 * f] += l * gl;
        acc[2 * f + 1] += r * gr;
        b->pos += step;
    }
}

static void mix_stream(Stream* s, float* acc, int64_t a, int n) {
    if (s->start < 0) return;
    if (s->paused) { if (s->start < a + n) s->start = a + n; return; }
    int64_t off = s->start > a ? s->start - a : 0;
    if (off >= n) return;
    int k = 0;
    for (; off + k < n && s->rd < s->wr; k++, s->rd++) {
        int64_t i = s->rd & (RING - 1);
        acc[2 * (off + k)] += s->ring[2 * i] / 32768.0f;
        acc[2 * (off + k) + 1] += s->ring[2 * i + 1] / 32768.0f;
    }
    s->start = a + off + k;
    if (s->rd == s->wr) s->start = -1;   /* ran dry: the next write places it again */
}

static void out_frames(const float* acc, int n, int64_t a) {
    static int16_t pcm[2 * CHUNK];
    int from = 0;
    if (!g_wav || g_out0 < 0) return;
    if (a < g_out0) from = (int)(g_out0 - a < n ? g_out0 - a : n);
    for (int k = 2 * from; k < 2 * n; k++) {
        float v = acc[k];
        double av = fabs(v);
        if (av > g_peak) g_peak = av;
        g_sumsq += (double)v * v;
        v *= 32767.0f;
        pcm[k] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
    }
    if (n > from) {
        fwrite(pcm + 2 * from, 4, (size_t)(n - from), g_wav);
        g_written += n - from;
    }
}

/* Mix up to the clock's now. Holding g_cs. */
static void mix_to(int64_t end) {
    for (int i = 0; i < g_nbuf; i++)
        if (g_buf[i]->playing && g_buf[i]->mixable) follow(g_buf[i], end);
    while (g_tl < end) {
        int n = (int)(end - g_tl < CHUNK ? end - g_tl : CHUNK);
        memset(g_acc, 0, sizeof(float) * 2 * (size_t)n);
        for (int i = 0; i < g_nbuf; i++)
            if (g_buf[i]->playing && g_buf[i]->mixable) mix_buffer(g_buf[i], g_acc, g_tl, n);
        for (int i = 0; i < MAX_STREAMS; i++)
            if (g_st[i].used) mix_stream(&g_st[i], g_acc, g_tl, n);
        out_frames(g_acc, n, g_tl);
        g_tl += n;
    }
}

static volatile LONG g_stop;
static HANDLE g_thread;
static DWORD WINAPI mixer(LPVOID unused) {
    (void)unused;
    while (!g_stop) {
        Sleep(10);
        EnterCriticalSection(&g_cs);
        mix_to(now_samples());
        LeaveCriticalSection(&g_cs);
    }
    return 0;
}

void recaudio_init(void) {
    if (g_on) return;
    InitializeCriticalSection(&g_cs);
    g_m0 = recaudio_clock_ms();
    g_on = 1;
    g_thread = CreateThread(NULL, 0, mixer, NULL, 0, NULL);
}

int recaudio_on(void) { return g_on; }

void recaudio_start(double t0_ms, const char* mp4) {
    if (!g_on) return;
    EnterCriticalSection(&g_cs);
    if (!g_wav) {
        lstrcpynA(g_mp4, mp4, MAX_PATH);
        _snprintf(g_wav_path, sizeof g_wav_path - 1, "%s.wav", mp4);
        g_wav = fopen(g_wav_path, "wb");
        if (!g_wav) LOG("cannot write %s: the recording has no sound", g_wav_path);
        else {
            static char wbuf[1 << 18];
            static const uint8_t hdr[44] = { 0 };
            setvbuf(g_wav, wbuf, _IOFBF, sizeof wbuf);
            fwrite(hdr, 1, sizeof hdr, g_wav);   /* written at the end, with the sizes */
            g_out0 = (int64_t)((t0_ms - g_m0) * (OUT_RATE / 1000.0));
            if (g_out0 < 0) g_out0 = 0;
            if (g_tl > g_out0) {   /* already mixed past the first frame: that much silence */
                static const int16_t zero[2 * 256];
                for (int64_t k = g_tl - g_out0; k > 0; k -= 256) fwrite(zero, 4, (size_t)(k < 256 ? k : 256), g_wav);
                g_written = g_tl - g_out0;
            }
        }
    }
    LeaveCriticalSection(&g_cs);
}

static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }

static int run_wait(char* cmd, DWORD ms) {
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    DWORD rc = 1;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) return -1;
    if (WaitForSingleObject(pi.hProcess, ms) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &rc);
    else { TerminateProcess(pi.hProcess, 1); rc = (DWORD)-2; }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)rc;
}

void recaudio_finish(long frames, int fps) {
    static volatile LONG once;
    if (!g_on || !g_wav || InterlockedExchange(&once, 1)) return;
    InterlockedExchange(&g_stop, 1);
    if (g_thread) WaitForSingleObject(g_thread, 2000);
    /* the game's own thread may be stopped holding the lock (a fault): the
     * file is finished anyway */
    int locked = 0;
    for (int i = 0; i < 100 && !(locked = TryEnterCriticalSection(&g_cs)); i++) Sleep(10);
    int64_t want = (int64_t)frames * OUT_RATE / (fps > 0 ? fps : 30);
    if (locked && g_written < want) mix_to(g_out0 + want);   /* the last few ms */
    if (g_written < want) {
        static const int16_t zero[2 * 256];
        for (int64_t k = want - g_written; k > 0; k -= 256) fwrite(zero, 4, (size_t)(k < 256 ? k : 256), g_wav);
    }
    fflush(g_wav);
    if (g_written > want) _chsize_s(_fileno(g_wav), 44 + want * 4);
    uint8_t h[44];
    memcpy(h, "RIFF", 4), put32(h + 4, (uint32_t)(36 + want * 4));
    memcpy(h + 8, "WAVEfmt ", 8), put32(h + 16, 16);
    put32(h + 20, 1 | 2 << 16);                 /* PCM, 2 channels */
    put32(h + 24, OUT_RATE), put32(h + 28, OUT_RATE * 4);
    put32(h + 32, 4 | 16 << 16);                /* 4 bytes a frame, 16 bits */
    memcpy(h + 36, "data", 4), put32(h + 40, (uint32_t)(want * 4));
    fseek(g_wav, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, g_wav);
    fclose(g_wav);
    g_wav = NULL;
    double secs = (double)want / OUT_RATE, rms = want ? sqrt(g_sumsq / (2.0 * (double)(g_written < want ? g_written : want))) : 0;
    LOG("%.2f s of sound, %ld buffers (%ld plays), %ld streams, %ld cursor jumps; peak %.1f dBFS, rms %.1f dBFS",
        secs, g_buffers, g_plays, g_streams, g_resyncs,
        g_peak > 0 ? 20 * log10(g_peak) : -120.0, rms > 0 ? 20 * log10(rms) : -120.0);
    if (locked) LeaveCriticalSection(&g_cs);

    /* the mp4 ffmpeg just closed, and the sound: one mp4 with both */
    if (GetFileAttributesA(g_mp4) == INVALID_FILE_ATTRIBUTES) { LOG("no %s: the sound stays in %s", g_mp4, g_wav_path); return; }
    char mux[MAX_PATH + 16], cmd[3 * MAX_PATH + 256];
    _snprintf(mux, sizeof mux - 1, "%s.mux.mp4", g_mp4);
    mux[sizeof mux - 1] = 0;
    _snprintf(cmd, sizeof cmd - 1, "ffmpeg -nostdin -loglevel error -y -i \"%s\" -i \"%s\" -map 0:v:0 -map 1:a:0 "
              "-c:v copy -c:a aac -b:a 192k \"%s\"", g_mp4, g_wav_path, mux);
    cmd[sizeof cmd - 1] = 0;
    int rc = run_wait(cmd, 120000);
    if (rc == 0 && MoveFileExA(mux, g_mp4, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(g_wav_path);
        LOG("muxed into %s", g_mp4);
    } else {
        DeleteFileA(mux);
        LOG("ffmpeg could not mux the sound in (%d): the video is %s, the sound %s", rc, g_mp4, g_wav_path);
    }
}
