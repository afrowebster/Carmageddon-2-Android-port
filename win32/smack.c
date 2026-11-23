/* smackw32.dll replacement: plays the videos recoded to C2V by tools/recode_video.py.
 * Every frame is decoded (the original could skip frames when playback fell behind, which
 * corrupts the following delta frames on slow devices). Timing follows the video's own audio
 * track when it has one. Only the calls Carmageddon II imports are implemented. */
#include <zlib.h>
#include "win32.h"
#include "../platform/plat.h"

/* guest SMACK struct (RAD Smacker 4.x) fields the game reads */
#define SMK_WIDTH 0x04
#define SMK_HEIGHT 0x08
#define SMK_FRAMES 0x0c
#define SMK_MSPERFRAME 0x10
#define SMK_NEWPALETTE 0x68
#define SMK_PALETTE 0x6c
#define SMK_PALTYPE 0x370
#define SMK_FRAMENUM 0x374
#define SMK_SIZE 0x400

typedef struct {
    uint32_t guest;
    uint8_t *file; size_t len;
    uint32_t w, h, frames, us, arate, asamples;
    uint32_t *offs;               /* frame record offsets */
    uint8_t *cur;                 /* current decoded frame */
    uint32_t frame;               /* index of the frame DoFrame shows next */
    int decoded_upto;             /* last frame decoded into cur (-1 none) */
    uint32_t dst, dst_x, dst_y, dst_pitch, dst_h;
    const int16_t *pcm;
    uint64_t apos;                /* audio position, 32.32 in source samples */
    int playing;
    uint32_t t0;                  /* clock origin (ms) */
} Video;

static Video *vids[8];
static Video *g_audio_vid;        /* video whose soundtrack is mixed */

static Video *vget(uint32_t g) {
    for (int i = 0; i < 8; i++) if (vids[i] && vids[i]->guest == g) return vids[i];
    return NULL;
}

int smack_audio_state(void) { Video *v = g_audio_vid; return v ? (v->playing ? 2 : 1) + (v->arate ? 10 : 0) : 0; }
/* called by the DirectSound mixer */
void smack_mix(int32_t *acc, int frames, int out_rate) {
    Video *v = g_audio_vid;
    if (!v || !v->playing || !v->arate) return;
    uint64_t step = ((uint64_t)v->arate << 32) / out_rate;
    for (int i = 0; i < frames; i++) {
        uint32_t s = (uint32_t)(v->apos >> 32);
        if (s >= v->asamples) return;
        int x = v->pcm[s];
        acc[2 * i] += x; acc[2 * i + 1] += x;
        v->apos += step;
    }
}

/* playback time in microseconds */
static uint64_t vclock(Video *v) {
    if (v->arate && v->playing && (v->apos >> 32) < v->asamples) return (v->apos >> 32) * 1000000ull / v->arate;
    return (uint64_t)(plat_ticks() - v->t0) * 1000ull;
}

static void decode_to(Video *v, uint32_t f) {
    while (v->decoded_upto < (int)f) {
        uint32_t n = v->decoded_upto + 1;
        const uint8_t *p = v->file + v->offs[n];
        uint32_t flags; memcpy(&flags, p, 4); p += 4;
        if (flags & 1) {
            memcpy(GPTR(v->guest + SMK_PALETTE), p, 768); p += 768;
            gw32(v->guest + SMK_NEWPALETTE, 1);
        }
        uint32_t zl; memcpy(&zl, p, 4); p += 4;
        static uint8_t *tmp; static size_t tmpn;
        size_t need = (size_t)v->w * v->h;
        if (tmpn < need) { tmp = realloc(tmp, need); tmpn = need; }
        uLongf out = need;
        if (uncompress(tmp, &out, p, zl) != Z_OK) LOG("smack: bad frame %u", n);
        for (size_t i = 0; i < need; i++) v->cur[i] ^= tmp[i];
        v->decoded_upto = n;
    }
}

API(SmackOpen) {
    const char *name = gstr(ARG(0));
    char w[600]; snprintf(w, sizeof w, "%s", name);
    char *dot = strrchr(w, '.');
    if (dot) strcpy(dot, ".c2v");
    char p[1200];
    if (!host_path(w, p, sizeof p, 0)) { LOG("SmackOpen(%s): no recoded video %s", name, w); return 0; }
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    Video *v = calloc(1, sizeof *v);
    v->file = malloc(n); v->len = n;
    if (fread(v->file, 1, n, f) != (size_t)n || memcmp(v->file, "C2V1", 4)) { fclose(f); free(v->file); free(v); return 0; }
    fclose(f);
    uint32_t hd[6]; memcpy(hd, v->file + 4, 24);
    v->w = hd[0]; v->h = hd[1]; v->frames = hd[2]; v->us = hd[3]; v->arate = hd[4]; v->asamples = hd[5];
    v->offs = malloc(v->frames * 4);
    size_t o = 28;
    for (uint32_t i = 0; i < v->frames; i++) {
        v->offs[i] = (uint32_t)o;
        uint32_t fl; memcpy(&fl, v->file + o, 4); o += 4 + ((fl & 1) ? 768 : 0);
        uint32_t zl; memcpy(&zl, v->file + o, 4); o += 4 + zl;
    }
    v->pcm = (const int16_t *)(v->file + o);
    if (o + (size_t)v->asamples * 2 > (size_t)n) v->asamples = (uint32_t)((n - o) / 2);
    v->cur = calloc(1, (size_t)v->w * v->h);
    v->decoded_upto = -1;
    v->guest = gcalloc(SMK_SIZE);
    gw32(v->guest, 0x32524d53);   /* 'SMK2' */
    gw32(v->guest + SMK_WIDTH, v->w); gw32(v->guest + SMK_HEIGHT, v->h); gw32(v->guest + SMK_FRAMES, v->frames);
    gw32(v->guest + SMK_MSPERFRAME, v->us / 1000);
    for (int i = 0; i < 8; i++) if (!vids[i]) { vids[i] = v; break; }
    LOG("SmackOpen(%s): %ux%u, %u frames, %u us/frame, audio %u Hz", name, v->w, v->h, v->frames, v->us, v->arate);
    return v->guest;
}
API(SmackClose) {
    Video *v = vget(ARG(0));
    if (!v) return 0;
    plat_audio_lock();
    if (g_audio_vid == v) g_audio_vid = NULL;
    plat_audio_unlock();
    for (int i = 0; i < 8; i++) if (vids[i] == v) vids[i] = NULL;
    free(v->file); free(v->offs); free(v->cur); free(v);
    gfree(ARG(0));
    return 0;
}
API(SmackToBuffer) {
    Video *v = vget(ARG(0));
    if (v) { v->dst_x = ARG(1); v->dst_y = ARG(2); v->dst_pitch = ARG(3); v->dst_h = ARG(4); v->dst = ARG(5); }
    return 0;
}
API(SmackDoFrame) {
    Video *v = vget(ARG(0));
    if (!v || v->frame >= v->frames) return 0;
    if (!v->playing) {
        v->playing = 1; v->t0 = plat_ticks();
        plat_audio_lock(); g_audio_vid = v; v->apos = 0; plat_audio_unlock();
    }
    gw32(v->guest + SMK_NEWPALETTE, 0);
    /* running late (slow device): catch up with the soundtrack. The frames in between are still
       decoded, so the picture stays correct; they are just not shown. */
    uint32_t due = (uint32_t)(vclock(v) / v->us);
    if (due > v->frame + 1) {
        uint32_t f = due < v->frames ? due : v->frames - 1;
        /* a palette change in a skipped frame must still reach the game */
        for (uint32_t k = v->frame; k < f; k++) {
            uint32_t fl; memcpy(&fl, v->file + v->offs[k + 1], 4);
            if (fl & 1) { f = k + 1; break; }
        }
        v->frame = f;
    }
    decode_to(v, v->frame);
    gw32(v->guest + SMK_FRAMENUM, v->frame);
    if (v->dst) {
        for (uint32_t y = 0; y < v->h && y < v->dst_h; y++)
            memcpy(GPTR(v->dst + (v->dst_y + y) * v->dst_pitch + v->dst_x), v->cur + (size_t)y * v->w, v->w);
    }
    return 0;
}
API(SmackNextFrame) {
    Video *v = vget(ARG(0));
    if (v && v->frame < v->frames) v->frame++;
    return 0;
}
/* nonzero while it is too early to show the next frame */
API(SmackWait) {
    Video *v = vget(ARG(0));
    if (!v || !v->playing) return 0;
    uint64_t due = (uint64_t)v->frame * v->us;
    if (vclock(v) >= due) return 0;
    plat_sleep(1);
    return 1;
}
API(SmackUseMMX) { return 0; }
API(SmackSoundUseDirectSound) { return 1; }
API(SmackBufferOpen) { return gcalloc(64); }

/* software-renderer path: 8-bit -> 16-bit blit with a palette */
API(SmackBlitOpen) { return gcalloc(16); }
static uint16_t blit_lut[256];
API(SmackBlitSetPalette) {
    const uint8_t *p = GPTR(ARG(1));
    for (int i = 0; i < 256; i++) blit_lut[i] = (uint16_t)(((p[3 * i] >> 3) << 11) | ((p[3 * i + 1] >> 2) << 5) | (p[3 * i + 2] >> 3));
    return 0;
}
API(SmackBlit) {
    uint32_t dst = ARG(1), dpitch = ARG(2), dx = ARG(3), dy = ARG(4), src = ARG(5), spitch = ARG(6);
    uint32_t sx = ARG(7), sy = ARG(8), w = ARG(9), h = ARG(10);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            gw16(dst + (dy + y) * dpitch + (dx + x) * 2, blit_lut[gr8(src + (sy + y) * spitch + sx + x)]);
    return 0;
}
API(SmackBlitClose) { gfree(ARG(0)); return 0; }

const ApiDef smack_apis[] = {
    {"smackw32.dll", "_SmackOpen@12", api_SmackOpen, 12}, {"smackw32.dll", "_SmackClose@4", api_SmackClose, 4},
    {"smackw32.dll", "_SmackToBuffer@28", api_SmackToBuffer, 28}, {"smackw32.dll", "_SmackDoFrame@4", api_SmackDoFrame, 4},
    {"smackw32.dll", "_SmackNextFrame@4", api_SmackNextFrame, 4}, {"smackw32.dll", "_SmackWait@4", api_SmackWait, 4},
    {"smackw32.dll", "_SmackUseMMX@4", api_SmackUseMMX, 4}, {"smackw32.dll", "_SmackSoundUseDirectSound@4", api_SmackSoundUseDirectSound, 4},
    {"smackw32.dll", "_SmackBufferOpen@24", api_SmackBufferOpen, 24}, {"smackw32.dll", "_SmackBlitOpen@4", api_SmackBlitOpen, 4},
    {"smackw32.dll", "_SmackBlitSetPalette@12", api_SmackBlitSetPalette, 12}, {"smackw32.dll", "_SmackBlit@44", api_SmackBlit, 44},
    {"smackw32.dll", "_SmackBlitClose@4", api_SmackBlitClose, 4},
    { 0 }
};
