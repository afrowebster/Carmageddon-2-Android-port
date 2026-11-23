/* Android entry: extract game data from APK assets to internal storage, then run */
#include <SDL.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include "../runtime/runtime.h"

int rt_main(const char *game_dir, const char *save_dir);

#include <dirent.h>
static void rmtree(const char *path) {
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1100]; snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        struct stat st;
        if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode)) rmtree(p); else unlink(p);
    }
    closedir(d);
    rmdir(path);
}

static void mkdirs(const char *path) {
    char t[1024]; snprintf(t, sizeof t, "%s", path);
    for (char *p = t + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(t, 0755); *p = '/'; }
    mkdir(t, 0755);
}

/* copy an APK asset to internal storage unless an identical-size copy exists. The size comes from actually
   reading the asset (SDL_RWsize is unreliable for compressed assets), and the copy is fsync'ed before it is
   renamed into place so a crash can't leave a truncated file that looks up to date. */
static int copy_asset(const char *asset, const char *dst) {
    SDL_RWops *in = SDL_RWFromFile(asset, "rb");
    if (!in) return 0;
    size_t cap = 1 << 20, n = 0, k;
    unsigned char *buf = malloc(cap);
    while ((k = SDL_RWread(in, buf + n, 1, cap - n)) > 0) {
        n += k;
        if (n == cap) { cap *= 2; buf = realloc(buf, cap); }
    }
    SDL_RWclose(in);
    struct stat st;
    if (n > 0 && stat(dst, &st) == 0 && (size_t)st.st_size == n) { free(buf); return 1; }
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    FILE *f = fopen(tmp, "wb");
    if (!f) { free(buf); return 0; }
    int ok = fwrite(buf, 1, n, f) == n;
    fflush(f); fsync(fileno(f)); fclose(f);
    free(buf);
    if (!ok) return 0;
    rename(tmp, dst);
    LOG("installed %s (%zu bytes)", dst, n);
    return 1;
}

int main(int argc, char *argv[]) {
    plat_init();
    const char *base = SDL_AndroidGetInternalStoragePath();
    char game[1024], save[1024];
    /* NB: SDL_RWFromFile("game/...") looks in <internal storage>/game/ before the APK assets, so the
       extracted copy must not live there (it would shadow the assets and never get updated) */
    char old[1024]; snprintf(old, sizeof old, "%s/game", base);
    rmtree(old);
    snprintf(game, sizeof game, "%s/gamedata", base);
    snprintf(save, sizeof save, "%s/save", base);
    mkdirs(game); mkdirs(save);
    /* game/stamp.txt identifies the bundled data set: extract only when it differs from the installed one */
    char want[64] = "", have[64] = "", sp[1100];
    SDL_RWops *st = SDL_RWFromFile("game/stamp.txt", "rb");
    if (st) { size_t k = SDL_RWread(st, want, 1, sizeof want - 1); want[k] = 0; SDL_RWclose(st); }
    snprintf(sp, sizeof sp, "%s/stamp.txt", game);
    FILE *sf = fopen(sp, "rb");
    if (sf) { size_t k = fread(have, 1, sizeof have - 1, sf); have[k] = 0; fclose(sf); }
    if (want[0] && !strcmp(want, have)) return rt_main(game, save);
    unlink(sp);
    /* game/manifest.txt lists files relative to the game dir */
    SDL_RWops *m = SDL_RWFromFile("game/manifest.txt", "rb");
    if (!m) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Carmageddon II", "Game data missing from APK (assets/game).", NULL);
        return 1;
    }
    Sint64 n = SDL_RWsize(m);
    char *list = calloc(1, n + 1);
    SDL_RWread(m, list, 1, n);
    SDL_RWclose(m);
    for (char *line = strtok(list, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        char src[1100], dst[1100];
        snprintf(src, sizeof src, "game/%s", line);
        snprintf(dst, sizeof dst, "%s/%s", game, line);
        char *slash = strrchr(dst, '/');
        if (slash) { *slash = 0; mkdirs(dst); *slash = '/'; }
        if (!copy_asset(src, dst)) LOG("asset copy failed: %s", src);
    }
    free(list);
    sf = fopen(sp, "wb");
    if (sf) { fputs(want, sf); fclose(sf); }
    return rt_main(game, save);
}
