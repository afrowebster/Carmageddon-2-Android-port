#pragma once
#include <SDL.h>
#ifdef __ANDROID__
#include <GLES3/gl3.h>
#else
#include <GLES3/gl3.h>
#endif

extern int g_plat_w, g_plat_h;
int plat_create_gl(int w, int h);
void plat_swap(void);
void plat_drawable_size(int *w, int *h);
float plat_screen_aspect(void);
void plat_restart(void);
int plat_fps_cap(void);
void plat_set_fps_cap(int cap);
void plat_cycle_fps_cap(void);

/* touch overlay */
void touch_init(void);
void touch_event(SDL_Event *e);
void touch_draw(void);
void touch_tick(void);
void touch_sensor(const float *d);

/* audio */
typedef void (*AudioMixFn)(int16_t *out, int frames, void *ud);
int plat_audio_open(int rate, AudioMixFn fn, void *ud);
void plat_audio_lock(void);
void plat_audio_unlock(void);
