/* CD audio (MCI cdaudio device) played from the GOG release's music/TrackNN.ogg files */
#define STB_VORBIS_NO_PUSHDATA_API
#include "../third_party/stb_vorbis.c"
#include "win32.h"
#include "../platform/plat.h"

static stb_vorbis *vf;
static int vrate, vch;
static int cur_track, playing, from_track, to_track;
static uint32_t notify_hwnd;
static int notify_pending;
static float vol = 1.0f;
static short dbuf[4096 * 2]; static int dlen, dpos;
static uint64_t frac;   /* 32.32 resample position within dbuf */

static int open_track(int t) {
    if (vf) { stb_vorbis_close(vf); vf = NULL; }
    char rel[64], p[1200];
    snprintf(rel, sizeof rel, "C:\\CARMA2\\music\\Track%02d.ogg", t);
    if (!host_path(rel, p, sizeof p, 0)) return 0;
    int err;
    vf = stb_vorbis_open_filename(p, &err, NULL);
    if (!vf) { LOG("cdaudio: cannot open %s", p); return 0; }
    stb_vorbis_info i = stb_vorbis_get_info(vf);
    vrate = i.sample_rate; vch = i.channels;
    dlen = dpos = 0; frac = 0;
    cur_track = t;
    LOG("cdaudio: playing track %d (%d Hz)", t, vrate);
    return 1;
}

void cdaudio_mix(int32_t *acc, int frames, int out_rate) {
    if (!playing || !vf) return;
    uint64_t step = ((uint64_t)vrate << 32) / out_rate;
    int g = (int)(vol * 256);
    for (int i = 0; i < frames; i++) {
        while (dpos >= dlen) {
            dlen = stb_vorbis_get_samples_short_interleaved(vf, 2, dbuf, 4096 * 2);
            dpos = 0;
            if (dlen <= 0) {
                dlen = 0;
                if (cur_track < to_track && open_track(cur_track + 1)) continue;
                playing = 0; notify_pending = 1;
                return;
            }
        }
        acc[2 * i] += (dbuf[2 * dpos] * g) >> 8;
        acc[2 * i + 1] += (dbuf[2 * dpos + 1] * g) >> 8;
        frac += step;
        dpos += (int)(frac >> 32); frac &= 0xffffffffu;
    }
}

/* called from the message pump: deliver MM_MCINOTIFY when a track ends */
void cdaudio_poll(void) {
    if (notify_pending && notify_hwnd) { notify_pending = 0; user32_post(notify_hwnd, 0x3B9, 1, 1); }
}

void cdaudio_set_volume(float v) { vol = v; }

static int track_count(void) {
    int n = 1;
    for (int t = 2; t < 100; t++) { char rel[64], p[1200]; snprintf(rel, sizeof rel, "C:\\CARMA2\\music\\Track%02d.ogg", t); if (!host_path(rel, p, sizeof p, 0)) break; n = t; }
    return n;
}

API(mciSendCommandA) {
    uint32_t dev = ARG(0), msg = ARG(1), fl = ARG(2), pp = ARG(3);
    TRACE("mciSendCommand(%u, %x, %x)", dev, msg, fl);
    switch (msg) {
    case 0x803: /* MCI_OPEN */
        if (pp) gw32(pp + 4, 1);
        return 0;
    case 0x804: /* MCI_CLOSE */
        plat_audio_lock(); playing = 0; plat_audio_unlock();
        return 0;
    case 0x80D: /* MCI_SET */ return 0;
    case 0x806: { /* MCI_PLAY */
        int f = (fl & 4) ? (gr32(pp + 4) & 0xff) : cur_track ? cur_track : 2;
        int t = (fl & 8) ? (gr32(pp + 8) & 0xff) : 99;
        if (f < 2) f = 2;
        plat_audio_lock();
        from_track = f; to_track = t;
        playing = open_track(f);
        notify_hwnd = (fl & 1) ? gr32(pp) : 0;
        notify_pending = 0;
        plat_audio_unlock();
        return 0;
    }
    case 0x808: case 0x809: /* MCI_STOP / MCI_PAUSE */
        plat_audio_lock(); playing = 0; plat_audio_unlock();
        return 0;
    case 0x855: /* MCI_RESUME */
        plat_audio_lock(); if (vf) playing = 1; plat_audio_unlock();
        return 0;
    case 0x814: { /* MCI_STATUS */
        uint32_t item = gr32(pp + 8), r = 0;
        switch (item) {
        case 1: r = 3 * 60 * 1000; break;
        case 2: r = cur_track; break;
        case 3: r = track_count(); break;
        case 4: r = playing ? 526 : 525; break;
        case 5: case 7: r = 1; break;
        case 8: r = cur_track; break;
        }
        gw32(pp + 4, r);
        return 0;
    }
    }
    LOG("mciSendCommand: unhandled msg %x", msg);
    return 0;
}

const ApiDef cdaudio_apis[] = { DEF("winmm.dll", mciSendCommandA, 4), { 0 } };
