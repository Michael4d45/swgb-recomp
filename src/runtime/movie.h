/* movie: the game's films, an MCIWnd played by FFmpeg (movie.c has the why). */
#ifndef MOVIE_H
#define MOVIE_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

/* --films DIR: a folder looked in for a film the game cannot find (NAME.avi,
 * then the Saga's Clone Campaigns name, xNAME.avi). NULL: none. */
extern const char* movie_dir;

/* ICInfo's body for the one codec the game asks about, Indeo 5: TRUE when
 * FFmpeg is there to decode it (or Windows has the codec after all). */
BOOL movie_icinfo(DWORD type, DWORD handler, void* icinfo);

/* MCIWndCreateA's body: a child window of parent that answers the MCIWnd
 * messages the game sends. NULL when the film cannot be played (missing,
 * not an AVI, no FFmpeg): the game then goes on without it. */
HWND movie_create(HWND parent, DWORD style, const char* file);

/* name (a path the game could not open) as a film in --films: the path
 * there in out, or 0. */
int movie_find(const char* name, char* out, int size);

#endif
