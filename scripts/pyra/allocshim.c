// allocshim - count heap allocations per thread, and name the call sites, without uprobes.
//
// Report 8 §8.31. The Pyra's kernel (5.6.19-m4) is built without CONFIG_UPROBE_EVENTS, so
// `perf probe -x libc malloc` cannot place a probe, and user-space unwinding does not work on this
// stack (§8.21). An LD_PRELOAD needs neither: it sees every allocation that crosses a PLT, and
// __builtin_return_address(0) - the link register, not an unwind - names the immediate caller.
//
// Counted: malloc, calloc, realloc, posix_memalign, aligned_alloc, memalign, strdup, strndup, and
// every operator new/delete variant (new is counted as new, not also as the malloc behind it).
// Not counted: allocations libc makes internally (fopen, qsort...), which do not cross the PLT.
//
// Interface, looked up with dlsym(RTLD_DEFAULT, ...) by the benchmark (benchmark builds only):
//   int  allocshim_read(uint64_t* out, int n)   the calling thread's counters, ALLOCSHIM_* order
//   void allocshim_sites_begin(void)            clear the site table; bin the calling thread's
//                                               allocations by return address from now on
//   void allocshim_sites_end(const char* path, double periods)
//                                               stop binning, write the table to path, largest
//                                               first, per period: count, bytes, dso, offset
//
// The offsets are relative to the DSO's load base; symbolize soh.elf ones on the host with
//   addr2line -f -C -e build-pyra/soh/soh.elf <offset>   (release: function names, no lines)
//
// Build: ./scripts/pyra/build.sh allocshim -> build-pyra/allocshim.so
// Use:   bench.sh ab ... LD_PRELOAD=/home/pyra/soh-bench2/allocshim.so SOH_BENCH_ALLOC_SITES=<dir>
//
// Costs a TLS increment per call, and a hash insert on the binned thread. Count with it; never
// time with it.

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void* __libc_malloc(size_t);
extern void* __libc_calloc(size_t, size_t);
extern void* __libc_realloc(void*, size_t);
extern void* __libc_memalign(size_t, size_t);
extern void __libc_free(void*);

enum {
    ALLOCSHIM_MALLOC,  // malloc, calloc, the memalign family, strdup/strndup
    ALLOCSHIM_REALLOC, // every realloc, whatever it did
    ALLOCSHIM_NEW,     // every operator new
    ALLOCSHIM_FREE,    // free of a non-null pointer
    ALLOCSHIM_DELETE,  // operator delete of a non-null pointer
    ALLOCSHIM_BYTES,   // bytes requested by all of the above that allocate
    ALLOCSHIM_COUNT
};

#define TLS __thread __attribute__((tls_model("initial-exec")))
static TLS uint64_t tCount[ALLOCSHIM_COUNT];
static TLS int tBinning; // this thread's allocations go in the site table
static TLS int tInShim;  // set while the shim itself calls out (dladdr, stdio)

#define SITES 8192 // power of two
static uintptr_t gSiteKey[SITES];
static uint64_t gSiteN[SITES];
static uint64_t gSiteBytes[SITES];
static uint64_t gSiteDropped;

// Only the binning thread writes the table, so no locking.
static inline void Site(uintptr_t ra, size_t bytes) {
    if (!tBinning || tInShim) {
        return;
    }
    uint32_t h = (uint32_t)(ra >> 1) * 2654435761u;
    for (int probe = 0; probe < 64; probe++) {
        uint32_t i = (h + (uint32_t)probe) & (SITES - 1);
        if (gSiteKey[i] == ra) {
            gSiteN[i]++;
            gSiteBytes[i] += bytes;
            return;
        }
        if (gSiteKey[i] == 0) {
            gSiteKey[i] = ra;
            gSiteN[i] = 1;
            gSiteBytes[i] = bytes;
            return;
        }
    }
    gSiteDropped++;
}

#define RA() ((uintptr_t)__builtin_return_address(0))

static inline void* CountAlloc(int kind, void* p, size_t bytes, uintptr_t ra) {
    tCount[kind]++;
    tCount[ALLOCSHIM_BYTES] += bytes;
    Site(ra, bytes);
    return p;
}

// --- libc --------------------------------------------------------------------------------------

void* malloc(size_t n) {
    return CountAlloc(ALLOCSHIM_MALLOC, __libc_malloc(n), n, RA());
}

void* calloc(size_t a, size_t b) {
    return CountAlloc(ALLOCSHIM_MALLOC, __libc_calloc(a, b), a * b, RA());
}

void* realloc(void* p, size_t n) {
    return CountAlloc(ALLOCSHIM_REALLOC, __libc_realloc(p, n), n, RA());
}

void free(void* p) {
    if (p != NULL) {
        tCount[ALLOCSHIM_FREE]++;
    }
    __libc_free(p);
}

void* memalign(size_t align, size_t n) {
    return CountAlloc(ALLOCSHIM_MALLOC, __libc_memalign(align, n), n, RA());
}

void* aligned_alloc(size_t align, size_t n) {
    return CountAlloc(ALLOCSHIM_MALLOC, __libc_memalign(align, n), n, RA());
}

int posix_memalign(void** out, size_t align, size_t n) {
    if (align < sizeof(void*) || (align & (align - 1)) != 0) {
        return EINVAL;
    }
    void* p = __libc_memalign(align, n);
    if (p == NULL && n != 0) {
        return ENOMEM;
    }
    *out = CountAlloc(ALLOCSHIM_MALLOC, p, n, RA());
    return 0;
}

char* strdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* p = __libc_malloc(n);
    if (p != NULL) {
        memcpy(p, s, n);
    }
    return CountAlloc(ALLOCSHIM_MALLOC, p, n, RA());
}

char* strndup(const char* s, size_t max) {
    size_t n = strnlen(s, max);
    char* p = __libc_malloc(n + 1);
    if (p != NULL) {
        memcpy(p, s, n);
        p[n] = '\0';
    }
    return CountAlloc(ALLOCSHIM_MALLOC, p, n + 1, RA());
}

// --- operator new / delete (Itanium names, 32-bit size_t) ------------------------------------
// A diagnostic, so an exhausted heap aborts instead of throwing: a C shim cannot throw.

static inline void* NewOrDie(size_t n, size_t align, uintptr_t ra) {
    void* p = align > 8 ? __libc_memalign(align, n ? n : 1) : __libc_malloc(n ? n : 1);
    if (p == NULL) {
        abort();
    }
    return CountAlloc(ALLOCSHIM_NEW, p, n, ra);
}

static inline void Delete(void* p) {
    if (p != NULL) {
        tCount[ALLOCSHIM_DELETE]++;
    }
    __libc_free(p);
}

void* _Znwj(size_t n) { return NewOrDie(n, 0, RA()); }
void* _Znaj(size_t n) { return NewOrDie(n, 0, RA()); }
void* _ZnwjRKSt9nothrow_t(size_t n, const void* t) { (void)t; return NewOrDie(n, 0, RA()); }
void* _ZnajRKSt9nothrow_t(size_t n, const void* t) { (void)t; return NewOrDie(n, 0, RA()); }
void* _ZnwjSt11align_val_t(size_t n, size_t a) { return NewOrDie(n, a, RA()); }
void* _ZnajSt11align_val_t(size_t n, size_t a) { return NewOrDie(n, a, RA()); }
void* _ZnwjSt11align_val_tRKSt9nothrow_t(size_t n, size_t a, const void* t) { (void)t; return NewOrDie(n, a, RA()); }
void* _ZnajSt11align_val_tRKSt9nothrow_t(size_t n, size_t a, const void* t) { (void)t; return NewOrDie(n, a, RA()); }

void _ZdlPv(void* p) { Delete(p); }
void _ZdaPv(void* p) { Delete(p); }
void _ZdlPvj(void* p, size_t n) { (void)n; Delete(p); }
void _ZdaPvj(void* p, size_t n) { (void)n; Delete(p); }
void _ZdlPvRKSt9nothrow_t(void* p, const void* t) { (void)t; Delete(p); }
void _ZdaPvRKSt9nothrow_t(void* p, const void* t) { (void)t; Delete(p); }
void _ZdlPvSt11align_val_t(void* p, size_t a) { (void)a; Delete(p); }
void _ZdaPvSt11align_val_t(void* p, size_t a) { (void)a; Delete(p); }
void _ZdlPvjSt11align_val_t(void* p, size_t n, size_t a) { (void)n; (void)a; Delete(p); }
void _ZdaPvjSt11align_val_t(void* p, size_t n, size_t a) { (void)n; (void)a; Delete(p); }

// --- interface ---------------------------------------------------------------------------------

int allocshim_read(uint64_t* out, int n) {
    int k = n < ALLOCSHIM_COUNT ? n : ALLOCSHIM_COUNT;
    for (int i = 0; i < k; i++) {
        out[i] = tCount[i];
    }
    return ALLOCSHIM_COUNT;
}

void allocshim_sites_begin(void) {
    memset(gSiteKey, 0, sizeof(gSiteKey));
    memset(gSiteN, 0, sizeof(gSiteN));
    memset(gSiteBytes, 0, sizeof(gSiteBytes));
    gSiteDropped = 0;
    tBinning = 1;
}

static int BySiteCount(const void* a, const void* b) {
    uint64_t na = gSiteN[*(const int*)a], nb = gSiteN[*(const int*)b];
    return na < nb ? 1 : na > nb ? -1 : 0;
}

void allocshim_sites_end(const char* path, double periods) {
    tBinning = 0;
    tInShim = 1;
    static int order[SITES];
    int used = 0;
    for (int i = 0; i < SITES; i++) {
        if (gSiteKey[i] != 0) {
            order[used++] = i;
        }
    }
    qsort(order, (size_t)used, sizeof(order[0]), BySiteCount);
    FILE* f = fopen(path, "w");
    if (f != NULL) {
        if (periods <= 0.0) {
            periods = 1.0;
        }
        fprintf(f, "# per period (%.0f periods): count bytes dso offset nearest-dynamic-symbol; "
                   "%d sites, %llu dropped\n",
                periods, used, (unsigned long long)gSiteDropped);
        for (int j = 0; j < used; j++) {
            int i = order[j];
            Dl_info info;
            const char* dso = "?";
            const char* sym = "";
            uintptr_t off = gSiteKey[i];
            if (dladdr((void*)gSiteKey[i], &info) != 0) {
                dso = info.dli_fname ? info.dli_fname : "?";
                off = gSiteKey[i] - (uintptr_t)info.dli_fbase;
                sym = info.dli_sname ? info.dli_sname : "";
            }
            fprintf(f, "%.2f %.0f %s 0x%lx %s\n", (double)gSiteN[i] / periods, (double)gSiteBytes[i] / periods,
                    dso, (unsigned long)off, sym);
        }
        fclose(f);
    }
    tInShim = 0;
}
