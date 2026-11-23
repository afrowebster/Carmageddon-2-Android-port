/* persistent settings: key=value lines in <save dir>/carma2.cfg; C2_<KEY> env vars override */
#include <ctype.h>
#include "../runtime/runtime.h"

typedef struct { char k[32]; char v[64]; } KV;
static KV kv[32];
static int nkv, loaded;

static void load(void) {
    if (loaded) return;
    loaded = 1;
    char p[1200]; snprintf(p, sizeof p, "%s/carma2.cfg", g_save_dir);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char l[128];
    while (fgets(l, sizeof l, f) && nkv < 32) {
        char *eq = strchr(l, '=');
        if (!eq) continue;
        *eq = 0;
        char *v = eq + 1; v[strcspn(v, "\r\n")] = 0;
        snprintf(kv[nkv].k, 32, "%s", l); snprintf(kv[nkv].v, 64, "%s", v); nkv++;
    }
    fclose(f);
}
const char *setting_str(const char *key, const char *def) {
    char env[48]; snprintf(env, sizeof env, "C2_%s", key);
    for (char *p = env; *p; p++) *p = toupper((unsigned char)*p);
    if (getenv(env)) return getenv(env);
    load();
    for (int i = 0; i < nkv; i++) if (!strcmp(kv[i].k, key)) return kv[i].v;
    return def;
}
int setting_int(const char *key, int def) { const char *s = setting_str(key, NULL); return s ? atoi(s) : def; }
void setting_set(const char *key, const char *val) {
    load();
    int i;
    for (i = 0; i < nkv; i++) if (!strcmp(kv[i].k, key)) break;
    if (i == nkv) { if (nkv >= 32) return; nkv++; snprintf(kv[i].k, 32, "%s", key); }
    snprintf(kv[i].v, 64, "%s", val);
    char p[1200]; snprintf(p, sizeof p, "%s/carma2.cfg", g_save_dir);
    FILE *f = fopen(p, "w");
    if (!f) return;
    for (int j = 0; j < nkv; j++) fprintf(f, "%s=%s\n", kv[j].k, kv[j].v);
    fclose(f);
}
void setting_set_int(const char *key, int v) { char b[16]; snprintf(b, sizeof b, "%d", v); setting_set(key, b); }
