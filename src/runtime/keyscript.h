/* keyscript: the --keys script's steps, parsed (keyscript.c; host.c runs them). */
#ifndef KEYSCRIPT_H
#define KEYSCRIPT_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* One step. at: ms from the start (or from the last on:NAME). click: 1 left, 2 right,
 * 3 left down, 4 left up. alt: 1 Alt+, 2 Ctrl+, 3 Shift+. win: X:Y are window places
 * (xs, ys: "R-3", "PL+60", "120"). */
typedef struct { DWORD at; int vk, x, y, click, move, alt, win, size, wshot, setcursor; char xs[24], ys[24], on[64], shot[MAX_PATH]; } step_t;

/* A key name (esc, enter, f1..f12, a letter or digit, ...) as a virtual key; 0: none. */
int key_by_name(const char* n);

/* spec, or with "@FILE" that file's text with its line breaks as commas (malloc'd).
 * An unreadable file ends the run (exit 1). */
char* keys_text(const char* spec);

/* One step ("SECONDS:what" or "on:NAME"); tok is changed. 0: nothing to do; a bad step
 * ends the run (exit 1) when strict (--keys), is skipped when not (--keys-live). */
int key_parse(char* tok, step_t* out, int strict);

/* A whole script (inline or @FILE), strict, into steps: how many, at most max. */
int keys_parse_all(const char* spec, step_t* steps, int max);

#endif
