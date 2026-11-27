/* RetailFrameGen, step 1: log how a running retail game shows its frames.
 *
 * Attaches to the game, puts breakpoints on libSceVideoOut's sceVideoOutRegisterBuffers,
 * sceVideoOutSubmitFlip and sceVideoOutSetFlipRate, and logs each call for 30 seconds to
 * /data/retailframegen/log.txt: the display buffers' size, pixel format and tiling, and when and
 * how the game flips. Nothing in the game is changed, and every breakpoint is taken out after.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "tracer.h"

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
#define MAX_FLIPS_LOGGED 600   /* every flip is counted; this many are written out */

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
    FILE *log;
    uint64_t start_us, last_flip_us;
    unsigned flips, flips_logged, registers, rates;
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

static int on_hit(Tracer *t, const Breakpoint *bp, const struct reg *r, void *context)
{
    Session *s = context;
    const uint64_t now = now_us();
    const double at = (now - s->start_us) / 1e6;

    if (strcmp(bp->name, "sceVideoOutRegisterBuffers") == 0) {
        /* (handle, startIndex, void *const *addresses, bufferNum, const attribute *) */
        BufferAttribute a;
        uint8_t raw[sizeof(a) + 16];
        s->registers++;
        say(s, "%8.3f register handle=%d start=%d count=%d", at, (int)r->r_rdi, (int)r->r_rsi,
            (int)r->r_rcx);
        if (tracer_read(t, (intptr_t)r->r_r8, &a, sizeof(a)) == 0) {
            say(s, " %ux%u pitch=%u format=0x%x tiling=%d aspect=%d option=0x%x\n", a.width,
                a.height, a.pitch_in_pixel, (unsigned)a.pixel_format, a.tiling_mode,
                a.aspect_ratio, a.option);
        } else {
            say(s, " (attribute unreadable)\n");
        }
        if (tracer_read(t, (intptr_t)r->r_r8, raw, sizeof(raw)) == 0) {
            say(s, "         attribute bytes:");
            for (size_t i = 0; i < sizeof(raw); i++) say(s, " %02x", raw[i]);
            say(s, "\n");
        }
        int count = (int)r->r_rcx;
        for (int i = 0; i < count && i < 16; i++) {
            uint64_t address = 0;
            if (tracer_read(t, (intptr_t)r->r_rdx + i * 8, &address, 8) == 0)
                say(s, "         buffer %d at 0x%016llx\n", (int)r->r_rsi + i,
                    (unsigned long long)address);
        }
    } else if (strcmp(bp->name, "sceVideoOutSubmitFlip") == 0) {
        /* (handle, bufferIndex, flipMode, flipArg) */
        s->flips++;
        if (s->flips_logged < MAX_FLIPS_LOGGED) {
            s->flips_logged++;
            say(s, "%8.3f flip handle=%d buffer=%d mode=%u arg=%lld since_last=%.2fms\n", at,
                (int)r->r_rdi, (int)r->r_rsi, (unsigned)r->r_rdx, (long long)r->r_rcx,
                s->last_flip_us ? (now - s->last_flip_us) / 1000.0 : 0.0);
        }
        s->last_flip_us = now;
    } else if (strcmp(bp->name, "sceVideoOutSetFlipRate") == 0) {
        /* (handle, rate): 0 = 60 Hz, 1 = 30 Hz, 2 = 20 Hz on the PS4 */
        s->rates++;
        say(s, "%8.3f flip rate handle=%d rate=%d\n", at, (int)r->r_rdi, (int)r->r_rsi);
    }
    return 0;
}

int main(void)
{
    Session s = { 0 };
    mkdir(DIR, 0777);
    s.log = fopen(LOG_PATH, "w");
    say(&s, "RetailFrameGen step 1: logging the frame hand-off\n");

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
        say(&s, "The game has no libSceVideoOut loaded\n");
        return 1;
    }

    static const char *const names[] = {
        "sceVideoOutRegisterBuffers", "sceVideoOutSubmitFlip", "sceVideoOutSetFlipRate",
    };
    intptr_t addrs[3];
    for (int i = 0; i < 3; i++) {
        addrs[i] = kernel_dynlib_dlsym(pid, handle, names[i]);
        say(&s, "%s at 0x%llx\n", names[i], (unsigned long long)addrs[i]);
    }

    Tracer t;
    if (tracer_attach(&t, pid) < 0) {
        say(&s, "Couldn't attach to the game\n");
        return 1;
    }
    for (int i = 0; i < 3; i++)
        if (addrs[i] && tracer_add(&t, addrs[i], names[i]) < 0)
            say(&s, "Couldn't put a breakpoint on %s\n", names[i]);

    s.start_us = now_us();
    int rc = tracer_run(&t, on_hit, &s, RUN_US);
    tracer_detach(&t);

    const double seconds = (now_us() - s.start_us) / 1e6;
    say(&s, "\ndone after %.1fs%s: %u flips (%.1f a second), %u buffer registrations, %u flip "
            "rate changes\n",
        seconds, rc < 0 ? " (the game went away)" : "", s.flips,
        seconds > 0 ? s.flips / seconds : 0.0, s.registers, s.rates);
    if (s.flips == 0)
        say(&s, "No sceVideoOutSubmitFlip calls: this game flips from the GPU's command buffer.\n");
    if (s.registers == 0)
        say(&s, "No buffer registrations: the game registered them before the payload attached.\n");
    if (s.log) fclose(s.log);
    return 0;
}
