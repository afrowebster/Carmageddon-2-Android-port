#include "plat.h"
#include "../runtime/runtime.h"

static SDL_AudioDeviceID dev;
static AudioMixFn mixfn;
static void *mixud;

static void cb(void *ud, Uint8 *stream, int len) {
    mixfn((int16_t *)stream, len / 4, mixud);
}

int plat_audio_open(int rate, AudioMixFn fn, void *ud) {
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = rate; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024; want.callback = cb;
    mixfn = fn; mixud = ud;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) { LOG("SDL_OpenAudioDevice failed: %s", SDL_GetError()); return 0; }
    SDL_PauseAudioDevice(dev, 0);
    LOG("audio %d Hz", have.freq);
    return 1;
}
void plat_audio_lock(void) { if (dev) SDL_LockAudioDevice(dev); }
void plat_audio_unlock(void) { if (dev) SDL_UnlockAudioDevice(dev); }
