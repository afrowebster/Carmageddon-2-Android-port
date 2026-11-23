#define _GNU_SOURCE
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <strings.h>
#include <ctype.h>
#include <setjmp.h>
#include "win32.h"

uint32_t valloc_reserve(uint32_t size);
void valloc_release(uint32_t p);
int valloc_is_reserved(uint32_t a);

static uint32_t last_error;
#define SETERR(e) (c->last_error = (e))

/* ---------------- handles ---------------- */
enum { H_FREE, H_FILE, H_MAPPING, H_THREAD, H_HEAP, H_STD };
typedef struct {
    int type;
    FILE *f;
    char path[1024];
    int writable;
    char final_path[1024];   /* non-empty: written to path (a .part file) and renamed here on close */
    uint32_t map_size;
    int file_h;
    CPU *cpu;
    pthread_t th;
} Handle;
#define MAXH 256
static Handle handles[MAXH];
#define HBASE 0x1000u
static pthread_mutex_t hmx = PTHREAD_MUTEX_INITIALIZER;

static uint32_t h_alloc(int type) {
    pthread_mutex_lock(&hmx);
    for (int i = 1; i < MAXH; i++) if (handles[i].type == H_FREE) {
        memset(&handles[i], 0, sizeof handles[i]);
        handles[i].type = type;
        pthread_mutex_unlock(&hmx);
        return HBASE + i * 4;
    }
    pthread_mutex_unlock(&hmx);
    rt_fatal("out of handles");
}
static Handle *h_get(uint32_t h) {
    if (h < HBASE || h >= HBASE + MAXH * 4) return NULL;
    Handle *p = &handles[(h - HBASE) / 4];
    return p->type == H_FREE ? NULL : p;
}

/* ---------------- paths ---------------- */
/* case-insensitive resolve of relative path components under base */
static int ci_resolve(const char *base, const char *rel, char *out, size_t outsz) {
    char cur[1200];
    snprintf(cur, sizeof cur, "%s", base);
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", rel);
    char *save = NULL;
    for (char *comp = strtok_r(tmp, "/", &save); comp; comp = strtok_r(NULL, "/", &save)) {
        if (!strcmp(comp, ".")) continue;
        char cand[1400];
        snprintf(cand, sizeof cand, "%s/%s", cur, comp);
        struct stat st;
        if (stat(cand, &st) == 0) { snprintf(cur, sizeof cur, "%s", cand); continue; }
        DIR *d = opendir(cur);
        int found = 0;
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) if (!strcasecmp(e->d_name, comp)) {
                snprintf(cand, sizeof cand, "%s/%s", cur, e->d_name);
                found = 1; break;
            }
            closedir(d);
        }
        if (!found) {
            /* not existing: keep literal remainder */
            snprintf(cand, sizeof cand, "%s/%s", cur, comp);
            snprintf(cur, sizeof cur, "%s", cand);
            char *rest = strtok_r(NULL, "", &save);
            if (rest) { snprintf(cand, sizeof cand, "%s/%s", cur, rest); snprintf(cur, sizeof cur, "%s", cand); }
            snprintf(out, outsz, "%s", cur);
            return 0;
        }
        snprintf(cur, sizeof cur, "%s", cand);
    }
    snprintf(out, outsz, "%s", cur);
    return 1;
}

char g_cwd[600] = "C:\\CARMA2";   /* current directory (Windows path) */

static void win_to_rel(const char *wp, char *rel, size_t n) {
    char t[1300];
    if (!(isalpha((unsigned char)wp[0]) && wp[1] == ':') && wp[0] != '\\' && wp[0] != '/')
        snprintf(t, sizeof t, "%s\\%s", g_cwd, wp);
    else if (wp[0] == '\\' || wp[0] == '/') snprintf(t, sizeof t, "C:%s", wp);
    else snprintf(t, sizeof t, "%s", wp);
    for (char *p = t; *p; p++) if (*p == '\\') *p = '/';
    const char *s = t;
    if (isalpha((unsigned char)s[0]) && s[1] == ':') {
        s += 2;
        /* strip our fake install dir */
        if (!strncasecmp(s, "/CARMA2/", 8)) s += 8;
        else if (!strncasecmp(s, "/CARMA2", 7) && !s[7]) s += 7;
    }
    while (*s == '/') s++;
    /* collapse "x/../" and "./" */
    char out[1300]; size_t o = 0;
    const char *q = s;
    while (*q) {
        const char *e = strchr(q, '/'); size_t l = e ? (size_t)(e - q) : strlen(q);
        if (l == 1 && q[0] == '.') {}
        else if (l == 2 && q[0] == '.' && q[1] == '.') { while (o > 0 && out[o - 1] != '/') o--; if (o > 0) o--; }
        else if (l) { if (o) out[o++] = '/'; memcpy(out + o, q, l); o += l; }
        q += l; if (*q == '/') q++;
    }
    out[o] = 0;
    snprintf(rel, n, "%s", out);
}

int host_path_in(const char *base, const char *winpath, char *out, size_t outsz) {
    char rel[1024];
    win_to_rel(winpath, rel, sizeof rel);
    if (!strcmp(rel, ".")) rel[0] = 0;
    return ci_resolve(base, rel, out, outsz);
}

/* for reading: prefer save dir copy, then game dir. for create: save dir */
int host_path(const char *winpath, char *out, size_t outsz, int for_create) {
    char rel[1024];
    win_to_rel(winpath, rel, sizeof rel);
    char a[1200];
    if (ci_resolve(g_save_dir, rel, a, sizeof a) || for_create) {
        snprintf(out, outsz, "%s", a);
        if (for_create) {
            /* make sure parent dirs exist */
            char d[1200]; snprintf(d, sizeof d, "%s", a);
            for (char *p = d + strlen(g_save_dir) + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(d, 0755); *p = '/'; }
        }
        return 1;
    }
    return ci_resolve(g_game_dir, rel, out, outsz);
}

/* ---------------- file API ---------------- */
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define INVALID_HANDLE 0xffffffffu

API(CreateFileA) {
    const char *name = gstr(ARG(0));
    uint32_t access = ARG(1), disp = ARG(4);
    char path[1200];
    int wr = (access & GENERIC_WRITE) != 0;
    /* when writing a file that only exists in the game dir, copy-on-write to save dir */
    if (wr) {
        char rp[1200];
        int exists_save;
        char rel[1024]; win_to_rel(name, rel, sizeof rel);
        exists_save = ci_resolve(g_save_dir, rel, rp, sizeof rp);
        if (!exists_save && (disp == 3 || disp == 4) && host_path(name, rp, sizeof rp, 0)) {
            /* OPEN_EXISTING/OPEN_ALWAYS: copy original */
            FILE *src = fopen(rp, "rb");
            host_path(name, path, sizeof path, 1);
            if (src) {
                FILE *dst = fopen(path, "wb");
                char buf[65536]; size_t k;
                while (dst && (k = fread(buf, 1, sizeof buf, src)) > 0) fwrite(buf, 1, k, dst);
                if (dst) fclose(dst);
                fclose(src);
            }
        }
        host_path(name, path, sizeof path, 1);
    } else {
        host_path(name, path, sizeof path, 0);
    }
    struct stat st;
    int exists = stat(path, &st) == 0;
    const char *mode;
    switch (disp) {
    case 1: /* CREATE_NEW */ if (exists) { SETERR(80); return INVALID_HANDLE; } mode = "w+b"; break;
    case 2: /* CREATE_ALWAYS */ mode = "w+b"; break;
    case 3: /* OPEN_EXISTING */ if (!exists) { SETERR(2); TRACE("CreateFileA %s: not found", path); return INVALID_HANDLE; } mode = wr ? "r+b" : "rb"; break;
    case 4: /* OPEN_ALWAYS */ mode = exists ? (wr ? "r+b" : "rb") : "w+b"; break;
    case 5: /* TRUNCATE_EXISTING */ if (!exists) { SETERR(2); return INVALID_HANDLE; } mode = "w+b"; break;
    default: mode = "rb";
    }
    /* files created from scratch are written to a temporary name and renamed into place on close,
       so a crash or a killed app (common on Android) never leaves a truncated file behind */
    char final_path[1024] = "";
    if (mode[0] == 'w') {
        snprintf(final_path, sizeof final_path, "%s", path);
        snprintf(path + strlen(path), sizeof path - strlen(path), ".part");
    }
    FILE *f = fopen(path, mode);
    LOG("CreateFileA(%s, %x, disp=%u) -> %s %s", name, access, disp, final_path[0] ? final_path : path, f ? "ok" : "FAIL");
    if (!f) { SETERR(2); return INVALID_HANDLE; }
    uint32_t h = h_alloc(H_FILE);
    Handle *hp = h_get(h);
    hp->f = f; hp->writable = wr;
    snprintf(hp->final_path, sizeof hp->final_path, "%s", final_path);
    snprintf(hp->path, sizeof hp->path, "%s", path);
    SETERR(exists && (disp == 2 || disp == 4) ? 183 : 0);
    return h;
}

API(ReadFile) {
    Handle *h = h_get(ARG(0));
    uint32_t buf = ARG(1), n = ARG(2), pread = ARG(3);
    if (!h || h->type != H_FILE) { SETERR(6); return 0; }
    size_t k = fread(GPTR(buf), 1, n, h->f);
    if (pread) gw32(pread, (uint32_t)k);
    return 1;
}
API(WriteFile) {
    uint32_t hh = ARG(0), buf = ARG(1), n = ARG(2), pw = ARG(3);
    if (hh == 0xfffffff5u || hh == 0xfffffff4u) { /* stdout/stderr */
        fwrite(GPTR(buf), 1, n, stderr);
        if (pw) gw32(pw, n);
        return 1;
    }
    Handle *h = h_get(hh);
    if (!h || h->type != H_FILE) { SETERR(6); return 0; }
    size_t k = fwrite(GPTR(buf), 1, n, h->f);
    if (pw) gw32(pw, (uint32_t)k);
    return 1;
}
API(SetFilePointer) {
    Handle *h = h_get(ARG(0));
    int32_t dist = (int32_t)ARG(1);
    uint32_t hi = ARG(2), method = ARG(3);
    if (!h || h->type != H_FILE) { SETERR(6); return 0xffffffffu; }
    int64_t d = dist;
    if (hi) d = ((int64_t)(int32_t)gr32(hi) << 32) | (uint32_t)dist;
    fseeko(h->f, d, method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
    int64_t pos = ftello(h->f);
    if (hi) gw32(hi, (uint32_t)(pos >> 32));
    return (uint32_t)pos;
}
API(GetFileSize) {
    Handle *h = h_get(ARG(0));
    if (!h || h->type != H_FILE) { SETERR(6); return 0xffffffffu; }
    long cur = ftell(h->f);
    fseek(h->f, 0, SEEK_END);
    long n = ftell(h->f);
    fseek(h->f, cur, SEEK_SET);
    if (ARG(1)) gw32(ARG(1), 0);
    return (uint32_t)n;
}
API(SetEndOfFile) {
    Handle *h = h_get(ARG(0));
    if (!h || h->type != H_FILE) return 0;
    fflush(h->f);
    return ftruncate(fileno(h->f), ftell(h->f)) == 0;
}
API(FlushFileBuffers) { Handle *h = h_get(ARG(0)); if (h && h->f) fflush(h->f); return 1; }
API(DeleteFileA) {
    char p[1200];
    host_path(gstr(ARG(0)), p, sizeof p, 1);
    return unlink(p) == 0;
}
API(GetFileType) { uint32_t h = ARG(0); if (h_get(h)) return 1; if (h >= 0xfffffff4u) return 2; return 0; }
API(GetStdHandle) { return ARG(0); }
API(SetStdHandle) { return 1; }
API(SetHandleCount) { return ARG(0); }

/* BY_HANDLE_FILE_INFORMATION: the CRT's _fstat uses it to get a file's size */
API(GetFileInformationByHandle) {
    Handle *h = h_get(ARG(0));
    uint32_t p = ARG(1);
    memset(GPTR(p), 0, 52);
    if (!h || h->type != H_FILE) { SETERR(6); return 0; }
    fflush(h->f);
    struct stat st;
    if (fstat(fileno(h->f), &st)) return 0;
    uint64_t ft = ((uint64_t)st.st_mtime + 11644473600ull) * 10000000ull;
    gw32(p, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
    for (int i = 0; i < 3; i++) { gw32(p + 4 + 8 * i, (uint32_t)ft); gw32(p + 8 + 8 * i, (uint32_t)(ft >> 32)); }
    gw32(p + 32, (uint32_t)((uint64_t)st.st_size >> 32));
    gw32(p + 36, (uint32_t)st.st_size);
    gw32(p + 40, 1);
    gw32(p + 48, (uint32_t)st.st_ino);
    return 1;
}

API(CloseHandle) {
    Handle *h = h_get(ARG(0));
    if (!h) return 1;
    if (h->type == H_FILE && h->f) {
        if (h->writable || h->final_path[0]) { fflush(h->f); fsync(fileno(h->f)); }
        fclose(h->f);
        if (h->final_path[0]) rename(h->path, h->final_path);
    }
    if (h->type == H_THREAD) { /* detached */ }
    h->type = H_FREE;
    return 1;
}

/* file mapping: copy into guest memory */
typedef struct { uint32_t addr, size; uint32_t file_h; int writable; } MapView;
static MapView views[64];
API(CreateFileMappingA) {
    uint32_t fh = ARG(0), prot = ARG(2), szlo = ARG(4);
    Handle *f = h_get(fh);
    if (!f || f->type != H_FILE) { SETERR(6); return 0; }
    uint32_t h = h_alloc(H_MAPPING);
    Handle *m = h_get(h);
    m->file_h = fh;
    m->writable = (prot & 0x04) != 0; /* PAGE_READWRITE */
    long cur = ftell(f->f); fseek(f->f, 0, SEEK_END); long n = ftell(f->f); fseek(f->f, cur, SEEK_SET);
    m->map_size = szlo ? szlo : (uint32_t)n;
    return h;
}
API(MapViewOfFile) {
    Handle *m = h_get(ARG(0));
    uint32_t off = ARG(2), n = ARG(4);
    if (!m || m->type != H_MAPPING) { SETERR(6); return 0; }
    Handle *f = h_get(m->file_h);
    if (!n) n = m->map_size - off;
    uint32_t p = valloc_reserve(n);
    long cur = ftell(f->f);
    fseek(f->f, off, SEEK_SET);
    size_t k = fread(GPTR(p), 1, n, f->f); (void)k;
    fseek(f->f, cur, SEEK_SET);
    for (int i = 0; i < 64; i++) if (!views[i].addr) {
        views[i].addr = p; views[i].size = n; views[i].file_h = m->file_h; views[i].writable = m->writable;
        break;
    }
    return p;
}
API(UnmapViewOfFile) {
    uint32_t p = ARG(0);
    for (int i = 0; i < 64; i++) if (views[i].addr == p) {
        Handle *f = h_get(views[i].file_h);
        if (views[i].writable && f && f->f) { fseek(f->f, 0, SEEK_SET); fwrite(GPTR(p), 1, views[i].size, f->f); fflush(f->f); }
        valloc_release(p);
        views[i].addr = 0;
    }
    return 1;
}

/* ---------------- memory ---------------- */
API(HeapCreate) { return 0x00500000u + 0x10000u; /* fake heap handle */ }
API(HeapDestroy) { return 1; }
API(HeapAlloc) {
    uint32_t fl = ARG(1), n = ARG(2);
    uint32_t p = gmalloc(n);
    if (fl & 8) memset(GPTR(p), 0, n);
    return p;
}
API(HeapFree) { gfree(ARG(2)); return 1; }
API(HeapReAlloc) {
    uint32_t fl = ARG(1), p = ARG(2), n = ARG(3);
    uint32_t old = gmsize(p);
    if (fl & 0x10) { /* HEAP_REALLOC_IN_PLACE_ONLY */
        if (p && gr32(p - 16) >= n) { gw32(p - 12, n); return p; }
        return 0;
    }
    uint32_t q = grealloc(p, n);
    if ((fl & 8) && n > old) memset(GPTR(q + old), 0, n - old);
    return q;
}
API(VirtualAlloc) {
    uint32_t a = ARG(0), n = ARG(1), type = ARG(2);
    if (a) {
        if (valloc_is_reserved(a)) return a;    /* commit within reservation */
        LOG("VirtualAlloc at fixed %08x not reserved", a);
        return 0;
    }
    (void)type;
    return valloc_reserve(n);
}
API(VirtualFree) {
    uint32_t a = ARG(0), type = ARG(2);
    if (type & 0x8000) valloc_release(a); /* MEM_RELEASE */
    return 1;
}
API(GlobalMemoryStatus) {
    uint32_t p = ARG(0);
    gw32(p, 32); gw32(p + 4, 30);
    gw32(p + 8, 256u << 20); gw32(p + 12, 200u << 20);
    gw32(p + 16, 512u << 20); gw32(p + 20, 400u << 20);
    gw32(p + 24, 2047u << 20); gw32(p + 28, 1800u << 20);
    return 0;
}
API(GetSystemInfo) {
    uint32_t p = ARG(0);
    memset(GPTR(p), 0, 36);
    gw32(p + 4, 4096); gw32(p + 8, 0x10000); gw32(p + 12, 0x7ffeffff); gw32(p + 16, 1);
    gw32(p + 20, 1); gw32(p + 24, 586); gw32(p + 28, 0x10000); gw16(p + 32, 6); gw16(p + 34, 0x0803);
    return 0;
}
API(IsBadReadPtr) { uint32_t p = ARG(0); return p < 0x10000 || p >= THUNK_BASE; }
API(IsBadWritePtr) { uint32_t p = ARG(0); return p < 0x10000 || p >= THUNK_BASE; }
API(IsBadCodePtr) { uint32_t p = ARG(0); return p < 0x10000; }

/* ---------------- process / module ---------------- */
API(GetModuleHandleA) {
    const char *n = gstr(ARG(0));
    if (!n) return g_image_base;
    if (!strncasecmp(n, "kernel32", 8)) return 0x7ff00000u;
    if (!strncasecmp(n, "user32", 6)) return 0x7fe00000u;
    LOG("GetModuleHandleA(%s) -> 0", n);
    return 0;
}
typedef struct { uint32_t h; char name[64]; } Mod;
static Mod mods[16] = { {0x7ff00000u, "kernel32.dll"}, {0x7fe00000u, "user32.dll"} };
static int nmods = 2;
API(LoadLibraryA) {
    const char *n = gstr(ARG(0));
    char lower[64]; int i;
    for (i = 0; n[i] && i < 63; i++) lower[i] = tolower((unsigned char)n[i]);
    lower[i] = 0;
    if (!strstr(lower, ".dll")) strncat(lower, ".dll", sizeof lower - strlen(lower) - 1);
    for (i = 0; i < nmods; i++) if (!strcmp(mods[i].name, lower)) return mods[i].h;
    LOG("LoadLibraryA(%s)", n);
    if (strcmp(lower, "user32.dll") && strcmp(lower, "kernel32.dll") && strcmp(lower, "ddraw.dll") && strcmp(lower, "dsound.dll") && strcmp(lower, "glide2x.dll") && strcmp(lower, "dinput.dll")) {
        SETERR(126);
        return 0;
    }
    mods[nmods].h = 0x7f000000u + nmods * 0x10000u;
    snprintf(mods[nmods].name, 64, "%s", lower);
    return mods[nmods++].h;
}
API(GetProcAddress) {
    uint32_t h = ARG(0), np = ARG(1);
    const char *dll = NULL;
    for (int i = 0; i < nmods; i++) if (mods[i].h == h) dll = mods[i].name;
    if (!dll || np < 0x10000) { LOG("GetProcAddress(%x, %x) failed", h, np); return 0; }
    const char *name = gstr(np);
    /* functions that we don't want the game to use: return NULL */
    if (!strcmp(name, "GetMonitorInfoA") || !strcmp(name, "IsProcessorFeaturePresent")) {
        LOG("GetProcAddress(%s, %s) -> NULL (unsupported)", dll, name);
        return 0;
    }
    uint32_t r = rt_module_export(dll, name);
    if (!r) { extern uint32_t win32_find(const char *, const char *); r = win32_find(dll, name); }
    LOG("GetProcAddress(%s, %s) -> %08x", dll, name, r);
    return r;
}
API(GetModuleFileNameA) {
    gstrcpy(ARG(1), "C:\\CARMA2\\CARMA2_HW.EXE", ARG(2));
    return strlen("C:\\CARMA2\\CARMA2_HW.EXE");
}
API(GetCommandLineA) {
    static uint32_t p;
    if (!p) p = gstrdup("\"C:\\CARMA2\\CARMA2_HW.EXE\"");
    return p;
}
API(GetEnvironmentStrings) {
    static uint32_t p;
    if (!p) { p = gcalloc(64); memcpy(GPTR(p), "PATH=C:\\WINDOWS\0\0", 17); }
    return p;
}
API(GetEnvironmentStringsW) { return 0; }
API(FreeEnvironmentStringsA) { return 1; }
API(FreeEnvironmentStringsW) { return 1; }
API(SetEnvironmentVariableA) { return 1; }
API(GetStartupInfoA) {
    uint32_t p = ARG(0);
    memset(GPTR(p), 0, 68);
    gw32(p, 68);
    return 0;
}
API(GetVersion) { return 0x0A280105u; /* Windows 2000-ish build 2600 */ }
API(GetVersionExA) {
    uint32_t p = ARG(0);
    gw32(p + 4, 5); gw32(p + 8, 1); gw32(p + 12, 2600); gw32(p + 16, 2);
    gstrcpy(p + 20, "Service Pack 1", 128);
    return 1;
}
API(GetWindowsDirectoryA) { gstrcpy(ARG(0), "C:\\WINDOWS", ARG(1)); return 10; }
API(GetCurrentProcess) { return 0xffffffffu; }
API(TerminateProcess) { LOG("TerminateProcess(%u)", ARG(1)); exit(ARG(1)); }
API(ExitProcess) { LOG("ExitProcess(%u)", ARG(0)); fflush(NULL); exit(ARG(0)); }
API(GetLastError) { return c->last_error; }
API(SetUnhandledExceptionFilter) { return 0; }
API(UnhandledExceptionFilter) { return 1; }
API(RaiseException) { rt_fatal("RaiseException(code=%08x, flags=%x, nargs=%u)", ARG(0), ARG(1), ARG(2)); }
API(RtlUnwind) { rt_fatal("RtlUnwind called"); }

/* ---------------- time ---------------- */
API(GetTickCount) { return plat_ticks(); }
static void fill_systime(uint32_t p, struct tm *t, int ms) {
    gw16(p, t->tm_year + 1900); gw16(p + 2, t->tm_mon + 1); gw16(p + 4, t->tm_wday);
    gw16(p + 6, t->tm_mday); gw16(p + 8, t->tm_hour); gw16(p + 10, t->tm_min);
    gw16(p + 12, t->tm_sec); gw16(p + 14, ms);
}
static void now_tv(struct timeval *tv) {
    gettimeofday(tv, NULL);
    const char *o = getenv("C2_TIME_OFFSET");   /* hours, for testing */
    if (o) tv->tv_sec += (time_t)(atof(o) * 3600);
}
API(GetLocalTime) {
    struct timeval tv; now_tv(&tv);
    time_t s = tv.tv_sec; struct tm t; localtime_r(&s, &t);
    fill_systime(ARG(0), &t, tv.tv_usec / 1000); return 0;
}
API(GetSystemTime) {
    struct timeval tv; now_tv(&tv);
    time_t s = tv.tv_sec; struct tm t; gmtime_r(&s, &t);
    fill_systime(ARG(0), &t, tv.tv_usec / 1000); return 0;
}
API(GetTimeZoneInformation) { memset(GPTR(ARG(0)), 0, 172); return 0; }

/* ---------------- threads ---------------- */
typedef struct { CPU *c; uint32_t fn, param; } ThreadStart;
static void *thread_main(void *arg) {
    ThreadStart *ts = arg;
    CPU *c = ts->c;
    t_cpu = c;
    jmp_buf jb;
    c->exit_jmp = &jb;
    if (!setjmp(jb)) {
        LOG("thread %d start fn=%08x", c->thread_id, ts->fn);
        rt_invoke(c, ts->fn, 1, ts->param);
    }
    LOG("thread %d exit", c->thread_id);
    free(ts);
    return NULL;
}
void k32_check_kill(CPU *c) {
    if (c->kill && c->exit_jmp) longjmp(*(jmp_buf *)c->exit_jmp, 1);
}
API(CreateThread) {
    static int tid = 1;
    uint32_t stack = ARG(1) ? ARG(1) : 0x100000;
    if (stack < 0x40000) stack = 0x40000;
    CPU *nc = rt_new_cpu(stack);
    nc->thread_id = ++tid;
    ThreadStart *ts = malloc(sizeof *ts);
    ts->c = nc; ts->fn = ARG(2); ts->param = ARG(3);
    uint32_t h = h_alloc(H_THREAD);
    Handle *hp = h_get(h);
    hp->cpu = nc;
    if (ARG(5)) gw32(ARG(5), nc->thread_id);
    pthread_attr_t at; pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_create(&hp->th, &at, thread_main, ts);
    return h;
}
API(TerminateThread) {
    Handle *h = h_get(ARG(0));
    if (h && h->type == H_THREAD && h->cpu) { h->cpu->kill = 1; LOG("TerminateThread %d", h->cpu->thread_id); }
    return 1;
}
API(SetThreadPriority) { return 1; }
API(Sleep) {
    k32_check_kill(c);
    plat_sleep(ARG(0));
    k32_check_kill(c);
    return 0;
}

/* ---------------- resources ---------------- */
static uint32_t res_root(void) {
    uint32_t pe = g_image_base + gr32(g_image_base + 0x3c);
    uint32_t rva = gr32(pe + 24 + 96 + 2 * 8);
    return rva ? g_image_base + rva : 0;
}
static int res_name_eq(uint32_t root, uint32_t entry_name, int id, const char *name) {
    if (entry_name & 0x80000000u) {
        if (!name) return 0;
        uint32_t s = root + (entry_name & 0x7fffffff);
        uint32_t n = gr16(s);
        if (strlen(name) != n) return 0;
        for (uint32_t i = 0; i < n; i++) if (toupper(gr16(s + 2 + 2 * i)) != toupper((unsigned char)name[i])) return 0;
        return 1;
    }
    return !name && (int)entry_name == id;
}
static uint32_t res_lookup(uint32_t root, uint32_t dir, int id, const char *name) {
    uint32_t nnamed = gr16(dir + 12), nid = gr16(dir + 14);
    for (uint32_t i = 0; i < nnamed + nid; i++) {
        uint32_t e = dir + 16 + i * 8;
        if (id < 0 && !name) return gr32(e + 4); /* first */
        if (res_name_eq(root, gr32(e), id, name)) return gr32(e + 4);
    }
    return 0xffffffffu;
}
uint32_t res_find_host(int type_id, const char *type_name, int name_id, const char *name_name, uint32_t *size) {
    uint32_t root = res_root();
    if (!root) return 0;
    uint32_t t = res_lookup(root, root, type_id, type_name);
    if (t == 0xffffffffu || !(t & 0x80000000u)) return 0;
    uint32_t n = res_lookup(root, root + (t & 0x7fffffff), name_id, name_name);
    if (n == 0xffffffffu || !(n & 0x80000000u)) return 0;
    uint32_t l = res_lookup(root, root + (n & 0x7fffffff), -1, NULL);
    if (l == 0xffffffffu) return 0;
    uint32_t de = root + l;
    if (size) *size = gr32(de + 4);
    return de;
}
uint32_t res_find(uint32_t type, uint32_t name, uint32_t *size) {
    return res_find_host(type < 0x10000 ? (int)type : -2, type < 0x10000 ? NULL : gstr(type),
                         name < 0x10000 ? (int)name : -2, name < 0x10000 ? NULL : gstr(name), size);
}
API(FindResourceA) {
    uint32_t name = ARG(1), type = ARG(2);
    uint32_t r = res_find(type, name, NULL);
    LOG("FindResourceA(%s, %s) -> %x", name < 0x10000 ? "#" : gstr(name), type < 0x10000 ? "#" : gstr(type), r);
    return r;
}
API(LoadResource) { uint32_t de = ARG(1); return de ? g_image_base + gr32(de) : 0; }
API(LockResource) { return ARG(0); }
API(FreeResource) { return 1; }

/* ---------------- locale / strings ---------------- */
API(GetACP) { return 1252; }
API(GetOEMCP) { return 437; }
API(GetCPInfo) {
    uint32_t p = ARG(1);
    memset(GPTR(p), 0, 20);
    gw32(p, 1); gw8(p + 4, '?');
    return 1;
}
API(MultiByteToWideChar) {
    uint32_t src = ARG(2); int32_t n = (int32_t)ARG(3); uint32_t dst = ARG(4); int32_t cap = (int32_t)ARG(5);
    if (n < 0) n = strlen(gstr(src)) + 1;
    if (!cap) return n;
    int k = n < cap ? n : cap;
    for (int i = 0; i < k; i++) gw16(dst + 2 * i, gr8(src + i));
    return k;
}
API(WideCharToMultiByte) {
    uint32_t src = ARG(2); int32_t n = (int32_t)ARG(3); uint32_t dst = ARG(4); int32_t cap = (int32_t)ARG(5);
    if (n < 0) { n = 0; while (gr16(src + 2 * n)) n++; n++; }
    if (!cap) return n;
    int k = n < cap ? n : cap;
    for (int i = 0; i < k; i++) { uint16_t w = gr16(src + 2 * i); gw8(dst + i, w < 256 ? w : '?'); }
    return k;
}
static uint16_t ctype1(int ch) {
    uint16_t t = 0;
    if (isupper(ch)) t |= 1;
    if (islower(ch)) t |= 2;
    if (isdigit(ch)) t |= 4;
    if (isspace(ch)) t |= 8;
    if (ispunct(ch)) t |= 0x10;
    if (iscntrl(ch)) t |= 0x20;
    if (ch == ' ' || ch == '\t') t |= 0x40;
    if (isxdigit(ch)) t |= 0x80;
    if (isalpha(ch)) t |= 0x100;
    return t;
}
API(GetStringTypeA) {
    uint32_t src = ARG(2); int32_t n = (int32_t)ARG(3); uint32_t out = ARG(4);
    if (n < 0) n = strlen(gstr(src)) + 1;
    for (int i = 0; i < n; i++) gw16(out + 2 * i, ctype1(gr8(src + i) & 0x7f));
    return 1;
}
API(GetStringTypeW) {
    uint32_t src = ARG(1); int32_t n = (int32_t)ARG(2); uint32_t out = ARG(3);
    if (n < 0) { n = 0; while (gr16(src + 2 * n)) n++; n++; }
    for (int i = 0; i < n; i++) { uint16_t w = gr16(src + 2 * i); gw16(out + 2 * i, w < 128 ? ctype1(w) : 0); }
    return 1;
}
API(LCMapStringA) {
    uint32_t fl = ARG(1), src = ARG(2); int32_t n = (int32_t)ARG(3); uint32_t dst = ARG(4); int32_t cap = (int32_t)ARG(5);
    if (n < 0) n = strlen(gstr(src)) + 1;
    if (!cap) return n;
    int k = n < cap ? n : cap;
    for (int i = 0; i < k; i++) {
        int ch = gr8(src + i);
        if (fl & 0x100) ch = tolower(ch); else if (fl & 0x200) ch = toupper(ch);
        gw8(dst + i, ch);
    }
    return k;
}
API(LCMapStringW) {
    uint32_t fl = ARG(1), src = ARG(2); int32_t n = (int32_t)ARG(3); uint32_t dst = ARG(4); int32_t cap = (int32_t)ARG(5);
    if (n < 0) { n = 0; while (gr16(src + 2 * n)) n++; n++; }
    if (!cap) return n;
    int k = n < cap ? n : cap;
    for (int i = 0; i < k; i++) {
        int ch = gr16(src + 2 * i);
        if (ch < 128) { if (fl & 0x100) ch = tolower(ch); else if (fl & 0x200) ch = toupper(ch); }
        gw16(dst + 2 * i, ch);
    }
    return k;
}
API(CompareStringA) {
    uint32_t fl = ARG(1), s1 = ARG(2); int32_t n1 = (int32_t)ARG(3); uint32_t s2 = ARG(4); int32_t n2 = (int32_t)ARG(5);
    if (n1 < 0) n1 = strlen(gstr(s1));
    if (n2 < 0) n2 = strlen(gstr(s2));
    int n = n1 < n2 ? n1 : n2;
    int r = (fl & 1) ? strncasecmp(gstr(s1), gstr(s2), n) : strncmp(gstr(s1), gstr(s2), n);
    if (!r) r = n1 - n2;
    return r < 0 ? 1 : r > 0 ? 3 : 2;
}
API(CompareStringW) {
    uint32_t s1 = ARG(2); int32_t n1 = (int32_t)ARG(3); uint32_t s2 = ARG(4); int32_t n2 = (int32_t)ARG(5);
    if (n1 < 0) { n1 = 0; while (gr16(s1 + 2 * n1)) n1++; }
    if (n2 < 0) { n2 = 0; while (gr16(s2 + 2 * n2)) n2++; }
    for (int i = 0; i < n1 && i < n2; i++) {
        int a = gr16(s1 + 2 * i), b = gr16(s2 + 2 * i);
        if (a != b) return a < b ? 1 : 3;
    }
    return n1 < n2 ? 1 : n1 > n2 ? 3 : 2;
}

const ApiDef k32_apis[] = {
    DEF("kernel32.dll", CreateFileA, 7), DEF("kernel32.dll", ReadFile, 5), DEF("kernel32.dll", WriteFile, 5),
    DEF("kernel32.dll", SetFilePointer, 4), DEF("kernel32.dll", GetFileSize, 2), DEF("kernel32.dll", SetEndOfFile, 1),
    DEF("kernel32.dll", FlushFileBuffers, 1), DEF("kernel32.dll", DeleteFileA, 1), DEF("kernel32.dll", GetFileType, 1),
    DEF("kernel32.dll", GetStdHandle, 1), DEF("kernel32.dll", SetStdHandle, 2), DEF("kernel32.dll", SetHandleCount, 1),
    DEF("kernel32.dll", CloseHandle, 1), DEF("kernel32.dll", GetFileInformationByHandle, 2), DEF("kernel32.dll", CreateFileMappingA, 6), DEF("kernel32.dll", MapViewOfFile, 5),
    DEF("kernel32.dll", UnmapViewOfFile, 1), DEF("kernel32.dll", HeapCreate, 3), DEF("kernel32.dll", HeapDestroy, 1),
    DEF("kernel32.dll", HeapAlloc, 3), DEF("kernel32.dll", HeapFree, 3), DEF("kernel32.dll", HeapReAlloc, 4),
    DEF("kernel32.dll", VirtualAlloc, 4), DEF("kernel32.dll", VirtualFree, 3), DEF("kernel32.dll", GlobalMemoryStatus, 1),
    DEF("kernel32.dll", GetSystemInfo, 1), DEF("kernel32.dll", IsBadReadPtr, 2), DEF("kernel32.dll", IsBadWritePtr, 2),
    DEF("kernel32.dll", IsBadCodePtr, 1), DEF("kernel32.dll", GetModuleHandleA, 1), DEF("kernel32.dll", LoadLibraryA, 1),
    DEF("kernel32.dll", GetProcAddress, 2), DEF("kernel32.dll", GetModuleFileNameA, 3), DEF("kernel32.dll", GetCommandLineA, 0),
    DEF("kernel32.dll", GetEnvironmentStrings, 0), DEF("kernel32.dll", GetEnvironmentStringsW, 0),
    DEF("kernel32.dll", FreeEnvironmentStringsA, 1), DEF("kernel32.dll", FreeEnvironmentStringsW, 1),
    DEF("kernel32.dll", SetEnvironmentVariableA, 2), DEF("kernel32.dll", GetStartupInfoA, 1), DEF("kernel32.dll", GetVersion, 0),
    DEF("kernel32.dll", GetVersionExA, 1), DEF("kernel32.dll", GetWindowsDirectoryA, 2), DEF("kernel32.dll", GetCurrentProcess, 0),
    DEF("kernel32.dll", TerminateProcess, 2), DEF("kernel32.dll", ExitProcess, 1), DEF("kernel32.dll", GetLastError, 0),
    DEF("kernel32.dll", SetUnhandledExceptionFilter, 1), DEF("kernel32.dll", UnhandledExceptionFilter, 1),
    DEF("kernel32.dll", RaiseException, 4), DEF("kernel32.dll", RtlUnwind, 4), DEF("kernel32.dll", GetTickCount, 0),
    DEF("kernel32.dll", GetLocalTime, 1), DEF("kernel32.dll", GetSystemTime, 1), DEF("kernel32.dll", GetTimeZoneInformation, 1),
    DEF("kernel32.dll", CreateThread, 6), DEF("kernel32.dll", TerminateThread, 2), DEF("kernel32.dll", SetThreadPriority, 2),
    DEF("kernel32.dll", Sleep, 1), DEF("kernel32.dll", FindResourceA, 3), DEF("kernel32.dll", LoadResource, 2),
    DEF("kernel32.dll", LockResource, 1), DEF("kernel32.dll", FreeResource, 1), DEF("kernel32.dll", GetACP, 0),
    DEF("kernel32.dll", GetOEMCP, 0), DEF("kernel32.dll", GetCPInfo, 2), DEF("kernel32.dll", MultiByteToWideChar, 6),
    DEF("kernel32.dll", WideCharToMultiByte, 8), DEF("kernel32.dll", GetStringTypeA, 5), DEF("kernel32.dll", GetStringTypeW, 4),
    DEF("kernel32.dll", LCMapStringA, 6), DEF("kernel32.dll", LCMapStringW, 6), DEF("kernel32.dll", CompareStringA, 6),
    DEF("kernel32.dll", CompareStringW, 6),
    { 0 }
};
