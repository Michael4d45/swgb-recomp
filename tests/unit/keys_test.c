/* src/runtime/keyscript.c without the game: the --keys script, step by step.
 *
 *   SECONDS:what      the time (fractions too), then a key name (esc enter space tab up
 *                     down left right bksp home end f1..f12, a letter or digit), alt+K,
 *                     ctrl+K, shift+K, click/rclick/move/ldown/lup:X:Y (game pixels),
 *                     wmove/wclick:X:Y (window places: L R C, PL+60, ...), size:W:H,
 *                     shot=FILE, wshot=FILE, setcursor
 *   on:NAME           wait for the game to open a file named NAME
 *   @FILE             the script from a file, a step to a line (CRLF too)
 *
 * A bad step is skipped by --keys-live (strict 0) and ends the run (exit 1) in --keys.
 *
 *   py -3 tools\test.py --level unit        (builds and runs every unit)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "keyscript.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static step_t one(const char* text, int* ok) {
    char buf[512];
    step_t st;
    memset(&st, 0xCC, sizeof st);
    lstrcpynA(buf, text, sizeof buf);
    *ok = key_parse(buf, &st, 0);
    return st;
}

#define STEP(text) (st = one(text, &ok), ok)

int main(int argc, char** argv) {
    step_t st;
    int ok;
    if (argc > 1) {                     /* the child: a strict parse of argv[1] */
        step_t s;
        char buf[256];
        lstrcpynA(buf, argv[1], sizeof buf);
        printf("parsed %d\n", key_parse(buf, &s, 1));
        return 0;
    }

    /* the mouse, in game pixels */
    CHECK(STEP("20:move:345:250") && st.at == 20000 && st.move && !st.click && st.x == 345 && st.y == 250 && !st.win, "move");
    CHECK(STEP("21:click:345:250") && st.click == 1 && st.move, "click");
    CHECK(STEP("21.5:rclick:10:20") && st.at == 21500 && st.click == 2 && st.x == 10 && st.y == 20, "rclick");
    CHECK(STEP("1:ldown:5:25") && st.click == 3 && st.x == 5 && st.y == 25, "ldown");
    CHECK(STEP("2:lup:795:420") && st.click == 4 && st.x == 795, "lup");
    CHECK(STEP("0.25:move:0:599") && st.at == 250 && st.y == 599, "fractions of a second");

    /* keys */
    CHECK(STEP("1:esc") && st.vk == VK_ESCAPE && !st.alt && !st.move, "esc");
    CHECK(STEP("1:ENTER") && st.vk == VK_RETURN, "names are not case-sensitive");
    CHECK(STEP("1:a") && st.vk == 'A', "a letter");
    CHECK(STEP("1:7") && st.vk == '7', "a digit");
    CHECK(STEP("1:f12") && st.vk == VK_F12, "f12");
    CHECK(STEP("1:f1") && st.vk == VK_F1, "f1");
    CHECK(STEP("1:bksp") && st.vk == VK_BACK, "bksp");
    CHECK(STEP("1:home") && st.vk == VK_HOME, "home");
    CHECK(STEP("1:space") && st.vk == VK_SPACE, "space");
    CHECK(STEP("1:alt+enter") && st.alt == 1 && st.vk == VK_RETURN, "alt+enter");
    CHECK(STEP("1:alt+f12") && st.alt == 1 && st.vk == VK_F12, "alt+f12");
    CHECK(STEP("1:ctrl+1") && st.alt == 2 && st.vk == '1', "ctrl+1");
    CHECK(STEP("1:shift+a") && st.alt == 3 && st.vk == 'A', "shift+a");

    /* the window */
    CHECK(STEP("5:size:1100:620") && st.size && st.x == 1100 && st.y == 620, "size");
    CHECK(STEP("6:wmove:R-3:C") && st.win && st.move && !st.click && !strcmp(st.xs, "R-3") && !strcmp(st.ys, "C"), "wmove");
    CHECK(STEP("6:wclick:PL+60:BB") && st.win && st.click == 1 && !strcmp(st.xs, "PL+60") && !strcmp(st.ys, "BB"), "wclick");
    CHECK(STEP("6:wmove:120:40") && !strcmp(st.xs, "120") && !strcmp(st.ys, "40"), "wmove in numbers");
    CHECK(STEP("7:setcursor") && st.setcursor, "setcursor");

    /* shots: the file made absolute (the run's working folder) */
    CHECK(STEP("4:shot=x.bmp") && !st.wshot && strlen(st.shot) > 6 && st.shot[1] == ':' && !strcmp(st.shot + strlen(st.shot) - 6, "\\x.bmp"), "shot: %s", st.shot);
    CHECK(STEP("4:wshot=C:\\t\\y.bmp") && st.wshot && !strcmp(st.shot, "C:\\t\\y.bmp"), "wshot: %s", st.shot);

    /* waits */
    CHECK(STEP("on:logo1") && !strcmp(st.on, "logo1") && st.at == 0, "on:");

    /* bad steps: skipped when not strict */
    CHECK(!STEP("8:frob"), "an unknown key");
    CHECK(!STEP("esc"), "no time");
    CHECK(!STEP("9:wmove:R"), "wmove with one coordinate");
    CHECK(!STEP("1:alt+frob"), "alt+ an unknown key");

    /* ... and the end of the run when strict (--keys): exit 1 */
    {
        char self[MAX_PATH], cmd[2 * MAX_PATH];
        GetModuleFileNameA(NULL, self, MAX_PATH);
        static const char* bad[] = { "8:frob", "9:wmove:R" };
        for (int i = 0; i < 2; i++) {
            _snprintf(cmd, sizeof cmd, "\"\"%s\" %s 2>&1\"", self, bad[i]);
            FILE* p = _popen(cmd, "r");
            char out[1024];
            size_t n = fread(out, 1, sizeof out - 1, p);
            out[n] = 0;
            int rc = _pclose(p);
            CHECK(rc == 1 && strstr(out, "--keys") && !strstr(out, "parsed"), "strict %s: exit %d, %s", bad[i], rc, out);
        }
        _snprintf(cmd, sizeof cmd, "\"\"%s\" 1:esc 2>&1\"", self);
        FILE* p = _popen(cmd, "r");
        char out[256];
        size_t n = fread(out, 1, sizeof out - 1, p);
        out[n] = 0;
        CHECK(_pclose(p) == 0 && strstr(out, "parsed 1"), "strict, a good step: %s", out);
    }

    /* a whole script, inline and from a file (CRLF, blank lines) */
    {
        static step_t steps[16];
        int n = keys_parse_all("20:move:40:565,21:click:40:565,on:intro,4:esc", steps, 16);
        CHECK(n == 4 && steps[1].click == 1 && !strcmp(steps[2].on, "intro") && steps[3].vk == VK_ESCAPE, "inline: %d steps", n);
        CHECK(keys_parse_all("1:a,2:b,3:c,4:d", steps, 3) == 3, "at most max steps");

        char path[MAX_PATH], spec[MAX_PATH + 1];
        GetTempPathA(MAX_PATH, path);
        _snprintf(path + strlen(path), 64, "keys_test_%lu.txt", GetCurrentProcessId());
        FILE* f = fopen(path, "wb");
        fputs("10:move:1:2\r\n11:click:1:2\r\n\r\n12:ctrl+3\n13:shot=s.bmp\n", f);
        fclose(f);
        spec[0] = '@';
        strcpy(spec + 1, path);
        n = keys_parse_all(spec, steps, 16);
        CHECK(n == 4 && steps[0].at == 10000 && steps[1].click == 1 && steps[2].alt == 2 && steps[2].vk == '3' && steps[3].shot[0],
              "@FILE: %d steps", n);
        char* t = keys_text(spec);
        CHECK(t && !strchr(t, '\n') && !strchr(t, '\r'), "@FILE: lines become commas");
        free(t);
        t = keys_text("1:a,2:b");
        CHECK(t && !strcmp(t, "1:a,2:b"), "inline text as it is");
        free(t);
        DeleteFileA(path);
    }

    printf(fails ? "%d FAILED\n" : "OK\n", fails);
    return fails != 0;
}
