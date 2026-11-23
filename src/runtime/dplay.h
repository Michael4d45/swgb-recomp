/* dplay: DirectPlay 4 inside the host (dplay.c). */
#ifndef SWGB_DPLAY_H
#define SWGB_DPLAY_H

/* --dplay host|native, --mp-port N, --mp-join HOST[,HOST], --mp-log, --mp-name NAME:
 * the number of argv entries used (0: not ours). */
int dplay_arg(int argc, char** argv, int i);
/* Before native32_bind: put our DirectPlay behind the game's imports (unless --dplay native). */
void dplay_init(void);
/* The LoadStringA shim: widescreen's, and a line when the game shows an out-of-sync message. */
void dplay_LoadStringA(void);
/* At exit: a line of totals for the logs. */
void dplay_finish(void);

#endif
