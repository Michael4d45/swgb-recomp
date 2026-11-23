/*
 * keyscript: the --keys script's steps, parsed. host.c runs them (keys_thread,
 * keys_live_thread, key_run); this half needs nothing of the game, so
 * tests/unit/keys_test.c checks it alone (tools/test.py --level unit).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "keyscript.h"

static const struct { const char* n; int vk; } g_keynames[] = {
    { "esc", VK_ESCAPE }, { "enter", VK_RETURN }, { "space", VK_SPACE }, { "tab", VK_TAB },
    { "up", VK_UP }, { "down", VK_DOWN }, { "left", VK_LEFT }, { "right", VK_RIGHT },
    { "bksp", VK_BACK }, { "f1", VK_F1 }, { "f2", VK_F2 }, { "f3", VK_F3 }, { "f4", VK_F4 },
    { "f5", VK_F5 }, { "f6", VK_F6 }, { "f7", VK_F7 }, { "f8", VK_F8 }, { "f9", VK_F9 }, { "f10", VK_F10 },
    { "f11", VK_F11 }, { "f12", VK_F12 }, { "home", VK_HOME }, { "end", VK_END },
};

int key_by_name(const char* n) {
    for (int i = 0; i < (int)(sizeof g_keynames / sizeof g_keynames[0]); i++)
        if (!_stricmp(n, g_keynames[i].n)) return g_keynames[i].vk;
    if (n[0] && !n[1] && isalnum((unsigned char)n[0])) return toupper((unsigned char)n[0]);
    return 0;
}

/* --keys @FILE reads the script from a file (a long one does not fit a command line); a newline
 * separates steps like a comma. */
char* keys_text(const char* spec) {
    if (spec[0] != '@') return _strdup(spec);
    FILE* f = fopen(spec + 1, "rb");
    if (!f) { fprintf(stderr, "--keys: cannot read %s\n", spec + 1); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* b = (char*)malloc(n + 1);
    n = (long)fread(b, 1, n, f);
    b[n] = 0;
    fclose(f);
    for (long i = 0; i < n; i++) if (b[i] == '\r' || b[i] == '\n') b[i] = ',';
    return b;
}

/* One step of a --keys script ("SECONDS:what"), or of --keys-live (where the seconds are
 * ignored: a live step runs when it is read). 0: nothing to do. */
int key_parse(char* tok, step_t* out, int strict) {
    step_t st = { 0 };
    char* c = strchr(tok, ':');
    if (!c) return 0;
    if (!strncmp(tok, "on:", 3)) {
        lstrcpynA(st.on, tok + 3, sizeof st.on);
        *out = st;
        return 1;
    }
    st.at = (DWORD)(atof(tok) * 1000);
    char* what = c + 1;
    if (!strncmp(what, "wshot=", 6)) {   /* the window as drawn: letterbox bars and all */
        GetFullPathNameA(what + 6, MAX_PATH, st.shot, NULL);
        st.wshot = 1;
    } else if (!strncmp(what, "shot=", 5)) {
        GetFullPathNameA(what + 5, MAX_PATH, st.shot, NULL);
    } else if (!strncmp(what, "size:", 5)) {   /* the window's client area: W:H */
        st.size = 1;
        sscanf(what + 5, "%d:%d", &st.x, &st.y);
    } else if (!strncmp(what, "wclick:", 7) || !strncmp(what, "wmove:", 6)) {   /* window pixels, mapped like the mouse */
        st.win = 1;
        st.click = what[1] == 'c';
        st.move = 1;
        /* X:Y, each a number or a place in the window: L R C (edges, centre), PL PR PT PB (the picture's
         * edges), BL BR BT BB (the middle of a bar), then +N or -N: wmove:R:C, wmove:L+40:T-3 */
        if (sscanf(strchr(what, ':') + 1, "%23[^:]:%23s", st.xs, st.ys) != 2) { fprintf(stderr, "--keys: %s\n", what); if (strict) exit(1); return 0; }
    } else if (!strcmp(what, "setcursor")) {   /* the pointer comes back into the window (WM_SETCURSOR) */
        st.setcursor = 1;
    } else if (!strncmp(what, "click:", 6) || !strncmp(what, "rclick:", 7) || !strncmp(what, "move:", 5) ||
               !strncmp(what, "ldown:", 6) || !strncmp(what, "lup:", 4)) {
        /* ldown and lup hold the left button across the moves between them: a drag
         * (the game's box select). */
        st.click = what[0] == 'c' ? 1 : what[0] == 'r' ? 2 : !strncmp(what, "ldown:", 6) ? 3 : what[1] == 'u' ? 4 : 0;
        st.move = 1;
        sscanf(strchr(what, ':') + 1, "%d:%d", &st.x, &st.y);
    } else if (!strncmp(what, "alt+", 4) && (st.vk = key_by_name(what + 4))) {
        st.alt = 1;   /* Alt+Enter, Alt+F12: the presenter's hotkeys */
    } else if (!strncmp(what, "ctrl+", 5) && (st.vk = key_by_name(what + 5))) {
        st.alt = 2;   /* Ctrl+1: the game's control groups */
    } else if (!strncmp(what, "shift+", 6) && (st.vk = key_by_name(what + 6))) {
        st.alt = 3;
    } else if (!(st.vk = key_by_name(what))) {
        fprintf(stderr, "--keys: unknown key \"%s\"\n", what);
        if (strict) exit(1);
        return 0;
    }
    *out = st;
    return 1;
}

int keys_parse_all(const char* spec, step_t* steps, int max) {
    char* buf = keys_text(spec);
    int n = 0;
    for (char* tok = strtok(buf, ","); tok && n < max; tok = strtok(NULL, ",")) {
        step_t st;
        if (key_parse(tok, &st, 1)) steps[n++] = st;
    }
    free(buf);
    return n;
}
