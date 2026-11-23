/* Part of libgame (the recompiled game code): its entry point and the runtime table. */
#define C2_GEN 1
#include "lifted.h"

const RtApi *g_rt;
extern const LiftedModule g_mod_exe, g_mod_bdd;
static const LiftedModule *const modules[] = { &g_mod_exe, &g_mod_bdd, 0 };

__attribute__((visibility("default")))
const LiftedModule *const *c2_game_init(const RtApi *rt) {
    if (!rt || rt->version != C2_RTAPI_VERSION) return 0;
    g_rt = rt;
    return modules;
}

#ifdef C2_FREESTANDING
/* the compiler may emit calls to these on its own; libgame links against no C library */
#undef memmove
#undef memset
void *memcpy(void *d, const void *s, size_t n) { unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d; }
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *a = d; const unsigned char *b = s;
    if (a < b) while (n--) *a++ = *b++; else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
void *memset(void *d, int v, size_t n) { unsigned char *a = d; while (n--) *a++ = (unsigned char)v; return d; }
#endif
