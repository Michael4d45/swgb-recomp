/* softdd: a software DirectDraw (softdd.c has the why). */
#ifndef SOFTDD_H
#define SOFTDD_H
#include <stdint.h>

extern int         softdd_headless;      /* present nowhere but --record */
extern int         softdd_scale;         /* windowed client = mode x scale */
extern const char* softdd_record;        /* mp4 path, or NULL */
extern int         softdd_stop_after;    /* exit after N presents, 0 = never */
extern volatile long softdd_frames;

/* Called with the front buffer (RGB565) on every present, or NULL. */
extern void (*softdd_on_present)(const uint8_t* bits, int w, int h, int pitch, long frame);

/* DirectDrawCreate's body: one IDirectDraw per process. */
long softdd_create(void** out);

/* --shot: write the next presented frame to path (.bmp). */
void softdd_shot(const char* path);

/* The game window's timer: re-present while a text field (a child window) is
 * up, so typing and the caret show between the game's own frames. */
void softdd_tick(void);
#define SOFTDD_TIMER 0x5D01

/* Present the front buffer now (a film's new frame, composed in). */
void softdd_refresh(void);

/* Close the recording, so the mp4 is complete. */
void softdd_finish(void);

#endif
