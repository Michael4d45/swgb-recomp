/* src/runtime/profile_core.c without a game thread to sample: what both
 * hosts' --profile samplers (src/runtime/profile.c, src/linux/profile.c)
 * hand their samples to.
 *
 *   profile_after_call  a return address: just after E8 rel32 or an FF /2 call
 *                       (register, [reg], [reg+d8], [d32], [sib+d32]), inside the image
 *   profile_scan        the frames in a stack copy: return addresses only, a
 *                       repeat dropped, at most PROFILE_MAXF
 *   profile_record      identical samples counted once; the mode, running and the
 *                       import keep them apart; a full table counts the rest as lost
 *   profile_write       the file tools/profile_report.py reads
 *   profile_arg         --profile (made absolute), --profile-window, --profile-hz
 *
 *   py -3 tools\test.py --level unit        (builds and runs every unit)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define ABS(p) ((p)[1] == ':')
#else
#include <unistd.h>
#define ABS(p) ((p)[0] == '/')
#endif

#include "profile.h"
#include "profile_core.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* A pretend executable: code with one call of each kind, each followed by the
 * address it returns to. */
static uint8_t g_img[256];
enum { AT_E8 = 16, AT_REG = 32, AT_MEM = 48, AT_D8 = 64, AT_D32 = 80, AT_SIB = 96, AT_NONE = 112 };

static void put(int at, const uint8_t* b, int n) { memcpy(g_img + at - n, b, (size_t)n); }
static uint32_t va(int at) { return (uint32_t)(uintptr_t)(g_img + at); }

static void test_after_call(void) {
    static const uint8_t e8[] = { 0xE8, 1, 2, 3, 4 }, reg[] = { 0xFF, 0xD0 }, mem[] = { 0xFF, 0x10 },
                         d8[] = { 0xFF, 0x50, 0x08 }, d32[] = { 0xFF, 0x15, 1, 2, 3, 4 },
                         sib[] = { 0xFF, 0x94, 0x24, 1, 2, 3, 4 }, none[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    memset(g_img, 0x90, sizeof g_img);
    put(AT_E8, e8, 5); put(AT_REG, reg, 2); put(AT_MEM, mem, 2); put(AT_D8, d8, 3);
    put(AT_D32, d32, 6); put(AT_SIB, sib, 7); put(AT_NONE, none, 7);
    profile_img_lo = (uintptr_t)g_img, profile_img_hi = (uintptr_t)(g_img + sizeof g_img);
    CHECK(profile_after_call(va(AT_E8)), "after E8 rel32");
    CHECK(profile_after_call(va(AT_REG)), "after call eax");
    CHECK(profile_after_call(va(AT_MEM)), "after call [eax]");
    CHECK(profile_after_call(va(AT_D8)), "after call [eax+8]");
    CHECK(profile_after_call(va(AT_D32)), "after call [d32]");
    CHECK(profile_after_call(va(AT_SIB)), "after call [esp+d32]");
    CHECK(!profile_after_call(va(AT_NONE)), "after no call");
    CHECK(!profile_after_call(va(4)), "within 8 bytes of the image's start");
    CHECK(!profile_after_call((uintptr_t)(g_img + sizeof g_img)), "past the image");
    CHECK(!profile_after_call(0x10), "nowhere near it");
}

static void test_scan(void) {
    uint32_t stack[64 + PROFILE_MAXF], f[PROFILE_MAXF];
    int n = 0;
    stack[n++] = va(AT_NONE);   /* not after a call */
    stack[n++] = va(AT_E8);
    stack[n++] = va(AT_E8);     /* the same again: kept once */
    stack[n++] = 12345;
    stack[n++] = va(AT_D32);
    f[0] = 0xEEEE;              /* the EIP */
    int depth = profile_scan(f, 1, (const uint8_t*)stack, (size_t)n * 4);
    CHECK(depth == 3 && f[0] == 0xEEEE && f[1] == va(AT_E8) && f[2] == va(AT_D32), "the frames: depth %d", depth);
    CHECK(profile_scan(f, 1, (const uint8_t*)stack, 7) == 1, "a partial dword is not read");
    for (int i = 0; i < 64 + PROFILE_MAXF; i++) stack[i] = va(i & 1 ? AT_E8 : AT_REG);
    CHECK(profile_scan(f, 1, (const uint8_t*)stack, sizeof stack) == PROFILE_MAXF, "at most PROFILE_MAXF frames");
}

static int g_modules;
static void modules(FILE* o) { g_modules++; fprintf(o, "module 00400000 00010000 test.exe\n"); }

static void test_record_and_write(const char* path) {
    uint32_t a[3] = { 0x1000, 0x2000, 0x3000 }, b[3] = { 0x1000, 0x2000, 0x3001 };
    CHECK(profile_table_init(), "no table");
    profile_record(a, 3, 4, 1, NULL);
    profile_record(a, 3, 4, 1, NULL);
    profile_record(a, 3, 4, 0, NULL);            /* waiting, not running */
    profile_record(a, 3, 2, 1, NULL);            /* the menus */
    profile_record(a, 3, 4, 1, "Sleep");         /* in an import */
    profile_record(b, 3, 4, 1, NULL);
    profile_record(a, 2, 4, 1, NULL);            /* a shorter chain */
    CHECK(profile_samples == 7 && profile_running == 6 && !profile_lost, "%ld samples, %ld running, %ld lost",
          profile_samples, profile_running, profile_lost);

    snprintf(profile_path, sizeof profile_path, "%s", path);
    profile_hz = 500;
    profile_wall = 1.5;
    profile_write(1.25, 2.5, modules);
    FILE* f = fopen(path, "r");
    char line[256];
    int head = 0, lines = 0, twice = 0, imp = 0, wait = 0, mods = 0;
    while (f && fgets(line, sizeof line, f)) {
        if (!strcmp(line, "# swgb-recomp profile 1\n")) head |= 1;
        if (!strcmp(line, "hz 500\n")) head |= 2;
        if (!strcmp(line, "wall_s 1.500\n")) head |= 4;
        if (!strcmp(line, "samples 7\n")) head |= 8;
        if (!strcmp(line, "cpu_thread_s 1.250\n")) head |= 16;
        if (!strcmp(line, "cpu_process_s 2.500\n")) head |= 32;
        if (!strncmp(line, "module ", 7)) mods++;
        if (strncmp(line, "s ", 2)) continue;
        lines++;
        if (!strcmp(line, "s 2 4 R - 1000 2000 3000\n")) twice++;
        if (!strcmp(line, "s 1 4 R Sleep 1000 2000 3000\n")) imp++;
        if (!strcmp(line, "s 1 4 W - 1000 2000 3000\n")) wait++;
    }
    if (f) fclose(f);
    remove(path);
    CHECK(head == 63, "the header (%d)", head);
    CHECK(mods == 1 && g_modules == 1, "the host's modules");
    CHECK(lines == 6 && twice == 1 && imp == 1 && wait == 1, "%d sample lines (twice %d, import %d, waiting %d)",
          lines, twice, imp, wait);

    long before = profile_lost;
    for (uint32_t i = 0; i < 30000; i++) { uint32_t g[1] = { 0x100000 + i }; profile_record(g, 1, 4, 1, NULL); }
    CHECK(profile_lost > before && profile_lost - before < 30000 - 20000, "a full table: %ld of 30000 lost",
          profile_lost - before);
}

static void test_arg(void) {
    char* a1[] = { "x", "--profile", "out.prof" };
    CHECK(profile_arg(3, a1, 1) == 2 && ABS(profile_path) && strstr(profile_path, "out.prof"), "--profile: %s", profile_path);
    char* a2[] = { "x", "--profile-window", "10:20.5" };
    CHECK(profile_arg(3, a2, 1) == 2 && profile_from == 10 && profile_to == 20.5, "--profile-window");
    char* a3[] = { "x", "--profile-hz", "250" };
    CHECK(profile_arg(3, a3, 1) == 2 && profile_hz == 250, "--profile-hz");
    char* a4[] = { "x", "--profile" };
    CHECK(profile_arg(2, a4, 1) == 0, "no value: not ours (the host says so)");
    char* a5[] = { "x", "--fps", "60" };
    CHECK(profile_arg(3, a5, 1) == 0, "someone else's");
}

int main(void) {
    char path[1024];
#ifdef _WIN32
    GetTempPathA(sizeof path, path);
    snprintf(path + strlen(path), 64, "profile_test_%lu.prof", GetCurrentProcessId());
#else
    snprintf(path, sizeof path, "%s/profile_test_%ld.prof", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (long)getpid());
#endif
    test_after_call();
    test_scan();
    test_record_and_write(path);
    test_arg();
    printf(fails ? "%d FAILED\n" : "OK\n", fails);
    return fails != 0;
}
