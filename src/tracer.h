/* RetailFrameGen: breakpoints in another process, through ptrace.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdint.h>
#include <sys/types.h>
#include <machine/reg.h>

#define TRACER_MAX_BREAKPOINTS 8

typedef struct {
    intptr_t addr;   /* in the traced process */
    uint8_t  saved;  /* the byte the int3 replaced */
    int      armed;
    const char *name;
} Breakpoint;

typedef struct {
    pid_t pid;
    Breakpoint bp[TRACER_MAX_BREAKPOINTS];
    int count;
} Tracer;

/* Called at each breakpoint hit, with the registers as the function was entered. Return 0 to go
 * on, anything else to stop tracing. */
typedef int (*TracerHit)(Tracer *t, const Breakpoint *bp, const struct reg *regs, void *context);

int  tracer_attach(Tracer *t, pid_t pid);
int  tracer_add(Tracer *t, intptr_t addr, const char *name);
int  tracer_read(Tracer *t, intptr_t addr, void *out, size_t size);
/* Runs the process until stop_after_us microseconds have passed or hit asks to stop. */
int  tracer_run(Tracer *t, TracerHit hit, void *context, uint64_t stop_after_us);
/* Takes every breakpoint out and lets the process go. */
void tracer_detach(Tracer *t);
