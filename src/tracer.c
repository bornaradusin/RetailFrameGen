/* RetailFrameGen: breakpoints in another process, through ptrace.
 *
 * Each breakpoint is an int3 at a function's first byte. A game has many threads, so:
 *  - the thread that stopped is asked for (PT_LWPINFO), and its own registers are read and set;
 *  - to run the real first instruction, every other thread is suspended, the original byte goes
 *    back, only that thread steps, the int3 goes in again and the others resume: no other thread
 *    can run past the function's start while it isn't armed;
 *  - a trap that isn't ours is logged and never passed to the game (the game would die of it);
 *  - before detaching, the breakpoints come out and the game runs a little longer still
 *    attached, so a thread that hit one just as tracing ended is put back to the function's
 *    start, not left after the int3.
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

#define QUIET_US 150000u   /* no stop for this long after the breakpoints came out: done */

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

int tracer_read_code(Tracer *t, intptr_t addr, void *out, size_t size)
{
    return io(t, PIOD_READ_I, addr, out, size);
}

static int poke_byte(Tracer *t, intptr_t addr, uint8_t value)
{
    return io(t, PIOD_WRITE_I, addr, &value, 1);
}

/* Waits for the process to stop, at most until deadline_us (0: no limit). Returns its stop
 * signal, -2 at the deadline, or -1 when it went away. */
static int wait_stop_until(Tracer *t, uint64_t deadline_us)
{
    int status, got;
    for (;;) {
        got = waitpid(t->pid, &status, deadline_us ? WNOHANG : 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (got == 0) {
            if (now_us() >= deadline_us) return -2;
            usleep(500);
            continue;
        }
        if (WIFSTOPPED(status)) return WSTOPSIG(status);
        if (WIFEXITED(status) || WIFSIGNALED(status)) return -1;
    }
}

static int wait_stop(Tracer *t)
{
    return wait_stop_until(t, 0);
}

/* The thread the process stopped for. */
static int stopped_thread(Tracer *t, lwpid_t *lwp)
{
    struct ptrace_lwpinfo info;
    memset(&info, 0, sizeof(info));
    if (ptrace(PT_LWPINFO, t->pid, (caddr_t)&info, sizeof(info)) < 0) return -1;
    *lwp = info.pl_lwpid;
    return 0;
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
    if (t->count >= TRACER_MAX_BREAKPOINTS || !addr) return TRACER_ADD_FULL;
    for (int i = 0; i < t->count; i++)
        if (t->bp[i].addr == addr) return TRACER_ADD_FULL;   /* two names for one function */
    Breakpoint *bp = &t->bp[t->count];
    uint8_t back = 0;
    memset(bp, 0, sizeof(*bp));
    if (io(t, PIOD_READ_I, addr, &bp->saved, 1) < 0) return TRACER_ADD_READ;
    if (poke_byte(t, addr, 0xcc) < 0) return TRACER_ADD_WRITE;
    if (io(t, PIOD_READ_I, addr, &back, 1) < 0 || back != 0xcc) {
        poke_byte(t, addr, bp->saved);
        return TRACER_ADD_NOT_THERE;
    }
    bp->addr = addr;
    bp->name = name;
    bp->armed = 1;
    t->count++;
    return 0;
}

/* The breakpoint at addr, armed or not. */
static Breakpoint *find(Tracer *t, intptr_t addr)
{
    for (int i = 0; i < t->count; i++)
        if (t->bp[i].addr == addr) return &t->bp[i];
    return NULL;
}

static void disarm(Tracer *t, Breakpoint *bp)
{
    if (bp->armed && poke_byte(t, bp->addr, bp->saved) == 0) bp->armed = 0;
}

/* Every thread but keep suspended (the process is stopped). */
static void suspend_others(Tracer *t, lwpid_t keep)
{
    lwpid_t list[TRACER_MAX_THREADS];
    int n = ptrace(PT_GETLWPLIST, t->pid, (caddr_t)list, TRACER_MAX_THREADS);
    t->suspended_count = 0;
    for (int i = 0; i < n; i++)
        if (list[i] != keep && ptrace(PT_SUSPEND, list[i], NULL, 0) == 0)
            t->suspended[t->suspended_count++] = list[i];
}

static void resume_others(Tracer *t)
{
    for (int i = 0; i < t->suspended_count; i++) ptrace(PT_RESUME, t->suspended[i], NULL, 0);
    t->suspended_count = 0;
}

static int rewind_stopped(Tracer *t, Breakpoint **out, struct reg *regs, lwpid_t *lwp);

/* lwp is at bp's address with the int3 there: runs the real first instruction in that thread
 * alone, then arms it again. Returns a signal that came meanwhile, to pass on, or -1. */
static int step_over(Tracer *t, Breakpoint *bp, lwpid_t lwp)
{
    int pass = 0;
    if (poke_byte(t, bp->addr, bp->saved) < 0) return -1;
    suspend_others(t, lwp);
    for (int tries = 0; tries < 100; tries++) {
        if (ptrace(PT_STEP, lwp, (caddr_t)1, 0) < 0) { resume_others(t); return -1; }
        int sig = wait_stop(t);
        if (sig < 0) return -1;
        lwpid_t who = 0;
        if (sig == SIGTRAP) {
            if (stopped_thread(t, &who) == 0 && who == lwp) break;
            /* Another thread's hit, from before it was suspended: back to the function's start,
             * where it runs the real instruction (the byte is out now), so it isn't logged. */
            struct reg regs;
            if (rewind_stopped(t, NULL, &regs, &who) < 0) { resume_others(t); return -1; }
        } else {
            pass = sig;   /* passed at the next continue */
            t->signals_passed++;
            t->last_signal = sig;
        }
    }
    if (bp->armed && poke_byte(t, bp->addr, 0xcc) < 0) bp->armed = 0;
    resume_others(t);
    return pass;
}

/* The thread the process stopped for, at a SIGTRAP: put back to the start of the function it
 * hit, when that was one of ours (armed or not). 1 when it was, 0 when not, -1 on error. */
static int rewind_stopped(Tracer *t, Breakpoint **out, struct reg *regs, lwpid_t *lwp)
{
    if (stopped_thread(t, lwp) < 0) return -1;
    if (ptrace(PT_GETREGS, *lwp, (caddr_t)regs, 0) < 0) return -1;
    Breakpoint *bp = find(t, (intptr_t)regs->r_rip - 1);
    if (!bp) {
        t->foreign_traps++;
        t->last_foreign_rip = (intptr_t)regs->r_rip;
        return 0;
    }
    regs->r_rip = bp->addr;
    if (ptrace(PT_SETREGS, *lwp, (caddr_t)regs, 0) < 0) return -1;
    if (out) *out = bp;
    return 1;
}

int tracer_run(Tracer *t, TracerHit hit, TracerTick tick, uint64_t tick_us, void *context,
               uint64_t stop_after_us)
{
    const uint64_t end = now_us() + stop_after_us;
    uint64_t next_tick = now_us() + tick_us;
    int pass = 0;

    for (;;) {
        if (ptrace(PT_CONTINUE, t->pid, (caddr_t)1, pass) < 0) return -1;
        pass = 0;

        int sig;
        for (;;) {
            uint64_t until = end < next_tick || !tick ? end : next_tick;
            sig = wait_stop_until(t, until);
            if (sig != -2) break;
            if (now_us() >= end) {
                /* Time's up: stop the game, keep whatever stopped it first for detaching. */
                kill(t->pid, SIGSTOP);
                sig = wait_stop(t);
                if (sig < 0) return -1;
                if (sig == SIGTRAP) {
                    Breakpoint *bp = NULL;
                    struct reg regs;
                    lwpid_t lwp;
                    if (rewind_stopped(t, &bp, &regs, &lwp) == 1) t->rewound_at_end++;
                } else if (sig != SIGSTOP) {
                    t->signals_passed++;
                    t->last_signal = sig;
                    t->pending_signal = sig;   /* tracer_detach passes it on */
                }
                return 0;
            }
            tick(t, context);
            next_tick = now_us() + tick_us;
        }
        if (sig < 0) return -1;

        if (sig != SIGTRAP) {
            /* The game's own signal: it gets it. */
            pass = sig;
            t->signals_passed++;
            t->last_signal = sig;
            continue;
        }

        Breakpoint *bp = NULL;
        struct reg regs;
        lwpid_t lwp;
        int ours = rewind_stopped(t, &bp, &regs, &lwp);
        if (ours < 0) return -1;
        if (!ours) continue;   /* logged, not passed */

        bp->hits++;
        int action = bp->armed ? hit(t, bp, &regs, lwp, context) : TRACER_GO;
        if (action == TRACER_DISARM || !bp->armed) {
            disarm(t, bp);   /* the thread runs the real instruction from the start */
        } else {
            int got = step_over(t, bp, lwp);
            if (got < 0) return -1;
            pass = got;
        }
        if (action == TRACER_STOP) {
            /* Stopped now (after the step): detach from here. */
            t->pending_signal = pass;   /* counted already */
            return 0;
        }
    }
}

int tracer_detach(Tracer *t)
{
    /* The process is stopped. A signal it stopped with that wasn't ours goes on to it. */
    int pass = t->pending_signal;
    int clean = 0;

    t->pending_signal = 0;
    for (int i = 0; i < t->count; i++) {
        uint8_t back = 0;
        disarm(t, &t->bp[i]);
        if (io(t, PIOD_READ_I, t->bp[i].addr, &back, 1) < 0 || back != t->bp[i].saved)
            t->restore_mismatches++;
    }

    /* Threads that hit an int3 just before it came out report it as the game runs on: each is
     * put back to the function's start. Done after a quiet moment, stopped by our SIGSTOP. */
    int asked_stop = 0;
    for (int rounds = 0; rounds < 10000; rounds++) {
        if (ptrace(PT_CONTINUE, t->pid, (caddr_t)1, pass) < 0) return -1;
        pass = 0;
        int sig = wait_stop_until(t, now_us() + QUIET_US);
        if (sig == -1) return -1;
        if (sig == -2) {
            kill(t->pid, SIGSTOP);
            asked_stop = 1;
            sig = wait_stop(t);
            if (sig < 0) return -1;
        }
        if (sig == SIGSTOP && asked_stop) { clean = 1; break; }
        asked_stop = 0;
        if (sig == SIGTRAP) {
            struct reg regs;
            lwpid_t lwp;
            if (rewind_stopped(t, NULL, &regs, &lwp) == 1) t->rewound_at_end++;
        } else if (sig != SIGSTOP) {
            pass = sig;
            t->signals_passed++;
            t->last_signal = sig;
        }
    }
    if (ptrace(PT_DETACH, t->pid, (caddr_t)1, pass) < 0) return -1;
    return clean ? 0 : -1;
}
