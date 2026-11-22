/* profile: a sampling profiler of the game thread (--profile FILE).
 * profile.c has the how; tools/profile_report.py reads the file. */
#ifndef PROFILE_H
#define PROFILE_H

/* A command-line switch this module owns: the number of argv entries it
 * took (0: not ours). */
int profile_arg(int argc, char** argv, int i);

/* On the game thread, before it enters the game: starts the sampler. */
void profile_start(void);

/* Writes the file (again): at exit, the watchdog, ExitProcess. */
void profile_finish(void);

#endif
