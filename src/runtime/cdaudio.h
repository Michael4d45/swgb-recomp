/*
 * cdaudio.c - the game's CD music from the Steam release's Ogg Vorbis tracks.
 *
 * Battlegrounds.exe plays its music from disc 2's audio tracks through
 * WINMM's MCI cdaudio device (mciSendCommandA) and sets its loudness on the
 * mixer's CD line. A Steam install has no audio CD: it has Music\TrackNN.ogg,
 * CD track NN (track 1 is the data track). This module is that CD: MCI_OPEN,
 * SET, STATUS, PLAY, SEEK, STOP, PAUSE, CLOSE and the MM_MCINOTIFY at the end
 * of a play, over the Ogg files (stb_vorbis), and a mixer with one CD line
 * whose volume control is the music's gain. It never touches the system's
 * mixer.
 *
 * Both hosts: on Windows (host.c) it plays on waveOut, in the process's own
 * audio session, which test runs mute; on win32hle (src/linux) on win32hle's
 * audio stream, under hle_dsound_set_master. docs/music.md has the design.
 */
#ifndef SWGB_CDAUDIO_H
#define SWGB_CDAUDIO_H
#include <stdint.h>

/* --music off|native|DIR, the host's argument loop: the number of argv
 * entries taken (0: not ours). off: no CD (the game carries on without
 * music); native: the real WINMM (or Clone Campaigns' WIN32.dll), each
 * call logged; DIR: the Ogg tracks' folder (default: the game's Music). */
int cdaudio_arg(int argc, char** argv, int i);

/* The shims, by import name, for the host's table (both hosts). */
void cdaudio_mciSendCommandA(void);
void cdaudio_mciGetErrorStringA(void);
void cdaudio_mixerGetNumDevs(void);
void cdaudio_mixerOpen(void);
void cdaudio_mixerClose(void);
void cdaudio_mixerGetLineInfoA(void);
void cdaudio_mixerGetLineControlsA(void);
void cdaudio_mixerGetControlDetailsA(void);
void cdaudio_mixerSetControlDetails(void);

/* ---- the core, without the guest's calling convention (the unit test) ---- */
/* An MCI command: dev, msg, flags as the game passes them; parms the guest's
 * parameter block as host memory. 0, or an MCIERR_ code. OPEN's device type
 * comes as type_id (MCI_OPEN_TYPE_ID) or type_name. */
uint32_t cdaudio_command(uint32_t dev, uint32_t msg, uint32_t flags, uint32_t* parms, const char* type_name);
void     cdaudio_set_dir(const char* dir);          /* the tracks' folder; NULL: the game's Music */
void     cdaudio_set_gain(float gain);              /* 0..1: what the mixer's CD volume sets */
float    cdaudio_gain(void);
void     cdaudio_shutdown(void);                    /* stop and close: the test, the host's exit */
/* Test hooks: an output that takes the PCM as fast as it comes (no device,
 * no clock), and the notifications delivered here instead of posted. */
extern int cdaudio_test_instant;
extern void (*cdaudio_pcm_hook)(const int16_t* pcm, int frames);   /* what the instant output takes */
extern void (*cdaudio_notify_hook)(uint32_t hwnd, uint32_t code, uint32_t dev);

#endif
