/* src/runtime/coverage.c without the game: an unresolved indirect call stops
 * the run with exit code 5 and says what to seed; SWGB_ICALL_CONTINUE=1
 * reports each address once and carries on; an exception handler's
 * continuation (the hook coverage.c chains to) is still handed to it.
 *
 *   cl /nologo /DRECOMP_FLAT_MEMORY /DRECOMP_LOCAL_REGS tools\coverage_test.c src\runtime\coverage.c
 *      /Isrc\runtime /I%PCRECOMP%\runtime\native32 /I%PCRECOMP%\runtime\recomp32
 *   coverage_test.exe                                      OK
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native32.h"
#include "recomp_trace.h"
#include "coverage.h"

static void f1(void) {}
const recomp_dispatch_entry_t recomp_dispatch_table[] = {
    { 0x00406E20u, f1 }, { 0x0056480Cu, f1 }, { 0x005648C0u, f1 },
};
const uint32_t recomp_dispatch_count = 3;

/* What coverage.c uses from native32, the tracer and softdd. */
int native32_in_guest(uint32_t va) { return va >= 0x00400000u && va < 0x007AF000u; }
void native32_dump_icalls(int n) { fprintf(stderr, "last indirect calls (newest first): %d\n", n); }
void recomp_dump_trace(const char* why) { (void)why; }
void recomp_trace_flush(void) {}
void softdd_finish(void) {}
const char* g_cur_import = "(none)";
uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp, g_cur_func;   /* native32.c's */

static int eh_seen;
static int fake_eh(uint32_t va) { eh_seen = va == 0x00632E6Fu; return 0; }

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stdout, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(int argc, char** argv) {
    if (argc > 1) {                     /* the child: one miss */
        recomp_eh_jump_hook = fake_eh;
        coverage_init();
        g_cur_func = 0x00406E20u;
        g_esp = 0;
        int r = recomp_eh_jump_hook((uint32_t)strtoul(argv[1], NULL, 0));
        fprintf(stderr, "returned %d\n", r);
        return 0;
    }
    char self[MAX_PATH], cmd[2 * MAX_PATH], out[8192];
    GetModuleFileNameA(NULL, self, MAX_PATH);
    for (int mode = 0; mode < 2; mode++) {
        _putenv(mode ? "SWGB_ICALL_CONTINUE=1" : "SWGB_ICALL_CONTINUE=");
        _snprintf(cmd, sizeof cmd, "\"\"%s\" 0x005648A0 2>&1\"", self);
        FILE* p = _popen(cmd, "r");
        size_t n = fread(out, 1, sizeof out - 1, p);
        out[n] = 0;
        int rc = _pclose(p);
        if (!mode) {
            CHECK(rc == 5);
            CHECK(strstr(out, "ICALL: unresolved VA 0x005648A0 from 0x00406E20") != NULL);
            CHECK(strstr(out, "nearest below: sub_0056480C (+0x94)") != NULL);
            CHECK(strstr(out, "seeds_from_logs.py") != NULL);
            CHECK(strstr(out, "returned") == NULL);
        } else {
            CHECK(rc == 0);
            CHECK(strstr(out, "carrying on") != NULL);
            CHECK(strstr(out, "returned 0") != NULL);
        }
        if (fails) fputs(out, stdout);
    }
    /* In this process: the previous hook (eh32) sees the address first. */
    _putenv("SWGB_ICALL_CONTINUE=1");   /* getenv reads the CRT's copy */
    recomp_eh_jump_hook = fake_eh;
    coverage_init();
    recomp_eh_jump_hook(0x00632E6Fu);
    CHECK(eh_seen);
    puts(fails ? "FAILED" : "OK");
    return fails != 0;
}
