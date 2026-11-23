/* The rest of the Win32 surface Carmageddon II and Smacker import:
   kernel32 extras (directories, INI files, time), user32/gdi32 stubs, winmm, wsock32 (no network),
   DirectInput (keyboard), IFORCE2 (no force feedback). */
#define _GNU_SOURCE
#include <sys/stat.h>
#include <dirent.h>
#include <fnmatch.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include "win32.h"

/* ---------------------------------------------------------------- kernel32 */
API(DisableThreadLibraryCalls) { return 1; }
API(GetCurrentProcessId) { return 0x1234; }
API(lstrcpyA) { strcpy((char *)GPTR(ARG(0)), gstr(ARG(1))); return ARG(0); }
API(GetProfileIntA) { return ARG(2); }
API(SetErrorMode) { return 0; }
API(LoadModule) { return 2; }
API(OutputDebugStringA) { LOG("ODS: %s", gstr(ARG(0))); return 0; }
API(FreeLibrary) { return 1; }
API(GetSystemDefaultLangID) { return 0x409; }
API(GetComputerNameA) { gstrcpy(ARG(0), "CARMA2", 7); if (ARG(1)) gw32(ARG(1), 6); return 1; }
extern char g_cwd[600];
API(GetCurrentDirectoryA) { gstrcpy(ARG(1), g_cwd, ARG(0)); return strlen(g_cwd); }
API(SetCurrentDirectoryA) {
    const char *d = gstr(ARG(0));
    char t[600];
    if (d[0] && d[1] == ':') snprintf(t, sizeof t, "%s", d);
    else if (d[0] == '\\') snprintf(t, sizeof t, "C:%s", d);
    else snprintf(t, sizeof t, "%s\\%s", g_cwd, d);
    size_t n = strlen(t); while (n > 3 && t[n - 1] == '\\') t[--n] = 0;
    snprintf(g_cwd, sizeof g_cwd, "%s", t);
    TRACE("SetCurrentDirectoryA(%s) -> %s", d, g_cwd);
    return 1;
}
API(GetSystemDirectoryA) { gstrcpy(ARG(0), "C:\\WINDOWS\\SYSTEM", ARG(1)); return 17; }
API(GetShortPathNameA) {
    const char *s = gstr(ARG(0)); uint32_t n = strlen(s);
    if (ARG(2) > n) gstrcpy(ARG(1), s, ARG(2));
    return n;
}
API(GetFullPathNameA) {
    const char *s = gstr(ARG(0));
    char b[600];
    if (s[0] && s[1] == ':') snprintf(b, sizeof b, "%s", s);
    else if (s[0] == '\\') snprintf(b, sizeof b, "C:%s", s);
    else snprintf(b, sizeof b, "C:\\CARMA2\\%s", s);
    uint32_t n = strlen(b);
    if (ARG(1) > n) gstrcpy(ARG(2), b, ARG(1));
    if (ARG(3)) { char *q = strrchr(b, '\\'); gw32(ARG(3), q ? ARG(2) + (uint32_t)(q - b) + 1 : 0); }
    return n;
}
API(GetDriveTypeA) { const char *s = gstr(ARG(0)); return (s && toupper((unsigned char)s[0]) == 'C') || !s ? 3 : 1; }
API(GetProcessHeap) { return 0x00510000u; }
API(PeekNamedPipe) { return 0; }
API(SetConsoleCtrlHandler) { return 1; }
API(FormatMessageA) { return 0; }
API(GlobalAlloc) { uint32_t p = gmalloc(ARG(1) ? ARG(1) : 1); if (ARG(0) & 0x40) memset(GPTR(p), 0, ARG(1)); return p; }
API(GlobalFree) { if (ARG(0)) gfree(ARG(0)); return 0; }
API(GlobalReAlloc) { return grealloc(ARG(0), ARG(1)); }
API(QueryPerformanceFrequency) { gw32(ARG(0), 1000000); gw32(ARG(0) + 4, 0); return 1; }
/* The game waits by polling the clock in a tight loop. When it is clearly spinning (hundreds of reads
   within the same millisecond) give the CPU a quarter millisecond back: on phones a pinned core
   means heat and throttling, and it competes with the GPU driver's threads. */
static void spin_relief(uint64_t us) {
    static int on = -1; if (on < 0) on = getenv("C2_SPINRELIEF") != NULL; if (!on) return;   /* off by default: it costs frame rate */
    static uint64_t last_ms; static uint32_t n;
    uint64_t ms = us / 1000;
    if (ms == last_ms) { if (++n >= 200) { n = 0; usleep(250); } }
    else { last_ms = ms; n = 0; }
}
extern uint64_t plat_time_us(void);
API(QueryPerformanceCounter) {
    uint64_t us = plat_time_us();
    spin_relief(us);
    gw32(ARG(0), (uint32_t)us); gw32(ARG(0) + 4, (uint32_t)(us >> 32));
    return 1;
}

static int stat_win(const char *wp, struct stat *st, char *hp, size_t n) {
    char p[1200];
    if (!host_path(wp, p, sizeof p, 0)) return -1;
    if (hp) snprintf(hp, n, "%s", p);
    return stat(p, st);
}
API(GetFileAttributesA) {
    struct stat st;
    if (stat_win(gstr(ARG(0)), &st, NULL, 0)) { c->last_error = 2; return 0xffffffffu; }
    return S_ISDIR(st.st_mode) ? 0x10 : 0x80;
}
API(SetFileAttributesA) { return 1; }
API(CreateDirectoryA) {
    char p[1200]; host_path(gstr(ARG(0)), p, sizeof p, 1);
    return mkdir(p, 0755) == 0 || errno == EEXIST;
}
API(MoveFileA) {
    char a[1200], b[1200];
    if (!host_path(gstr(ARG(0)), a, sizeof a, 0)) return 0;
    host_path(gstr(ARG(1)), b, sizeof b, 1);
    return rename(a, b) == 0;
}

/* FILETIME = 100ns since 1601 */
static void unix_to_ft(time_t t, uint32_t ft) {
    uint64_t v = ((uint64_t)t + 11644473600ull) * 10000000ull;
    gw32(ft, (uint32_t)v); gw32(ft + 4, (uint32_t)(v >> 32));
}
API(FileTimeToLocalFileTime) { gw32(ARG(1), gr32(ARG(0))); gw32(ARG(1) + 4, gr32(ARG(0) + 4)); return 1; }
API(FileTimeToSystemTime) {
    uint64_t v = gr32(ARG(0)) | ((uint64_t)gr32(ARG(0) + 4) << 32);
    time_t t = (time_t)(v / 10000000ull - 11644473600ull);
    struct tm tm; gmtime_r(&t, &tm);
    uint32_t s = ARG(1);
    gw16(s, tm.tm_year + 1900); gw16(s + 2, tm.tm_mon + 1); gw16(s + 4, tm.tm_wday); gw16(s + 6, tm.tm_mday);
    gw16(s + 8, tm.tm_hour); gw16(s + 10, tm.tm_min); gw16(s + 12, tm.tm_sec); gw16(s + 14, 0);
    return 1;
}
API(GetDateFormatA) {
    char b[64]; uint32_t st = ARG(2);
    if (st) snprintf(b, sizeof b, "%02d/%02d/%04d", gr16(st + 2), gr16(st + 6), gr16(st));
    else { time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); strftime(b, sizeof b, "%m/%d/%Y", &tm); }
    uint32_t n = strlen(b) + 1;
    if (ARG(5)) gstrcpy(ARG(4), b, ARG(5));
    return n;
}
API(GetTimeFormatA) {
    char b[64]; uint32_t st = ARG(2);
    if (st) snprintf(b, sizeof b, "%02d:%02d:%02d", gr16(st + 8), gr16(st + 10), gr16(st + 12));
    else { time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); strftime(b, sizeof b, "%H:%M:%S", &tm); }
    uint32_t n = strlen(b) + 1;
    if (ARG(5)) gstrcpy(ARG(4), b, ARG(5));
    return n;
}

/* FindFirstFile / FindNextFile: merges the save dir and the game dir */
typedef struct { int used; char names[2048][64]; uint32_t sizes[2048]; int dirs[2048]; int n, i; } Find;
static Find *finds[16];
static void fill_fd(Find *f, uint32_t fd) {
    memset(GPTR(fd), 0, 320);
    gw32(fd, f->dirs[f->i] ? 0x10 : 0x80);
    unix_to_ft(time(NULL), fd + 20);
    gw32(fd + 32, f->sizes[f->i]);
    gstrcpy(fd + 44, f->names[f->i], 260);
}
static void scan_dir(Find *f, const char *dir, const char *pat) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && f->n < 2048) {
        if (fnmatch(pat, e->d_name, FNM_CASEFOLD)) continue;
        int dup = 0;
        for (int k = 0; k < f->n; k++) if (!strcasecmp(f->names[k], e->d_name)) dup = 1;
        if (dup || strlen(e->d_name) >= 64) continue;
        char p[1500]; struct stat st;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (stat(p, &st)) continue;
        snprintf(f->names[f->n], 64, "%s", e->d_name);
        f->sizes[f->n] = (uint32_t)st.st_size; f->dirs[f->n] = S_ISDIR(st.st_mode);
        f->n++;
    }
    closedir(d);
}
int host_path_in(const char *base, const char *winpath, char *out, size_t outsz);
API(FindFirstFileA) {
    char w[600]; snprintf(w, sizeof w, "%s", gstr(ARG(0)));
    for (char *q = w; *q; q++) if (*q == '/') *q = '\\';
    char *sl = strrchr(w, '\\');
    char pat[260], dir[600];
    if (sl) { snprintf(pat, sizeof pat, "%s", sl + 1); *sl = 0; snprintf(dir, sizeof dir, "%s", w); }
    else { snprintf(pat, sizeof pat, "%s", w); snprintf(dir, sizeof dir, "."); }
    if (!strcmp(pat, "*.*")) strcpy(pat, "*");
    int slot = -1;
    for (int i = 0; i < 16; i++) if (!finds[i]) { slot = i; break; }
    if (slot < 0) return 0xffffffffu;
    Find *f = calloc(1, sizeof *f);
    char hp[1200];
    if (host_path_in(g_save_dir, dir, hp, sizeof hp)) scan_dir(f, hp, pat);
    if (host_path_in(g_game_dir, dir, hp, sizeof hp)) scan_dir(f, hp, pat);
    TRACE("FindFirstFileA(%s) -> %d", gstr(ARG(0)), f->n);
    if (!f->n) { free(f); c->last_error = 2; return 0xffffffffu; }
    finds[slot] = f;
    fill_fd(f, ARG(1));
    return 0x00600000u + slot;
}
API(FindNextFileA) {
    uint32_t h = ARG(0) - 0x00600000u;
    if (h >= 16 || !finds[h]) return 0;
    Find *f = finds[h];
    if (++f->i >= f->n) { c->last_error = 18; return 0; }
    fill_fd(f, ARG(1));
    return 1;
}
API(FindClose) {
    uint32_t h = ARG(0) - 0x00600000u;
    if (h < 16 && finds[h]) { free(finds[h]); finds[h] = NULL; }
    return 1;
}

/* INI files */
static int ini_read(const char *file, char **out) {
    char p[1200];
    const char *f = file ? file : "win.ini";
    if (!strchr(f, '\\') && !strchr(f, '/')) { char t[600]; snprintf(t, sizeof t, "C:\\WINDOWS\\%s", f); if (!host_path(t, p, sizeof p, 0)) return 0; }
    else if (!host_path(f, p, sizeof p, 0)) return 0;
    FILE *fp = fopen(p, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    *out = malloc(n + 1);
    n = fread(*out, 1, n, fp); (*out)[n] = 0;
    fclose(fp);
    return 1;
}
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}
API(GetPrivateProfileStringA) {
    const char *sec = gstr(ARG(0)), *key = gstr(ARG(1)), *def = gstr(ARG(2)), *file = gstr(ARG(5));
    uint32_t buf = ARG(3), sz = ARG(4);
    char *txt = NULL;
    const char *res = def ? def : "";
    char found[1024];
    if (sec && key && ini_read(file, &txt)) {
        int in = 0;
        for (char *l = strtok(txt, "\n"); l; l = strtok(NULL, "\n")) {
            char *t = trim(l);
            if (*t == '[') { char *e = strchr(t, ']'); if (e) *e = 0; in = !strcasecmp(t + 1, sec); continue; }
            char *eq = strchr(t, '=');
            if (in && eq) { *eq = 0; if (!strcasecmp(trim(t), key)) { snprintf(found, sizeof found, "%s", trim(eq + 1)); res = found; break; } }
        }
    }
    free(txt);
    TRACE("GetPrivateProfileString([%s] %s, %s) = %s", sec ? sec : "", key ? key : "", file ? file : "", res);
    gstrcpy(buf, res, sz);
    return strlen(res) < sz ? strlen(res) : sz - 1;
}
API(GetPrivateProfileSectionA) {
    const char *sec = gstr(ARG(0)), *file = gstr(ARG(3));
    uint32_t buf = ARG(1), sz = ARG(2), o = 0;
    char *txt = NULL;
    if (sz < 2) return 0;
    if (sec && ini_read(file, &txt)) {
        int in = 0;
        for (char *l = strtok(txt, "\n"); l; l = strtok(NULL, "\n")) {
            char *t = trim(l);
            if (*t == '[') { char *e = strchr(t, ']'); if (e) *e = 0; in = !strcasecmp(t + 1, sec); continue; }
            if (in && *t && *t != ';') {
                uint32_t n = strlen(t);
                if (o + n + 2 > sz) break;
                memcpy(GPTR(buf + o), t, n + 1); o += n + 1;
            }
        }
    }
    free(txt);
    gw8(buf + o, 0);
    return o;
}

/* ---------------------------------------------------------------- user32 / gdi32 */
API(ret0) { return 0; }
API(ret1) { return 1; }
API(SetRect) { uint32_t r = ARG(0); gw32(r, ARG(1)); gw32(r + 4, ARG(2)); gw32(r + 8, ARG(3)); gw32(r + 12, ARG(4)); return 1; }
API(GetForegroundWindow) { return user32_main_hwnd(); }
API(GetActiveWindow) { return user32_main_hwnd(); }
API(GetFocus) { return user32_main_hwnd(); }
API(FindWindowA) { return 0; }
API(IsWindow) { return ARG(0) != 0; }
API(BeginPaint) { memset(GPTR(ARG(1)), 0, 64); return 0x00030001u; }
API(GetWindowRect) {
    uint32_t r = ARG(1); gw32(r, 0); gw32(r + 4, 0); gw32(r + 8, 640); gw32(r + 12, 480); return 1;
}
API(SendMessageA) { return user32_send(c, ARG(0), ARG(1), ARG(2), ARG(3)); }
API(LoadStringA) { if (ARG(3)) gw8(ARG(2), 0); return 0; }
API(SystemParametersInfoA) { return 0; }
API(ShowCursor) { return ARG(0) ? 0 : -1; }
API(GetDeviceCaps) {
    switch (ARG(1)) {
    case 12: return 16;   /* BITSPIXEL */
    case 14: return 1;    /* PLANES */
    case 8: return 640; case 10: return 480;
    case 38: return 0;    /* RASTERCAPS: no palette */
    case 104: return 256; /* SIZEPALETTE */
    }
    return 0;
}
API(GetSystemPaletteEntries) {
    uint32_t n = ARG(2), p = ARG(3);
    if (p) for (uint32_t i = 0; i < n; i++) { uint32_t k = ARG(1) + i; gw32(p + i * 4, (k & 0xff) * 0x010101u); }
    return n;
}
API(CreatePalette) { return 0x00050001u; }
API(CreateCompatibleDC) { return 0x00030002u; }
API(SelectObject) { return 0x00040000u; }
API(GetSystemPaletteUse) { return 1; }

/* ---------------------------------------------------------------- winmm */
API(timeGetTime) { return plat_ticks(); }
API(waveOutOpen) { return 6; /* MMSYSERR_NODRIVER */ }
API(waveOutGetID) { return 6; }
API(timeSetEvent) { LOG("timeSetEvent(%u) unsupported", ARG(0)); return 0; }

/* ---------------------------------------------------------------- wsock32: no network */
API(WSAStartup) { return 10091; /* WSASYSNOTREADY */ }
API(WSAGetLastError) { return 10091; }
API(sock_fail) { return 0xffffffffu; }

/* ---------------------------------------------------------------- IFORCE2: no device */
API(IFLoadProjectFile) { return 0; }

const ApiDef misc2_apis[] = {
    DEF("kernel32.dll", DisableThreadLibraryCalls, 1), DEF("kernel32.dll", GetCurrentProcessId, 0),
    DEF("kernel32.dll", lstrcpyA, 2), DEF("kernel32.dll", GetProfileIntA, 3), DEF("kernel32.dll", SetErrorMode, 1),
    DEF("kernel32.dll", LoadModule, 2), DEF("kernel32.dll", OutputDebugStringA, 1), DEF("kernel32.dll", FreeLibrary, 1),
    DEF("kernel32.dll", GetSystemDefaultLangID, 0), DEF("kernel32.dll", GetComputerNameA, 2),
    DEF("kernel32.dll", GetCurrentDirectoryA, 2), DEF("kernel32.dll", SetCurrentDirectoryA, 1),
    DEF("kernel32.dll", GetSystemDirectoryA, 2), DEF("kernel32.dll", GetShortPathNameA, 3),
    DEF("kernel32.dll", GetFullPathNameA, 4), DEF("kernel32.dll", GetDriveTypeA, 1), DEF("kernel32.dll", GetProcessHeap, 0),
    DEF("kernel32.dll", PeekNamedPipe, 6), DEF("kernel32.dll", SetConsoleCtrlHandler, 2), DEF("kernel32.dll", FormatMessageA, 7),
    DEF("kernel32.dll", GlobalAlloc, 2), DEF("kernel32.dll", GlobalFree, 1), DEF("kernel32.dll", GlobalReAlloc, 3),
    DEF("kernel32.dll", QueryPerformanceFrequency, 1), DEF("kernel32.dll", QueryPerformanceCounter, 1),
    DEF("kernel32.dll", GetFileAttributesA, 1), DEF("kernel32.dll", SetFileAttributesA, 2),
    DEF("kernel32.dll", CreateDirectoryA, 2), DEF("kernel32.dll", MoveFileA, 2),
    DEF("kernel32.dll", FileTimeToLocalFileTime, 2), DEF("kernel32.dll", FileTimeToSystemTime, 2),
    DEF("kernel32.dll", GetDateFormatA, 6),
    DEF("kernel32.dll", GetTimeFormatA, 6), DEF("kernel32.dll", FindFirstFileA, 2), DEF("kernel32.dll", FindNextFileA, 2),
    DEF("kernel32.dll", FindClose, 1), DEF("kernel32.dll", GetPrivateProfileStringA, 6),
    DEF("kernel32.dll", GetPrivateProfileSectionA, 4),

    {"user32.dll", "ReleaseCapture", api_ret1, 0}, {"user32.dll", "SetCapture", api_ret0, 4},
    DEF("user32.dll", SetRect, 5), DEF("user32.dll", ShowCursor, 1), DEF("user32.dll", FindWindowA, 2),
    {"user32.dll", "IsIconic", api_ret0, 4}, {"user32.dll", "SetForegroundWindow", api_ret1, 4},
    DEF("user32.dll", SendMessageA, 4), {"user32.dll", "EndPaint", api_ret1, 8},
    {"user32.dll", "SetWindowPos", api_ret1, 28}, DEF("user32.dll", BeginPaint, 2),
    DEF("user32.dll", GetForegroundWindow, 0), DEF("user32.dll", GetActiveWindow, 0), DEF("user32.dll", GetFocus, 0),
    {"user32.dll", "GetWindowThreadProcessId", api_ret0, 8}, {"user32.dll", "GetTopWindow", api_ret0, 4},
    DEF("user32.dll", IsWindow, 1), DEF("user32.dll", LoadStringA, 4), {"user32.dll", "IsWindowVisible", api_ret1, 4},
    {"user32.dll", "IsChild", api_ret0, 8}, DEF("user32.dll", SystemParametersInfoA, 4), DEF("user32.dll", GetWindowRect, 2),
    {"user32.dll", "GetClassLongA", api_ret0, 8}, {"user32.dll", "SetWindowLongA", api_ret0, 12},
    {"user32.dll", "CallWindowProcA", api_ret0, 20}, {"user32.dll", "GetWindow", api_ret0, 8},
    {"user32.dll", "ScreenToClient", api_ret1, 8}, {"user32.dll", "InvalidateRect", api_ret1, 12},
    {"user32.dll", "MoveWindow", api_ret1, 24},

    {"gdi32.dll", "GetPixel", api_ret0, 12}, {"gdi32.dll", "SetPixel", api_ret0, 16},
    {"gdi32.dll", "CreateCompatibleBitmap", api_ret0, 12}, {"gdi32.dll", "GetDIBits", api_ret0, 28},
    {"gdi32.dll", "SetStretchBltMode", api_ret1, 8}, {"gdi32.dll", "StretchDIBits", api_ret0, 52},
    {"gdi32.dll", "BitBlt", api_ret1, 36}, {"gdi32.dll", "PatBlt", api_ret1, 24}, {"gdi32.dll", "DeleteDC", api_ret1, 4},
    DEF("gdi32.dll", GetDeviceCaps, 2), DEF("gdi32.dll", CreatePalette, 1), {"gdi32.dll", "SelectPalette", api_ret0, 12},
    {"gdi32.dll", "RealizePalette", api_ret0, 4}, {"gdi32.dll", "DeleteObject", api_ret1, 4},
    DEF("gdi32.dll", GetSystemPaletteUse, 1), DEF("gdi32.dll", GetSystemPaletteEntries, 4),
    {"gdi32.dll", "SetPaletteEntries", api_ret0, 16}, DEF("gdi32.dll", CreateCompatibleDC, 1),
    DEF("gdi32.dll", SelectObject, 2), {"gdi32.dll", "CreateFontA", api_ret0, 56},

    DEF("winmm.dll", timeGetTime, 0), DEF("winmm.dll", waveOutOpen, 6), DEF("winmm.dll", waveOutGetID, 2),
    DEF("winmm.dll", timeSetEvent, 5), {"winmm.dll", "timeKillEvent", api_ret0, 4},
    {"winmm.dll", "waveOutUnprepareHeader", api_ret0, 12}, {"winmm.dll", "waveOutSetVolume", api_ret0, 8},
    {"winmm.dll", "waveOutReset", api_ret0, 4}, {"winmm.dll", "waveOutClose", api_ret0, 4},
    {"winmm.dll", "waveOutPrepareHeader", api_ret0, 12}, {"winmm.dll", "waveOutWrite", api_ret0, 12},

    {"advapi32.dll", "RegCloseKey", api_ret0, 4},

    DEF("wsock32.dll", WSAStartup, 2), DEF("wsock32.dll", WSAGetLastError, 0), {"wsock32.dll", "WSACleanup", api_ret0, 0},
    {"wsock32.dll", "socket", api_sock_fail, 12}, {"wsock32.dll", "sendto", api_sock_fail, 24},
    {"wsock32.dll", "closesocket", api_ret0, 4}, {"wsock32.dll", "bind", api_sock_fail, 12},
    {"wsock32.dll", "getsockname", api_sock_fail, 12}, {"wsock32.dll", "recvfrom", api_sock_fail, 24},
    {"wsock32.dll", "ioctlsocket", api_sock_fail, 12}, {"wsock32.dll", "setsockopt", api_sock_fail, 20},

    {"iforce2.dll", "_IFLoadProjectFile@8", api_IFLoadProjectFile, 8},
    {"iforce2.dll", "_IFReleaseProject@4", api_ret0, 4}, {"iforce2.dll", "_IFCreateEffectStructs@12", api_ret0, 12},
    { 0 }
};
