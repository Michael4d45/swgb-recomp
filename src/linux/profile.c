/*
 * profile.c - --profile FILE on the win32hle host (Linux): where the game
 * thread's time goes, by sampling. The Windows host's src/runtime/profile.c
 * has the method; this is the same sampler with POSIX parts, writing the same
 * file, which tools/profile_report.py reads (it names ELF addresses with nm).
 *
 * A thread of its own wakes about every millisecond (at a random point of
 * each interval) and sends the game thread a signal. Its handler, on the game
 * thread, copies the interrupted EIP and the top of the stack into a buffer
 * and posts a semaphore; nothing else happens in the handler. The sampler
 * then scans the copy: values that point into the executable just after a
 * call instruction are the chain of lifted callers (the lift is one C
 * function per guest function, without frame pointers; a few stale return
 * addresses come along).
 *
 * A sample is counted as running when the game thread's CPU clock moved by a
 * quarter of the interval since the last one (a thread asleep in a wait is
 * not CPU time). EIP outside the executable: the import in progress
 * (g_cur_import). The file is rewritten every 10 s and at the end.
 *
 *   --profile FILE              sample the whole run
 *   --profile-window A:B        only seconds A to B after the start
 *   --profile-hz N              samples a second (default 1000)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <limits.h>

#include "win32hle.h"
#include "guest_addrs.h"
#include "profile.h"

extern char __executable_start[], etext[];

static char   g_path[PATH_MAX];
static double g_from, g_to = 1e30;
static int    g_hz = 1000;

static pthread_t g_thread;          /* the game thread */
static clockid_t g_thread_clock;
static uintptr_t g_stack_top;       /* its stack's top, near enough: main's frame */
static uintptr_t g_img_lo, g_img_hi;

#define MAXF   48
#define COPY   (96 * 1024)
#define TAB    (1 << 15)
typedef struct {
    uint32_t n;
    uint8_t  mode, run, depth, pad;
    const char* imp;
    uint32_t hash;
    uint32_t f[MAXF];
} stk_t;
static stk_t* g_tab;
static long   g_used, g_lost, g_samples, g_running;
static double g_wall;
static double g_cpu_thread0, g_cpu_proc0, g_cpu_thread1, g_cpu_proc1;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_done;

/* What the handler hands over. */
static uint8_t  g_buf[COPY];
static size_t   g_buf_n;
static uint32_t g_eip, g_mode;
static const char* g_imp;
static sem_t    g_got;

#define SIG_SAMPLE (SIGRTMIN + 5)

static uint32_t mode_of(void) {
    uint32_t g = MEM32(G_GAME);
    return g >= 0x10000 && g < 0xC0000000u ? MEM32(g + 0x1DC) : 0;
}

static void on_sample(int sig, siginfo_t* si, void* ctx) {
    (void)sig, (void)si;
    int saved = errno;
    ucontext_t* uc = (ucontext_t*)ctx;
#if defined(__i386__)
    uintptr_t eip = (uintptr_t)uc->uc_mcontext.gregs[REG_EIP], esp = (uintptr_t)uc->uc_mcontext.gregs[REG_ESP];
#elif defined(__x86_64__)
    uintptr_t eip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP], esp = (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
#else
    uintptr_t eip = 0, esp = 0;
    (void)uc;
#endif
    g_eip = (uint32_t)eip;
    g_buf_n = 0;
    if (esp && esp < g_stack_top) {
        size_t n = g_stack_top - esp;
        if (n > COPY) n = COPY;
        memcpy(g_buf, (const void*)esp, n & ~(size_t)3);
        g_buf_n = n & ~(size_t)3;
    }
    g_imp = eip < g_img_lo || eip >= g_img_hi ? g_cur_import : NULL;
    g_mode = mode_of();
    sem_post(&g_got);
    errno = saved;
}

static int after_call(uintptr_t p) {
    if (p < g_img_lo + 8 || p >= g_img_hi) return 0;
    const uint8_t* b = (const uint8_t*)p;
    return b[-5] == 0xE8 || (b[-2] == 0xFF && (b[-1] & 0x38) == 0x10) ||
           (b[-3] == 0xFF && (b[-2] & 0x38) == 0x10) || (b[-6] == 0xFF && (b[-5] & 0x38) == 0x10) ||
           (b[-7] == 0xFF && (b[-6] & 0x38) == 0x10);
}

static double clock_s(clockid_t c) {
    struct timespec t;
    return clock_gettime(c, &t) ? 0 : t.tv_sec + t.tv_nsec / 1e9;
}
static void cpu_now(double* thread, double* proc) {
    *thread = clock_s(g_thread_clock);
    *proc = clock_s(CLOCK_PROCESS_CPUTIME_ID);
}

static void record(const uint32_t* f, int depth, uint8_t mode, uint8_t run, const char* imp) {
    uint32_t h = 2166136261u ^ mode ^ (run << 8) ^ (uint32_t)(uintptr_t)imp;
    for (int i = 0; i < depth; i++) h = (h ^ f[i]) * 16777619u;
    g_samples++;
    g_running += run;
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
    g_lost++;
}

/* The modules: each mapped file's lowest address and its extent, from /proc/self/maps. */
static void write_modules(FILE* o) {
    FILE* m = fopen("/proc/self/maps", "r");
    char line[PATH_MAX + 128], last[PATH_MAX] = "";
    unsigned long lo = 0, hi = 0;
    if (!m) return;
    while (fgets(line, sizeof line, m)) {
        unsigned long a, b, off;
        char perm[8], path[PATH_MAX] = "";
        if (sscanf(line, "%lx-%lx %7s %lx %*s %*s %4095[^\n]", &a, &b, perm, &off, path) < 4 || path[0] != '/') continue;
        if (strcmp(path, last)) {
            if (last[0]) fprintf(o, "module %08lX %08lX %s\n", lo, hi - lo, last);
            snprintf(last, sizeof last, "%s", path);
            lo = a;
        }
        hi = b;
    }
    if (last[0]) fprintf(o, "module %08lX %08lX %s\n", lo, hi - lo, last);
    fclose(m);
}

static void write_file(void) {
    FILE* o = fopen(g_path, "w");
    if (!o) { fprintf(stderr, "[profile] cannot write %s\n", g_path); return; }
    fprintf(o, "# swgb-recomp profile 1\n");
    fprintf(o, "hz %d\nwall_s %.3f\nsamples %ld\nrunning %ld\nlost %ld\n", g_hz, g_wall, g_samples, g_running, g_lost);
    fprintf(o, "cpu_thread_s %.3f\ncpu_process_s %.3f\n", g_cpu_thread1 - g_cpu_thread0, g_cpu_proc1 - g_cpu_proc0);
    write_modules(o);
    for (int i = 0; i < TAB; i++) {
        stk_t* s = &g_tab[i];
        if (!s->n) continue;
        fprintf(o, "s %u %u %c %s", s->n, s->mode, s->run ? 'R' : 'W', s->imp ? s->imp : "-");
        for (int k = 0; k < s->depth; k++) fprintf(o, " %X", s->f[k]);
        fputc('\n', o);
    }
    fclose(o);
}

static void* sampler(void* unused) {
    (void)unused;
    uint32_t f[MAXF], rng = (uint32_t)time(NULL) | 1;
    double t0 = clock_s(CLOCK_MONOTONIC), last_t = t0, last_cpu = clock_s(g_thread_clock), next_write = t0 + 10;
    int in_window = 0;
    while (!g_done) {
        rng = rng * 1103515245u + 12345u;   /* a random point of each interval: 0.5 to 1.5 of it */
        long ns = (long)(1e9 / g_hz * (512 + (rng >> 22)) / 1024);
        struct timespec d = { ns / 1000000000L, ns % 1000000000L };
        nanosleep(&d, NULL);
        double t = clock_s(CLOCK_MONOTONIC), at = t - t0, cpu = clock_s(g_thread_clock);
        uint8_t run = (cpu - last_cpu) * 4 > (t - last_t);   /* ran a quarter of the interval */
        last_cpu = cpu;
        if (at < g_from || at > g_to) {
            if (in_window && at > g_to) { pthread_mutex_lock(&g_lock); cpu_now(&g_cpu_thread1, &g_cpu_proc1); pthread_mutex_unlock(&g_lock); in_window = 0; }
            last_t = t;
            continue;
        }
        pthread_mutex_lock(&g_lock);
        if (!in_window) { cpu_now(&g_cpu_thread0, &g_cpu_proc0); in_window = 1; last_t = t; }
        g_wall += t - last_t;
        last_t = t;
        if (pthread_kill(g_thread, SIG_SAMPLE) == 0) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_nsec += 50000000L;
            if (until.tv_nsec >= 1000000000L) until.tv_sec++, until.tv_nsec -= 1000000000L;
            while (sem_timedwait(&g_got, &until) && errno == EINTR) {}
            int depth = 0;
            if (g_eip) {
                f[depth++] = g_eip;
                for (size_t k = 0; k + 4 <= g_buf_n && depth < MAXF; k += 4) {
                    uint32_t v;
                    memcpy(&v, g_buf + k, 4);
                    if (after_call(v) && v != f[depth - 1]) f[depth++] = v;
                }
                record(f, depth, (uint8_t)g_mode, run, g_imp);
            }
            g_eip = 0;
        }
        cpu_now(&g_cpu_thread1, &g_cpu_proc1);
        if (t >= next_write) { next_write = t + 10; write_file(); }
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

int profile_arg(int argc, char** argv, int i) {
    if (!strcmp(argv[i], "--profile") && i + 1 < argc) {
        char cwd[PATH_MAX];
        if (argv[i + 1][0] == '/' || !getcwd(cwd, sizeof cwd)) snprintf(g_path, sizeof g_path, "%s", argv[i + 1]);
        else snprintf(g_path, sizeof g_path, "%s/%s", cwd, argv[i + 1]);   /* the game runs in its own folder */
        return 2;
    }
    if (!strcmp(argv[i], "--profile-window") && i + 1 < argc) {
        if (sscanf(argv[i + 1], "%lf:%lf", &g_from, &g_to) != 2 || g_to <= g_from) {
            fprintf(stderr, "--profile-window A:B (seconds, A < B)\n");
            exit(1);
        }
        return 2;
    }
    if (!strcmp(argv[i], "--profile-hz") && i + 1 < argc) {
        g_hz = atoi(argv[i + 1]);
        if (g_hz < 1 || g_hz > 1000) { fprintf(stderr, "--profile-hz: 1 to 1000\n"); exit(1); }
        return 2;
    }
    return 0;
}

void profile_start(void) {
    if (!g_path[0]) return;
    g_tab = (stk_t*)calloc(TAB, sizeof *g_tab);
    if (!g_tab) { fprintf(stderr, "[profile] out of memory\n"); g_path[0] = 0; return; }
    g_thread = pthread_self();
    pthread_getcpuclockid(g_thread, &g_thread_clock);
    g_stack_top = (uintptr_t)__builtin_frame_address(0);
    g_img_lo = (uintptr_t)__executable_start;
    g_img_hi = (uintptr_t)etext;
    sem_init(&g_got, 0, 0);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sample;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIG_SAMPLE, &sa, NULL);
    pthread_t t;
    pthread_create(&t, NULL, sampler, NULL);
    pthread_detach(t);
    printf("  profile: %d samples a second -> %s\n", g_hz, g_path);
}

void profile_finish(void) {
    if (!g_path[0] || !g_tab) return;
    pthread_mutex_lock(&g_lock);
    g_done = 1;
    write_file();
    pthread_mutex_unlock(&g_lock);
    fprintf(stderr, "[profile] %ld samples (%ld running, %ld lost) -> %s\n", g_samples, g_running, g_lost, g_path);
}
