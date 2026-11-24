/* game-specific hooks (addresses listed in tools/hooks.txt) */
#include "win32.h"
void rt_commit(uint32_t lo, uint32_t hi);

#define BDD_BASE 0x10000000u

/* the race loop's per-frame HUD/timer update ran recently: we are in a race (not a menu or the
   post-race wrecks gallery, which also draw 3D cars) */
extern uint32_t plat_ticks(void);
static uint32_t race_frame_ms;
static int race_seen;
int game_racing(void) { return race_seen && plat_ticks() - race_frame_ms < 300; }

void rt_hook(CPU *c, uint32_t at) {
    switch (at) {
    case 0x4933c1:      /* race frame: the race loop (0x492950) updating the timer display */
        race_frame_ms = plat_ticks(); race_seen = 1;
        break;
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
