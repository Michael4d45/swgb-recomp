/*
 * profile_core: the half of --profile that is the same on both hosts (the
 * method is in src/runtime/profile.c's header). The sampler of each host
 * stops the game thread, takes its EIP, the import in progress and a copy of
 * the top of its stack, and hands them here: the copy is scanned for return
 * addresses into the executable, and identical samples are counted together
 * in a fixed table (nothing is allocated while sampling: on Windows the game
 * thread is suspended and may hold the heap's lock). tests/unit/profile_test.c
 * checks this file alone.
 *
 * The file, "# swgb-recomp profile 1": hz, wall_s, samples, running, lost,
 * cpu_thread_s, cpu_process_s, then "module LO SIZE PATH" lines, then one
 * "s COUNT MODE R|W IMPORT|- EIP FRAME..." line per distinct sample, in hex.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest.h"
#include "guest_addrs.h"
#include "hostargs.h"
#include "profile.h"
#include "profile_core.h"

char   profile_path[4096];
double profile_from, profile_to = 1e30;
int    profile_hz = 1000;
uintptr_t profile_img_lo, profile_img_hi;
long   profile_samples, profile_running, profile_lost;
double profile_wall;

#define TAB (1 << 15)                /* distinct samples */
typedef struct {
    uint32_t n;                      /* 0: free */
    uint8_t  mode, run, depth, pad;
    const char* imp;
    uint32_t hash;
    uint32_t f[PROFILE_MAXF];
} stk_t;
static stk_t* g_tab;
static long   g_used;

int profile_table_init(void) {
    g_tab = (stk_t*)calloc(TAB, sizeof *g_tab);
    return g_tab != NULL;
}

int profile_after_call(uintptr_t p) {
    if (p < profile_img_lo + 8 || p >= profile_img_hi) return 0;
    const uint8_t* b = (const uint8_t*)p;
    return b[-5] == 0xE8 || (b[-2] == 0xFF && (b[-1] & 0x38) == 0x10) ||
           (b[-3] == 0xFF && (b[-2] & 0x38) == 0x10) || (b[-6] == 0xFF && (b[-5] & 0x38) == 0x10) ||
           (b[-7] == 0xFF && (b[-6] & 0x38) == 0x10);
}

int profile_scan(uint32_t* f, int depth, const uint8_t* stack, size_t n) {
    for (size_t k = 0; k + 4 <= n && depth < PROFILE_MAXF; k += 4) {
        uint32_t v;
        memcpy(&v, stack + k, 4);
        if (profile_after_call(v) && (!depth || v != f[depth - 1])) f[depth++] = v;
    }
    return depth;
}

void profile_record(const uint32_t* f, int depth, uint8_t mode, uint8_t run, const char* imp) {
    uint32_t h = 2166136261u ^ mode ^ (run << 8) ^ (uint32_t)(uintptr_t)imp;
    for (int i = 0; i < depth; i++) h = (h ^ f[i]) * 16777619u;
    profile_samples++;
    profile_running += run;
    for (uint32_t i = h & (TAB - 1), k = 0; k < 64; k++, i = (i + 1) & (TAB - 1)) {
        stk_t* s = &g_tab[i];
        if (!s->n) {
            if (g_used >= TAB * 3 / 4) break;
            s->hash = h; s->mode = mode; s->run = run; s->imp = imp; s->depth = (uint8_t)depth;
            memcpy(s->f, f, depth * sizeof *f);
            s->n = 1;
            g_used++;
            return;
        }
        if (s->hash == h && s->mode == mode && s->run == run && s->imp == imp && s->depth == depth &&
            !memcmp(s->f, f, depth * sizeof *f)) { s->n++; return; }
    }
    profile_lost++;
}

uint32_t profile_mode(void) {
    uint32_t g = guest_read32(G_GAME);
    return g ? guest_read32(g + G_GAME_MODE) : 0;
}

void profile_write(double cpu_thread_s, double cpu_process_s, void (*modules)(FILE*)) {
    FILE* o = fopen(profile_path, "w");
    if (!o) { fprintf(stderr, "[profile] cannot write %s\n", profile_path); return; }
    fprintf(o, "# swgb-recomp profile 1\n");
    fprintf(o, "hz %d\nwall_s %.3f\nsamples %ld\nrunning %ld\nlost %ld\n", profile_hz, profile_wall,
            profile_samples, profile_running, profile_lost);
    fprintf(o, "cpu_thread_s %.3f\ncpu_process_s %.3f\n", cpu_thread_s, cpu_process_s);
    if (modules) modules(o);
    for (int i = 0; g_tab && i < TAB; i++) {
        stk_t* s = &g_tab[i];
        if (!s->n) continue;
        fprintf(o, "s %u %u %c %s", s->n, s->mode, s->run ? 'R' : 'W', s->imp ? s->imp : "-");
        for (int k = 0; k < s->depth; k++) fprintf(o, " %X", s->f[k]);
        fputc('\n', o);
    }
    fclose(o);
}

int profile_arg(int argc, char** argv, int i) {
    if (!strcmp(argv[i], "--profile") && i + 1 < argc) {
        host_full_path(argv[i + 1], profile_path, sizeof profile_path);
        return 2;
    }
    if (!strcmp(argv[i], "--profile-window") && i + 1 < argc) {
        if (sscanf(argv[i + 1], "%lf:%lf", &profile_from, &profile_to) != 2 || profile_to <= profile_from) {
            fprintf(stderr, "--profile-window A:B (seconds, A < B)\n");
            exit(1);
        }
        return 2;
    }
    if (!strcmp(argv[i], "--profile-hz") && i + 1 < argc) {
        profile_hz = (int)host_int_arg("--profile-hz", argv[i + 1], 1, 1000);
        return 2;
    }
    return 0;
}
