/* core runtime: memory, loader, dispatch, fpu helpers */
#define _GNU_SOURCE
#include <stdarg.h>
#include <math.h>
#include <sys/mman.h>
#include <pthread.h>
#include <time.h>
#include <fenv.h>
#include <strings.h>
#include "runtime.h"
#include "lifted.h"

uint8_t *g_mem;
int g_trace;
uint32_t g_image_base, g_image_size;
char g_game_dir[1024] = ".";
char g_save_dir[1024] = ".";
__thread CPU *t_cpu;

static FILE *g_logf;
static pthread_mutex_t log_mx = PTHREAD_MUTEX_INITIALIZER;

#ifdef __ANDROID__
#include <android/log.h>
#endif

void rt_log(const char *fmt, ...) {
    va_list ap;
    pthread_mutex_lock(&log_mx);
    va_start(ap, fmt);
#ifdef __ANDROID__
    __android_log_vprint(ANDROID_LOG_INFO, "carma2", fmt, ap);
#else
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
#endif
    va_end(ap);
    if (g_logf) { va_start(ap, fmt); vfprintf(g_logf, fmt, ap); fputc('\n', g_logf); fflush(g_logf); va_end(ap); }
    pthread_mutex_unlock(&log_mx);
}

void rt_fatal(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rt_log("FATAL: %s", buf);
    CPU *c = t_cpu;
    if (c) {
        rt_log("  eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x",
               c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi);
        /* crude backtrace: scan stack for code addresses */
        uint32_t sp = c->esp;
        char line[512]; int n = 0;
        line[0] = 0;
        for (int i = 0; i < 256 && sp + i * 4 < GUEST_HEAP_HI; i++) {
            uint32_t v = gr32(sp + i * 4);
            if (rt_is_code(v) && n < 480) n += snprintf(line + n, sizeof line - n, " %x", v);
        }
        rt_log("  stack code refs:%s", line);
    }
    abort();
}

/* ---------------- dispatch ---------------- */
static GuestFn *g_pages[1u << 20];   /* two-level code map: page -> 4096 entries */
static void reg_fn(uint32_t a, GuestFn f) {
    GuestFn *pg = g_pages[a >> 12];
    if (!pg) pg = g_pages[a >> 12] = calloc(4096, sizeof(GuestFn));
    pg[a & 0xfff] = f;
}
static inline GuestFn lookup_fn(uint32_t a) {
    GuestFn *pg = g_pages[a >> 12];
    return pg ? pg[a & 0xfff] : NULL;
}
int rt_is_code(uint32_t a) { return a < THUNK_BASE && lookup_fn(a) != NULL; }

typedef struct { const char *name; HostFn fn; int argbytes; uint64_t ns; uint32_t calls; } HostEntry;
static HostEntry g_hosts[8192];
static int g_nhosts;
static pthread_mutex_t host_mx = PTHREAD_MUTEX_INITIALIZER;

uint32_t rt_host(const char *name, HostFn fn, int argbytes) {
    pthread_mutex_lock(&host_mx);
    int i = g_nhosts++;
    if (i >= 8192) rt_fatal("too many host thunks");
    g_hosts[i].name = name; g_hosts[i].fn = fn; g_hosts[i].argbytes = argbytes;
    pthread_mutex_unlock(&host_mx);
    return THUNK_BASE + (uint32_t)i * 4;
}

uint32_t gheap_top(void);
#include <signal.h>
#ifndef __ANDROID__
#include <execinfo.h>
/* debug: kill -USR1 <pid> logs the host stack (symbolize with addr2line -f -e carma2) */
static void usr1_bt(int sig) {
    void *bt[48]; int n = backtrace(bt, 48);
    char line[2048]; int o = 0;
    for (int i = 0; i < n && o < 2000; i++) o += snprintf(line + o, sizeof line - o, " %p", bt[i]);
    rt_log("USR1 stack:%s", line);
}
#endif
int g_prof_api;   /* C2_PROF: time every host API call */
uint32_t g_watch_addr;   /* debug: C2_WATCH=hexaddr logs value changes at API calls */
void rt_call(CPU *c, uint32_t t) {
    if (t < THUNK_BASE) {
        GuestFn f = lookup_fn(t);
        if (f) { f(c); return; }
        rt_fatal("call to unknown code address %08x", t);
    }
    {
        uint32_t i = (t - THUNK_BASE) / 4;
        if (i < (uint32_t)g_nhosts) {
            HostEntry *h = &g_hosts[i];
            TRACE("api %s", h->name);
            if (g_watch_addr && (g_watch_addr < GUEST_HEAP_LO || g_watch_addr + 4 <= gheap_top())) { static uint32_t last; uint32_t v = gr32(g_watch_addr);
                if (v != last) { LOG("watch %08x: %08x -> %08x before API %s (ret %08x)", g_watch_addr, last, v, h->name, gr32(c->esp)); last = v; } }
            if (!h->fn) rt_fatal("unimplemented API: %s", h->name);
            uint32_t r;
            if (g_prof_api) {
                struct timespec t0, t1;
                clock_gettime(CLOCK_MONOTONIC, &t0);
                r = h->fn(c);
                clock_gettime(CLOCK_MONOTONIC, &t1);
                h->ns += (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull + (t1.tv_nsec - t0.tv_nsec); h->calls++;
            } else r = h->fn(c);
            c->eax = r;
            c->esp += 4 + h->argbytes;
            return;
        }
    }
    rt_fatal("call to bad address %08x", t);
}

void rt_badjump(CPU *c, uint32_t at, uint32_t t) { rt_fatal("bad switch jump at %08x -> %08x", at, t); }
void rt_trap(CPU *c, uint32_t at) { rt_fatal("trap at %08x", at); }
void rt_unimpl(CPU *c, uint32_t at, const char *what) { rt_fatal("unimplemented instruction at %08x: %s", at, what); }
void rt_fallthrough(CPU *c, uint32_t at) { rt_fatal("fell off function end at %08x", at); }
void rt_divzero(CPU *c, uint32_t at) { rt_fatal("integer divide by zero at %08x", at); }

void rt_cpuid(CPU *c) {
    switch (c->eax) {
    case 0: c->eax = 1; c->ebx = 0x756e6547; c->edx = 0x49656e69; c->ecx = 0x6c65746e; break; /* GenuineIntel */
    case 1: c->eax = 0x0673; c->ebx = 0; c->ecx = 0; c->edx = 0x0080a979; break; /* P6, fpu tsc cx8 cmov; no mmx */
    default: c->eax = c->ebx = c->ecx = c->edx = 0;
    }
}

uint64_t rt_rdtsc(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    /* pretend 500 MHz */
    return (uint64_t)ts.tv_sec * 500000000ull + (uint64_t)ts.tv_nsec / 2;
}

uint32_t rt_invoke(CPU *c, uint32_t fn, int n, ...) {
    uint32_t args[16];
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    uint32_t sesp = c->esp, sebx = c->ebx, sesi = c->esi, sedi = c->edi, sebp = c->ebp;
    uint32_t sp = c->esp - 64; /* leave some room below caller's frame */
    sp &= ~3u;
    for (int i = n - 1; i >= 0; i--) { sp -= 4; gw32(sp, args[i]); }
    sp -= 4; gw32(sp, CB_RETURN);
    c->esp = sp;
    rt_call(c, fn);
    uint32_t r = c->eax;
    c->esp = sesp; c->ebx = sebx; c->esi = sesi; c->edi = sedi; c->ebp = sebp;
    return r;
}

/* ---------------- fpu helpers ---------------- */
void fpu_init(CPU *c) { c->fcw = 0x027f; c->fsw = 0; c->ftop = 0; for (int i = 0; i < 8; i++) c->fpr[i] = 0; }

void fpu_fxam(CPU *c) {
    double v = ST(0);
    c->fsw &= ~0x4700;
    if (signbit(v)) c->fsw |= 0x0200;
    switch (fpclassify(v)) {
    case FP_NAN: c->fsw |= 0x0100; break;
    case FP_INFINITE: c->fsw |= 0x0500; break;
    case FP_ZERO: c->fsw |= 0x4000; break;
    case FP_SUBNORMAL: c->fsw |= 0x4400; break;
    default: c->fsw |= 0x0400; break;
    }
}
void fpu_fxtract(CPU *c) {
    int e; double m = frexp(ST(0), &e);
    ST(0) = (double)(e - 1);
    FPUSH(m * 2.0);
}
double fpu_round(CPU *c, double v) {
    switch ((c->fcw >> 10) & 3) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}
uint32_t fpu_toint32(CPU *c, double v) {
    v = fpu_round(c, v);
    if (!(v >= -2147483648.0 && v <= 2147483647.0)) return 0x80000000u;
    return (uint32_t)(int32_t)v;
}
uint16_t fpu_toint16(CPU *c, double v) {
    v = fpu_round(c, v);
    if (!(v >= -32768.0 && v <= 32767.0)) return 0x8000;
    return (uint16_t)(int16_t)v;
}
uint64_t fpu_toint64(CPU *c, double v) {
    v = fpu_round(c, v);
    if (!(v >= -9223372036854775808.0 && v < 9223372036854775808.0)) return 0x8000000000000000ull;
    return (uint64_t)(int64_t)v;
}
double fpu_ld80(const uint8_t *p) {
    uint64_t m; uint16_t se;
    memcpy(&m, p, 8); memcpy(&se, p + 8, 2);
    int e = se & 0x7fff; int s = se >> 15;
    double r;
    if (e == 0 && m == 0) r = 0;
    else if (e == 0x7fff) r = (m << 1) ? NAN : INFINITY;
    else r = ldexp((double)m, e - 16383 - 63);
    return s ? -r : r;
}
void fpu_st80(uint8_t *p, double v) {
    uint64_t m = 0; uint16_t se = 0;
    if (signbit(v)) se = 0x8000, v = -v;
    if (v == 0) { }
    else if (isinf(v)) { se |= 0x7fff; m = 0x8000000000000000ull; }
    else if (isnan(v)) { se |= 0x7fff; m = 0xC000000000000000ull; }
    else { int e; double f = frexp(v, &e); m = (uint64_t)ldexp(f, 64); se |= (uint16_t)(e - 1 + 16383); }
    memcpy(p, &m, 8); memcpy(p + 8, &se, 2);
}
void fpu_fnstenv(CPU *c, uint32_t a) {
    memset(GPTR(a), 0, 28);
    gw16(a, c->fcw); gw16(a + 4, FSW()); gw16(a + 8, 0);
}
void fpu_fldenv(CPU *c, uint32_t a) { c->fcw = gr16(a); c->fsw = gr16(a + 4); c->ftop = (c->fsw >> 11) & 7; }
void fpu_fnsave(CPU *c, uint32_t a) {
    fpu_fnstenv(c, a);
    for (int i = 0; i < 8; i++) fpu_st80(g_mem + a + 28 + i * 10, ST(i));
    fpu_init(c);
}
void fpu_frstor(CPU *c, uint32_t a) {
    fpu_fldenv(c, a);
    for (int i = 0; i < 8; i++) ST(i) = fpu_ld80(g_mem + a + 28 + i * 10);
}

/* ---------------- cpu / loader ---------------- */
CPU *rt_new_cpu(uint32_t stack_size) {
    CPU *c = calloc(1, sizeof(CPU));
    fpu_init(c);
    c->fs_base = GUEST_TEB;
    if (stack_size) {
        uint32_t st = gmalloc(stack_size);
        c->esp = (st + stack_size - 16) & ~15u;
    }
    return c;
}

void rt_commit(uint32_t lo, uint32_t hi) {
    lo &= ~0xfffu; hi = (hi + 0xfff) & ~0xfffu;
    if (mprotect(g_mem + lo, hi - lo, PROT_READ | PROT_WRITE)) rt_fatal("commit %08x-%08x failed", lo, hi);
}
static void map_memory(void) {
    void *p = mmap(NULL, 0x100000000ull, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) rt_fatal("cannot reserve 4GB guest address space");
    g_mem = p;
    /* low area: TEB, stacks, image. Heaps are committed on demand (heap.c) */
    if (mprotect(g_mem + 0x10000, GUEST_HEAP_LO - 0x10000, PROT_READ | PROT_WRITE))
        rt_fatal("mprotect failed");
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n);
    if (fread(b, 1, n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f); *len = n; return b;
}

typedef struct { char name[64]; uint32_t base, size, entry; } GuestMod;
static GuestMod g_gmods[8];
static int g_ngmods;

void rt_register_funcs(const LiftedModule *m) {
    for (int i = 0; i < m->nfuncs; i++) reg_fn(m->funcs[i].addr, m->funcs[i].fn);
}

uint32_t rt_module_handle(const char *dll) {
    for (int i = 0; i < g_ngmods; i++) if (!strcasecmp(g_gmods[i].name, dll)) return g_gmods[i].base;
    return 0;
}

uint32_t rt_module_export(const char *dll, const char *name) {
    uint32_t base = rt_module_handle(dll);
    if (!base) return 0;
    uint32_t pe = base + gr32(base + 0x3c);
    uint32_t ed = gr32(pe + 0x78);
    if (!ed) return 0;
    ed += base;
    uint32_t nnames = gr32(ed + 24), funcs = base + gr32(ed + 28), names = base + gr32(ed + 32), ords = base + gr32(ed + 36);
    for (uint32_t i = 0; i < nnames; i++)
        if (!strcmp((const char *)GPTR(base + gr32(names + 4 * i)), name))
            return base + gr32(funcs + 4 * gr16(ords + 2 * i));
    return 0;
}

/* map a PE at 'base' (relocating if needed), register its lifted code and resolve imports */
static uint32_t load_pe(const char *path, const LiftedModule *m) {
    size_t len;
    uint8_t *b = read_file(path, &len);
    if (!b) rt_fatal("cannot read %s", path);
    uint32_t pe = *(uint32_t *)(b + 0x3c);
    uint16_t nsec = *(uint16_t *)(b + pe + 6);
    uint16_t optsz = *(uint16_t *)(b + pe + 20);
    uint8_t *opt = b + pe + 24;
    uint32_t pref = *(uint32_t *)(opt + 28);
    uint32_t size = *(uint32_t *)(opt + 56);
    uint32_t hdrsz = *(uint32_t *)(opt + 60);
    uint32_t base = m->base;
    rt_commit(base, base + size);
    memcpy(g_mem + base, b, hdrsz);
    uint8_t *sec = opt + optsz;
    for (int i = 0; i < nsec; i++, sec += 40) {
        uint32_t vsz = *(uint32_t *)(sec + 8), va = *(uint32_t *)(sec + 12);
        uint32_t rsz = *(uint32_t *)(sec + 16), roff = *(uint32_t *)(sec + 20);
        uint32_t n = rsz < vsz ? rsz : vsz;
        if (!vsz) n = rsz;
        if (roff + n > len) n = len - roff;
        memcpy(g_mem + base + va, b + roff, n);
    }
    free(b);
    if (base != pref) {
        uint32_t delta = base - pref;
        uint32_t rva = gr32(base + pe + 24 + 136), rsz = gr32(base + pe + 24 + 140);
        uint32_t p = base + rva, end = p + rsz;
        while (p < end) {
            uint32_t page = gr32(p), bs = gr32(p + 4);
            if (!bs) break;
            for (uint32_t k = 8; k < bs; k += 2) {
                uint16_t e = gr16(p + k);
                if ((e >> 12) == 3) { uint32_t a = base + page + (e & 0xfff); gw32(a, gr32(a) + delta); }
            }
            p += bs;
        }
        gw32(base + pe + 24 + 28, base);
    }
    rt_register_funcs(m);
    for (int i = 0; i < m->nimports; i++) {
        uint32_t t = rt_module_export(m->imports[i].dll, m->imports[i].name);
        if (!t) t = win32_resolve(m->imports[i].dll, m->imports[i].name);
        gw32(m->imports[i].iat, t);
    }
    GuestMod *g = &g_gmods[g_ngmods++];
    const char *bn = strrchr(path, '/'); bn = bn ? bn + 1 : path;
    snprintf(g->name, sizeof g->name, "%s", bn);
    g->base = base; g->size = size;
    uint32_t ep = *(uint32_t *)(g_mem + base + pe + 40);
    g->entry = ep ? base + ep : 0;
    LOG("loaded %s at %08x size %x", path, base, size);
    return base;
}

static void setup_teb(void) {
    /* TEB: exception list, stack base/limit, self, PEB */
    gw32(GUEST_TEB + 0x00, 0xffffffffu);
    gw32(GUEST_TEB + 0x04, GUEST_STACK_HI);
    gw32(GUEST_TEB + 0x08, GUEST_STACK_LO);
    gw32(GUEST_TEB + 0x18, GUEST_TEB);
    gw32(GUEST_TEB + 0x30, GUEST_PEB);
    gw32(GUEST_PEB + 0x08, g_image_base);
}

extern void heap_init(void);

extern void game_init(void);

/* ---------------- the recompiled game code (libgame) ---------------- */
static double c_trunc(double x) { return trunc(x); }
static const RtApi g_api_template = {
    C2_RTAPI_VERSION, 0, rt_call, rt_badjump, rt_trap, rt_unimpl, rt_fallthrough, rt_divzero, rt_cpuid, rt_hook, rt_rdtsc,
    fpu_fxam, fpu_fxtract, fpu_init, fpu_fnstenv, fpu_fldenv, fpu_fnsave, fpu_frstor, fpu_ld80, fpu_st80, fpu_round,
    fpu_toint32, fpu_toint16, fpu_toint64, memmove, memset,
    sin, cos, tan, atan2, ldexp, log2, fmod, exp2, remainder, c_trunc,
};
static RtApi g_api;

#ifdef C2_DYNGAME
#include <dlfcn.h>
/* Android: the game code is a separate library, built on the user's machine from their copy of the game */
static const LiftedModule *const *load_game(void) {
    void *h = dlopen("libgame.so", RTLD_NOW);
    if (!h) rt_fatal("cannot load libgame.so: %s", dlerror());
    C2GameInit init = (C2GameInit)dlsym(h, "c2_game_init");
    if (!init) rt_fatal("libgame.so has no c2_game_init");
    const LiftedModule *const *m = init(&g_api);
    if (!m) rt_fatal("libgame.so was built for a different runtime version");
    return m;
}
#else
static const LiftedModule *const *load_game(void) { return c2_game_init(&g_api); }
#endif

int rt_main(const char *game_dir, const char *save_dir) {
    snprintf(g_game_dir, sizeof g_game_dir, "%s", game_dir);
    snprintf(g_save_dir, sizeof g_save_dir, "%s", save_dir);
    char p[1200];
    snprintf(p, sizeof p, "%s/carma2_run.log", save_dir);
    g_logf = fopen(p, "w");
    if (getenv("C2_TRACE")) g_trace = 1;
    if (getenv("C2_PROF")) g_prof_api = 1;
#ifndef __ANDROID__
    signal(SIGUSR1, usr1_bt);
#endif
    if (getenv("C2_WATCH")) g_watch_addr = (uint32_t)strtoul(getenv("C2_WATCH"), NULL, 16);
    map_memory();
    heap_init();
    win32_init();
    setup_teb();
    CPU *c = rt_new_cpu(0);
    t_cpu = c;
    c->esp = GUEST_STACK_HI - 16;
    snprintf(p, sizeof p, "%s/CARMA2_HW.EXE", game_dir);
    g_api = g_api_template;
    g_api.mem = g_mem;
    const LiftedModule *const *mods = load_game();
    g_image_base = load_pe(p, mods[0]);
    g_image_size = g_gmods[g_ngmods - 1].size;
    gw32(GUEST_PEB + 0x08, g_image_base);
    /* Glide driver: loaded by BRender's own loader, pinned at its preferred base by a hook */
    rt_register_funcs(mods[1]);
    game_init();
    uint32_t entry = g_gmods[0].entry;
    c->esp = GUEST_STACK_HI - 16;
    gw32(c->esp, CB_RETURN);
    LOG("entering %08x", entry);
    rt_call(c, entry);
    LOG("entry returned eax=%x", c->eax);
    return (int)c->eax;
}

/* profiling: the host APIs that took the most time since the last call (and reset) */
void rt_prof_report(void) {
    int idx[5] = { -1, -1, -1, -1, -1 };
    for (int i = 0; i < g_nhosts; i++) {
        for (int k = 0; k < 5; k++) {
            if (idx[k] < 0 || g_hosts[i].ns > g_hosts[idx[k]].ns) {
                for (int m = 4; m > k; m--) idx[m] = idx[m - 1];
                idx[k] = i; break;
            }
        }
    }
    for (int k = 0; k < 5; k++) if (idx[k] >= 0 && g_hosts[idx[k]].ns > 1000000)
        rt_log("prof:   %s %u calls %.1f ms", g_hosts[idx[k]].name, g_hosts[idx[k]].calls, g_hosts[idx[k]].ns / 1e6);
    for (int i = 0; i < g_nhosts; i++) { g_hosts[i].ns = 0; g_hosts[i].calls = 0; }
}
void rt_prof_report_reset(void) { for (int i = 0; i < g_nhosts; i++) { g_hosts[i].ns = 0; g_hosts[i].calls = 0; } }
/* total time spent in host APIs since the last call (ms), and reset */
double rt_prof_api_ms(void) {
    uint64_t t = 0;
    for (int i = 0; i < g_nhosts; i++) { t += g_hosts[i].ns; g_hosts[i].ns = 0; g_hosts[i].calls = 0; }
    return t / 1e6;
}
