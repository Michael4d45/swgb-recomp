/* keyscript: the --keys script's steps, parsed (keyscript.c; host.c runs them). */
#ifndef KEYSCRIPT_H
#define KEYSCRIPT_H
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define KEYSCRIPT_PATH MAX_PATH
#else
#define KEYSCRIPT_PATH 512   /* the Linux and macOS host (src/linux) parses with this too */
#endif

/* One step. at: ms from the start (or from the last on:NAME). click: 1 left, 2 right,
 * 3 left down, 4 left up. alt: 1 Alt+, 2 Ctrl+, 3 Shift+. win: X:Y are window places
 * (xs, ys: "R-3", "PL+60", "120"). */
typedef struct { unsigned long at; int vk, x, y, click, move, alt, win, size, wshot, setcursor; char xs[24], ys[24], on[64], shot[KEYSCRIPT_PATH]; } step_t;

/* Windows' virtual-key codes, by value, so the grammar builds where windows.h does not exist. */
#define KS_VK_BACK 0x08
#define KS_VK_TAB 0x09
#define KS_VK_RETURN 0x0D
#define KS_VK_ESCAPE 0x1B
#define KS_VK_SPACE 0x20
#define KS_VK_END 0x23
#define KS_VK_HOME 0x24
#define KS_VK_LEFT 0x25
#define KS_VK_UP 0x26
#define KS_VK_RIGHT 0x27
#define KS_VK_DOWN 0x28
#define KS_VK_F1 0x70   /* F1..F12: 0x70..0x7B */

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
