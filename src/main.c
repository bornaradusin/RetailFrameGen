/* RetailFrameGen, step 1 (0.3): can the game's memory be read, without touching the game?
 *
 * Attaching to a retail game with ptrace broke it (it went black), and ptrace's memory reads
 * didn't read the game's memory at all: every address read as the same bytes. So this version
 * doesn't attach, stop or change the game. It finds the functions a game can show its frames
 * with, as before, and reads each one's first bytes with the PS5's own debug memory read
 * (mdbg_copyout), to log whether that really reads the game's code. Only reads: nothing is
 * written to the game, and the SDK's fallback through physical memory is never used.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/user.h>

#include <ps5/kernel.h>
#include <ps5/mdbg.h>

#define DIR       "/data/retailframegen"
#define LOG_PATH  DIR "/log.txt"
#define PID_PATH  DIR "/pid.txt"
#define CODE_BYTES 16

typedef struct {
    const char *library;
    const char *name;
} Watched;

static const Watched watched[] = {
    { "libSceVideoOut.sprx", "sceVideoOutOpen" },
    { "libSceVideoOut.sprx", "sceVideoOutRegisterBuffers" },
    { "libSceVideoOut.sprx", "sceVideoOutRegisterBuffers2" },
    { "libSceVideoOut.sprx", "sceVideoOutSetFlipRate" },
    { "libSceVideoOut.sprx", "sceVideoOutSubmitChangeBufferAttribute" },
    { "libSceVideoOut.sprx", "sceVideoOutSubmitChangeBufferAttribute2" },
    { "libSceVideoOut.sprx", "sceVideoOutSubmitFlip" },
    { "libSceAgcDriver.sprx", "sceAgcDriverSubmitDcb" },
    { "libSceAgcDriver.sprx", "sceAgcDriverSubmitMultiDcbs" },
    { "libkernel.sprx", "sceKernelUsleep" },
    { "libkernel.sprx", "sceKernelGetProcessTime" },
    { "libkernel.sprx", "scePthreadMutexLock" },
};
#define WATCHED (int)(sizeof(watched) / sizeof(watched[0]))

static const char *const libraries[] = {
    "libSceVideoOut.sprx", "libSceAgc.sprx", "libSceAgcDriver.sprx", "libSceGnmDriver.sprx",
    "libSceGnmDriverForNeoMode.sprx", "libkernel.sprx",
};

static FILE *log_file;

static void say(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    if (log_file) { vfprintf(log_file, format, args); fflush(log_file); }
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

/* x86-64 function starts seen in PS5 libraries: push rbp, endbr64, a REX prefix, jmp. */
static int looks_like_code(const uint8_t *b)
{
    return b[0] == 0x55 || (b[0] == 0xf3 && b[1] == 0x0f && b[2] == 0x1e && b[3] == 0xfa) ||
           (b[0] & 0xf0) == 0x40 || b[0] == 0xe9 || b[0] == 0xeb || b[0] == 0xff;
}

int main(void)
{
    mkdir(DIR, 0777);
    log_file = fopen(LOG_PATH, "w");
    say("RetailFrameGen step 1 (0.3.0): reading the game's code, without attaching to it\n");

    say("Waiting up to 2 minutes for a game to start...\n");
    pid_t pid = -1;
    for (int tries = 0; tries < 1200 && (pid = find_game()) < 0; tries++) usleep(100000);
    if (pid < 0) {
        say("No game started: start a game, or put its PID in " PID_PATH "\n");
        return 1;
    }
    say("game pid=%d\n", (int)pid);

    uint32_t handle = 0;
    int loaded = -1;
    for (int tries = 0; tries < 2000 && (loaded = kernel_dynlib_handle(pid, "libSceVideoOut.sprx",
                                                                      &handle)) < 0; tries++)
        usleep(5000);
    if (loaded < 0) {
        say("The game has no libSceVideoOut loaded after 10 seconds\n");
        return 1;
    }
    for (size_t i = 0; i < sizeof(libraries) / sizeof(libraries[0]); i++) {
        uint32_t h = 0;
        say("library %s: %s\n", libraries[i],
            kernel_dynlib_handle(pid, libraries[i], &h) < 0 ? "not loaded" : "loaded");
    }

    uint8_t code[WATCHED][CODE_BYTES];
    int read_ok[WATCHED] = { 0 };
    int found = 0, read = 0, like_code = 0, distinct = 0;
    for (int i = 0; i < WATCHED; i++) {
        uint32_t h = 0;
        intptr_t addr = kernel_dynlib_handle(pid, watched[i].library, &h) < 0
                            ? 0 : kernel_dynlib_dlsym(pid, h, watched[i].name);
        if (!addr) {
            say("%s: not in the game\n", watched[i].name);
            continue;
        }
        found++;
        say("%s at 0x%llx:", watched[i].name, (unsigned long long)addr);
        if (mdbg_copyout(pid, addr, code[i], CODE_BYTES) != 0) {
            say(" unreadable\n");
            continue;
        }
        read_ok[i] = 1;
        read++;
        for (int b = 0; b < CODE_BYTES; b++) say(" %02x", code[i][b]);
        if (looks_like_code(code[i])) { like_code++; say("  (looks like a function's start)"); }
        say("\n");
        int seen = 0;
        for (int j = 0; j < i && !seen; j++)
            seen = read_ok[j] && !memcmp(code[i], code[j], CODE_BYTES);
        if (!seen) distinct++;
    }

    say("\n%d functions found, %d read, %d different, %d look like a function's start\n", found,
        read, distinct, like_code);
    if (read == 0)
        say("The game's code can't be read this way either.\n");
    else if (distinct < read)
        say("Some functions read as the same bytes: this may not be reading the game's code.\n");
    else if (like_code * 2 >= read)
        say("Reading works: each function reads differently, as code. The next step can put a "
            "logger inside the game.\n");
    else
        say("Each function reads differently, but not clearly as code: send this log.\n");
    say("Nothing in the game was stopped or changed.\n");
    if (log_file) fclose(log_file);
    return 0;
}
