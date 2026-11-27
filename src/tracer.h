/* RetailFrameGen: breakpoints in another process, through ptrace.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdint.h>
#include <sys/types.h>
#include <machine/reg.h>

#define TRACER_MAX_BREAKPOINTS 16
#define TRACER_MAX_THREADS     512

typedef struct {
    intptr_t addr;      /* in the traced process */
    uint8_t  saved;     /* the byte the int3 replaced */
    int      armed;
    const char *name;
    unsigned hits;
} Breakpoint;

typedef struct {
    pid_t pid;
    Breakpoint bp[TRACER_MAX_BREAKPOINTS];
    int count;

    /* What happened besides our breakpoints, for the log. */
    unsigned foreign_traps;     /* int3s or traps that weren't ours: not passed to the game */
    intptr_t last_foreign_rip;
    unsigned signals_passed;    /* other signals, passed to the game as they came */
    int last_signal;
    unsigned rewound_at_end;    /* hits still waiting when the breakpoints came out */
    unsigned restore_mismatches; /* breakpoints whose byte didn't read back as it was, after */
    int pending_signal;         /* the game's, from the stop tracing ended at: passed when detaching */

    lwpid_t suspended[TRACER_MAX_THREADS];
    int suspended_count;
} Tracer;

enum { TRACER_GO = 0, TRACER_DISARM = 1, TRACER_STOP = 2 };

/* Called at each breakpoint hit, with the hitting thread's registers as the function was
 * entered. TRACER_GO goes on, TRACER_DISARM takes this breakpoint out for good, TRACER_STOP
 * ends tracing. */
typedef int (*TracerHit)(Tracer *t, Breakpoint *bp, const struct reg *regs, lwpid_t lwp,
                         void *context);
/* Called about every tick_us while tracing (from the tracer, with the game running). */
typedef void (*TracerTick)(Tracer *t, void *context);

int  tracer_attach(Tracer *t, pid_t pid);
/* Puts an int3 at addr and reads it back: 0, or TRACER_ADD_* why not (nothing is left changed). */
enum { TRACER_ADD_FULL = -1, TRACER_ADD_READ = -2, TRACER_ADD_WRITE = -3, TRACER_ADD_NOT_THERE = -4 };
int  tracer_add(Tracer *t, intptr_t addr, const char *name);
int  tracer_read(Tracer *t, intptr_t addr, void *out, size_t size);
/* Reads code: through I space, as breakpoints are put in. */
int  tracer_read_code(Tracer *t, intptr_t addr, void *out, size_t size);
/* Runs the process until stop_after_us microseconds have passed or hit asks to stop. Ends with
 * the process stopped and still attached: 0, or -1 when it went away. */
int  tracer_run(Tracer *t, TracerHit hit, TracerTick tick, uint64_t tick_us, void *context,
                uint64_t stop_after_us);
/* Takes every breakpoint out, rewinds any thread still waiting at one, and lets the process go:
 * 0, or -1 when that didn't go cleanly. */
int  tracer_detach(Tracer *t);
