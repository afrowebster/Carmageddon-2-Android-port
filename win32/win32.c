#include <strings.h>
#include <pthread.h>
#include "win32.h"

static const ApiDef *tables[] = { k32_apis, user32_apis, gdi32_apis, misc_apis, dsound_apis, glide_apis, misc2_apis, dinput_apis, cdaudio_apis, smack_apis };

typedef struct { const ApiDef *d; uint32_t addr; } Resolved;
static Resolved res[512];
static int nres;

static uint32_t missing(CPU *c) { rt_fatal("missing API called"); }

/* 0 when not implemented (for GetProcAddress) */
uint32_t win32_find(const char *dll, const char *name) {
    for (unsigned t = 0; t < sizeof tables / sizeof *tables; t++)
        for (const ApiDef *d = tables[t]; d->name; d++)
            if (!strcasecmp(d->dll, dll) && !strcmp(d->name, name)) return win32_resolve(dll, name);
    return 0;
}

uint32_t win32_resolve(const char *dll, const char *name) {
    for (int i = 0; i < nres; i++)
        if (!strcasecmp(res[i].d->dll, dll) && !strcmp(res[i].d->name, name)) return res[i].addr;
    for (unsigned t = 0; t < sizeof tables / sizeof *tables; t++)
        for (const ApiDef *d = tables[t]; d->name; d++)
            if (!strcasecmp(d->dll, dll) && !strcmp(d->name, name)) {
                uint32_t a = rt_host(d->name, d->fn, d->argbytes);
                res[nres].d = d; res[nres].addr = a; nres++;
                return a;
            }
    char *n = malloc(strlen(dll) + strlen(name) + 4);
    sprintf(n, "%s!%s", dll, name);
    LOG("unresolved import %s", n);
    return rt_host(n, NULL, 0);
}

void win32_init(void) {}

/* ---------------- COM ---------------- */
static void *hostobjs[65536];
static int nhostobjs = 1;
static pthread_mutex_t cmx = PTHREAD_MUTEX_INITIALIZER;

uint32_t com_make_vtbl(const char *iface, const ComMethod *m, int n) {
    uint32_t v = gcalloc(n * 4);
    for (int i = 0; i < n; i++) {
        char *nm = malloc(strlen(iface) + strlen(m[i].name ? m[i].name : "?") + 3);
        sprintf(nm, "%s::%s", iface, m[i].name ? m[i].name : "?");
        gw32(v + i * 4, rt_host(nm, m[i].fn, m[i].nargs * 4));
    }
    return v;
}
uint32_t com_new(uint32_t vtbl, uint32_t extra, void *host) {
    uint32_t o = gcalloc(8 + extra);
    gw32(o, vtbl);
    pthread_mutex_lock(&cmx);
    int id = nhostobjs++;
    if (id >= 65536) rt_fatal("too many COM objects");
    hostobjs[id] = host;
    pthread_mutex_unlock(&cmx);
    gw32(o + 4, id);
    return o;
}
void *com_host(uint32_t obj) {
    uint32_t id = gr32(obj + 4);
    return id < 65536 ? hostobjs[id] : NULL;
}
