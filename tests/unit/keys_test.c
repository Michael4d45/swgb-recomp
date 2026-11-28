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
 *   python3 tools/test.py --level unit      (Linux and macOS: the same parser)
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define SEP "\\"
#define ABS(p) ((p)[1] == ':')
#define WSHOT_IN "C:\\t\\y.bmp"
#define SELF_CMD(args) "\"\"%s\" " args " 2>&1\""   /* cmd.exe: the outer quotes go */
#else
#include <unistd.h>
#include <limits.h>
#define VK_ESCAPE KS_VK_ESCAPE
#define VK_RETURN KS_VK_RETURN
#define VK_F1 KS_VK_F1
#define VK_F12 (KS_VK_F1 + 11)
#define VK_BACK KS_VK_BACK
#define VK_HOME KS_VK_HOME
#define VK_SPACE KS_VK_SPACE
#define MAX_PATH PATH_MAX
#define lstrcpynA(d, s, n) snprintf((d), (n), "%s", (s))
#define _snprintf snprintf
#define _popen popen
/* pclose gives a wait status: the exit code is its high byte */
#define _pclose(p) (pclose(p) >> 8)
#define GetModuleFileNameA(m, buf, n) do { ssize_t k_ = readlink("/proc/self/exe", (buf), (n) - 1); (buf)[k_ > 0 ? k_ : 0] = 0; } while (0)
#define GetTempPathA(n, buf) snprintf((buf), (n), "%s/", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp")
#define GetCurrentProcessId() ((unsigned long)getpid())
#define DeleteFileA(p) remove(p)
#define SEP "/"
#define ABS(p) ((p)[0] == '/')
#define WSHOT_IN "/t/y.bmp"
#define SELF_CMD(args) "'%s' " args " 2>&1"
#endif

#include "hostargs.h"
#include "keyscript.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#ifdef _WIN32
#define SLEEP_MS(ms) Sleep(ms)
#else
#define SLEEP_MS(ms) usleep((ms) * 1000)
#endif

/* The host's half, for keys_live_poll and keys_start: what ran. */
static volatile int g_ran;
static step_t g_last;
static void record_step(step_t* st) { g_last = *st; g_ran++; }

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
    if (argc > 1) {                     /* the child: strict parse/validation checks */
        if (argc >= 4 && !strcmp(argv[1], "--place")) {
            const int pic[4] = { 100, 0, 900, 600 };
            printf("place %d\n", keys_place(argv[2], atoi(argv[3]), 1000, 600, pic));
            return 0;
        }
        if (argc >= 6 && !strcmp(argv[1], "--int")) {
            printf("int %ld\n", host_int_arg(argv[2], argv[3], atol(argv[4]), atol(argv[5])));
            return 0;
        }
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
    CHECK(STEP("4:shot=x.bmp") && !st.wshot && strlen(st.shot) > 6 && ABS(st.shot) && !strcmp(st.shot + strlen(st.shot) - 6, SEP "x.bmp"), "shot: %s", st.shot);
    CHECK(STEP("4:wshot=" WSHOT_IN) && st.wshot && !strcmp(st.shot, WSHOT_IN), "wshot: %s", st.shot);

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
            _snprintf(cmd, sizeof cmd, SELF_CMD("%s"), self, bad[i]);
            FILE* p = _popen(cmd, "r");
            char out[1024];
            size_t n = fread(out, 1, sizeof out - 1, p);
            out[n] = 0;
            int rc = _pclose(p);
            CHECK(rc == 1 && strstr(out, "--keys") && !strstr(out, "parsed"), "strict %s: exit %d, %s", bad[i], rc, out);
        }
        _snprintf(cmd, sizeof cmd, SELF_CMD("1:esc"), self);
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

    /* ---- keys_place: window places ---- */
    {
        const int pic[4] = { 100, 0, 900, 600 };   /* a 1000x600 client, the picture with bars left and right */
        const int vbar[4] = { 0, 60, 800, 540 };   /* top and bottom bars */
        CHECK(keys_place("L", 0, 1000, 600, pic) == 0 && keys_place("R", 0, 1000, 600, pic) == 999 &&
              keys_place("C", 0, 1000, 600, pic) == 500, "L R C");
        CHECK(keys_place("PL", 0, 1000, 600, pic) == 100 && keys_place("PR", 0, 1000, 600, pic) == 899, "PL PR");
        CHECK(keys_place("BL", 0, 1000, 600, pic) == 50 && keys_place("BR", 0, 1000, 600, pic) == 950, "the bars' middles");
        CHECK(keys_place("T", 1, 1000, 600, pic) == 0 && keys_place("B", 1, 1000, 600, pic) == 599 &&
              keys_place("pb", 1, 1000, 600, pic) == 599 && keys_place("C", 1, 1000, 600, pic) == 300, "T B pb C on y");
        CHECK(keys_place("PT", 1, 800, 600, vbar) == 60 && keys_place("BT", 1, 800, 600, vbar) == 30 &&
              keys_place("BB", 1, 800, 600, vbar) == 570, "PT BT BB on y");
        CHECK(keys_place("PL+60", 0, 1000, 600, pic) == 160 && keys_place("R-3", 0, 1000, 600, pic) == 996, "+N, -N");
        CHECK(keys_place("120", 0, 1000, 600, pic) == 120 && keys_place("-5", 1, 1000, 600, pic) == -5, "plain pixels");

        char self[MAX_PATH], cmd[2 * MAX_PATH], out[256];
        GetModuleFileNameA(NULL, self, MAX_PATH);
        _snprintf(cmd, sizeof cmd, SELF_CMD("--place ZZ 0"), self);
        FILE* p = _popen(cmd, "r");
        size_t n = fread(out, 1, sizeof out - 1, p);
        out[n] = 0;
        CHECK(_pclose(p) == 1 && strstr(out, "--keys: no place"), "unknown place exits 1: %s", out);
    }

    /* ---- keys_live_poll: --keys-live as the file grows ---- */
    {
        char path[MAX_PATH];
        GetTempPathA(MAX_PATH, path);
        _snprintf(path + strlen(path), 64, "keys_live_%lu.txt", GetCurrentProcessId());
        FILE* f = fopen(path, "wb");
        keys_live lv;
        memset(&lv, 0, sizeof lv);
        lv.path = path;
        g_ran = 0;
        CHECK(keys_live_poll(&lv, record_step) == 0, "an empty file");
        fputs("0:move:10:20\n5:click:1:2,0:on:map,0:nokey,0:ent", f);   /* the last step not ended yet */
        fflush(f);
        int n = keys_live_poll(&lv, record_step);
        CHECK(n == 2 && g_ran == 2 && g_last.click == 1 && g_last.x == 1, "two steps: %d (a wait and a bad key skipped)", n);
        fputs("er\r\n", f);
        fflush(f);
        n = keys_live_poll(&lv, record_step);
        CHECK(n == 1 && g_last.vk == VK_RETURN, "the step finished later: %d", n);
        CHECK(keys_live_poll(&lv, record_step) == 0, "nothing new");
        fclose(f);
        DeleteFileA(path);
        lv.pos = 0;
        CHECK(keys_live_poll(&lv, record_step) == 0, "no file: nothing");
    }

    /* ---- keys_start: on:NAME waits for that open; the next one listens early ---- */
    {
        static step_t steps[8];
        int n = keys_parse_all("on:map,0:a,on:sound,0:b", steps, 8);
        g_ran = 0;
        keys_file_opened("data\\map.dat");          /* before the script starts: nobody waits */
        keys_start(steps, n, NULL, record_step);
        keys_file_opened("data\\other.dat");
        SLEEP_MS(300);
        CHECK(g_ran == 0, "ran before the file it waits for: %d", g_ran);
        keys_file_opened("DATA\\MAP.DAT");           /* any case, part of the name */
        for (int t = 0; t < 500 && g_ran < 1; t++) SLEEP_MS(10);
        CHECK(g_ran == 1 && g_last.vk == 'A', "after map: %d", g_ran);
        keys_file_opened("sound\\x.wav");           /* listened for since 0:a */
        for (int t = 0; t < 500 && g_ran < 2; t++) SLEEP_MS(10);
        CHECK(g_ran == 2 && g_last.vk == 'B', "after sound: %d", g_ran);
    }

    /* ---- hostargs: shared test-run rule and integer flag validation ---- */
    {
        CHECK(!host_is_test_run(0, 0, NULL, 0), "not a test run");
        CHECK(!host_is_test_run(0, 0, "", 0), "empty live_file is not a test run");
        CHECK(host_is_test_run(1, 0, NULL, 0), "headless is a test run");
        CHECK(host_is_test_run(0, 1, NULL, 0), "--keys is a test run");
        CHECK(host_is_test_run(0, 0, "live.txt", 0), "--keys-live is a test run");
        CHECK(host_is_test_run(0, 0, NULL, 10), "--watchdog is a test run");

        CHECK(host_int_arg("--record-fps", "1", 1, 1000) == 1 &&
              host_int_arg("--record-fps", "30", 1, 1000) == 30 &&
              host_int_arg("--record-fps", "1000", 1, 1000) == 1000, "host_int_arg --record-fps valid");
        char max_buf[32];
        _snprintf(max_buf, sizeof max_buf, "%d", INT_MAX);
        CHECK(host_int_arg("--frames", "1", 1, INT_MAX) == 1 &&
              host_int_arg("--frames", max_buf, 1, INT_MAX) == INT_MAX, "host_int_arg --frames valid");

        static const struct { const char* flag; const char* val; long lo; long hi; } bad[] = {
            { "--record-fps", "0", 1, 1000 },
            { "--record-fps", "-1", 1, 1000 },
            { "--record-fps", "1001", 1, 1000 },
            { "--record-fps", "abc", 1, 1000 },
            { "--record-fps", "10x", 1, 1000 },
            { "--record-fps", "", 1, 1000 },
            { "--frames", "99999999999999999999", 1, INT_MAX },
        };
        char self[MAX_PATH], cmd[2 * MAX_PATH], out[512];
        GetModuleFileNameA(NULL, self, MAX_PATH);
        for (int i = 0; i < (int)(sizeof bad / sizeof bad[0]); i++) {
            _snprintf(cmd, sizeof cmd, SELF_CMD("--int %s \"%s\" %ld %ld"), self,
                      bad[i].flag, bad[i].val, bad[i].lo, bad[i].hi);
            FILE* p = _popen(cmd, "r");
            size_t n = fread(out, 1, sizeof out - 1, p);
            out[n] = 0;
            int rc = _pclose(p);
            CHECK(rc == 1 && strstr(out, bad[i].flag) && !strstr(out, "int "),
                  "bad %s \"%s\": rc %d out %s", bad[i].flag, bad[i].val, rc, out);
        }
    }

    printf(fails ? "%d FAILED\n" : "OK\n", fails);
    return fails != 0;
}
