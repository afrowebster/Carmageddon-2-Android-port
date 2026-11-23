/* DirectSound on SDL audio: software mixer of guest-memory buffers */
#include <math.h>
#include "win32.h"
#include "../platform/plat.h"

#define DS_OK 0
#define OUT_RATE 44100

typedef struct DSB {
    uint32_t obj;
    int primary;
    uint32_t mem, size;          /* guest buffer */
    int ch, bits, rate, freq;
    int playing, looping;
    uint64_t pos;                /* 32.32 fixed point byte position (frames * blockalign) */
    int32_t vol, pan;
    float gl, gr;
    int refs;
    struct DSB *next;
} DSB;

static DSB *bufs;
static uint32_t vt_ds, vt_dsb;
static uint32_t g_ds_obj;
static int audio_ok;

static void update_gain(DSB *b) {
    float g = powf(10.0f, b->vol / 2000.0f);
    float l = 1, r = 1;
    if (b->pan > 0) l = powf(10.0f, -b->pan / 2000.0f);
    if (b->pan < 0) r = powf(10.0f, b->pan / 2000.0f);
    if (b->vol <= -10000) g = 0;
    b->gl = g * l; b->gr = g * r;
}

static void mix(int16_t *out, int frames, void *ud) {
    static int32_t acc[8192 * 2];
    if (frames > 8192) frames = 8192;
    memset(acc, 0, frames * 2 * sizeof(int32_t));
    for (DSB *b = bufs; b; b = b->next) {
        if (!b->playing || !b->size) continue;
        int bal = b->ch * (b->bits / 8);
        uint32_t nframes = b->size / bal;
        uint64_t step = ((uint64_t)b->freq << 32) / OUT_RATE;
        uint64_t p = b->pos; /* in frames, 32.32 */
        const uint8_t *src = g_mem + b->mem;
        int gl = (int)(b->gl * 256), gr = (int)(b->gr * 256);
        for (int i = 0; i < frames; i++) {
            uint32_t f = (uint32_t)(p >> 32);
            if (f >= nframes) {
                if (b->looping) { p -= (uint64_t)nframes << 32; f = (uint32_t)(p >> 32); if (f >= nframes) { p = 0; f = 0; } }
                else { b->playing = 0; p = 0; break; }
            }
            uint32_t f2 = f + 1 < nframes ? f + 1 : (b->looping ? 0 : f);
            const uint8_t *s = src + f * bal, *s2 = src + f2 * bal;
            int l, r, l2, r2;
            if (b->bits == 16) { l = *(const int16_t *)s; r = b->ch == 2 ? *(const int16_t *)(s + 2) : l;
                                 l2 = *(const int16_t *)s2; r2 = b->ch == 2 ? *(const int16_t *)(s2 + 2) : l2; }
            else { l = ((int)s[0] - 128) << 8; r = b->ch == 2 ? ((int)s[1] - 128) << 8 : l;
                   l2 = ((int)s2[0] - 128) << 8; r2 = b->ch == 2 ? ((int)s2[1] - 128) << 8 : l2; }
            int fr = (int)((p >> 24) & 0xff);
            l += ((l2 - l) * fr) >> 8; r += ((r2 - r) * fr) >> 8;
            acc[2 * i] += (l * gl) >> 8;
            acc[2 * i + 1] += (r * gr) >> 8;
            p += step;
        }
        b->pos = p;
    }
    {   static uint32_t cnt; if ((cnt += frames) >= 44100 && getenv("C2_AUDIODBG")) { cnt = 0; int n = 0, np = 0; float g = 0;
            for (DSB *b = bufs; b; b = b->next) { n++; if (b->playing) { np++; g = b->gl; } }
            extern int smack_audio_state(void);
            LOG("audio: %d buffers, %d playing (gain %.3f), video audio %d", n, np, g, smack_audio_state()); } }
    { extern void cdaudio_mix(int32_t *, int, int); cdaudio_mix(acc, frames, OUT_RATE); }
    { extern void smack_mix(int32_t *, int, int); smack_mix(acc, frames, OUT_RATE); }
    for (int i = 0; i < frames * 2; i++) {
        int v = acc[i];
        out[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
    }
    {   /* C2_WAVOUT=path: also write the mix to a raw 16-bit stereo 44.1 kHz file (testing) */
        static FILE *wf; static int init;
        if (!init) { init = 1; const char *w = getenv("C2_WAVOUT"); if (w) wf = fopen(w, "wb"); }
        if (wf) fwrite(out, 4, frames, wf);
    }
}

static DSB *dsb_new(void) {
    DSB *b = calloc(1, sizeof *b);
    b->refs = 1;
    b->obj = com_new(vt_dsb, 0, b);
    update_gain(b);
    plat_audio_lock();
    b->next = bufs; bufs = b;
    plat_audio_unlock();
    return b;
}

#define B() COM_HOST(DSB)
static uint32_t ok(CPU *c) { return DS_OK; }
static uint32_t ds_CreateSoundBuffer(CPU *c) {
    uint32_t d = ARG(1), out = ARG(2);
    uint32_t fl = gr32(d + 4), bytes = gr32(d + 8), wfx = gr32(d + 16);
    DSB *b = dsb_new();
    if (fl & 1) { b->primary = 1; b->ch = 2; b->bits = 16; b->rate = b->freq = 22050; }
    else {
        b->size = bytes;
        b->mem = gcalloc(bytes + 16);
        b->ch = gr16(wfx + 2); b->rate = b->freq = gr32(wfx + 4); b->bits = gr16(wfx + 14);
    }
    TRACE("CreateSoundBuffer flags=%x bytes=%u %dch %dHz %dbit", fl, bytes, b->ch, b->rate, b->bits);
    gw32(out, b->obj);
    return DS_OK;
}
static uint32_t ds_GetCaps(CPU *c) {
    uint32_t p = ARG(1);
    uint32_t n = gr32(p);
    memset(GPTR(p + 4), 0, n - 4);
    gw32(p + 4, 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x400 | 0x800); /* continuous rate, mono/stereo, 8/16 secondary */
    gw32(p + 8, 4000); gw32(p + 12, 48000);
    return DS_OK;
}
static uint32_t ds_DuplicateSoundBuffer(CPU *c) {
    DSB *o = com_host(ARG(1));
    DSB *b = dsb_new();
    b->mem = o->mem; b->size = o->size; b->ch = o->ch; b->bits = o->bits; b->rate = o->rate; b->freq = o->freq;
    b->vol = o->vol; b->pan = o->pan; update_gain(b);
    gw32(ARG(2), b->obj);
    return DS_OK;
}
static uint32_t ds_GetSpeakerConfig(CPU *c) { gw32(ARG(1), 4); return DS_OK; }
static uint32_t ds_Release(CPU *c) { return 0; }

static uint32_t dsb_AddRef(CPU *c) { return ++B()->refs; }
static uint32_t dsb_Release(CPU *c) {
    DSB *b = B();
    if (--b->refs > 0) return b->refs;
    plat_audio_lock();
    b->playing = 0;
    plat_audio_unlock();
    return 0;
}
static uint32_t dsb_GetCaps(CPU *c) {
    DSB *b = B(); uint32_t p = ARG(1);
    gw32(p + 4, 0xe0); gw32(p + 8, b->size); gw32(p + 12, 0); gw32(p + 16, 0);
    return DS_OK;
}
static uint32_t bytes_pos(DSB *b) {
    int bal = b->ch * (b->bits / 8);
    return (uint32_t)(b->pos >> 32) * bal;
}
static uint32_t dsb_GetCurrentPosition(CPU *c) {
    DSB *b = B();
    plat_audio_lock();
    uint32_t p = b->size ? bytes_pos(b) % b->size : 0;
    plat_audio_unlock();
    uint32_t lead = (uint32_t)((uint64_t)b->freq * b->ch * (b->bits / 8) * 20 / 1000);
    lead -= lead % (b->ch * (b->bits / 8) ? b->ch * (b->bits / 8) : 1);
    if (ARG(1)) gw32(ARG(1), p);
    if (ARG(2)) gw32(ARG(2), b->size ? (p + lead) % b->size : 0);
    return DS_OK;
}
static uint32_t dsb_GetFormat(CPU *c) {
    DSB *b = B(); uint32_t p = ARG(1);
    if (p) {
        gw16(p, 1); gw16(p + 2, b->ch); gw32(p + 4, b->rate);
        gw32(p + 8, b->rate * b->ch * b->bits / 8); gw16(p + 12, b->ch * b->bits / 8); gw16(p + 14, b->bits); gw16(p + 16, 0);
    }
    if (ARG(3)) gw32(ARG(3), 18);
    return DS_OK;
}
static uint32_t dsb_GetVolume(CPU *c) { gw32(ARG(1), B()->vol); return DS_OK; }
static uint32_t dsb_GetPan(CPU *c) { gw32(ARG(1), B()->pan); return DS_OK; }
static uint32_t dsb_GetFrequency(CPU *c) { gw32(ARG(1), B()->freq); return DS_OK; }
static uint32_t dsb_GetStatus(CPU *c) {
    DSB *b = B();
    gw32(ARG(1), (b->playing ? 1 : 0) | (b->playing && b->looping ? 4 : 0));
    return DS_OK;
}
static uint32_t dsb_Lock(CPU *c) {
    DSB *b = B();
    uint32_t off = ARG(1), n = ARG(2), p1 = ARG(3), l1 = ARG(4), p2 = ARG(5), l2 = ARG(6), fl = ARG(7);
    if (!b->size) return 0x88780032u;
    if (fl & 1) { plat_audio_lock(); off = bytes_pos(b) % b->size; plat_audio_unlock(); }
    if (fl & 2) n = b->size;
    if (n > b->size) n = b->size;
    off %= b->size;
    uint32_t first = n, second = 0;
    if (off + n > b->size) { first = b->size - off; second = n - first; }
    gw32(p1, b->mem + off); gw32(l1, first);
    if (b->primary && getenv("C2_TRACE_DS")) LOG("primary Lock off=%u n=%u fl=%x play=%u", off, n, fl, bytes_pos(b) % b->size);
    if (p2) gw32(p2, second ? b->mem : 0);
    if (l2) gw32(l2, second);
    return DS_OK;
}
static uint32_t dsb_Unlock(CPU *c) { return DS_OK; }
static uint32_t dsb_Play(CPU *c) {
    DSB *b = B();
    plat_audio_lock();
    b->playing = 1; b->looping = (ARG(3) & 1) != 0 || b->primary;
    plat_audio_unlock();
    return DS_OK;
}
static uint32_t dsb_Stop(CPU *c) {
    DSB *b = B();
    plat_audio_lock();
    b->playing = 0;
    plat_audio_unlock();
    return DS_OK;
}
static uint32_t dsb_SetCurrentPosition(CPU *c) {
    DSB *b = B();
    int bal = b->ch * (b->bits / 8);
    plat_audio_lock();
    b->pos = bal ? ((uint64_t)(ARG(1) / bal) << 32) : 0;
    plat_audio_unlock();
    return DS_OK;
}
static uint32_t dsb_SetFormat(CPU *c) {
    DSB *b = B(); uint32_t w = ARG(1);
    b->ch = gr16(w + 2); b->rate = b->freq = gr32(w + 4); b->bits = gr16(w + 14);
    LOG("wfx: tag=%u ch=%u rate=%u avg=%u align=%u bits=%u", gr16(w), gr16(w+2), gr32(w+4), gr32(w+8), gr16(w+12), gr16(w+14));
    LOG("primary SetFormat %dch %dHz %dbit", b->ch, b->rate, b->bits);
    if (b->primary) {
        int bal = b->ch * b->bits / 8;
        uint32_t sz = (b->rate * bal / 4) & ~(uint32_t)(bal * 4 - 1); /* ~250ms */
        plat_audio_lock();
        b->playing = 0; b->pos = 0;
        b->mem = gcalloc(sz + 16);
        if (b->bits == 8) memset(GPTR(b->mem), 0x80, sz);
        b->size = sz;
        plat_audio_unlock();
        LOG("primary buffer %u bytes", sz);
    }
    return DS_OK;
}
static uint32_t dsb_SetVolume(CPU *c) { DSB *b = B(); b->vol = (int32_t)ARG(1); update_gain(b); return DS_OK; }
static uint32_t dsb_SetPan(CPU *c) { DSB *b = B(); b->pan = (int32_t)ARG(1); update_gain(b); return DS_OK; }
static uint32_t dsb_SetFrequency(CPU *c) { DSB *b = B(); b->freq = ARG(1) ? ARG(1) : b->rate; return DS_OK; }

static void build(void) {
    if (vt_ds) return;
    static const ComMethod ds[] = {
        {"QueryInterface", NULL, 3}, {"AddRef", ok, 1}, {"Release", ds_Release, 1},
        {"CreateSoundBuffer", ds_CreateSoundBuffer, 4}, {"GetCaps", ds_GetCaps, 2},
        {"DuplicateSoundBuffer", ds_DuplicateSoundBuffer, 3}, {"SetCooperativeLevel", ok, 3}, {"Compact", ok, 1},
        {"GetSpeakerConfig", ds_GetSpeakerConfig, 2}, {"SetSpeakerConfig", ok, 2}, {"Initialize", ok, 2},
    };
    vt_ds = com_make_vtbl("IDirectSound", ds, sizeof ds / sizeof *ds);
    static const ComMethod sb[] = {
        {"QueryInterface", NULL, 3}, {"AddRef", dsb_AddRef, 1}, {"Release", dsb_Release, 1},
        {"GetCaps", dsb_GetCaps, 2}, {"GetCurrentPosition", dsb_GetCurrentPosition, 3}, {"GetFormat", dsb_GetFormat, 4},
        {"GetVolume", dsb_GetVolume, 2}, {"GetPan", dsb_GetPan, 2}, {"GetFrequency", dsb_GetFrequency, 2},
        {"GetStatus", dsb_GetStatus, 2}, {"Initialize", ok, 3}, {"Lock", dsb_Lock, 8}, {"Play", dsb_Play, 4},
        {"SetCurrentPosition", dsb_SetCurrentPosition, 2}, {"SetFormat", dsb_SetFormat, 2}, {"SetVolume", dsb_SetVolume, 2},
        {"SetPan", dsb_SetPan, 2}, {"SetFrequency", dsb_SetFrequency, 2}, {"Stop", dsb_Stop, 1}, {"Unlock", dsb_Unlock, 5},
        {"Restore", ok, 1},
    };
    vt_dsb = com_make_vtbl("IDirectSoundBuffer", sb, sizeof sb / sizeof *sb);
}

API(DirectSoundCreate) {
    build();
    if (getenv("C2_NOSOUND")) return 0x88780078u; /* DSERR_NODRIVER */
    if (!audio_ok) audio_ok = plat_audio_open(OUT_RATE, mix, NULL);
    if (!audio_ok) return 0x88780078u;
    if (!g_ds_obj) g_ds_obj = com_new(vt_ds, 0, NULL);
    gw32(ARG(1), g_ds_obj);
    LOG("DirectSoundCreate ok");
    return DS_OK;
}

const ApiDef dsound_apis[] = { {"dsound.dll", "#1", api_DirectSoundCreate, 12}, {"dsound.dll", "DirectSoundCreate", api_DirectSoundCreate, 12}, { 0 } };
