/*
 * profile.c - --profile FILE on the win32hle host (Linux): where the game
 * thread's time goes, by sampling. The Windows host's src/runtime/profile.c
 * has the method; the scan, the table, the file (which tools/profile_report.py
 * reads; it names ELF addresses with nm) and the options are
 * src/runtime/profile_core.c, shared with it. This file is how a POSIX host
 * stops the game thread and reads it.
 *
 * A thread of its own wakes about every millisecond (at a random point of
 * each interval) and sends the game thread a signal. Its handler, on the game
 * thread, copies the interrupted EIP and the top of the stack into a buffer
 * and posts a semaphore; nothing else happens in the handler. The sampler
 * then scans the copy.
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
#include "profile.h"
#include "profile_core.h"

extern char __executable_start[], etext[];

static pthread_t g_thread;          /* the game thread */
static clockid_t g_thread_clock;
static uintptr_t g_stack_top;       /* its stack's top, near enough: main's frame */

static double g_cpu_thread0, g_cpu_proc0, g_cpu_thread1, g_cpu_proc1;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_done, g_started;

/* What the handler hands over. */
static uint8_t  g_buf[PROFILE_COPY];
static size_t   g_buf_n;
static uint32_t g_eip, g_mode;
static const char* g_imp;
static sem_t    g_got;

#define SIG_SAMPLE (SIGRTMIN + 5)

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
        if (n > PROFILE_COPY) n = PROFILE_COPY;
        memcpy(g_buf, (const void*)esp, n & ~(size_t)3);
        g_buf_n = n & ~(size_t)3;
    }
    g_imp = eip < profile_img_lo || eip >= profile_img_hi ? g_cur_import : NULL;
    g_mode = profile_mode();
    sem_post(&g_got);
    errno = saved;
}

static double clock_s(clockid_t c) {
    struct timespec t;
    return clock_gettime(c, &t) ? 0 : t.tv_sec + t.tv_nsec / 1e9;
}
static void cpu_now(double* thread, double* proc) {
    *thread = clock_s(g_thread_clock);
    *proc = clock_s(CLOCK_PROCESS_CPUTIME_ID);
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
    profile_write(g_cpu_thread1 - g_cpu_thread0, g_cpu_proc1 - g_cpu_proc0, write_modules);
}

static void* sampler(void* unused) {
    (void)unused;
    uint32_t f[PROFILE_MAXF], rng = (uint32_t)time(NULL) | 1;
    double t0 = clock_s(CLOCK_MONOTONIC), last_t = t0, last_cpu = clock_s(g_thread_clock), next_write = t0 + 10;
    int in_window = 0;
    while (!g_done) {
        rng = rng * 1103515245u + 12345u;   /* a random point of each interval: 0.5 to 1.5 of it */
        long ns = (long)(1e9 / profile_hz * (512 + (rng >> 22)) / 1024);
        struct timespec d = { ns / 1000000000L, ns % 1000000000L };
        nanosleep(&d, NULL);
        double t = clock_s(CLOCK_MONOTONIC), at = t - t0, cpu = clock_s(g_thread_clock);
        uint8_t run = (cpu - last_cpu) * 4 > (t - last_t);   /* ran a quarter of the interval */
        last_cpu = cpu;
        if (at < profile_from || at > profile_to) {
            if (in_window && at > profile_to) { pthread_mutex_lock(&g_lock); cpu_now(&g_cpu_thread1, &g_cpu_proc1); pthread_mutex_unlock(&g_lock); in_window = 0; }
            last_t = t;
            continue;
        }
        pthread_mutex_lock(&g_lock);
        if (!in_window) { cpu_now(&g_cpu_thread0, &g_cpu_proc0); in_window = 1; last_t = t; }
        profile_wall += t - last_t;
        last_t = t;
        if (pthread_kill(g_thread, SIG_SAMPLE) == 0) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_nsec += 50000000L;
            if (until.tv_nsec >= 1000000000L) until.tv_sec++, until.tv_nsec -= 1000000000L;
            while (sem_timedwait(&g_got, &until) && errno == EINTR) {}
            if (g_eip) {
                f[0] = g_eip;
                profile_record(f, profile_scan(f, 1, g_buf, g_buf_n), (uint8_t)g_mode, run, g_imp);
            }
            g_eip = 0;
        }
        cpu_now(&g_cpu_thread1, &g_cpu_proc1);
        if (t >= next_write) { next_write = t + 10; write_file(); }
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

void profile_start(void) {
    if (!profile_path[0]) return;
    if (!profile_table_init()) { fprintf(stderr, "[profile] out of memory\n"); profile_path[0] = 0; return; }
    g_thread = pthread_self();
    pthread_getcpuclockid(g_thread, &g_thread_clock);
    g_stack_top = (uintptr_t)__builtin_frame_address(0);
    profile_img_lo = (uintptr_t)__executable_start;
    profile_img_hi = (uintptr_t)etext;
    sem_init(&g_got, 0, 0);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sample;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIG_SAMPLE, &sa, NULL);
    g_started = 1;
    pthread_t t;
    pthread_create(&t, NULL, sampler, NULL);
    pthread_detach(t);
    printf("  profile: %d samples a second -> %s\n", profile_hz, profile_path);
}

void profile_finish(void) {
    if (!g_started) return;
    pthread_mutex_lock(&g_lock);
    g_done = 1;
    write_file();
    pthread_mutex_unlock(&g_lock);
    fprintf(stderr, "[profile] %ld samples (%ld running, %ld lost) -> %s\n", profile_samples, profile_running,
            profile_lost, profile_path);
}
