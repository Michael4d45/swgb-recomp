/* src/runtime/resolve.c without the game: the cache in front of the lifted
 * code's indirect calls gives recomp_lookup's answer, asks it once per
 * address, keeps "not lifted" too, never keeps an answer the dispatch table
 * does not give for that address, and is not asked while the caller is one of
 * the image's __local_unwind2 copies. SWGB_NO_RESOLVE_CACHE turns it off.
 *
 *   py -3 tools\test.py --level unit        (builds and runs every unit)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native32.h"
#include "resolve.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void f1(void) {}
static void f2(void) {}
static void f3(void) {}
static void funclet(void) {}   /* what native32 answers for a funclet: depends on the caller */

const recomp_dispatch_entry_t recomp_dispatch_table[] = {
    { 0x00401000u, f1 }, { 0x00402000u, f2 }, { 0x00403000u, f3 },
};
const uint32_t recomp_dispatch_count = 3;
uint32_t g_cur_func;
uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp;

/* native32's lookup, counted */
static int lookups;
static uint32_t lu2_caller;
recomp_func_t recomp_lookup(uint32_t va) {
    lookups++;
    if (va == 0x00404000u) return funclet;           /* not the table's: never cached */
    if (va == 0x00403000u && g_cur_func == lu2_caller) return funclet;
    for (uint32_t i = 0; i < recomp_dispatch_count; i++)
        if (recomp_dispatch_table[i].address == va) return recomp_dispatch_table[i].func;
    return NULL;
}

/* A fake image: PE headers, then one __local_unwind2 copy (resolve_init's pattern). */
static uint8_t* fake_image(uint32_t* lu2) {
    static const uint8_t pre[] = { 0x53, 0x56, 0x57, 0x8B, 0x44, 0x24, 0x10, 0x50, 0x6A, 0xFE, 0x68 };
    static const uint8_t post[] = { 0x64, 0xFF, 0x35, 0, 0, 0, 0, 0x64, 0x89, 0x25, 0, 0, 0, 0 };
    const uint32_t size = 0x4000;
    uint8_t* b = (uint8_t*)VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)b;
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(b + 0x80);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->OptionalHeader.SizeOfImage = size;
    uint32_t o = 0x2345;
    memcpy(b + o, pre, sizeof pre);
    memset(b + o + sizeof pre, 0xAA, 4);             /* the scope table's address */
    memcpy(b + o + 15, post, sizeof post);
    *lu2 = (uint32_t)(uintptr_t)b + o;
    return b;
}

int main(void) {
    uint32_t lu2;
    uint8_t* img = fake_image(&lu2);
    lu2_caller = lu2;
    resolve_init((uint32_t)(uintptr_t)img);

    /* a lifted function: looked up once, then from the cache */
    g_cur_func = 0x00401234u;
    lookups = 0;
    CHECK(resolve_cached(0x00401000u) == f1);
    CHECK(resolve_cached(0x00401000u) == f1);
    CHECK(resolve_cached(0x00401000u) == f1);
    CHECK(lookups == 1);

    /* not lifted (native code, an import): NULL, also kept */
    lookups = 0;
    CHECK(resolve_cached(0x77E4DAA0u) == NULL);
    CHECK(resolve_cached(0x77E4DAA0u) == NULL);
    CHECK(lookups == 1);

    /* an answer that is not the dispatch table's for that address: never kept */
    lookups = 0;
    CHECK(resolve_cached(0x00404000u) == funclet);
    CHECK(resolve_cached(0x00404000u) == funclet);
    CHECK(lookups == 2);

    /* the caller is __local_unwind2: the cache is not asked (NULL: the caller goes on to
     * recomp_lookup itself), and what was cached before is not handed out */
    CHECK(resolve_cached(0x00403000u) == f3);        /* cached, with an ordinary caller */
    g_cur_func = lu2;
    lookups = 0;
    CHECK(resolve_cached(0x00403000u) == NULL);
    CHECK(resolve_cached(0x00401000u) == NULL);
    CHECK(lookups == 0);
    g_cur_func = 0x00401234u;
    CHECK(resolve_cached(0x00403000u) == f3);

    /* two addresses in one slot ((va ^ va >> 12) & 4095): each answer stays right */
    uint32_t a = 0x00402000u, b = a + 1;
    while (((a ^ (a >> 12)) & 4095) != ((b ^ (b >> 12)) & 4095)) b++;
    CHECK(resolve_cached(a) == f2);
    CHECK(resolve_cached(b) == NULL);
    CHECK(resolve_cached(a) == f2);

    /* every address in a sweep agrees with recomp_lookup */
    int agree = 1;
    for (uint32_t va = 0x00400000u; va < 0x00410000u; va += 0x100) {
        recomp_func_t want = recomp_lookup(va);
        if (want == funclet) continue;
        if (resolve_cached(va) != want || resolve_cached(va) != want) agree = 0;
    }
    CHECK(agree);

    /* SWGB_NO_RESOLVE_CACHE: the old path (always NULL from the cache) */
    SetEnvironmentVariableA("SWGB_NO_RESOLVE_CACHE", "1");
    resolve_init((uint32_t)(uintptr_t)img);
    CHECK(resolve_cached(0x00401000u) == NULL);
    SetEnvironmentVariableA("SWGB_NO_RESOLVE_CACHE", NULL);

    puts(fails ? "FAILED" : "OK");
    return fails != 0;
}
