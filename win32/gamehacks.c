/* game-specific hooks (addresses listed in tools/hooks.txt) */
#include "win32.h"
void rt_commit(uint32_t lo, uint32_t hi);

#define BDD_BASE 0x10000000u

void rt_hook(CPU *c, uint32_t at) {
    switch (at) {
    case 0x53111f:
        /* BRender ImageLoad: the arena was just allocated and aligned into ebx.
           Place the driver image at its preferred base instead, where its code was lifted. */
        rt_commit(BDD_BASE, BDD_BASE + 0x100000);
        LOG("ImageLoad: arena %08x -> %08x", c->ebx, BDD_BASE);
        c->ebx = BDD_BASE;
        break;
    default:
        LOG("unknown hook %x", at);
    }
}

void game_init(void) {}
