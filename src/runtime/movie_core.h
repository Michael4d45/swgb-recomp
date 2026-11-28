/* movie_core: the films' platform-neutral half (movie_core.c), which both
 * hosts' players share: src/runtime/movie.c (an MCIWnd of its own, waveOut)
 * and src/linux/movie.c (win32hle's windows and mixer). The window, the
 * ffmpeg processes and the sound device stay with each host. */
#ifndef MOVIE_CORE_H
#define MOVIE_CORE_H
#include <stddef.h>
#include <stdint.h>

/* ---- the film's size and rate, from its AVI header ---- */
typedef struct { int w, h; uint32_t rate, scale; int has_audio; } avi_info;

/* 0: missing, not an AVI, or larger than 4096 either way. Without a video
 * stream header the rate is the main header's microseconds a frame. */
int avi_probe(const char* path, avi_info* ai);

/* --films DIR: the file to look for there, by the name the game opened
 * (any folder; only .avi): x 0 is NAME.avi itself, x 1 the Saga's xNAME.avi.
 * 0: not a film name. */
int film_name(const char* guest_name, int x, char* out, size_t n);

/* ---- the picture ----
 * The film palette, which the game installs from MCIWNDM_GETPALETTE: black, a
 * 6x6x6 colour cube, the greys between the cube's, white. As PALETTEENTRY
 * dwords, r | g << 8 | b << 16 (the flags byte 0). */
const uint32_t* film_palette_entries(void);

/* pe (PALETTEENTRY dwords) is the film palette: entries 1 to 254, colour
 * only (the game keeps 0 and 255 black and white, which it is anyway). */
int film_palette_is(const uint32_t* pe);

/* RGB555 -> the nearest entry of pe, for a palette that is not the film's:
 * remade only when pe changes. */
const uint8_t* film_lut(const uint32_t* pe);

/* A pixel as an entry of the film palette, ordered-dithered by t (0..15, a 4x4
 * Bayer matrix's): near-greys on its greys, the rest on its cube; black,
 * white and the cube's own colours stay flat. */
uint8_t film_index(int r, int g, int b, int t);

/* Row y of a window cw pixels wide, from a BGRA frame row vw wide (scaled by
 * the nearest pixel): dithered into the film palette (lut NULL), or each
 * pixel's nearest entry through lut (film_lut). */
void film_row8(uint8_t* row, int cw, const uint32_t* src, int vw, int y, const uint8_t* lut);

/* ---- the clock ----
 * In ms from the film's start: the sound's position while it plays (the
 * host's device answers that), else the wall clock from base_ms at
 * base_tick, less the time paused. Ticks are a host's ms clock. Each host
 * holds its own lock around these. */
typedef struct {
    int paused;
    int audio_on;                 /* the sound is the clock */
    double base_ms;
    uint32_t base_tick, pause_tick, pause_total;
} film_clock;

/* The wall clock's time (the clock while audio_on is 0). */
double film_wall_ms(const film_clock* c, uint32_t now);
/* No sound: the wall clock, from 0 now. */
void film_clock_start(film_clock* c, uint32_t now);
/* The sound ran out at sound_ms: if it was the clock, the wall clock carries
 * on from there; if not, nothing changes. */
void film_clock_to_wall(film_clock* c, uint32_t now, double sound_ms);
/* Pause or resume; 1 if that changed anything. */
int film_clock_pause(film_clock* c, int pause, uint32_t now);

/* MCIWNDM_GETMODEA's answer: "playing", "paused" or "stopped" (stopped ends the film). */
const char* film_mode(int playing, int paused, int done);

#endif
