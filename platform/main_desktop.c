#include "../runtime/runtime.h"
int rt_main(const char *game_dir, const char *save_dir);
extern char **g_argv;
int main(int argc, char **argv) {
    g_argv = argv;
    const char *gd = argc > 1 ? argv[1] : "game/app";
    const char *sd = argc > 2 ? argv[2] : "save";
    char cmd[1100]; snprintf(cmd, sizeof cmd, "mkdir -p '%s'", sd); if (system(cmd)) {}
    plat_init();
    return rt_main(gd, sd);
}
