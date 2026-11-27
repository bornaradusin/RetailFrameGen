/* RetailFrameGen, step 1 (0.4): where does the game keep these functions' addresses?
 *
 * Attaching to a retail game with ptrace broke it, so nothing here attaches to, stops or
 * changes the game. 0.3.0 showed the PS5's own debug memory read (mdbg_copyout) reads the
 * game's real code. This version also finds the game's import entries: when the game calls a
 * library function, it reads the function's address from a slot in its own writable memory.
 * It lists the game's memory regions (kern.proc.vmmap), searches its writable ones for the
 * functions' addresses, and logs each slot found and the region it's in. The next step points
 * such a slot at a small logger. Only reads: nothing is written to the game, and the SDK's
 * fallback through physical memory is never used.
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
#include <errno.h>

#include <ps5/kernel.h>
#include <ps5/mdbg.h>

#define DIR       "/data/retailframegen"
#define LOG_PATH  DIR "/log.txt"
#define PID_PATH  DIR "/pid.txt"
#define CODE_BYTES 16
#define CHUNK      (1u << 20)                 /* read at a time */
#define NEAR_GAME  0x40000000ull              /* regions this close above the game's base first */
#define FAR_LIMIT  (16ull << 20)              /* other regions searched only when this small */
#define TOTAL_LIMIT (768ull << 20)            /* searched at most */
#define MAX_REGIONS 4096
#define MAX_SLOTS  8                          /* slots logged per function */

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

typedef struct {
    uint64_t start, end;
    int prot, type;
    char path[64];
} Region;

static Region regions[MAX_REGIONS];
static int region_count;

/* The game's memory regions, from the kernel, without attaching. -1 when it says no. */
static int read_regions(pid_t pid)
{
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_VMMAP, pid };
    size_t size = 0;
    if (sysctl(mib, 4, NULL, &size, NULL, 0) < 0) return -1;
    size = size * 4 / 3 + 4096;
    char *buf = malloc(size);
    if (!buf) return -1;
    if (sysctl(mib, 4, buf, &size, NULL, 0) < 0) { free(buf); return -1; }
    region_count = 0;
    for (size_t at = 0; at + sizeof(int) <= size && region_count < MAX_REGIONS;) {
        const struct kinfo_vmentry *e = (const struct kinfo_vmentry *)(buf + at);
        if (e->kve_structsize <= 0) break;
        Region *r = &regions[region_count++];
        r->start = e->kve_start;
        r->end = e->kve_end;
        r->prot = e->kve_protection;
        r->type = e->kve_type;
        snprintf(r->path, sizeof(r->path), "%s", e->kve_path);
        at += (size_t)e->kve_structsize;
    }
    free(buf);
    return 0;
}

typedef struct {
    const char *name;
    uint64_t base;
} Module;

static Module modules[8];
static int module_count;

/* The module whose image starts nearest below address. */
static const char *module_of(uint64_t address)
{
    const Module *best = NULL;
    for (int i = 0; i < module_count; i++)
        if (modules[i].base && modules[i].base <= address && (!best || modules[i].base > best->base))
            best = &modules[i];
    return best ? best->name : "?";
}

typedef struct {
    intptr_t addr;
    int slots;
} Target;

static Target targets[WATCHED];
static uint64_t searched;
static uint8_t chunk[CHUNK];

/* Searches [start, end) for 8-byte values equal to a function's address. */
static void search(pid_t pid, const Region *region, uint64_t start, uint64_t end)
{
    for (uint64_t at = start; at < end && searched < TOTAL_LIMIT; at += CHUNK) {
        size_t len = end - at < CHUNK ? (size_t)(end - at) : CHUNK;
        if (mdbg_copyout(pid, (intptr_t)at, chunk, len) != 0) {
            say("  couldn't read 0x%llx-0x%llx\n", (unsigned long long)at,
                (unsigned long long)(at + len));
            continue;
        }
        searched += len;
        for (size_t off = 0; off + 8 <= len; off += 8) {
            uint64_t value;
            memcpy(&value, chunk + off, 8);
            if (value < 0x10000) continue;
            for (int i = 0; i < WATCHED; i++) {
                if (!targets[i].addr || value != (uint64_t)targets[i].addr) continue;
                if (++targets[i].slots <= MAX_SLOTS)
                    say("  slot for %s at 0x%llx (region 0x%llx-0x%llx %s%s%s \"%s\", near %s)\n",
                        watched[i].name, (unsigned long long)(at + off),
                        (unsigned long long)region->start, (unsigned long long)region->end,
                        region->prot & KVME_PROT_READ ? "r" : "-",
                        region->prot & KVME_PROT_WRITE ? "w" : "-",
                        region->prot & KVME_PROT_EXEC ? "x" : "-", region->path,
                        module_of(at + off));
            }
        }
    }
}

int main(void)
{
    mkdir(DIR, 0777);
    log_file = fopen(LOG_PATH, "w");
    say("RetailFrameGen step 1 (0.4.1): finding the game's import slots, without attaching to it\n");

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
        targets[i].addr = addr;
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
        say("Reading works: each function reads differently, as code.\n");
    else
        say("Each function reads differently, but not clearly as code: send this log.\n");

    /* The modules, to say which one a slot belongs to. */
    static const char *const module_names[] = {
        "eboot.bin", "libkernel.sprx", "libSceVideoOut.sprx", "libSceAgcDriver.sprx", "libSceAgc.sprx",
    };
    uint64_t game_base = 0;
    for (size_t i = 0; i < sizeof(module_names) / sizeof(module_names[0]); i++) {
        uint32_t h = 0;
        if (kernel_dynlib_handle(pid, module_names[i], &h) < 0) continue;
        uint64_t base = (uint64_t)kernel_dynlib_mapbase_addr(pid, h);
        modules[module_count].name = module_names[i];
        modules[module_count++].base = base;
        say("module %s at 0x%llx\n", module_names[i], (unsigned long long)base);
        if (i == 0) game_base = base;
    }
    if (!game_base) say("The game's own image (eboot.bin) wasn't found: only small regions are searched\n");

    say("\nThe game's memory regions:\n");
    if (read_regions(pid) < 0) {
        say("  the kernel didn't list them (errno %d)", errno);
        if (game_base) {
            say(": searching the 256 MB from the game's base instead\n");
            Region whole = { game_base, game_base + (256ull << 20), KVME_PROT_READ, 0, "(game base)" };
            search(pid, &whole, whole.start, whole.end);
        } else {
            say("\n");
        }
    } else {
        int writable = 0;
        for (int i = 0; i < region_count; i++) {
            const Region *r = &regions[i];
            if (r->prot & KVME_PROT_WRITE) writable++;
            say("  0x%llx-0x%llx %s%s%s type=%d %lluK \"%s\"\n", (unsigned long long)r->start,
                (unsigned long long)r->end, r->prot & KVME_PROT_READ ? "r" : "-",
                r->prot & KVME_PROT_WRITE ? "w" : "-", r->prot & KVME_PROT_EXEC ? "x" : "-",
                r->type, (unsigned long long)((r->end - r->start) >> 10), r->path);
        }
        say("%d regions, %d writable\n\nSearching the writable ones and the read-only import tables "
            "near the game's base, then small ones:\n", region_count, writable);
        /* Near the game's own image first: its import slots are there. Then other small ones.
         * Each image has a small read-only anonymous region (type 1) between its constants and its
         * data: its import table, filled in at start and then made read-only, so those are
         * searched too. Big regions (heaps, graphics memory) and device memory are left alone. */
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < region_count && searched < TOTAL_LIMIT; i++) {
                const Region *r = &regions[i];
                int near = game_base && r->start >= game_base && r->start < game_base + NEAR_GAME;
                int import_table = r->prot == KVME_PROT_READ && r->type == KVME_TYPE_DEFAULT;
                if (!((r->prot & KVME_PROT_WRITE) || import_table) || r->type == KVME_TYPE_DEVICE ||
                    r->type == KVME_TYPE_PHYS || r->type == KVME_TYPE_MGTDEVICE)
                    continue;
                if (pass == 0 ? !near : (near || r->end - r->start > FAR_LIMIT)) continue;
                search(pid, r, r->start, r->end);
            }
    }
    say("\nsearched %llu MB\n", (unsigned long long)(searched >> 20));
    int with_slots = 0;
    for (int i = 0; i < WATCHED; i++) {
        if (!targets[i].addr) continue;
        say("  %s: %d slot%s\n", watched[i].name, targets[i].slots, targets[i].slots == 1 ? "" : "s");
        if (targets[i].slots) with_slots++;
    }
    if (with_slots)
        say("Import slots found for %d functions: the next step can point one at a logger.\n", with_slots);
    else
        say("No slot holds any of these addresses in what was searched: send this log.\n");
    say("Nothing in the game was stopped or changed.\n");
    if (log_file) fclose(log_file);
    return 0;
}
