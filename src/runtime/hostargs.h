/* Command-line helpers both hosts' sources use (host.c, src/linux/main.c,
 * keyscript.c, profile_core.c), so a value means and fails the same on each. */
#ifndef SWGB_HOSTARGS_H
#define SWGB_HOSTARGS_H
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define HOSTARGS_INLINE static __inline
#else
#include <unistd.h>
#define HOSTARGS_INLINE static inline
#endif

/* A path named on the command line, made absolute before the game runs in a
 * folder of its own (the hosts chdir into --game). It need not exist yet: a
 * shot or a profile is written there later. */
#ifdef _WIN32
HOSTARGS_INLINE void host_full_path(const char* in, char* out, size_t cap) {
    if (!GetFullPathNameA(in, (DWORD)cap, out, NULL)) snprintf(out, cap, "%s", in);
}
#else
HOSTARGS_INLINE void host_full_path(const char* in, char* out, size_t cap) {
    char cwd[4096];
    if (in[0] == '/' || !getcwd(cwd, sizeof cwd)) snprintf(out, cap, "%s", in);
    else snprintf(out, cap, "%s/%s", cwd, in);
}
#endif

/* FLAG's value s, a whole number lo to hi, or the run ends (exit 1) saying
 * so: no flag silently becomes a default or 0. */
HOSTARGS_INLINE long host_int_arg(const char* flag, const char* s, long lo, long hi) {
    char* end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno == ERANGE || end == s || *end || v < lo || v > hi) {
        fprintf(stderr, "%s: a whole number from %ld to %ld, not \"%s\"\n", flag, lo, hi, s);
        exit(1);
    }
    return v;
}

/* A test run: muted unless --sound, and message boxes answer themselves
 * (nobody is there to click one). One rule for both hosts. */
HOSTARGS_INLINE int host_is_test_run(int headless, int nsteps, const char* live_file, unsigned long watchdog_s) {
    return headless || nsteps > 0 || (live_file && live_file[0]) || watchdog_s > 0;
}

#endif
