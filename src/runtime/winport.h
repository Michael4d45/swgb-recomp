/*
 * winport.h - POSIX stand-ins for the few Win32 calls a host-side runtime
 * file makes itself (dplay.c: sockets, threads, events, critical sections,
 * UTF-16 names), so the Linux and macOS host (src/linux, SWGB_WIN32HLE) builds
 * the same file. Only what those files use, with Windows' meaning: DWORD is
 * 32 bits, WCHAR is UTF-16, an event is manual- or auto-reset.
 *
 * These are the host's own objects. The guest's (an event the game made with
 * CreateEventA) are win32hle's handles: set them with hle_set_event.
 */
#ifndef SWGB_WINPORT_H
#define SWGB_WINPORT_H
#ifdef _WIN32
#error "winport.h is for the win32hle host (Linux, macOS)"
#endif

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>

/* ---- types: Windows' sizes (an i386 long is 32 bits, so %lu stays right) ---- */
#if UINTPTR_MAX == 0xFFFFFFFFu
typedef unsigned long DWORD;
typedef long LONG;
typedef unsigned long ULONG;
#else
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef uint32_t ULONG;
#endif
typedef long long LONG64;
typedef unsigned int UINT;
typedef int BOOL;
typedef uint16_t WCHAR;
typedef int32_t HRESULT;
typedef void* HANDLE;
typedef void* LPVOID;
typedef unsigned short u_short;
#define TRUE 1
#define FALSE 0
#define WINAPI
#define __stdcall
#define __declspec(x) __declspec_##x
#define __declspec_thread __thread

typedef struct { DWORD Data1; uint16_t Data2, Data3; uint8_t Data4[8]; } GUID;
static inline int IsEqualGUID(const GUID* a, const GUID* b) { return !memcmp(a, b, sizeof *a); }
static inline HRESULT CoCreateGuid(GUID* g) {
    FILE* f = fopen("/dev/urandom", "rb");
    size_t n = f ? fread(g, 1, sizeof *g, f) : 0;
    if (f) fclose(f);
    if (n != sizeof *g) {                                  /* no urandom: time and address */
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        g->Data1 = (uint32_t)t.tv_nsec ^ (uint32_t)getpid() << 16;
        g->Data2 = (uint16_t)t.tv_sec, g->Data3 = (uint16_t)(t.tv_sec >> 16);
        memcpy(g->Data4, &t, sizeof g->Data4);
    }
    return 0;
}

/* ---- strings ---- */
#define _stricmp strcasecmp
#define _snprintf snprintf
static inline char* lstrcpynA(char* d, const char* s, int n) { if (n > 0) snprintf(d, (size_t)n, "%s", s); return d; }
static inline int wcslen16(const WCHAR* s) { int n = 0; while (s[n]) n++; return n; }
static inline WCHAR* lstrcpynW(WCHAR* d, const WCHAR* s, int n) {
    int i = 0;
    for (; i < n - 1 && s[i]; i++) d[i] = s[i];
    if (n > 0) d[i] = 0;
    return d;
}
#define CP_ACP 0
#define CP_UTF8 65001
/* The ANSI code page as Latin-1: each byte is the code point. */
static inline int MultiByteToWideChar(UINT cp, DWORD f, const char* s, int n, WCHAR* out, int cap) {
    (void)cp, (void)f;
    int len = n < 0 ? (int)strlen(s) + 1 : n;
    if (!cap) return len;
    int k = 0;
    for (; k < len && k < cap; k++) out[k] = (uint8_t)s[k];
    return k;
}
static inline int WideCharToMultiByte(UINT cp, DWORD f, const WCHAR* s, int n, char* out, int cap,
                                      const char* dflt, BOOL* used) {
    (void)f, (void)dflt, (void)used;
    int len = n < 0 ? wcslen16(s) + 1 : n, k = 0;
    for (int i = 0; i < len; i++) {
        unsigned c = s[i];
        char b[3];
        int m;
        if (cp != CP_UTF8) b[0] = (char)(c < 0x100 ? c : '?'), m = 1;
        else if (c < 0x80) b[0] = (char)c, m = 1;
        else if (c < 0x800) b[0] = (char)(0xC0 | c >> 6), b[1] = (char)(0x80 | (c & 63)), m = 2;
        else b[0] = (char)(0xE0 | c >> 12), b[1] = (char)(0x80 | (c >> 6 & 63)), b[2] = (char)(0x80 | (c & 63)), m = 3;
        if (cap) { if (k + m > cap) break; memcpy(out + k, b, (size_t)m); }
        k += m;
    }
    return k;
}

/* ---- time ---- */
static inline DWORD GetTickCount(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (DWORD)((uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u);
}
static inline void Sleep(DWORD ms) { usleep((useconds_t)ms * 1000u); }

/* ---- atomics ---- */
#define InterlockedIncrement(p) __sync_add_and_fetch((p), 1)
#define InterlockedDecrement(p) __sync_sub_and_fetch((p), 1)
#define InterlockedExchange(p, v) __sync_lock_test_and_set((p), (v))

/* ---- critical sections: recursive, as Windows' are ---- */
typedef pthread_mutex_t CRITICAL_SECTION;
static inline void InitializeCriticalSection(CRITICAL_SECTION* c) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(c, &a);
    pthread_mutexattr_destroy(&a);
}
#define EnterCriticalSection pthread_mutex_lock
#define LeaveCriticalSection pthread_mutex_unlock

/* ---- events and threads, waited on alike ---- */
typedef struct {
    int thread;                /* 1: a thread (wait = join) */
    pthread_t t;
    pthread_mutex_t m;
    pthread_cond_t c;
    int manual, set;
} winport_obj;
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u
#define INFINITE 0xFFFFFFFFu
static inline HANDLE CreateEventA(void* sa, BOOL manual, BOOL initial, const char* name) {
    (void)sa, (void)name;
    winport_obj* o = (winport_obj*)calloc(1, sizeof *o);
    pthread_mutex_init(&o->m, NULL);
    pthread_cond_init(&o->c, NULL);
    o->manual = manual, o->set = initial;
    return o;
}
static inline BOOL SetEvent(HANDLE h) {
    winport_obj* o = (winport_obj*)h;
    pthread_mutex_lock(&o->m);
    o->set = 1;
    pthread_cond_broadcast(&o->c);
    pthread_mutex_unlock(&o->m);
    return TRUE;
}
static inline BOOL ResetEvent(HANDLE h) {
    winport_obj* o = (winport_obj*)h;
    pthread_mutex_lock(&o->m);
    o->set = 0;
    pthread_mutex_unlock(&o->m);
    return TRUE;
}
static inline DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    winport_obj* o = (winport_obj*)h;
    if (o->thread) { pthread_join(o->t, NULL); o->thread = 2; return WAIT_OBJECT_0; }
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += ms / 1000u;
    until.tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (until.tv_nsec >= 1000000000L) until.tv_sec++, until.tv_nsec -= 1000000000L;
    pthread_mutex_lock(&o->m);
    int rc = 0;
    while (!o->set && rc != ETIMEDOUT)
        rc = ms == INFINITE ? pthread_cond_wait(&o->c, &o->m) : pthread_cond_timedwait(&o->c, &o->m, &until);
    int got = o->set;
    if (got && !o->manual) o->set = 0;
    pthread_mutex_unlock(&o->m);
    return got ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}
typedef DWORD (*LPTHREAD_START_ROUTINE)(void*);
typedef struct { LPTHREAD_START_ROUTINE fn; void* arg; } winport_start;
static inline void* winport_thread(void* p) {
    winport_start s = *(winport_start*)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}
static inline HANDLE CreateThread(void* sa, size_t stack, LPTHREAD_START_ROUTINE fn, void* arg, DWORD flags, DWORD* id) {
    (void)sa, (void)stack, (void)flags, (void)id;
    winport_obj* o = (winport_obj*)calloc(1, sizeof *o);
    winport_start* s = (winport_start*)malloc(sizeof *s);
    s->fn = fn, s->arg = arg;
    o->thread = 1;
    if (pthread_create(&o->t, NULL, winport_thread, s)) { free(s); free(o); return NULL; }
    return o;
}
static inline BOOL CloseHandle(HANDLE h) {
    winport_obj* o = (winport_obj*)h;
    if (!o) return FALSE;
    if (o->thread == 1) pthread_detach(o->t);
    if (!o->thread) { pthread_cond_destroy(&o->c); pthread_mutex_destroy(&o->m); }
    free(o);
    return TRUE;
}

/* ---- sockets ---- */
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define closesocket close
#define WSAGetLastError() errno

#endif
