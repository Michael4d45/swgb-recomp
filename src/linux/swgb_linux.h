/* What the Linux and macOS host's files share (src/linux). */
#ifndef SWGB_LINUX_H
#define SWGB_LINUX_H

extern char swgb_cd_letter;   /* the play disc's drive letter, or 0 */
extern int  swgb_test_run;    /* --headless, --keys or --watchdog: boxes answer themselves */

/* shims.c: the game's own answers, registered ahead of win32hle's. */
void swgb_register_shims(void);

/* fields.c: compose EDIT controls into the primary (softdd's compose). */
void swgb_fields_init(void);

/* main.c: a file open the game made (--files, --keys on:NAME). */
void swgb_file_seen(const char* name, int ok);

/* movie.c: the films (MCIWnd over ffmpeg), as src/runtime/movie.c on Windows. */
#include <stddef.h>
#include <stdint.h>
extern const char* swgb_films_dir;                  /* --films DIR */
void swgb_movie_register(void);                     /* MCIWndCreateA, ICInfo: before the shims */
int  swgb_movie_find(const char* guest_name, char* host, size_t n);   /* a film in --films DIR */
/* fields.c's compose: the film window hwnd at l,t..r,b of the frame; 0: not a film. */
int  swgb_movie_draw(uint32_t hwnd, uint8_t* px, int pitch, int bpp, int l, int t, int r, int b, const uint32_t* pal);

/* main.c: --files (every open the game makes, printed). */
extern int swgb_log_files;

/* main.c: changes to the primary so far (a present, as softdd counts them). */
extern volatile long swgb_frames;

/* framerate.c: --fps N, --fps-log (as src/runtime/framerate.c on Windows). */
int  framerate_arg(int argc, char** argv, int i);   /* arguments it took, or 0 */
void framerate_register(void);                      /* its wrappers: before the shims */
void framerate_start(int headless);                 /* after hle_screen_open */
void framerate_on_present(void);                    /* main.c's present count, holding the machine */

#endif
