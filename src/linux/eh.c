/* Minimal C++ / SEH frame stack for the lifted C on win32hle.
 *
 * The lift emits RECOMP_EH_FRAME / RECOMP_EH_LEAVE, which call recomp_eh_push
 * and recomp_eh_pop. On Windows those live in native32's eh32.c. win32hle has
 * no guest-exception dispatch yet (docs/linux-macos.md gap 6); a normal run
 * never raises, so the host only needs the push/pop stack so the binary links
 * and frames nest correctly around setjmp landing pads.
 */
#include <stdio.h>
#include <stdlib.h>

#include "recomp_types.h"

static __thread recomp_ehframe_t* t_frames;

void recomp_eh_push(recomp_ehframe_t* f) {
    f->prev = t_frames;
    t_frames = f;
}

void recomp_eh_pop(recomp_ehframe_t* f) {
    /* A longjmp past this frame already dropped it. */
    for (recomp_ehframe_t* p = t_frames; p; p = p->prev)
        if (p == f) { t_frames = f->prev; return; }
}

recomp_ehframe_t* recomp_eh_owner(uint32_t esp) {
    for (recomp_ehframe_t* f = t_frames; f; f = f->prev)
        if (f->entry_esp > esp) return f;
    return NULL;
}

void recomp_eh_resume(recomp_ehframe_t* f, uint32_t resume) {
    (void)f;
    (void)resume;
    fprintf(stderr, "[swgb] recomp_eh_resume: guest exceptions are not handled on win32hle\n");
    abort();
}
