/* src/runtime/cdaudio.c without the game or a sound card (the file itself is
 * compiled in here, CDAUDIO_NO_SHIMS: the core, not the guest's calls). The
 * output is cdaudio_test_sink, which hears a play as fast as it comes, or
 * holds it (takes nothing) so that it is still playing when a stop, pause
 * or second play comes: nothing here waits on the clock.
 *
 *   decode    tests/data/tone.ogg (2 s: 441 Hz left, 882 Hz right, at 1/16
 *             of full scale; ffmpeg's sine source, volume=0.5, libvorbis
 *             -q:a 0): its length, rate, tones and level through stb_vorbis
 *   seek      a seek then a decode gives the same samples as decoding from
 *             the start, at the start, the middle, a frame edge and the end
 *   status    a folder with Track02, Track03 and Track05 (copies of it): 5
 *             tracks, track 1 data, 4 an empty gap; MCI_STATUS's numbers,
 *             lengths and positions in TMSF, MSF and milliseconds
 *   play      from/to in TMSF plays exactly the track (the samples that
 *             reach the output are the track's), then MM_MCINOTIFY
 *             successful and the position at the next track
 *   lines     the mixer's CD and wave-out lines found by component type,
 *             their volume controls by type, the values read back as set;
 *             any other line or control is not ours (WINMM's)
 *   music     the music slider as the game sets the CD line: its middle
 *             plays the track at (1/2)^3, its bottom step (1%) is silence,
 *             its top the track itself
 *   effects   the sound slider as the game sets the wave-out line: its gain
 *             goes to cdaudio_effects_volume (cubed too, its bottom under
 *             -100 dB), and the music plays on untouched
 *   abort     a stop aborts the notify, a second play supersedes it, a
 *             pause aborts it; a seek moves the position
 *   errors    out-of-range tracks, from after to, another device id
 *   no disc   no TrackNN.ogg, or --music off: MCI_OPEN fails, quietly
 *
 *   py -3 tools\test.py --level unit        (builds and runs every unit)
 */
#define CDAUDIO_NO_SHIMS
#include "../../src/runtime/cdaudio.c"

#ifdef _WIN32
#include <direct.h>
#define MKDIR(d) _mkdir(d)
#else
#include <sys/stat.h>
#define MKDIR(d) mkdir(d, 0755)
#endif

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char g_fixture[1024], g_disc[1024];

static void here(const char* file, char* out, size_t n, const char* rel) {
    snprintf(out, n, "%s", file);
    char* s = out + strlen(out);
    while (s > out && s[-1] != '/' && s[-1] != '\\') s--;
    snprintf(s, n - (size_t)(s - out), "%s", rel);
}

static int copy_file(const char* from, const char* to) {
    FILE* a = fopen(from, "rb");
    FILE* b = a ? fopen(to, "wb") : NULL;
    char buf[8192];
    size_t k;
    if (!b) { if (a) fclose(a); return 0; }
    while ((k = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, k, b);
    fclose(a);
    fclose(b);
    return 1;
}

/* ---- the sink and the notifications ---- */
#define TRACK_FRAMES 88200
static int16_t* g_full;          /* the fixture, decoded from the start */
static int g_full_n;
static int16_t* g_heard;
static volatile int g_heard_n;
static volatile int g_hold;      /* the sink takes nothing: a play goes on until stopped */
static int sink(const int16_t* pcm, int frames) {
    if (g_hold) return 0;
    if (g_heard_n + frames <= 4 * TRACK_FRAMES) memcpy(g_heard + 2 * g_heard_n, pcm, (size_t)frames * 4);
    g_heard_n += frames;
    return frames;
}

static volatile int g_notes, g_last_code;
static volatile uint32_t g_last_hwnd;
static HANDLE g_noted;
static void on_notify(uint32_t hwnd, uint32_t code, uint32_t dev) {
    (void)dev;
    g_last_hwnd = hwnd, g_last_code = (int)code;
    g_notes++;
    SetEvent(g_noted);
}
/* The player thread's notification: no time is measured, the wait only
 * bounds a hang. */
static int wait_notes(int n) {
    while (g_notes < n)
        if (WaitForSingleObject(g_noted, 10000) != WAIT_OBJECT_0) return 0;
    return 1;
}

static uint32_t cmd(uint32_t msg, uint32_t flags, uint32_t* p) { return cdaudio_command(CD_DEVICE_ID, msg, flags, p, NULL); }
static uint32_t status(uint32_t item, int track) {
    uint32_t p[4] = { 0, 0xDEAD, item, (uint32_t)track };
    uint32_t rc = cmd(C_STATUS, F_ITEM | F_WAIT | (track ? F_TRACK : 0), p);
    return rc ? 0xFFFFFFFFu : p[1];
}
static void set_format(uint32_t f) {
    uint32_t sp[3] = { 0, f };
    cmd(C_SET, F_SET_TIME_FORMAT | F_WAIT, sp);
}
/* One track (3, the same tone as 2) played to its end through the sink:
 * the largest difference from the fixture at gain g, or -1 when it did not
 * play exactly its length. */
static int play_track3(float g) {
    uint32_t pp[3] = { 0x1234, 0x03, 0x04 };
    int n0 = g_notes;
    set_format(FMT_TMSF);
    g_heard_n = 0;
    cmd(C_PLAY, F_NOTIFY | F_FROM | F_TO, pp);
    if (!wait_notes(n0 + 1) || g_last_code != NOTIFY_SUCCESSFUL || g_heard_n != TRACK_FRAMES) return -1;
    int worst = 0;
    for (int i = 0; i < 2 * TRACK_FRAMES; i++) {
        int want = g >= 0.999f ? g_full[i] : (int)(int16_t)(g_full[i] * g);
        int d = abs(g_heard[i] - want);
        if (d > worst) worst = d;
    }
    return worst;
}
/* The game's slider write (0x00542DD0, and the music player's): -100*s
 * for s steps down from the top, then linearly into the control's range. */
static uint32_t slider_value(int steps_down) { return (uint32_t)(0xFFFF * (10000 - 100 * steps_down) / 10000); }
static uint32_t set_line(uint32_t ctrl, uint32_t value) {
    uint32_t val[2] = { value, value }, det[6] = { 24, ctrl, 2, 0, 4, 0 };
    det[5] = (uint32_t)(uintptr_t)val;
    return mixer_details(det, 1);
}

/* ---- decode and seek ---- */
static void test_decode(void) {
    int err = 0;
    stb_vorbis* v = stb_vorbis_open_filename(g_fixture, &err, NULL);
    CHECK(v != NULL, "open %s: error %d", g_fixture, err);
    if (!v) return;
    stb_vorbis_info in = stb_vorbis_get_info(v);
    unsigned len = stb_vorbis_stream_length_in_samples(v);
    CHECK(in.sample_rate == 44100 && in.channels == 2, "rate %u, %d channels", in.sample_rate, in.channels);
    CHECK(len == TRACK_FRAMES, "length %u samples, not 88200", len);
    g_full = (int16_t*)calloc((size_t)len + 64, 4);
    g_full_n = stb_vorbis_get_samples_short_interleaved(v, 2, g_full, (int)(len + 64) * 2);
    CHECK(g_full_n == (int)len, "decoded %d of %u", g_full_n, len);
    /* The tones: sign changes over the middle second, and the level. */
    int zl = 0, zr = 0;
    double sl = 0;
    for (int i = 22050; i < 66150; i++) {
        if ((g_full[2 * i] >= 0) != (g_full[2 * i - 2] >= 0)) zl++;
        if ((g_full[2 * i + 1] >= 0) != (g_full[2 * i - 1] >= 0)) zr++;
        sl += (double)g_full[2 * i] * g_full[2 * i];
    }
    double rms = sqrt(sl / 44100);
    CHECK(zl > 870 && zl < 894, "left: %d sign changes in a second, not 882 (441 Hz)", zl);
    CHECK(zr > 1750 && zr < 1778, "right: %d sign changes in a second, not 1764 (882 Hz)", zr);
    CHECK(rms > 1300 && rms < 1600, "left RMS %.0f, not ~1448 (a sine at 1/16 of full scale)", rms);
    stb_vorbis_close(v);
}

static void test_seek(void) {
    stb_vorbis* v = stb_vorbis_open_filename(g_fixture, NULL, NULL);
    unsigned len = TRACK_FRAMES;
    static const unsigned at[] = { 0, 1, 1023, 4410, 44100, 66150, 87000 };
    int16_t buf[2 * 1200];
    for (int k = 0; k < (int)(sizeof at / sizeof at[0]); k++) {
        CHECK(stb_vorbis_seek(v, at[k]), "seek to %u failed", at[k]);
        int n = stb_vorbis_get_samples_short_interleaved(v, 2, buf, 2 * 1000);
        int want = (int)len - (int)at[k] < 1000 ? (int)len - (int)at[k] : 1000;
        CHECK(n == want, "after a seek to %u: %d frames, not %d", at[k], n, want);
        int worst = 0;
        for (int i = 0; i < 2 * n; i++) {
            int d = abs(buf[i] - g_full[2 * at[k] + i]);
            if (d > worst) worst = d;
        }
        CHECK(worst <= 1, "after a seek to %u: off by %d from the decode from the start", at[k], worst);
    }
    stb_vorbis_close(v);
}

/* ---- the disc ---- */
static int open_disc(void) {
    char to[1100];
    MKDIR(g_disc);
    static const int tracks[] = { 2, 3, 5 };
    for (int i = 0; i < 3; i++) {
        snprintf(to, sizeof to, "%s/Track%02d.ogg", g_disc, tracks[i]);
        CHECK(copy_file(g_fixture, to), "copy to %s", to);
    }
    cdaudio_set_dir(g_disc);
    uint32_t op[5] = { 0, 0, DEVTYPE_CD_AUDIO, 0, 0 };
    uint32_t rc = cdaudio_command(0, C_OPEN, F_OPEN_TYPE | F_OPEN_TYPE_ID | 0x100 | F_WAIT, op, NULL);
    CHECK(rc == 0 && op[1] == CD_DEVICE_ID, "open: %u, device %u", rc, op[1]);
    return rc == 0;
}

static void test_status(void) {
    CHECK(status(3, 0) == 5, "number of tracks %u, not 5", status(3, 0));
    CHECK(status(5, 0) == 1 && status(4, 0) == MODE_STOP, "media present, stopped");
    CHECK(status(0x4001, 1) == TRACK_OTHER && status(0x4001, 2) == TRACK_AUDIO && status(0x4001, 4) == TRACK_OTHER,
          "track types %u %u %u", status(0x4001, 1), status(0x4001, 2), status(0x4001, 4));
    /* MSF (the default): track 1 is 2 s of data, so track 2 starts at 0:02:00. */
    CHECK(status(2, 2) == 0x000200, "track 2 at MSF %06X, not 000200", status(2, 2));
    CHECK(status(1, 2) == 0x000200, "track 2 is MSF %06X long, not 2 s", status(1, 2));
    CHECK(status(1, 4) == 0, "track 4 (missing) is %06X long, not 0", status(1, 4));
    CHECK(status(1, 0) == 0x000800, "the disc is MSF %06X, not 0:08:00", status(1, 0));
    set_format(FMT_MS);
    CHECK(status(2, 3) == 4000 && status(2, 5) == 6000, "ms: track 3 at %u, track 5 at %u", status(2, 3), status(2, 5));
    uint32_t sp[3] = { 0, 99 };
    CHECK(cmd(C_SET, F_SET_TIME_FORMAT, sp) == E_BAD_TIME_FORMAT, "a bad time format is refused");
    set_format(FMT_TMSF);
    CHECK(status(2, 3) == 0x03, "TMSF: track 3 at %08X", status(2, 3));
    CHECK(status(1, 3) == 0x000200, "TMSF: track 3's length is MSF %06X", status(1, 3));
}

/* The game's play: from track 2 to the start of track 3, with notify. */
static void test_play(void) {
    uint32_t pp[3] = { 0x1234, 0x02, 0x03 };
    int n0 = g_notes;
    set_format(FMT_TMSF);
    g_heard_n = 0;
    CHECK(cmd(C_PLAY, F_NOTIFY | F_FROM | F_TO, pp) == 0, "play 2 to 3");
    CHECK(wait_notes(n0 + 1) && g_last_code == NOTIFY_SUCCESSFUL && g_last_hwnd == 0x1234,
          "the end of track 2: %d notes, last %d to %X", g_notes - n0, g_last_code, g_last_hwnd);
    CHECK(g_heard_n == TRACK_FRAMES, "track 2 gave %d frames, not 88200", g_heard_n);
    int worst = 0;
    for (int i = 0; i < 2 * TRACK_FRAMES && g_heard_n == TRACK_FRAMES; i++) {
        int d = abs(g_heard[i] - g_full[i]);
        if (d > worst) worst = d;
    }
    CHECK(worst <= 1, "what played is not the track (off by %d)", worst);
    CHECK(status(2, 0) == 0x03 && status(4, 0) == MODE_STOP && status(8, 0) == 3,
          "after it: at %08X, mode %u, track %u", status(2, 0), status(4, 0), status(8, 0));
}

static void test_lines(void) {
    uint32_t line[42] = { 168 };
    line[6] = SRC_CD;
    CHECK(mixer_line(line, 3) == 0 && line[3] == LINE_CD && line[9] == 1, "the CD line by component type");
    uint32_t wave[42] = { 168 };
    wave[6] = SRC_WAVE;
    CHECK(mixer_line(wave, 3) == 0 && wave[3] == LINE_WAVE && wave[9] == 1 && wave[2] != line[2],
          "the wave-out line by component type");
    uint32_t mic[42] = { 168 };
    mic[6] = 0x1003;                                       /* SRC_MICROPHONE */
    CHECK(mixer_line(mic, 3) == MX_NOT_OURS, "the microphone line is WINMM's");
    uint32_t ctl[37], lc[6] = { 24, LINE_CD, VOLUME, 0, 148, 0 };
    lc[5] = (uint32_t)(uintptr_t)ctl;
    CHECK(mixer_controls(lc, 2) == 0 && ctl[1] == CTRL_CD && ctl[25] == 0 && ctl[26] == 0xFFFF, "the CD line's volume");
    lc[1] = LINE_WAVE;
    CHECK(mixer_controls(lc, 2) == 0 && ctl[1] == CTRL_WAVE && ctl[26] == 0xFFFF, "the wave-out line's volume");
    lc[2] = CTRL_CD;
    CHECK(mixer_controls(lc, 1) == 0 && lc[1] == LINE_CD, "the CD control by id");
    /* Read back as set, whatever the gain: the game restores what it read. */
    uint32_t val[2] = { 0 }, det[6] = { 24, CTRL_CD, 2, 0, 4, 0 };
    det[5] = (uint32_t)(uintptr_t)val;
    CHECK(set_line(CTRL_CD, 0x8000) == 0 && mixer_details(det, 0) == 0 && val[0] == 0x8000 && val[1] == 0x8000,
          "the CD line read back %X", val[0]);
    det[1] = CTRL_WAVE;
    CHECK(set_line(CTRL_WAVE, 0x1234) == 0 && mixer_details(det, 0) == 0 && val[0] == 0x1234, "the wave line read back %X", val[0]);
    det[1] = 7;
    CHECK(mixer_details(det, 1) == MX_NOT_OURS, "another control is WINMM's");
    set_line(CTRL_CD, 0xFFFF), set_line(CTRL_WAVE, 0xFFFF);
}

static void test_music(void) {
    set_line(CTRL_CD, slider_value(50));                     /* the slider's middle: 0x7FFF */
    float g = g_gain;
    CHECK(fabs(g - 0.125f) < 0.001f, "the middle is a gain of %.4f, not 1/8", g);
    int off = play_track3(g);
    CHECK(off >= 0 && off <= 1, "at the middle: off by %d from the track at %.4f", off, g);
    set_line(CTRL_CD, slider_value(99));                     /* the bottom step: 655, 1% */
    CHECK(g_gain < 1.5e-6f, "the bottom step is a gain of %g", g_gain);
    off = play_track3(g_gain);
    int loud = 0;
    for (int i = 0; i < 2 * g_heard_n && i < 2 * TRACK_FRAMES; i++) if (g_heard[i]) loud++;
    CHECK(off == 0 && loud == 0, "at the bottom: %d samples not silent", loud);
    set_line(CTRL_CD, slider_value(0));
    CHECK(g_gain == 1.0f && play_track3(1.0f) == 0, "at the top: not the track itself");
}

static float g_fx = -1;
static int g_fx_calls;
static void on_effects(float gain) { g_fx = gain, g_fx_calls++; }

static void test_effects(void) {
    cdaudio_effects_volume = on_effects;
    set_line(CTRL_WAVE, slider_value(31));                   /* the farm profile's 69% */
    CHECK(g_fx_calls == 1 && fabs(g_fx - 0.3285f) < 0.001f, "69%%: the effects' gain %.4f, not 0.69^3", g_fx);
    set_line(CTRL_WAVE, slider_value(99));
    CHECK(g_fx_calls == 2 && g_fx < 1.5e-6f, "the bottom step: the effects' gain %g, not under -100 dB", g_fx);
    /* The music is the CD line's alone. */
    CHECK(g_gain == 1.0f, "the wave line moved the music's gain to %.4f", g_gain);
    CHECK(play_track3(1.0f) == 0, "with the sound slider at its bottom the music is not the track itself");
    set_line(CTRL_WAVE, 0xFFFF);
    CHECK(g_fx == 1.0f, "the top: the effects' gain %.4f", g_fx);
    cdaudio_effects_volume = NULL;
}

/* A stop aborts the notify; a second play supersedes the first; a pause
 * aborts it. The sink holds, so each play is still playing. */
static void test_abort(void) {
    uint32_t pp[3] = { 0x1000, 0x02, 0 };
    int n0 = g_notes;
    set_format(FMT_TMSF);
    g_hold = 1;
    cmd(C_PLAY, F_NOTIFY | F_FROM, pp);
    CHECK(status(4, 0) == MODE_PLAY, "playing: mode %u", status(4, 0));
    cmd(C_STOP, F_WAIT, pp);
    CHECK(g_notes == n0 + 1 && g_last_code == NOTIFY_ABORTED, "stop: %d notes, last %d", g_notes - n0, g_last_code);
    CHECK(status(4, 0) == MODE_STOP, "stopped: mode %u", status(4, 0));
    pp[0] = 0x1111;
    cmd(C_PLAY, F_NOTIFY | F_FROM, pp);
    pp[0] = 0x2222;
    cmd(C_PLAY, F_NOTIFY | F_FROM, pp);
    CHECK(g_notes == n0 + 2 && g_last_code == NOTIFY_SUPERSEDED && g_last_hwnd == 0x1111,
          "superseded: %d notes, last %d to %X", g_notes - n0, g_last_code, g_last_hwnd);
    cmd(C_PAUSE, F_WAIT, pp);
    CHECK(g_notes == n0 + 3 && g_last_code == NOTIFY_ABORTED && g_last_hwnd == 0x2222 && status(4, 0) == MODE_PAUSE,
          "pause: %d notes, last %d, mode %u", g_notes - n0, g_last_code, status(4, 0));
    uint32_t seek[2] = { 0, 0x05 };
    CHECK(cmd(C_SEEK, F_TO | F_WAIT, seek) == 0 && status(2, 0) == 0x05 && status(4, 0) == MODE_STOP, "seek to track 5");
    g_hold = 0;
}

static void test_errors(void) {
    uint32_t pp[3] = { 0, 0x09, 0 };
    set_format(FMT_TMSF);
    CHECK(cmd(C_PLAY, F_FROM, pp) == E_OUTOFRANGE, "track 9 is out of range");
    pp[1] = 0x03, pp[2] = 0x02;
    CHECK(cmd(C_PLAY, F_FROM | F_TO, pp) == E_OUTOFRANGE, "from after to is out of range");
    CHECK(cdaudio_command(0x55, C_STOP, 0, pp, NULL) == E_INVALID_DEVICE_ID, "another device id is not ours");
    uint32_t seq[5] = { 0, 0, 0, 0, 0 };
    CHECK(cdaudio_command(0, C_OPEN, F_OPEN_TYPE, seq, "sequencer") == E_INVALID_DEVICE_NAME, "a sequencer is not ours");
}

/* No tracks, or --music off: the open fails, so the game goes without. */
static void test_no_disc(void) {
    char none[1100];
    uint32_t op[5] = { 0, 0, DEVTYPE_CD_AUDIO, 0, 0 };
    CHECK(cmd(C_CLOSE, F_WAIT, op) == 0, "close");
    snprintf(none, sizeof none, "%s/none", g_disc);
    cdaudio_set_dir(none);
    uint32_t rc = cdaudio_command(0, C_OPEN, F_OPEN_TYPE | F_OPEN_TYPE_ID, op, NULL);
    CHECK(rc == E_INVALID_DEVICE_NAME, "no Music folder: open gave %u", rc);
    char* argv[] = { "host", "--music", "off" };
    CHECK(cdaudio_arg(3, argv, 1) == 2, "--music off is an argument");
    cdaudio_set_dir(g_disc);
    rc = cdaudio_command(0, C_OPEN, F_OPEN_TYPE | F_OPEN_TYPE_ID, op, NULL);
    CHECK(rc == E_INVALID_DEVICE_NAME, "--music off: open gave %u", rc);
    g_off = 0;
}

int main(int argc, char** argv) {
    (void)argc;
    setvbuf(stdout, NULL, _IONBF, 0);
    here(__FILE__, g_fixture, sizeof g_fixture, "../data/tone.ogg");
    here(argv[0], g_disc, sizeof g_disc, "cdaudio_disc");
    test_decode();
    if (g_full) {
        test_seek();
        cdaudio_test_sink = sink;
        cdaudio_notify_hook = on_notify;
        g_noted = CreateEventA(NULL, FALSE, FALSE, NULL);
        g_heard = (int16_t*)calloc(4 * TRACK_FRAMES, 4);
        if (open_disc()) {
            test_status();
            test_play();
            test_lines();
            test_music();
            test_effects();
            test_abort();
            test_errors();
            test_no_disc();
        }
    }
    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
