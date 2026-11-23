/* SDL2 platform layer: window, GL ES context, input, timing */
#include <SDL.h>
#include <time.h>
#include "../win32/win32.h"
#include "plat.h"

static SDL_Window *win;
static SDL_GLContext glc;
static uint32_t t0;
int g_plat_w = 640, g_plat_h = 480;
static int g_win_w, g_win_h;

void plat_init(void) {
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    if (getenv("C2_TOUCH")) SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER | SDL_INIT_SENSOR) < 0)
        rt_fatal("SDL_Init: %s", SDL_GetError());
    t0 = SDL_GetTicks();
}

/* C2_VCLOCK=fps (testing): the game's clock advances exactly 1/fps seconds per frame (plus 1 us per
   clock read, so waiting loops still progress), whatever the real frame rate. Lets a slow machine
   behave, for the game, like one running at any frame rate. */
static int vclock_fps = -1;
static uint64_t vclock_us;
int plat_vclock(void) { if (vclock_fps < 0) vclock_fps = getenv("C2_VCLOCK") ? atoi(getenv("C2_VCLOCK")) : 0; return vclock_fps; }
void plat_vclock_frame(void) { if (plat_vclock() > 0) vclock_us += 1000000ull / vclock_fps; }
uint64_t plat_time_us(void) {
    if (plat_vclock() > 0) return 100000000ull + vclock_us++;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
}
uint32_t plat_ticks(void) {
    if (plat_vclock() > 0) return (uint32_t)(100000 + vclock_us / 1000);
    return SDL_GetTicks() - t0 + 100000;
}
void plat_sleep(uint32_t ms) { SDL_Delay(ms); }

/* frame rate cap: 30, 60, 120 or 0 = unlimited (setting "fpscap"). The vsync interval is chosen to
   match the display (120 Hz: 30 -> 4, 60 -> 2, 120 -> 1); unlimited turns vsync off. */
static int fps_cap = 60, display_hz = 60;
int plat_fps_cap(void) { return fps_cap; }
void plat_set_fps_cap(int cap) {
    if (cap != 30 && cap != 60 && cap != 120 && cap != 0) cap = 60;
    fps_cap = cap;
    setting_set_int("fpscap", cap);
    int interval = 0;
    if (cap > 0 && !getenv("C2_NOVSYNC")) {
        interval = (display_hz + cap / 2) / cap;   /* refreshes per frame */
        if (interval < 1) interval = 1;
    }
    if (SDL_GL_SetSwapInterval(interval) != 0 && interval > 1) SDL_GL_SetSwapInterval(1);
    LOG("frame rate cap %d (display %d Hz, swap interval %d)", cap, display_hz, interval);
}
void plat_cycle_fps_cap(void) {
    static const int caps[] = { 30, 60, 120, 0 };
    int i = 0; while (i < 4 && caps[i] != fps_cap) i++;
    plat_set_fps_cap(caps[(i + 1) % 4]);
}

int plat_create_gl(int w, int h) {
    g_plat_w = w; g_plat_h = h;
    if (win) return 1;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    Uint32 fl = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#ifdef __ANDROID__
    fl |= SDL_WINDOW_FULLSCREEN;
#endif
    int sw = w * 2, sh = h * 2;
    const char *sc = getenv("C2_SCALE");
    if (sc) { sw = w * atoi(sc); sh = h * atoi(sc); }
    const char *ws = getenv("C2_WIN");
    if (ws) sscanf(ws, "%dx%d", &sw, &sh);
    else fl |= SDL_WINDOW_FULLSCREEN_DESKTOP;   /* fullscreen only (C2_WIN=WxH gives a window, for testing) */
    win = SDL_CreateWindow("Carmageddon II", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, sw, sh, fl);
    if (!win) rt_fatal("SDL_CreateWindow: %s", SDL_GetError());
#ifndef __ANDROID__
    {   /* window icon: the PC game's icon (extracted by tools/make_icon.py) */
        char ip[1200]; snprintf(ip, sizeof ip, "%s/icon.bmp", g_game_dir);
        SDL_Surface *ic = SDL_LoadBMP(ip);
        if (ic) { SDL_SetWindowIcon(win, ic); SDL_FreeSurface(ic); }
    }
#endif
    glc = SDL_GL_CreateContext(win);
    if (!glc) rt_fatal("SDL_GL_CreateContext: %s", SDL_GetError());
    SDL_GL_MakeCurrent(win, glc);
    {
        SDL_DisplayMode dm;
        display_hz = (SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(win), &dm) == 0 && dm.refresh_rate > 0) ? dm.refresh_rate : 60;
        plat_set_fps_cap(setting_int("fpscap", 60));
    }
    SDL_GL_GetDrawableSize(win, &g_win_w, &g_win_h);
    LOG("GL: %s / %s / %s", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
    touch_init();
    return 1;
}

void plat_drawable_size(int *w, int *h) {
    if (win) SDL_GL_GetDrawableSize(win, &g_win_w, &g_win_h);
    *w = g_win_w; *h = g_win_h;
}

void plat_swap(void) {
    touch_draw();
    static int nswap; nswap++;
    const char *ws = getenv("C2_WINSHOT");
    int hit = 0;
    for (const char *q = ws; q && *q; ) { if (atoi(q) == nswap) hit = 1; q = strchr(q, ','); if (q) q++; }
    if (hit) {
        int w, h; plat_drawable_size(&w, &h);
        unsigned char *b = malloc((size_t)w * h * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, b);
        char p[1200]; snprintf(p, sizeof p, "%s/winshot_%05d.ppm", g_save_dir, nswap);
        FILE *f = fopen(p, "wb");
        if (f) { fprintf(f, "P6\n%d %d\n255\n", w, h); for (int y = h - 1; y >= 0; y--) for (int x = 0; x < w; x++) fwrite(b + (y * w + x) * 4, 1, 3, f); fclose(f); }
        free(b);
    }
    SDL_GL_SwapWindow(win);
    {   /* frame rate cap: software pacing on top of the vsync interval */
        extern int plat_vclock(void);
        static uint64_t last;
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now = (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
        uint64_t min_us = fps_cap > 0 ? 1000000ull / fps_cap - 500 : 0;   /* a little slack so vsync sets the pace */
        if (!plat_vclock() && min_us && last && now - last < min_us) {
            uint64_t wait = min_us - (now - last);
            if (wait > 1500) SDL_Delay((uint32_t)(wait / 1000) - 1);
            do { clock_gettime(CLOCK_MONOTONIC, &ts); now = (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000; } while (now - last < min_us);
        }
        last = now;
    }
}

static int sdl_to_vk(SDL_Keycode k) {
    if (k >= SDLK_a && k <= SDLK_z) return 'A' + (k - SDLK_a);
    if (k >= SDLK_0 && k <= SDLK_9) return '0' + (k - SDLK_0);
    if (k >= SDLK_F1 && k <= SDLK_F12) return 0x70 + (k - SDLK_F1);
    switch (k) {
    case SDLK_RETURN: case SDLK_KP_ENTER: return 0x0D;
    case SDLK_ESCAPE: case SDLK_AC_BACK: return 0x1B;
    case SDLK_SPACE: return 0x20;
    case SDLK_BACKSPACE: return 0x08;
    case SDLK_TAB: return 0x09;
    case SDLK_LEFT: return 0x25; case SDLK_UP: return 0x26; case SDLK_RIGHT: return 0x27; case SDLK_DOWN: return 0x28;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return 0x10;
    case SDLK_LCTRL: case SDLK_RCTRL: return 0x11;
    case SDLK_LALT: case SDLK_RALT: return 0x12;
    case SDLK_PAGEUP: return 0x21; case SDLK_PAGEDOWN: return 0x22; case SDLK_END: return 0x23; case SDLK_HOME: return 0x24;
    case SDLK_INSERT: return 0x2D; case SDLK_DELETE: return 0x2E; case SDLK_PAUSE: return 0x13;
    case SDLK_KP_0: return 0x60; case SDLK_KP_1: return 0x61; case SDLK_KP_2: return 0x62; case SDLK_KP_3: return 0x63;
    case SDLK_KP_4: return 0x64; case SDLK_KP_5: return 0x65; case SDLK_KP_6: return 0x66; case SDLK_KP_7: return 0x67;
    case SDLK_KP_8: return 0x68; case SDLK_KP_9: return 0x69;
    case SDLK_KP_MULTIPLY: return 0x6A; case SDLK_KP_PLUS: return 0x6B; case SDLK_KP_MINUS: return 0x6D;
    case SDLK_KP_PERIOD: return 0x6E; case SDLK_KP_DIVIDE: return 0x6F;
    case SDLK_MINUS: return 0xBD; case SDLK_EQUALS: return 0xBB; case SDLK_PERIOD: return 0xBE; case SDLK_COMMA: return 0xBC;
    }
    return 0;
}

/* scripted input for automated testing: C2_KEYS="t:vk:down,..." (t in ms since start) */
static struct { uint32_t t; int vk, down; } script[256];
static int nscript, iscript;
static void load_script(void) {
    static int loaded;
    if (loaded) return;
    loaded = 1;
    const char *s = getenv("C2_KEYS");
    while (s && *s && nscript < 256) {
        unsigned t; int vk, d, n;
        if (sscanf(s, "%u:%i:%d%n", &t, &vk, &d, &n) != 3) break;
        script[nscript].t = t; script[nscript].vk = vk; script[nscript].down = d; nscript++;
        s += n; if (*s == ',') s++;
    }
}

/* ---- game controller -> the game's default keyboard controls ----
   D-pad: arrows (menus; in a race the camera)      A: Enter      B / Start: Esc
   left stick / triggers: steer, accelerate (RT), brake/reverse (LT)
   X: handbrake (Space)   Y: repair (Backspace)   LB: next power-up (])   RB: use power-up (Alt)
   Back: map (Tab)   L3: recover (Insert)   R3: cockpit view (C)
   right stick: left/right camera (arrows), up horn (H), down target lock (T) */
extern void dinput_key(int, int);
extern int dinput_from_sdl(int);
static void pad_key(int sc, int vk, int down) {
    dinput_key(dinput_from_sdl(sc), down);
    if (vk) user32_key_event(vk, down);
}
static void pad_button(int b, int down) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: pad_key(SDL_SCANCODE_UP, 0x26, down); break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: pad_key(SDL_SCANCODE_DOWN, 0x28, down); break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: pad_key(SDL_SCANCODE_LEFT, 0x25, down); break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: pad_key(SDL_SCANCODE_RIGHT, 0x27, down); break;
    case SDL_CONTROLLER_BUTTON_A: pad_key(SDL_SCANCODE_RETURN, 0x0D, down); break;
    case SDL_CONTROLLER_BUTTON_B: case SDL_CONTROLLER_BUTTON_START: pad_key(SDL_SCANCODE_ESCAPE, 0x1B, down); break;
    case SDL_CONTROLLER_BUTTON_X: pad_key(SDL_SCANCODE_SPACE, 0x20, down); break;
    case SDL_CONTROLLER_BUTTON_Y: pad_key(SDL_SCANCODE_BACKSPACE, 0x08, down); break;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: pad_key(SDL_SCANCODE_RIGHTBRACKET, 0, down); break;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: pad_key(SDL_SCANCODE_LALT, 0, down); break;
    case SDL_CONTROLLER_BUTTON_BACK: pad_key(SDL_SCANCODE_TAB, 0x09, down); break;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: pad_key(SDL_SCANCODE_INSERT, 0x2D, down); break;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: pad_key(SDL_SCANCODE_C, 'C', down); break;
    }
}
/* an analog axis drives two keys (negative / positive side) with hysteresis */
static void axis_keys(int *state, int v, int on, int off, int scn, int scp) {
    int ns = *state;
    if (v <= -on) ns = -1; else if (v >= on) ns = 1; else if (v > -off && v < off) ns = 0;
    if (ns == *state) return;
    if (*state == -1 && scn) pad_key(scn, 0, 0);
    if (*state == 1 && scp) pad_key(scp, 0, 0);
    if (ns == -1 && scn) pad_key(scn, 0, 1);
    if (ns == 1 && scp) pad_key(scp, 0, 1);
    *state = ns;
}
static void pad_axis(int axis, int v) {
    static int st[SDL_CONTROLLER_AXIS_MAX];
    switch (axis) {
    case SDL_CONTROLLER_AXIS_LEFTX: axis_keys(&st[axis], v, 9000, 6000, SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_6); break;
    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: axis_keys(&st[axis], v, 8000, 4000, 0, SDL_SCANCODE_KP_8); break;
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT: axis_keys(&st[axis], v, 8000, 4000, 0, SDL_SCANCODE_KP_2); break;
    case SDL_CONTROLLER_AXIS_RIGHTX: axis_keys(&st[axis], v, 16000, 10000, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT); break;
    case SDL_CONTROLLER_AXIS_RIGHTY: axis_keys(&st[axis], v, 20000, 12000, SDL_SCANCODE_H, SDL_SCANCODE_T); break;
    }
}

void plat_pump_events(void) {
    { extern void cdaudio_poll(void); cdaudio_poll(); }
    load_script();
    uint32_t now = plat_ticks() - 100000;
    while (iscript < nscript && script[iscript].t <= now) {
        user32_key_event(script[iscript].vk, script[iscript].down);
        { extern void dinput_key(int, int); extern int dinput_from_sdl(int);
          int vk = script[iscript].vk, sc = 0;
          if (vk >= 'A' && vk <= 'Z') sc = SDL_SCANCODE_A + vk - 'A';
          else if (vk >= '1' && vk <= '9') sc = SDL_SCANCODE_1 + vk - '1';
          else if (vk == '0') sc = SDL_SCANCODE_0;
          else switch (vk) { case 0x0D: sc = SDL_SCANCODE_RETURN; break; case 0x1B: sc = SDL_SCANCODE_ESCAPE; break;
            case 0x20: sc = SDL_SCANCODE_SPACE; break; case 0x25: sc = SDL_SCANCODE_LEFT; break; case 0x26: sc = SDL_SCANCODE_UP; break;
            case 0x27: sc = SDL_SCANCODE_RIGHT; break; case 0x28: sc = SDL_SCANCODE_DOWN; break;
            case 0x64: sc = SDL_SCANCODE_KP_4; break; case 0x66: sc = SDL_SCANCODE_KP_6; break;
            case 0x68: sc = SDL_SCANCODE_KP_8; break; case 0x62: sc = SDL_SCANCODE_KP_2; break; }
          if (sc) dinput_key(dinput_from_sdl(sc), script[iscript].down); }
        iscript++;
    }
    touch_tick();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: user32_quit_request(); break;
        case SDL_KEYDOWN: case SDL_KEYUP: {
#ifdef __ANDROID__
            /* letters/digits arrive via SDL_TEXTINPUT when the soft keyboard is up */
            if (SDL_IsTextInputActive() && ((e.key.keysym.sym >= SDLK_a && e.key.keysym.sym <= SDLK_z) || (e.key.keysym.sym >= SDLK_0 && e.key.keysym.sym <= SDLK_9) || e.key.keysym.sym == SDLK_SPACE)) break;
#endif
            if (e.key.keysym.sym == SDLK_F9) { if (e.type == SDL_KEYDOWN && !e.key.repeat) plat_cycle_fps_cap(); break; }
            int vk = sdl_to_vk(e.key.keysym.sym);
            if (vk) user32_key_event(vk, e.type == SDL_KEYDOWN);
            if (!e.key.repeat) { extern void dinput_key(int, int); extern int dinput_from_sdl(int);
                dinput_key(dinput_from_sdl(e.key.keysym.scancode), e.type == SDL_KEYDOWN); }
            break;
        }
        case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
            if ((e.type == SDL_MOUSEMOTION ? e.motion.which : e.button.which) == SDL_TOUCH_MOUSEID) break;
            extern int glide_to_game(float, float, int *, int *);
            int ww, wh, gx, gy; SDL_GetWindowSize(win, &ww, &wh);
            int mx = e.type == SDL_MOUSEMOTION ? e.motion.x : e.button.x, my = e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y;
            if (ww <= 0 || wh <= 0 || !glide_to_game((float)mx / ww, (float)my / wh, &gx, &gy)) break;
            int btn = e.type == SDL_MOUSEMOTION ? 0 : (e.button.button == SDL_BUTTON_RIGHT ? 2 : 0);
            int what = e.type == SDL_MOUSEMOTION ? 0 : e.type == SDL_MOUSEBUTTONDOWN ? (btn ? 3 : 1) : (btn ? 4 : 2);
            user32_mouse_event(gx, gy, (e.type == SDL_MOUSEMOTION && (e.motion.state & SDL_BUTTON_LMASK)) ? 1 : 0, what);
            break;
        }
        case SDL_FINGERDOWN: case SDL_FINGERUP: case SDL_FINGERMOTION:
            touch_event(&e);
            break;
#ifdef __ANDROID__
        case SDL_TEXTINPUT: {
            /* soft keyboard: turn characters into key presses */
            for (const char *p = e.text.text; *p; p++) {
                int ch = (unsigned char)*p, vk = 0;
                if (ch >= 'a' && ch <= 'z') vk = ch - 32;
                else if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == ' ') vk = ch;
                if (vk) { user32_key_event(vk, 1); user32_key_event(vk, 0); }
            }
            break;
        }
#endif
        case SDL_SENSORUPDATE: touch_sensor(e.sensor.data); break;
        case SDL_CONTROLLERDEVICEADDED: SDL_GameControllerOpen(e.cdevice.which); LOG("controller added: %s", SDL_GameControllerNameForIndex(e.cdevice.which)); break;
        case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP:
            pad_button(e.cbutton.button, e.type == SDL_CONTROLLERBUTTONDOWN);
            break;
        case SDL_CONTROLLERAXISMOTION:
            pad_axis(e.caxis.axis, e.caxis.value);
            break;
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) user32_activate(1);
            else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST && !getenv("C2_IGNORE_FOCUS")) user32_activate(0);
            break;
        case SDL_APP_WILLENTERBACKGROUND: user32_activate(0); break;
        case SDL_APP_DIDENTERFOREGROUND: user32_activate(1); break;
        }
    }
}

float plat_screen_aspect(void) {
    int w = 0, h = 0;
    const char *ws = getenv("C2_WIN");
    if (ws && sscanf(ws, "%dx%d", &w, &h) == 2 && h > 0) return (float)w / h;
#ifdef __ANDROID__
    /* the usable window (minus display cutouts) is what we fill */
    if (!win) plat_create_gl(640, 480);
    SDL_GL_GetDrawableSize(win, &w, &h);
    if (w > 0 && h > 0) { float a = (float)w / h; return a < 1 ? 1 / a : a; }
#endif
    SDL_DisplayMode m;
    if (SDL_GetDesktopDisplayMode(0, &m) == 0 && m.h > 0) { float a = (float)m.w / m.h; return a < 1 ? 1 / a : a; }
    return 4.0f / 3.0f;
}

/* restart the whole game (used when switching aspect mode: the game picks its resolution at startup) */
#ifdef __ANDROID__
#include <jni.h>
void plat_restart(void) {
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject act = (jobject)SDL_AndroidGetActivity();
    if (env && act) {
        jclass cls = (*env)->GetObjectClass(env, act);
        jmethodID m = (*env)->GetMethodID(env, cls, "restartApp", "()V");
        if (m) (*env)->CallVoidMethod(env, act, m);
    }
    exit(0);
}
#else
#include <unistd.h>
char **g_argv;
void plat_restart(void) {
    fflush(NULL);
    SDL_Quit();
    execv("/proc/self/exe", g_argv);
    exit(0);
}
#endif
