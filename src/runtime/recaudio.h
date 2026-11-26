/* recaudio: the sound in --record's mp4 (recaudio.c, docs/movies.md "The sound"). */
#ifndef RECAUDIO_H
#define RECAUDIO_H
#include <stdint.h>

/* --record: start the mixer, before the game runs. Without it every call
 * below does nothing (and DirectSound is the game's own, unwrapped). */
void recaudio_init(void);
int  recaudio_on(void);

/* The recording's clock, in ms: softdd times its frames by it and the mixer
 * its samples, so the two never drift apart. */
double recaudio_clock_ms(void);

/* softdd: the first frame went out at t0 (recaudio_clock_ms), the mp4 is
 * mp4. Then, after ffmpeg has closed the video: frames frames at fps were
 * written; the sound is cut or padded to exactly that long and muxed in. */
void recaudio_start(double t0_ms, const char* mp4);
void recaudio_finish(long frames, int fps);

/* DSOUND.dll#1 (the host's shim calls it with the machine let go). */
long recaudio_DirectSoundCreate(const void* guid, void** out, void* outer);
/* The proxy over a real IDirectSound (the unit test hands it its own). */
long recaudio_wrap(void* real_dsound, void** out);

/* The tap for sound the host plays itself (the films' waveOut, the CD
 * music): 16-bit PCM at any rate, mono or stereo. plays_in_ms is how long
 * from now until the first frame of this chunk is heard (what is queued
 * before it); the stream is placed on the recording's timeline by it and
 * kept there. -1 from open when not recording: write and close then do
 * nothing. */
int  recaudio_stream_open(int rate, int channels);
void recaudio_stream_write(int id, const int16_t* pcm, int frames, double plays_in_ms);
void recaudio_stream_pause(int id, int paused);
void recaudio_stream_close(int id);

#endif
