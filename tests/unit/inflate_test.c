/* src/runtime/inflate.c without the game: the host's body for the game's
 * deflate decoder (sub_0062343B), called the way the lifted code calls it
 * (cdecl, eight arguments on the guest stack, `ret` popped by the callee).
 *
 * A raw deflate stream (as genie.dat, scenarios and saved games are) comes
 * back byte for byte in chunks of any size, input and output; 0 means call
 * again, -1 the end of the stream, -2 corrupt data; two streams decoded in
 * turn keep apart by their state block; a state block the guest zeroes again
 * starts a new stream.
 *
 *   py -3 tools\test.py --level unit        (builds and runs every unit)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native32.h"
#include "miniz.h"

void swgb_inflate(void);

uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp, g_cur_func;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define STATE_SIZE 0xAD90
#define STATE_RESUME 0xA958

static uint32_t g_stack[64];

/* One call, as the lifted code makes it: a return address, then the arguments. */
static int call_inflate(const uint8_t* in, uint32_t* in_len, uint8_t* out, uint32_t* out_len, uint8_t* state) {
    uint32_t* sp = g_stack + 16;
    sp[0] = 0xDEADBEEFu;                              /* the return address */
    sp[1] = (uint32_t)(uintptr_t)in; sp[2] = 0;       /* in, in_off */
    sp[3] = (uint32_t)(uintptr_t)in_len;
    sp[4] = (uint32_t)(uintptr_t)out; sp[5] = 0;      /* out, out_off */
    sp[6] = (uint32_t)(uintptr_t)out_len;
    sp[7] = (uint32_t)(uintptr_t)state;
    sp[8] = 0;                                        /* final */
    g_esp = (uint32_t)(uintptr_t)sp;
    swgb_inflate();
    CHECK(g_esp == (uint32_t)(uintptr_t)sp + 4);      /* popped the return address only (cdecl) */
    return (int)g_eax;
}

static uint8_t* deflate_raw(const uint8_t* src, size_t n, size_t* out_n) {
    mz_stream z;
    memset(&z, 0, sizeof z);
    mz_deflateInit2(&z, 6, MZ_DEFLATED, -MZ_DEFAULT_WINDOW_BITS, 9, MZ_DEFAULT_STRATEGY);
    size_t cap = mz_deflateBound(&z, (mz_ulong)n) + 64;
    uint8_t* out = (uint8_t*)malloc(cap);
    z.next_in = src; z.avail_in = (unsigned)n;
    z.next_out = out; z.avail_out = (unsigned)cap;
    mz_deflate(&z, MZ_FINISH);
    *out_n = cap - z.avail_out;
    mz_deflateEnd(&z);
    return out;
}

typedef struct { const uint8_t* in; size_t in_n, in_pos; uint8_t* out; size_t out_pos; uint8_t* state; int done; } job_t;

/* One call of a job with at most cin bytes in and cout bytes of room out. */
static int step(job_t* j, uint32_t cin, uint32_t cout, size_t out_cap) {
    uint32_t in_len = (uint32_t)(j->in_n - j->in_pos < cin ? j->in_n - j->in_pos : cin);
    uint32_t out_len = (uint32_t)(out_cap - j->out_pos < cout ? out_cap - j->out_pos : cout);
    int r = call_inflate(j->in + j->in_pos, &in_len, j->out + j->out_pos, &out_len, j->state);
    j->in_pos += in_len;
    j->out_pos += out_len;
    if (r == -1) j->done = 1;
    return r;
}

static void make_data(uint8_t* d, size_t n, uint32_t seed) {
    static const char text[] = "Star Wars Galactic Battlegrounds genie.dat: units, techs, civilizations. ";
    for (size_t i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        d[i] = (i / 4096) % 2 ? (uint8_t)(seed >> 16) : (uint8_t)text[i % (sizeof text - 1)];
    }
}

int main(void) {
    const size_t N = 300000;
    uint8_t* src = (uint8_t*)malloc(N), *src2 = (uint8_t*)malloc(N);
    make_data(src, N, 1);
    make_data(src2, N, 2);
    size_t zn, zn2;
    uint8_t* z = deflate_raw(src, N, &zn), *z2 = deflate_raw(src2, N, &zn2);
    uint8_t* out = (uint8_t*)malloc(N + 64), *out2 = (uint8_t*)malloc(N + 64);
    uint8_t* st = (uint8_t*)calloc(1, STATE_SIZE), *st2 = (uint8_t*)calloc(1, STATE_SIZE);

    /* chunk sizes: one call with everything, then small odd chunks either way */
    static const uint32_t chunks[][2] = { { 0x7FFFFFFF, 0x7FFFFFFF }, { 7, 0x7FFFFFFF }, { 0x7FFFFFFF, 13 }, { 1, 1 }, { 4096, 4096 } };
    for (int c = 0; c < 5; c++) {
        memset(st, 0, STATE_SIZE);
        job_t j = { z, zn, 0, out, 0, st, 0 };
        int r = 0, calls = 0, bad = 0;
        while (!j.done && calls < 10000000) {
            r = step(&j, chunks[c][0], chunks[c][1], N + 64);
            calls++;
            if (r != 0 && r != -1) { bad = 1; break; }
        }
        CHECK(!bad);
        CHECK(j.done);
        CHECK(j.out_pos == N);
        CHECK(!memcmp(out, src, N));
        CHECK(*(uint32_t*)(st + STATE_RESUME) != 0);    /* not fresh any more */
        if (fails) { printf("  chunks %u/%u: %d calls, %u of %u bytes\n", chunks[c][0], chunks[c][1], calls, (unsigned)j.out_pos, (unsigned)N); break; }
    }

    /* two streams in turn, each with its own state block */
    {
        memset(st, 0, STATE_SIZE);
        memset(st2, 0, STATE_SIZE);
        job_t a = { z, zn, 0, out, 0, st, 0 }, b = { z2, zn2, 0, out2, 0, st2, 0 };
        int calls = 0, bad = 0;
        while ((!a.done || !b.done) && calls < 1000000) {
            if (!a.done && step(&a, 333, 1000, N + 64) == -2) bad = 1;
            if (!b.done && step(&b, 500, 777, N + 64) == -2) bad = 1;
            calls++;
        }
        CHECK(!bad);
        CHECK(a.done && b.done);
        CHECK(a.out_pos == N && !memcmp(out, src, N));
        CHECK(b.out_pos == N && !memcmp(out2, src2, N));
    }

    /* the guest zeroes the block again: a new stream, from its first byte */
    {
        memset(st, 0, STATE_SIZE);
        job_t a = { z, zn, 0, out, 0, st, 0 };
        for (int i = 0; i < 5; i++) step(&a, 100, 100, N + 64);   /* part of the first stream */
        memset(st, 0, STATE_SIZE);
        job_t b = { z2, zn2, 0, out2, 0, st, 0 };
        int calls = 0;
        while (!b.done && calls++ < 1000000) if (step(&b, 4096, 4096, N + 64) == -2) break;
        CHECK(b.done && b.out_pos == N && !memcmp(out2, src2, N));
    }

    /* corrupt data: a final block of the reserved type 3 */
    {
        uint8_t bad[16] = { 0x07, 0x12, 0x34, 0x56 };
        memset(st, 0, STATE_SIZE);
        uint32_t in_len = sizeof bad, out_len = 256;
        int r = call_inflate(bad, &in_len, out, &out_len, st);
        CHECK(r == -2);
    }

    puts(fails ? "FAILED" : "OK");
    return fails != 0;
}
