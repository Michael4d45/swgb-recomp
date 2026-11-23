/* dplay: DirectPlay 4 inside the host (dplay.c). */
#ifndef SWGB_DPLAY_H
#define SWGB_DPLAY_H

/* --dplay host|native, --mp-port N, --mp-join HOST[,HOST], --mp-log, --mp-name NAME:
 * the number of argv entries used (0: not ours). */
int dplay_arg(int argc, char** argv, int i);
/* Before native32_bind (win32hle: before the host's other shims): put our DirectPlay behind the
 * game's imports (unless --dplay native, Windows only). */
void dplay_init(void);
/* A string the game loaded (LoadStringA's id): a line when it is an out-of-sync message. */
void dplay_string(unsigned id);
#ifndef SWGB_WIN32HLE
/* The LoadStringA shim: widescreen's, and dplay_string. */
void dplay_LoadStringA(void);
#endif
/* At exit: a line of totals for the logs. */
void dplay_finish(void);

#endif
