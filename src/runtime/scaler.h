/* scaler: the frame -> the picture present.c draws, only what changed
 * (scaler.c has the why). */
#ifndef SCALER_H
#define SCALER_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

typedef struct { int n; int* i0; int* i1; int* w; int cap; } scaler_axis_t;

/* One picture's state; zero it to start. present.c keeps one for the window
 * and one for --keys wshot. */
typedef struct {
    int w, h, bpp, dw, dh, mode, one;   /* what the picture is of; one: 1:1 */
    uint32_t lut[256];                  /* the palette as 0x00RRGGBB */
    uint8_t* prev;  size_t prev_cap;    /* the last frame, as it came */
    uint32_t* src;  size_t src_cap;     /* the frame as 0x00RRGGBB (w x h) */
    uint32_t* rows; size_t rows_cap;    /* each source row scaled across (dw x h) */
    uint32_t* img;  size_t img_cap;     /* the picture (dw x dh) */
    scaler_axis_t ax, ay;
} scaler_t;

/* Brings the picture (dw x dh, PRES_* filter mode) up to date with the frame
 * (8-bit with pal, 16-bit RGB565 or 32-bit). force: as if every row changed.
 * [*y0, *y1): the picture rows that changed (empty: none). 0: cannot (an
 * unknown format, no memory). */
int scaler_run(scaler_t* sc, const uint8_t* bits, int w, int h, int pitch, int bpp, const PALETTEENTRY* pal,
               int dw, int dh, int mode, int force, int* y0, int* y1);

/* The picture, dw x dh 0x00RRGGBB, top row first. */
const uint32_t* scaler_picture(const scaler_t* sc);

#endif
