/* RetailFrameGen: breakpoints in another process, through ptrace.
 *
 * Each breakpoint is an int3 at a function's first byte. At a hit the registers are handed to
 * the caller, the original byte goes back, the process steps one instruction, and the int3 goes
 * in again. That is slow (two stops a call), which is fine for logging a few calls a frame.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "tracer.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/wait.h>

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static int io(Tracer *t, int op, intptr_t addr, void *buf, size_t size)
{
    struct ptrace_io_desc desc = { op, (void *)addr, buf, size };
    if (ptrace(PT_IO, t->pid, (caddr_t)&desc, 0) < 0) return -1;
    return desc.piod_len == size ? 0 : -1;
}

int tracer_read(Tracer *t, intptr_t addr, void *out, size_t size)
{
    return io(t, PIOD_READ_D, addr, out, size);
}

static int poke_byte(Tracer *t, intptr_t addr, uint8_t value)
{
    return io(t, PIOD_WRITE_I, addr, &value, 1);
}

/* Waits for the process to stop. Returns its stop signal, or -1 when it went away. */
static int wait_stop(Tracer *t)
{
    int status;
    for (;;) {
        if (waitpid(t->pid, &status, 0) < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (WIFSTOPPED(status)) return WSTOPSIG(status);
        if (WIFEXITED(status) || WIFSIGNALED(status)) return -1;
    }
}

int tracer_attach(Tracer *t, pid_t pid)
{
    memset(t, 0, sizeof(*t));
    t->pid = pid;
    if (ptrace(PT_ATTACH, pid, NULL, 0) < 0) return -1;
    return wait_stop(t) < 0 ? -1 : 0;
}

int tracer_add(Tracer *t, intptr_t addr, const char *name)
{
    if (t->count >= TRACER_MAX_BREAKPOINTS || !addr) return -1;
    Breakpoint *bp = &t->bp[t->count];
    if (io(t, PIOD_READ_I, addr, &bp->saved, 1) < 0) return -1;
    if (poke_byte(t, addr, 0xcc) < 0) return -1;
    bp->addr = addr;
    bp->name = name;
    bp->armed = 1;
    t->count++;
    return 0;
}

static Breakpoint *find(Tracer *t, intptr_t addr)
{
    for (int i = 0; i < t->count; i++)
        if (t->bp[i].armed && t->bp[i].addr == addr) return &t->bp[i];
    return NULL;
}

int tracer_run(Tracer *t, TracerHit hit, void *context, uint64_t stop_after_us)
{
    const uint64_t end = now_us() + stop_after_us;
    int signal_to_pass = 0;

    for (;;) {
        if (ptrace(PT_CONTINUE, t->pid, (caddr_t)1, signal_to_pass) < 0) return -1;
        signal_to_pass = 0;

        /* Wait for a stop, but no later than the end: then stop the game ourselves. */
        int status, got;
        while ((got = waitpid(t->pid, &status, WNOHANG)) == 0) {
            if (now_us() >= end) {
                kill(t->pid, SIGSTOP);
                return wait_stop(t) < 0 ? -1 : 0;
            }
            usleep(500);
        }
        if (got < 0) return -1;
        if (WIFEXITED(status) || WIFSIGNALED(status)) return -1;
        if (!WIFSTOPPED(status)) continue;

        int sig = WSTOPSIG(status);
        if (sig != SIGTRAP) { signal_to_pass = sig; continue; }

        struct reg regs;
        if (ptrace(PT_GETREGS, t->pid, (caddr_t)&regs, 0) < 0) return -1;
        Breakpoint *bp = find(t, (intptr_t)regs.r_rip - 1);
        if (!bp) { signal_to_pass = SIGTRAP; continue; }   /* not ours: the game's own */

        /* Back to the function's first instruction, as it was entered. */
        regs.r_rip = bp->addr;
        if (ptrace(PT_SETREGS, t->pid, (caddr_t)&regs, 0) < 0) return -1;
        int stop = hit(t, bp, &regs, context);

        /* Run the real first instruction, then put the int3 back. */
        if (poke_byte(t, bp->addr, bp->saved) < 0) return -1;
        if (ptrace(PT_STEP, t->pid, (caddr_t)1, 0) < 0) return -1;
        if (wait_stop(t) < 0) return -1;
        if (poke_byte(t, bp->addr, 0xcc) < 0) return -1;

        if (stop) return 0;
    }
}

void tracer_detach(Tracer *t)
{
    for (int i = 0; i < t->count; i++)
        if (t->bp[i].armed) {
            poke_byte(t, t->bp[i].addr, t->bp[i].saved);
            t->bp[i].armed = 0;
        }
    ptrace(PT_DETACH, t->pid, (caddr_t)1, 0);
}
