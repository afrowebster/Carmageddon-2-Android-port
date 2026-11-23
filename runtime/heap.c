/* guest memory allocator */
#include <pthread.h>
#include "runtime.h"

#define VALLOC_LO 0xA0000000u
#define NCLASS 40

static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static uint32_t brk_heap = GUEST_HEAP_LO;
static uint32_t brk_valloc = VALLOC_LO;
static uint32_t freelist[NCLASS];
static uint32_t heap_committed = GUEST_HEAP_LO, valloc_committed = VALLOC_LO;
void rt_commit(uint32_t lo, uint32_t hi);
#define CHUNK 0x01000000u
static void grow_heap(void) {
    while (brk_heap > heap_committed) { rt_commit(heap_committed, heap_committed + CHUNK); heap_committed += CHUNK; }
}
static void grow_valloc(void) {
    while (brk_valloc > valloc_committed) { rt_commit(valloc_committed, valloc_committed + CHUNK); valloc_committed += CHUNK; }
}
/* block header (16 bytes before user ptr): [0]=class size (usable) [4]=requested size [8]=magic */
#define MAGIC 0x48454150u

static uint32_t class_size(int k) {
    /* 16,24,32,48,64,96,... */
    uint32_t base = 16u << (k / 2);
    return (k & 1) ? base + base / 2 : base;
}
static int class_of(uint32_t n) {
    for (int k = 0; k < NCLASS; k++) if (class_size(k) >= n) return k;
    return -1;
}

typedef struct { uint32_t addr, size; } Big;
static Big bigfree[4096];
static int nbigfree;

void heap_init(void) {}

uint32_t gmalloc(uint32_t n) {
    if (n == 0) n = 1;
    pthread_mutex_lock(&mx);
    int k = class_of(n);
    uint32_t p, cap;
    if (k >= 0 && k < 30) {
        cap = class_size(k);
        if (freelist[k]) {
            p = freelist[k];
            freelist[k] = gr32(p);
        } else {
            p = brk_heap + 16;
            brk_heap += cap + 16;
            if (brk_heap >= VALLOC_LO) rt_fatal("guest heap exhausted");
            grow_heap();
        }
    } else {
        cap = (n + 0xfff) & ~0xfffu;
        p = 0;
        for (int i = 0; i < nbigfree; i++) if (bigfree[i].size >= cap) {
            p = bigfree[i].addr; cap = bigfree[i].size;
            bigfree[i] = bigfree[--nbigfree];
            break;
        }
        if (!p) {
            p = brk_heap + 16;
            brk_heap += cap + 16;
            if (brk_heap >= VALLOC_LO) rt_fatal("guest heap exhausted");
            grow_heap();
        }
        k = 255;
    }
    gw32(p - 16, cap); gw32(p - 12, n); gw32(p - 8, MAGIC); gw32(p - 4, (uint32_t)k);
    pthread_mutex_unlock(&mx);
    return p;
}

uint32_t gcalloc(uint32_t n) {
    uint32_t p = gmalloc(n);
    memset(GPTR(p), 0, n);
    return p;
}

void gfree(uint32_t p) {
    if (!p) return;
    if (gr32(p - 8) != MAGIC) { LOG("gfree: bad pointer %08x", p); return; }
    pthread_mutex_lock(&mx);
    uint32_t k = gr32(p - 4);
    gw32(p - 8, 0);
    if (k < NCLASS) { gw32(p, freelist[k]); freelist[k] = p; }
    else if (nbigfree < 4096) { bigfree[nbigfree].addr = p; bigfree[nbigfree].size = gr32(p - 16); nbigfree++; }
    pthread_mutex_unlock(&mx);
}

uint32_t gmsize(uint32_t p) { return p ? gr32(p - 12) : 0; }

uint32_t grealloc(uint32_t p, uint32_t n) {
    if (!p) return gmalloc(n);
    uint32_t cap = gr32(p - 16);
    if (n <= cap) { gw32(p - 12, n); return p; }
    uint32_t q = gmalloc(n);
    memcpy(GPTR(q), GPTR(p), gr32(p - 12));
    gfree(p);
    return q;
}

uint32_t gstrdup(const char *s) {
    uint32_t n = strlen(s) + 1, p = gmalloc(n);
    memcpy(GPTR(p), s, n);
    return p;
}

/* VirtualAlloc-style region allocator (64K granular, zeroed) */
typedef struct { uint32_t addr, size; int used; } VReg;
static VReg vregs[4096];
static int nvregs;

uint32_t valloc_reserve(uint32_t size) {
    size = (size + 0xffff) & ~0xffffu;
    pthread_mutex_lock(&mx);
    uint32_t p = 0;
    for (int i = 0; i < nvregs; i++) if (!vregs[i].used && vregs[i].size == size) { vregs[i].used = 1; p = vregs[i].addr; break; }
    if (!p) {
        p = brk_valloc; brk_valloc += size;
        if (brk_valloc >= GUEST_HEAP_HI) rt_fatal("valloc exhausted");
        grow_valloc();
        if (nvregs < 4096) { vregs[nvregs].addr = p; vregs[nvregs].size = size; vregs[nvregs].used = 1; nvregs++; }
    }
    pthread_mutex_unlock(&mx);
    memset(GPTR(p), 0, size);
    return p;
}
int valloc_is_reserved(uint32_t a) {
    for (int i = 0; i < nvregs; i++) if (vregs[i].used && a >= vregs[i].addr && a < vregs[i].addr + vregs[i].size) return 1;
    return 0;
}
void valloc_release(uint32_t p) {
    pthread_mutex_lock(&mx);
    for (int i = 0; i < nvregs; i++) if (vregs[i].addr == p) vregs[i].used = 0;
    pthread_mutex_unlock(&mx);
}
uint32_t gheap_top(void) { return brk_heap; }
