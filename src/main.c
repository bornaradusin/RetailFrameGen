/* RetailFrameGen, step 1: log how a running retail game shows its frames.
 *
 * Attaches to the game and puts breakpoints on the functions a game can show its frames with:
 * libSceVideoOut's buffer registration and flips (the PS4 ones and the PS5 "2" ones), and, when
 * the game has them, the graphics drivers' submit-and-flip functions. Each call is logged for 30
 * seconds to /data/retailframegen/log.txt, with how far it got as it goes, so a log cut short
 * still shows where. Nothing in the game is changed, and every breakpoint is taken out after.
 *
 * Function names that a game's libraries don't have are logged as "not in the game": several
 * here are best guesses at what PS5 games use, and the log says which ones are real.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "tracer.h"

#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/user.h>

#include <ps5/kernel.h>

#define DIR       "/data/retailframegen"
#define LOG_PATH  DIR "/log.txt"
#define PID_PATH  DIR "/pid.txt"
#define RUN_US    (30u * 1000000u)
#define TICK_US   (5u * 1000000u)
#define DETAILED_CALLS 8      /* calls of a function logged with their arguments */
#define DUMPED_CALLS   3      /* of those, with the memory their arguments point at */
#define TIMED_CALLS    60     /* calls of a function logged with the time since the one before */
#define FREQUENT_CAP   300    /* a frequent function's breakpoint comes out after this many */

/* The display buffer attribute as libSceVideoOut takes it (PS4 layout; the PS5 may add fields,
 * so the raw bytes are logged too). */
typedef struct {
    int32_t  pixel_format;
    int32_t  tiling_mode;
    int32_t  aspect_ratio;
    uint32_t width;
    uint32_t height;
    uint32_t pitch_in_pixel;
    uint32_t option;
    uint32_t reserved0;
    uint64_t reserved1;
} BufferAttribute;

typedef struct {
    const char *library;
    const char *name;
    int frequent;   /* called every frame: its breakpoint comes out after FREQUENT_CAP calls */
} Watched;

/* What a game may show its frames with. The PS4 libSceVideoOut names are known; the rest are
 * the likely PS5 ones, checked against what the game's libraries really have. */
static const Watched watched[] = {
    { "libSceVideoOut.sprx", "sceVideoOutOpen", 0 },
    { "libSceVideoOut.sprx", "sceVideoOutRegisterBuffers", 0 },
    { "libSceVideoOut.sprx", "sceVideoOutRegisterBuffers2", 0 },
    { "libSceVideoOut.sprx", "sceVideoOutSetFlipRate", 0 },
    { "libSceVideoOut.sprx", "sceVideoOutSubmitChangeBufferAttribute", 0 },
    { "libSceVideoOut.sprx", "sceVideoOutSubmitChangeBufferAttribute2", 0 },
    { "libSceVideoOut.sprx", "sceVideoOutSubmitFlip", 1 },
    { "libSceAgcDriver.sprx", "sceAgcDriverSubmitDcb", 1 },
    { "libSceAgcDriver.sprx", "sceAgcDriverSubmitMultiDcbs", 1 },
    { "libSceGnmDriver.sprx", "sceGnmSubmitAndFlipCommandBuffers", 1 },
    { "libSceGnmDriver.sprx", "sceGnmSubmitAndFlipCommandBuffersForWorkload", 1 },
    { "libSceGnmDriver.sprx", "sceGnmSubmitCommandBuffers", 1 },
};
#define WATCHED (int)(sizeof(watched) / sizeof(watched[0]))

/* Libraries logged as loaded or not, to see which graphics library the game uses. */
static const char *const libraries[] = {
    "libSceVideoOut.sprx", "libSceAgc.sprx", "libSceAgcDriver.sprx", "libSceGnmDriver.sprx",
    "libSceGnmDriverForNeoMode.sprx",
};

typedef struct {
    const Watched *what;
    uint64_t first_us, last_us;
    int disarmed_after;   /* calls when its breakpoint came out, 0 while in */
} Watch;

typedef struct {
    FILE *log;
    uint64_t start_us;
    Watch watch[TRACER_MAX_BREAKPOINTS];   /* by breakpoint index */
    unsigned hits;
} Session;

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void say(Session *s, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    if (s->log) { vfprintf(s->log, format, args); fflush(s->log); }
    va_end(args);
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}

/* The game: the PID in pid.txt, else the one process called eboot.bin. */
static pid_t find_game(void)
{
    FILE *f = fopen(PID_PATH, "r");
    if (f) {
        int pid = 0;
        int ok = fscanf(f, "%d", &pid) == 1;
        fclose(f);
        if (ok && pid > 0) return pid;
    }

    int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_PROC };
    size_t size = 0;
    if (sysctl(mib, 3, NULL, &size, NULL, 0) < 0) return -1;
    size += size / 4;
    struct kinfo_proc *procs = malloc(size);
    if (!procs || sysctl(mib, 3, procs, &size, NULL, 0) < 0) { free(procs); return -1; }

    pid_t found = -1;
    for (size_t i = 0; i < size / sizeof(*procs); i++)
        if (procs[i].ki_pid != getpid() && strcmp(procs[i].ki_comm, "eboot.bin") == 0) {
            found = procs[i].ki_pid;
            break;
        }
    free(procs);
    return found;
}

static void dump(Session *s, Tracer *t, const char *label, uint64_t address)
{
    uint8_t bytes[48];
    if (address < 0x10000 || tracer_read(t, (intptr_t)address, bytes, sizeof(bytes)) < 0) return;
    say(s, "           %s ->", label);
    for (size_t i = 0; i < sizeof(bytes); i++) say(s, " %02x", bytes[i]);
    say(s, "\n");
}

static void register_buffers(Session *s, Tracer *t, const struct reg *r)
{
    /* (handle, startIndex, void *const *addresses, bufferNum, const attribute *) */
    BufferAttribute a;
    if (tracer_read(t, (intptr_t)r->r_r8, &a, sizeof(a)) == 0)
        say(s, "           as PS4 buffers: start=%d count=%d %ux%u pitch=%u format=0x%x tiling=%d "
               "aspect=%d option=0x%x\n", (int)r->r_rsi, (int)r->r_rcx, a.width, a.height,
            a.pitch_in_pixel, (unsigned)a.pixel_format, a.tiling_mode, a.aspect_ratio, a.option);
    for (int i = 0; i < (int)r->r_rcx && i < 16; i++) {
        uint64_t address = 0;
        if (tracer_read(t, (intptr_t)r->r_rdx + i * 8, &address, 8) == 0)
            say(s, "           buffer %d at 0x%016llx\n", (int)r->r_rsi + i,
                (unsigned long long)address);
    }
}

static int on_hit(Tracer *t, Breakpoint *bp, const struct reg *r, lwpid_t lwp, void *context)
{
    Session *s = context;
    Watch *w = &s->watch[bp - t->bp];
    const uint64_t now = now_us();
    const double at = (now - s->start_us) / 1e6;

    s->hits++;
    if (bp->hits == 1) {
        w->first_us = now;
        say(s, "%8.3f first call of %s\n", at, bp->name);
    }
    if (bp->hits <= DETAILED_CALLS) {
        say(s, "%8.3f %s #%u thread=%d rdi=0x%llx rsi=0x%llx rdx=0x%llx rcx=0x%llx r8=0x%llx "
               "r9=0x%llx\n", at, bp->name, bp->hits, (int)lwp,
            (unsigned long long)r->r_rdi, (unsigned long long)r->r_rsi,
            (unsigned long long)r->r_rdx, (unsigned long long)r->r_rcx,
            (unsigned long long)r->r_r8, (unsigned long long)r->r_r9);
        if (bp->hits <= DUMPED_CALLS) {
            dump(s, t, "rdi", (uint64_t)r->r_rdi);
            dump(s, t, "rsi", (uint64_t)r->r_rsi);
            dump(s, t, "rdx", (uint64_t)r->r_rdx);
            dump(s, t, "rcx", (uint64_t)r->r_rcx);
            dump(s, t, "r8", (uint64_t)r->r_r8);
            dump(s, t, "r9", (uint64_t)r->r_r9);
        }
        if (!strcmp(bp->name, "sceVideoOutRegisterBuffers")) register_buffers(s, t, r);
    } else if (bp->hits <= TIMED_CALLS) {
        say(s, "%8.3f %s #%u since_last=%.2fms\n", at, bp->name, bp->hits,
            (now - w->last_us) / 1000.0);
    }
    w->last_us = now;

    if (w->what->frequent && bp->hits >= FREQUENT_CAP) {
        w->disarmed_after = (int)bp->hits;
        say(s, "%8.3f %s: %u calls, its breakpoint comes out so the game runs at full speed\n", at,
            bp->name, bp->hits);
        return TRACER_DISARM;
    }
    return TRACER_GO;
}

static void on_tick(Tracer *t, void *context)
{
    Session *s = context;
    say(s, "%8.3f still watching: %u calls caught", (now_us() - s->start_us) / 1e6, s->hits);
    if (t->foreign_traps) say(s, ", %u other traps", t->foreign_traps);
    if (t->signals_passed) say(s, ", %u signals passed to the game", t->signals_passed);
    say(s, "\n");
}

int main(void)
{
    Session s;
    memset(&s, 0, sizeof(s));
    mkdir(DIR, 0777);
    s.log = fopen(LOG_PATH, "w");
    say(&s, "RetailFrameGen step 1 (0.2): logging the frame hand-off\n");

    /* ptrace and reading another process need the debugger's rights. */
    kernel_set_ucred_authid(getpid(), 0x4800000000010003l);

    /* Waits for the game to start, so its buffer registrations at start are seen too. */
    say(&s, "Waiting up to 2 minutes for a game to start...\n");
    pid_t pid = -1;
    for (int tries = 0; tries < 1200 && (pid = find_game()) < 0; tries++) usleep(100000);
    if (pid < 0) {
        say(&s, "No game started: start a game, or put its PID in " PID_PATH "\n");
        return 1;
    }
    say(&s, "game pid=%d\n", (int)pid);

    /* Right after the game starts, its libraries may not be loaded yet. */
    uint32_t handle = 0;
    int loaded = -1;
    for (int tries = 0; tries < 2000 && (loaded = kernel_dynlib_handle(pid, "libSceVideoOut.sprx",
                                                                      &handle)) < 0; tries++)
        usleep(5000);
    if (loaded < 0) {
        say(&s, "The game has no libSceVideoOut loaded after 10 seconds\n");
        return 1;
    }
    for (size_t i = 0; i < sizeof(libraries) / sizeof(libraries[0]); i++) {
        uint32_t h = 0;
        say(&s, "library %s: %s\n", libraries[i],
            kernel_dynlib_handle(pid, libraries[i], &h) < 0 ? "not loaded" : "loaded");
    }

    intptr_t addrs[WATCHED];
    for (int i = 0; i < WATCHED; i++) {
        uint32_t h = 0;
        addrs[i] = kernel_dynlib_handle(pid, watched[i].library, &h) < 0
                       ? 0 : kernel_dynlib_dlsym(pid, h, watched[i].name);
        if (addrs[i]) say(&s, "%s at 0x%llx\n", watched[i].name, (unsigned long long)addrs[i]);
        else say(&s, "%s: not in the game\n", watched[i].name);
    }

    Tracer *t = calloc(1, sizeof(*t));
    if (!t) { say(&s, "Out of memory\n"); return 1; }
    say(&s, "Attaching to the game...\n");
    if (tracer_attach(t, pid) < 0) {
        say(&s, "Couldn't attach to the game\n");
        return 1;
    }
    say(&s, "Attached\n");
    int armed = 0;
    for (int i = 0; i < WATCHED; i++) {
        if (!addrs[i]) continue;
        int index = t->count;
        if (tracer_add(t, addrs[i], watched[i].name) < 0) {
            say(&s, "Couldn't put a breakpoint on %s\n", watched[i].name);
            continue;
        }
        s.watch[index].what = &watched[i];
        armed++;
    }
    say(&s, "%d breakpoints in; watching for 30 seconds\n", armed);

    s.start_us = now_us();
    int rc = tracer_run(t, on_hit, on_tick, TICK_US, &s, RUN_US);
    const double seconds = (now_us() - s.start_us) / 1e6;
    say(&s, "%8.3f %s; taking the breakpoints out\n", seconds,
        rc < 0 ? "the game went away, or tracing failed" : "done watching");
    int detached = rc < 0 ? -1 : tracer_detach(t);

    say(&s, "\nafter %.1fs:\n", seconds);
    for (int i = 0; i < t->count; i++) {
        const Breakpoint *bp = &t->bp[i];
        const Watch *w = &s.watch[i];
        say(&s, "  %s: %u calls", bp->name, bp->hits);
        if (bp->hits > 1 && w->last_us > w->first_us)
            say(&s, " (%.1f a second while watched)",
                (bp->hits - 1) / ((w->last_us - w->first_us) / 1e6));
        if (w->disarmed_after) say(&s, ", watched for the first %d", w->disarmed_after);
        say(&s, "\n");
    }
    say(&s, "  other traps (not passed to the game): %u", t->foreign_traps);
    if (t->foreign_traps) say(&s, ", the last at 0x%llx", (unsigned long long)t->last_foreign_rip);
    say(&s, "\n  signals passed to the game: %u", t->signals_passed);
    if (t->signals_passed) say(&s, ", the last %d", t->last_signal);
    say(&s, "\n  calls caught just as the breakpoints came out, put back: %u\n", t->rewound_at_end);
    say(&s, "  %s\n", rc < 0 ? "the game went away while watched"
                     : detached == 0 ? "detached cleanly: the game runs on as before"
                                     : "detaching didn't go cleanly");
    if (s.hits == 0 && rc == 0)
        say(&s, "Nothing was caught: the game didn't call any of the functions above while it was "
                "watched. That doesn't say yet how it shows its frames.\n");
    if (s.log) fclose(s.log);
    free(t);
    return 0;
}
