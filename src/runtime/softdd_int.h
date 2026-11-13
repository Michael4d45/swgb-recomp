/* softdd's objects. */
#pragma once
#define CINTERFACE
#include <windows.h>
#include <ddraw.h>
#include <stdint.h>

typedef struct Surf {
    IDirectDrawSurfaceVtbl* lpVtbl;
    IDirectDrawSurface3Vtbl* v3;         /* the same object as IDirectDrawSurface2/3 */
    LONG      refs;
    DWORD     caps;
    int       w, h, pitch, bpp;
    uint32_t  rmask, gmask, bmask, amask;    /* 16 and 32 bits; 8 is palettized */
    uint8_t*  bits;
    HBITMAP   hbm;
    HDC       hdc;                       /* lazily, for GetDC */
    struct Surf* back;                   /* primary -> its back buffer */
    struct Surf* zbuf;                   /* AddAttachedSurface'd depth buffer */
    DWORD     ck_lo, ck_hi;
    int       has_ck;
    IDirectDrawPalette* pal;
    IDirectDrawClipper* clip;
    uint32_t  version;                   /* bumped on every write: textures re-upload */
} Surf;

typedef struct {
    IDirectDrawVtbl* lpVtbl;
    IDirectDraw2Vtbl* v2;
    LONG  refs;
    HWND  hwnd;
    DWORD coop;
    int   w, h, bpp;
    Surf* primary;
} DD;

typedef struct {
    IDirectDrawPaletteVtbl* lpVtbl;
    LONG refs;
    DWORD caps;
    PALETTEENTRY e[256];
} Pal;

Surf* softdd_surf(void* iface);          /* any of a surface's interfaces -> the surface */
DD*   softdd_dd(void);

