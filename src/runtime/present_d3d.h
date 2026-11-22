/* present_d3d: the picture through Direct3D 11 (present_d3d.c has the why). */
#ifndef PRESENT_D3D_H
#define PRESENT_D3D_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

extern int present_d3d_enabled;   /* --present d3d11 */

/* 1: Direct3D is (or may yet be) how this window is drawn. */
int present_d3d_ready(HWND wnd);

/* The frame (w x h 0x00RRGGBB, rows [y0, y1) new since the last call) into
 * the window's client area (cw x ch), scaled into r with a PRES_* filter,
 * black around it. full: upload every row. 0: failed, use GDI (a lasting
 * failure turns Direct3D off for the run). */
int present_d3d_draw(HWND wnd, const uint32_t* pic, int w, int h, int y0, int y1, int cw, int ch, const RECT* r,
                     int filter, int full);

#endif
