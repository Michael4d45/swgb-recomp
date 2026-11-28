/*
 * keyscript: the --keys script, parsed and timed. Each host runs a step its
 * own way (key_run in host.c and src/linux/main.c: messages to a window, or
 * win32hle's input); everything else is here, the same on both: the grammar,
 * window places, the script's clock and its on:NAME waits, and the
 * --keys-live reader. This half needs nothing of the game, so
 * tests/unit/keys_test.c checks it alone (tools/test.py --level unit). The
 * threads and events are Win32's, through winport.h on the Linux and macOS
 * host.
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <strings.h>
#include <unistd.h>
#include "winport.h"
#define _strdup strdup
#endif
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hostargs.h"
#include "keyscript.h"

static const struct { const char* n; int vk; } g_keynames[] = {
    { "esc", KS_VK_ESCAPE }, { "enter", KS_VK_RETURN }, { "space", KS_VK_SPACE }, { "tab", KS_VK_TAB },
    { "up", KS_VK_UP }, { "down", KS_VK_DOWN }, { "left", KS_VK_LEFT }, { "right", KS_VK_RIGHT },
    { "bksp", KS_VK_BACK }, { "f1", KS_VK_F1 }, { "f2", KS_VK_F1 + 1 }, { "f3", KS_VK_F1 + 2 }, { "f4", KS_VK_F1 + 3 },
    { "f5", KS_VK_F1 + 4 }, { "f6", KS_VK_F1 + 5 }, { "f7", KS_VK_F1 + 6 }, { "f8", KS_VK_F1 + 7 }, { "f9", KS_VK_F1 + 8 },
    { "f10", KS_VK_F1 + 9 }, { "f11", KS_VK_F1 + 10 }, { "f12", KS_VK_F1 + 11 }, { "home", KS_VK_HOME }, { "end", KS_VK_END },
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
        snprintf(st.on, sizeof st.on, "%s", tok + 3);
        *out = st;
        return 1;
    }
    st.at = (unsigned long)(atof(tok) * 1000);
    char* what = c + 1;
    if (!strncmp(what, "wshot=", 6)) {   /* the window as drawn: letterbox bars and all */
        host_full_path(what + 6, st.shot, sizeof st.shot);
        st.wshot = 1;
    } else if (!strncmp(what, "shot=", 5)) {
        host_full_path(what + 5, st.shot, sizeof st.shot);
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

int keys_place(const char* t, int axis, int cw, int ch, const int pic[4]) {
    char name[8] = "";
    int n = 0, base;
    const char* q = t;
    while (isalpha((unsigned char)*q) && n < 7) name[n++] = *q++;
    name[n] = 0;
    if (!n) return atoi(t);
    if (axis == 0) {
        if (!_stricmp(name, "L")) base = 0;
        else if (!_stricmp(name, "R")) base = cw - 1;
        else if (!_stricmp(name, "C")) base = cw / 2;
        else if (!_stricmp(name, "PL")) base = pic[0];
        else if (!_stricmp(name, "PR")) base = pic[2] - 1;
        else if (!_stricmp(name, "BL")) base = pic[0] / 2;
        else if (!_stricmp(name, "BR")) base = (pic[2] + cw) / 2;
        else { fprintf(stderr, "--keys: no place %s on x\n", name); exit(1); }
    } else {
        if (!_stricmp(name, "T")) base = 0;
        else if (!_stricmp(name, "B")) base = ch - 1;
        else if (!_stricmp(name, "C")) base = ch / 2;
        else if (!_stricmp(name, "PT")) base = pic[1];
        else if (!_stricmp(name, "PB")) base = pic[3] - 1;
        else if (!_stricmp(name, "BT")) base = pic[1] / 2;
        else if (!_stricmp(name, "BB")) base = (pic[3] + ch) / 2;
        else { fprintf(stderr, "--keys: no place %s on y\n", name); exit(1); }
    }
    return base + (*q ? atoi(q) : 0);
}

/* ---- on:NAME ----------------------------------------------------------------
 * One name is listened for at a time: the next on:NAME, from the step before
 * it (an open during that step's wait is not missed). g_seen holds an open
 * that came before its wait began; the event wakes the wait. Both under
 * g_wait_cs: the game's thread matches while the script's thread arms. */
static CRITICAL_SECTION g_wait_cs;
static HANDLE g_seen_ev;            /* auto-reset */
static char g_wait[64];
static int g_seen;
static volatile int g_waits;        /* keys_start made the above */

static step_t* g_steps;
static int g_nsteps;
static keys_run_fn g_run;
static char g_live_path[4096];
static DWORD g_t0;

static void arm(const char* name) {
    EnterCriticalSection(&g_wait_cs);
    if (!g_wait[0] && !g_seen) snprintf(g_wait, sizeof g_wait, "%s", name);
    LeaveCriticalSection(&g_wait_cs);
}

static void wait_file(const char* name) {
    arm(name);
    WaitForSingleObject(g_seen_ev, INFINITE);
    EnterCriticalSection(&g_wait_cs);
    g_seen = 0;
    LeaveCriticalSection(&g_wait_cs);
}

static int contains_ci(const char* s, const char* sub) {
    size_t n = strlen(sub);
    for (; *s; s++) {
        size_t k = 0;
        while (k < n && s[k] && tolower((unsigned char)s[k]) == tolower((unsigned char)sub[k])) k++;
        if (k == n) return 1;
    }
    return 0;
}

void keys_file_opened(const char* name) {
    if (!g_waits || !name) return;
    EnterCriticalSection(&g_wait_cs);
    if (g_wait[0] && contains_ci(name, g_wait)) {
        g_wait[0] = 0;
        g_seen = 1;
        SetEvent(g_seen_ev);
    }
    LeaveCriticalSection(&g_wait_cs);
}

static DWORD WINAPI script_thread(LPVOID unused) {
    (void)unused;
    DWORD t0 = GetTickCount();
    for (int i = 0; i < g_nsteps; i++) {
        step_t* st = &g_steps[i];
        if (st->on[0]) {
            wait_file(st->on);
            t0 = GetTickCount();
            printf("[keys] %s opened: times count from here\n", st->on);
            continue;
        }
        for (int j = i + 1; j < g_nsteps; j++)   /* the next wait listens from now */
            if (g_steps[j].on[0]) { arm(g_steps[j].on); break; }
        DWORD at = t0 + st->at, now = GetTickCount();
        if ((int)(at - now) > 0) Sleep(at - now);
        g_run(st);
    }
    return 0;
}

int keys_live_poll(keys_live* lv, keys_run_fn run) {
    FILE* f = fopen(lv->path, "rb");
    int c, k = 0;
    if (!f) return 0;
    fseek(f, lv->pos, SEEK_SET);
    while ((c = fgetc(f)) != EOF) {
        lv->pos++;
        if (c == '\n' || c == '\r' || c == ',') {
            step_t st;
            lv->line[lv->n] = 0;
            if (lv->n && key_parse(lv->line, &st, 0) && !st.on[0]) {
                st.at = GetTickCount() - g_t0;
                run(&st);
                k++;
            }
            lv->n = 0;
        } else if (lv->n < sizeof lv->line - 1) lv->line[lv->n++] = (char)c;
    }
    fclose(f);
    return k;
}

static DWORD WINAPI live_thread(LPVOID unused) {
    (void)unused;
    keys_live lv;
    memset(&lv, 0, sizeof lv);
    lv.path = g_live_path;
    for (;;) {
        keys_live_poll(&lv, g_run);
        Sleep(200);
    }
    return 0;
}

void keys_start(step_t* steps, int n, const char* live_file, keys_run_fn run) {
    g_steps = steps, g_nsteps = n, g_run = run;
    g_t0 = GetTickCount();
    if (n) {
        InitializeCriticalSection(&g_wait_cs);
        g_seen_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
        g_waits = 1;
        if (steps[0].on[0]) arm(steps[0].on);
        CloseHandle(CreateThread(NULL, 0, script_thread, NULL, 0, NULL));
    }
    if (live_file && live_file[0]) {
        snprintf(g_live_path, sizeof g_live_path, "%s", live_file);
        CloseHandle(CreateThread(NULL, 0, live_thread, NULL, 0, NULL));
    }
}
