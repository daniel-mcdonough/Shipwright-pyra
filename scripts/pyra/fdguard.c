// fdguard - find out who is closing a file descriptor that does not belong to them.
//
// Built for report 7 section 7.7: spdlog's log file fails to flush with EBADF, and log records
// then appear *inside* a save file. That symptom pair has exactly one mechanism - something
// closes the log's descriptor without telling spdlog, the number is freed, the next fopen()
// (the save file) is handed the same number, and the logger's stale FILE* writes into it.
// Nothing in Shipwright or libultraship calls close() on Linux, so the caller is in a library.
//
// This shim is an LD_PRELOAD, deliberately: it needs no rebuild of the game, so it can be run
// against the exact binary that corrupted a save.
//
// It does two things:
//   1. Reports any close()/fclose()/dup2() that lands on a descriptor currently naming a
//      watched path, with a backtrace of the caller.
//   2. Remembers that descriptor number, and reports the next open() that is handed it, naming
//      the file that just inherited it. That second line is the corruption, caught in the act.
//
// Usage on the device:
//   ./scripts/pyra/build.sh fdguard        -> build-pyra/fdguard.so, armhf
//   (or on the device: gcc -O2 -funwind-tables -shared -fPIC -o fdguard.so fdguard.c -ldl -lpthread;
//    without -funwind-tables backtrace() prints nothing on ARM)
//   FDGUARD_MATCH='Ship of Harkinian.log' LD_PRELOAD=./fdguard.so ./soh 2>fdguard.txt
//
// FDGUARD_MATCH  substring matched against the target of /proc/self/fd/N (default: ".log")
// FDGUARD_ABORT  set to 1 to abort() on the first catch, for a core file
//
// A close that bypasses these wrappers (a raw syscall, or a libc-internal close) would leave no
// report, and silence would read as "nobody closed it". So a watcher thread also polls every
// descriptor that was opened on a watched path, and says so when one stops naming it with no
// wrapper having seen it go. Every line carries CLOCK_MONOTONIC and wall time, to line up with
// the game log.
//
// Deliberately not thread-safe beyond what the race allows: the marks are a plain array of
// sig_atomic_t. A missed report is acceptable, a crash in the diagnostic is not.

#define _GNU_SOURCE

// Debian's armhf toolchain turns on large-file offsets by default, which makes glibc's headers
// redirect open -> open64 and fopen -> fopen64. Under that redirection our open() and our open64()
// would both try to define the symbol open64, and the assembler refuses. This shim does no
// offset-bearing I/O of its own, so it simply opts out of the redirection and then defines both
// names explicitly - which is what interposing needs anyway, since the game's std::ofstream calls
// the 64 variants and a stray library close may call either.
#undef _FILE_OFFSET_BITS
#undef _TIME_BITS

#include <dlfcn.h>
#include <pthread.h>
#include <time.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FDGUARD_MAX_FD 4096

static int (*real_close)(int);
static int (*real_close_range)(unsigned int, unsigned int, int);
static void (*real_closefrom)(int);
static int (*real_dup2)(int, int);
static int (*real_open)(const char*, int, ...);
static int (*real_openat)(int, const char*, int, ...);
static int (*real_fclose)(FILE*);
static FILE* (*real_fopen)(const char*, const char*);
// armhf builds with _FILE_OFFSET_BITS=64, so libstdc++ - and therefore every std::ofstream in
// the game, including the one that writes the save - calls open64/fopen64, not open/fopen.
// Interposing only the unsuffixed names would catch the close and miss the theft.
static int (*real_open64)(const char*, int, ...);
static int (*real_openat64)(int, const char*, int, ...);
static FILE* (*real_fopen64)(const char*, const char*);

static const char* g_match = ".log";
static int g_abort = 0;
static volatile sig_atomic_t g_stolen[FDGUARD_MAX_FD];
// Descriptors opened on a watched path (1), and those a wrapper has already reported closing (2).
static volatile sig_atomic_t g_watched[FDGUARD_MAX_FD];

// Writes straight to fd 2. Nothing here may go through stdio: stdio is the thing under
// investigation, and a printf() here would be one more buffered write racing the bug.
static void note(const char* fmt, ...) {
    char buf[640];
    struct timespec mono, wall;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    clock_gettime(CLOCK_REALTIME, &wall);
    struct tm tm;
    localtime_r(&wall.tv_sec, &tm);
    int h = snprintf(buf, sizeof(buf), "[%02d:%02d:%02d.%03ld mono %ld.%03ld] ", tm.tm_hour, tm.tm_min, tm.tm_sec,
                     wall.tv_nsec / 1000000, (long)mono.tv_sec, mono.tv_nsec / 1000000);
    va_list ap;
    va_start(ap, fmt);
    int n = h + vsnprintf(buf + h, sizeof(buf) - h, fmt, ap);
    va_end(ap);
    if (n > 0) {
        ssize_t ignored = write(2, buf, (size_t)(n > (int)sizeof(buf) ? (int)sizeof(buf) : n));
        (void)ignored;
    }
}

static void backtrace_to_stderr(void) {
    void* frames[32];
    int n = backtrace(frames, 32);
    backtrace_symbols_fd(frames, n, 2);
}

// Returns 1 and fills out[] if fd currently names a path containing FDGUARD_MATCH.
static int fd_is_watched(int fd, char* out, size_t outlen) {
    if (fd < 0) {
        return 0;
    }
    char link[64];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, out, outlen - 1);
    if (n <= 0) {
        return 0;
    }
    out[n] = '\0';
    return strstr(out, g_match) != NULL;
}

static const char* readlink_or(int fd, char* out, size_t outlen) {
    char link[64];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, out, outlen - 1);
    if (n <= 0) {
        return "(closed)";
    }
    out[n] = '\0';
    return out;
}

static void mark_stolen(int fd) {
    if (fd >= 0 && fd < FDGUARD_MAX_FD) {
        g_stolen[fd] = 1;
        if (g_watched[fd]) {
            g_watched[fd] = 2;
        }
    }
}

// An open that lands on a watched path: start watching that number.
static void maybe_watch(int fd, const char* name) {
    if (fd >= 0 && fd < FDGUARD_MAX_FD && name != NULL && strstr(name, g_match) != NULL) {
        g_watched[fd] = 1;
        note("FDGUARD: fd %d opened on watched file %s\n", fd, name);
    }
}

// Polls the watched numbers. A descriptor that no longer names a watched path, with no wrapper
// having reported it, went by a path this shim does not interpose.
static void* watcher(void* arg) {
    (void)arg;
    for (;;) {
        struct timespec t = { 0, 2 * 1000 * 1000 };
        nanosleep(&t, NULL);
        for (int fd = 0; fd < FDGUARD_MAX_FD; fd++) {
            if (!g_watched[fd]) {
                continue;
            }
            char path[512];
            if (fd_is_watched(fd, path, sizeof(path))) {
                continue;
            }
            if (g_watched[fd] == 1) {
                note("FDGUARD: fd %d no longer names a watched file and no wrapper saw it close "
                     "(raw syscall, libc-internal close, or exec): now '%s'\n",
                     fd, readlink_or(fd, path, sizeof(path)));
                g_stolen[fd] = 1;
            }
            g_watched[fd] = 0;
        }
    }
    return NULL;
}

// The immediate caller, by library and offset. ARM's unwinder gives backtrace() nothing unless the
// shim is built with -funwind-tables, and even then stops at a stripped library; the return
// address of the wrapper never fails. Offsets are against the library's load base (addr2line-ready).
static void report_caller(void* ra) {
    Dl_info info;
    if (ra != NULL && dladdr(ra, &info) && info.dli_fname != NULL) {
        note("FDGUARD:   called from %s+0x%lx (%s)\n", info.dli_fname,
             (unsigned long)((char*)ra - (char*)info.dli_fbase), info.dli_sname ? info.dli_sname : "?");
    } else {
        note("FDGUARD:   called from %p (no dladdr)\n", ra);
    }
}

static void report_close(const char* how, int fd, const char* path) {
    note("FDGUARD: %s(%d) on watched file %s\n", how, fd, path);
    backtrace_to_stderr();
    mark_stolen(fd);
    if (g_abort) {
        abort();
    }
}

// The second half of the evidence: the descriptor number we just lost, handed to someone else.
static void report_reuse(int fd, const char* what, const char* name) {
    if (fd < 0 || fd >= FDGUARD_MAX_FD || !g_stolen[fd]) {
        return;
    }
    g_stolen[fd] = 0;
    note("FDGUARD: fd %d was the watched file, %s() just gave it to %s\n", fd, what,
         name ? name : "(unknown)");
    backtrace_to_stderr();
}

__attribute__((constructor)) static void fdguard_init(void) {
    real_close = dlsym(RTLD_NEXT, "close");
    real_close_range = dlsym(RTLD_NEXT, "close_range");
    real_closefrom = dlsym(RTLD_NEXT, "closefrom");
    real_dup2 = dlsym(RTLD_NEXT, "dup2");
    real_open = dlsym(RTLD_NEXT, "open");
    real_openat = dlsym(RTLD_NEXT, "openat");
    real_fclose = dlsym(RTLD_NEXT, "fclose");
    real_fopen = dlsym(RTLD_NEXT, "fopen");
    real_open64 = dlsym(RTLD_NEXT, "open64");
    real_openat64 = dlsym(RTLD_NEXT, "openat64");
    real_fopen64 = dlsym(RTLD_NEXT, "fopen64");

    const char* m = getenv("FDGUARD_MATCH");
    if (m != NULL && *m != '\0') {
        g_match = m;
    }
    const char* a = getenv("FDGUARD_ABORT");
    g_abort = (a != NULL && *a == '1');

    note("FDGUARD: watching descriptors whose path contains \"%s\"\n", g_match);
    pthread_t th;
    if (pthread_create(&th, NULL, watcher, NULL) == 0) {
        pthread_detach(th);
    }
}

int close(int fd) {
    char path[512];
    if (real_close != NULL && fd_is_watched(fd, path, sizeof(path))) {
        report_caller(__builtin_return_address(0));
        report_close("close", fd, path);
    }
    return real_close(fd);
}

int fclose(FILE* fp) {
    char path[512];
    // Via a void* because fclose declares fp nonnull and gcc warns on comparing it; a caller that
    // passes NULL is already broken, but this shim must not be the thing that crashes on it.
    const void* given = fp;
    if (given != NULL && real_fclose != NULL) {
        int fd = fileno(fp);
        if (fd_is_watched(fd, path, sizeof(path))) {
            report_caller(__builtin_return_address(0));
            report_close("fclose", fd, path);
        }
    }
    return real_fclose(fp);
}

// dup2 closes its target silently. That is the quietest way to lose a descriptor, so it is
// worth naming separately from close().
int dup2(int oldfd, int newfd) {
    char path[512];
    if (real_dup2 != NULL && oldfd != newfd && fd_is_watched(newfd, path, sizeof(path))) {
        report_caller(__builtin_return_address(0));
        report_close("dup2-over", newfd, path);
    }
    return real_dup2(oldfd, newfd);
}

int open(const char* pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, unsigned int);
        va_end(ap);
    }
    int fd = real_open(pathname, flags, mode);
    report_reuse(fd, "open", pathname);
    maybe_watch(fd, pathname);
    return fd;
}

int openat(int dirfd, const char* pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, unsigned int);
        va_end(ap);
    }
    int fd = real_openat(dirfd, pathname, flags, mode);
    report_reuse(fd, "openat", pathname);
    maybe_watch(fd, pathname);
    return fd;
}

FILE* fopen(const char* pathname, const char* mode) {
    FILE* fp = real_fopen(pathname, mode);
    if (fp != NULL) {
        report_reuse(fileno(fp), "fopen", pathname);
        maybe_watch(fileno(fp), pathname);
    }
    return fp;
}

int open64(const char* pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, unsigned int);
        va_end(ap);
    }
    int fd = real_open64(pathname, flags, mode);
    report_reuse(fd, "open64", pathname);
    maybe_watch(fd, pathname);
    return fd;
}

int openat64(int dirfd, const char* pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, unsigned int);
        va_end(ap);
    }
    int fd = real_openat64(dirfd, pathname, flags, mode);
    report_reuse(fd, "openat64", pathname);
    maybe_watch(fd, pathname);
    return fd;
}

FILE* fopen64(const char* pathname, const char* mode) {
    FILE* fp = real_fopen64(pathname, mode);
    if (fp != NULL) {
        report_reuse(fileno(fp), "fopen64", pathname);
        maybe_watch(fileno(fp), pathname);
    }
    return fp;
}

// glibc 2.34+: closes a whole range in one call, with no per-descriptor close() to interpose.
int close_range(unsigned int first, unsigned int last, int flags) {
    char path[512];
    for (unsigned int fd = first; fd <= last && fd < FDGUARD_MAX_FD; fd++) {
        if (fd_is_watched((int)fd, path, sizeof(path))) {
            note("FDGUARD: close_range(%u, %u, 0x%x) covers watched fd %u\n", first, last, flags, fd);
            report_close("close_range", (int)fd, path);
        }
    }
    return real_close_range != NULL ? real_close_range(first, last, flags) : -1;
}

void closefrom(int lowfd) {
    char path[512];
    for (int fd = lowfd < 0 ? 0 : lowfd; fd < FDGUARD_MAX_FD; fd++) {
        if (fd_is_watched(fd, path, sizeof(path))) {
            report_close("closefrom", fd, path);
        }
    }
    if (real_closefrom != NULL) {
        real_closefrom(lowfd);
    }
}
