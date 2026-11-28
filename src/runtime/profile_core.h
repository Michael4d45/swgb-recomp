/* profile_core: what the two samplers share (profile_core.c): the options, the
 * table of distinct samples, reading return addresses off a copied stack, and
 * the file tools/profile_report.py reads. How the game thread is stopped and
 * its EIP and stack read is each host's (src/runtime/profile.c with
 * SuspendThread, src/linux/profile.c with a signal). */
#ifndef PROFILE_CORE_H
#define PROFILE_CORE_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define PROFILE_MAXF 48              /* frames kept per sample, f[0] the EIP */
#define PROFILE_COPY (96 * 1024)     /* stack bytes read per sample */

/* The options (profile_arg): the file ("" when not profiling), the window in
 * seconds after the start, samples a second. */
extern char   profile_path[4096];
extern double profile_from, profile_to;
extern int    profile_hz;

/* The host's executable, where the lifted code is: return addresses point
 * into it. Set by the host's profile_start. */
extern uintptr_t profile_img_lo, profile_img_hi;

/* Counts so far; profile_wall is the seconds sampled. */
extern long   profile_samples, profile_running, profile_lost;
extern double profile_wall;

/* The table; 0: out of memory. */
int profile_table_init(void);

/* p is a return address in the executable: just after a call (E8 rel32, FF /2
 * with a 0, 1 or 4 byte displacement, FF /2 register). */
int profile_after_call(uintptr_t p);

/* The frames in a copy of the stack (n bytes, read from its top down),
 * appended to f[0..depth): the new depth. */
int profile_scan(uint32_t* f, int depth, const uint8_t* stack, size_t n);

/* One sample, counted with the identical ones before it. */
void profile_record(const uint32_t* f, int depth, uint8_t mode, uint8_t run, const char* imp);

/* The game's mode, [[G_GAME]+G_GAME_MODE] (2 menus, 4 in game), or 0. */
uint32_t profile_mode(void);

/* The file, rewritten: the header, the host's modules (module LO SIZE PATH
 * lines), the samples. */
void profile_write(double cpu_thread_s, double cpu_process_s, void (*modules)(FILE*));

#endif
