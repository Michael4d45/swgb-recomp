/* framerate: frame counters (--fps-log) and --fps N. framerate.c has the why. */
#ifndef FRAMERATE_H
#define FRAMERATE_H

/* A command-line switch this module owns: the number of argv entries it
 * took (0: not ours). */
int framerate_arg(int argc, char** argv, int i);

/* After DirectDrawCreate: wraps the surface methods it counts. */
void framerate_on_ddraw(void* dd);

/* The shims this module adds. */
void framerate_timeGetTime(void);
void framerate_PeekMessageA(void);

/* Once the game runs: the logging thread. */
void framerate_start(void);

#endif
