/* Touch controls (Android, or desktop with C2_TOUCH=1).
 *  - menus: the screen is a mouse (touch = move the pointer and click)
 *  - races: on-screen buttons mapped to the game's default keys
 *      left:  steer left / right, repair, recover
 *      right: accelerate, brake/reverse, handbrake, use power-up, next power-up
 *      top-right: Esc (pause menu)
 * "Race" is detected from the renderer: a frame that draws depth-buffered 3D geometry. */
#include <math.h>
#include "plat.h"
#include "../win32/win32.h"

extern void dinput_key(int, int);
extern int dinput_from_sdl(int);
extern int glide_in_3d(void);
extern int glide_to_game(float sx, float sy, int *gx, int *gy);

enum { IC_LEFT, IC_RIGHT, IC_UP, IC_DOWN, IC_BAR, IC_STAR, IC_PLUS, IC_ROOF, IC_CROSS, IC_NEXT };
typedef struct { float x, y, r; int sc; float col[3]; int icon; int race_only; } Btn;
enum { NB = 10 };
static Btn btns[NB];
static float scr_a = 16.0f / 9.0f;
static int visible;
/* performance readout (three-finger tap toggles it; C2_PERF=1 on desktop) */
static int perf_on;
extern void glide_perf(float *fps, float *game, float *gl, float *swap);
static GLuint prog, vbo, vao;
static GLint u_col, u_sc;

typedef struct { SDL_FingerID id; int active; int btn; int mouse; } Finger;
static Finger fingers[10];
static int held[NB];

static void press(int b, int down) {
    if (b < 0) return;
    if (down) { if (held[b]++ == 0) { dinput_key(dinput_from_sdl(btns[b].sc), 1); if (btns[b].sc == SDL_SCANCODE_ESCAPE) user32_key_event(0x1B, 1); } }
    else if (held[b] > 0 && --held[b] == 0) { dinput_key(dinput_from_sdl(btns[b].sc), 0); if (btns[b].sc == SDL_SCANCODE_ESCAPE) user32_key_event(0x1B, 0); }
}

static void layout(void) {
    int w, h;
    plat_drawable_size(&w, &h);
    if (w <= 0 || h <= 0) return;
    float a = (float)w / h;   /* coordinates: y in [0,1], x in [0,a] */
    scr_a = a;
    btns[0] = (Btn){0.17f, 0.80f, 0.13f, SDL_SCANCODE_KP_4, {0.8f, 0.8f, 0.8f}, IC_LEFT, 1};
    btns[1] = (Btn){0.47f, 0.80f, 0.13f, SDL_SCANCODE_KP_6, {0.8f, 0.8f, 0.8f}, IC_RIGHT, 1};
    /* right side; the bottom-right corner stays clear for the head-up map */
    btns[2] = (Btn){a - 0.21f, 0.69f, 0.13f, SDL_SCANCODE_KP_8, {0.2f, 0.85f, 0.3f}, IC_UP, 1};
    btns[3] = (Btn){a - 0.47f, 0.84f, 0.10f, SDL_SCANCODE_KP_2, {0.9f, 0.25f, 0.2f}, IC_DOWN, 1};
    btns[4] = (Btn){a - 0.13f, 0.40f, 0.08f, SDL_SCANCODE_SPACE, {0.95f, 0.75f, 0.2f}, IC_BAR, 1};
    btns[5] = (Btn){a - 0.40f, 0.52f, 0.07f, SDL_SCANCODE_LALT, {0.8f, 0.4f, 0.95f}, IC_STAR, 1};
    btns[6] = (Btn){a - 0.36f, 0.33f, 0.055f, SDL_SCANCODE_RIGHTBRACKET, {0.8f, 0.4f, 0.95f}, IC_NEXT, 1};
    btns[7] = (Btn){0.12f, 0.47f, 0.065f, SDL_SCANCODE_BACKSPACE, {0.3f, 0.6f, 0.95f}, IC_PLUS, 1};
    btns[8] = (Btn){0.32f, 0.47f, 0.065f, SDL_SCANCODE_INSERT, {0.3f, 0.6f, 0.95f}, IC_ROOF, 1};
    btns[9] = (Btn){a - 0.09f, 0.10f, 0.06f, SDL_SCANCODE_ESCAPE, {0.9f, 0.3f, 0.2f}, IC_CROSS, 0};
}

static int hit(float x, float y, int race) {
    int best = -1; float bd = 1e9f;
    for (int i = 0; i < NB; i++) {
        if (btns[i].race_only && !race) continue;
        float dx = x - btns[i].x, dy = y - btns[i].y, d = sqrtf(dx * dx + dy * dy);
        if (d < btns[i].r * 1.25f && d < bd) { bd = d; best = i; }
    }
    return best;
}

void touch_init(void) {
    perf_on = getenv("C2_PERF") != NULL;
#ifdef __ANDROID__
    visible = 1;
#else
    visible = getenv("C2_TOUCH") != NULL;
#endif
    layout();
}

void touch_event(SDL_Event *e) {
    if (!visible) return;
    layout();
    float x = e->tfinger.x * scr_a, y = e->tfinger.y;
    int race = glide_in_3d();
    int slot = -1;
    for (int i = 0; i < 10; i++) if (fingers[i].active && fingers[i].id == e->tfinger.fingerId) slot = i;
    if (e->type == SDL_FINGERDOWN) {
        int nact = 0; for (int i = 0; i < 10; i++) nact += fingers[i].active;
        if (nact == 2) { perf_on = !perf_on; LOG("performance readout %s", perf_on ? "on" : "off"); }
        if (slot < 0) for (int i = 0; i < 10; i++) if (!fingers[i].active) { slot = i; break; }
        if (slot < 0) return;
        Finger *f = &fingers[slot];
        f->active = 1; f->id = e->tfinger.fingerId;
        f->btn = hit(x, y, race); f->mouse = 0;
        if (perf_on && y < 0.07f && x < 1.0f) { plat_cycle_fps_cap(); f->btn = -1; return; }   /* tap the readout: next frame rate cap */
        if (f->btn >= 0) { press(f->btn, 1); return; }
        if (!race) {
            /* menus: the touch is the mouse */
            int gx, gy;
            if (glide_to_game(e->tfinger.x, e->tfinger.y, &gx, &gy)) {
                user32_mouse_event(gx, gy, 0, 0);
                user32_mouse_event(gx, gy, 1, 1);
                f->mouse = 1;
            }
        }
    } else if (slot >= 0) {
        Finger *f = &fingers[slot];
        int gx, gy;
        if (e->type == SDL_FINGERMOTION) {
            if (f->mouse) { if (glide_to_game(e->tfinger.x, e->tfinger.y, &gx, &gy)) user32_mouse_event(gx, gy, 1, 0); return; }
            /* sliding a finger between buttons (e.g. steer left -> right) */
            int b = hit(x, y, race);
            if (b != f->btn && b >= 0 && btns[b].race_only) { press(f->btn, 0); f->btn = b; press(b, 1); }
            else if (b < 0 && f->btn >= 0) {
                float dx = x - btns[f->btn].x, dy = y - btns[f->btn].y;
                if (sqrtf(dx * dx + dy * dy) > btns[f->btn].r * 2.0f) { press(f->btn, 0); f->btn = -1; }
            }
        } else { /* up */
            if (f->mouse && glide_to_game(e->tfinger.x, e->tfinger.y, &gx, &gy)) user32_mouse_event(gx, gy, 0, 2);
            press(f->btn, 0);
            f->active = 0; f->btn = -1; f->mouse = 0;
        }
    }
}

/* drop held buttons when switching between menus and races */
void touch_tick(void) {
    static int last_race = -1;
    int race = glide_in_3d();
    if (race != last_race) {
        for (int i = 0; i < 10; i++) if (fingers[i].active && fingers[i].btn >= 0 && btns[fingers[i].btn].race_only && !race) {
            press(fingers[i].btn, 0); fingers[i].btn = -1;
        }
        last_race = race;
    }
}
void touch_sensor(const float *d) { (void)d; }

/* ---------------- drawing ---------------- */
static const char *VS = "#version 300 es\nlayout(location=0) in vec2 p; layout(location=1) in vec4 c; uniform vec2 sc; out vec4 vc; void main(){ vc=c; gl_Position=vec4(p.x*sc.x-1.0, 1.0-p.y*sc.y, 0.0, 1.0); }\n";
static const char *FS = "#version 300 es\nprecision mediump float; in vec4 vc; out vec4 o; void main(){ o=vc; }\n";

static GLuint sh(GLenum t, const char *s) { GLuint x = glCreateShader(t); glShaderSource(x, 1, &s, NULL); glCompileShader(x); return x; }
static float vb[4096]; static int nvb;
static void tri(float ax, float ay, float bx, float by, float cx, float cy) {
    if (nvb + 6 > 4096) return;
    vb[nvb++] = ax; vb[nvb++] = ay; vb[nvb++] = bx; vb[nvb++] = by; vb[nvb++] = cx; vb[nvb++] = cy;
}
static void quad(float x0, float y0, float x1, float y1) { tri(x0, y0, x1, y0, x0, y1); tri(x1, y0, x1, y1, x0, y1); }
static void disc(float x, float y, float r, float r0) {
    for (int i = 0; i < 40; i++) {
        float a0 = i * 6.2831853f / 40, a1 = (i + 1) * 6.2831853f / 40;
        float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
        tri(x + c0 * r0, y + s0 * r0, x + c0 * r, y + s0 * r, x + c1 * r, y + s1 * r);
        if (r0 > 0) tri(x + c0 * r0, y + s0 * r0, x + c1 * r, y + s1 * r, x + c1 * r0, y + s1 * r0);
    }
}
/* all shapes go into one coloured batch, drawn with a single call per frame */
static float batch[32768]; static int nbatch;
static void flushdraw(float r, float g, float b, float a) {
    for (int i = 0; i + 1 < nvb && nbatch + 6 <= (int)(sizeof batch / sizeof *batch); i += 2) {
        float *d = batch + nbatch;
        d[0] = vb[i]; d[1] = vb[i + 1]; d[2] = r; d[3] = g; d[4] = b; d[5] = a;
        nbatch += 6;
    }
    nvb = 0;
}
static void icon(const Btn *b) {
    float x = b->x, y = b->y, s = b->r * 0.5f;
    switch (b->icon) {
    case IC_LEFT: tri(x - s, y, x + s * 0.7f, y - s, x + s * 0.7f, y + s); break;
    case IC_RIGHT: tri(x + s, y, x - s * 0.7f, y - s, x - s * 0.7f, y + s); break;
    case IC_UP: tri(x, y - s, x - s, y + s * 0.7f, x + s, y + s * 0.7f); break;
    case IC_DOWN: tri(x, y + s, x - s, y - s * 0.7f, x + s, y - s * 0.7f); break;
    case IC_BAR: quad(x - s, y - s * 0.25f, x + s, y + s * 0.25f); break;
    case IC_STAR: tri(x, y - s, x - s * 0.9f, y + s * 0.6f, x + s * 0.9f, y + s * 0.6f); tri(x, y + s, x - s * 0.9f, y - s * 0.6f, x + s * 0.9f, y - s * 0.6f); break;
    case IC_NEXT: tri(x + s, y, x - s * 0.2f, y - s * 0.8f, x - s * 0.2f, y + s * 0.8f); quad(x - s, y - s * 0.8f, x - s * 0.5f, y + s * 0.8f); break;
    case IC_PLUS: quad(x - s, y - s * 0.25f, x + s, y + s * 0.25f); quad(x - s * 0.25f, y - s, x + s * 0.25f, y + s); break;
    case IC_ROOF: tri(x, y - s, x - s, y, x + s, y); quad(x - s * 0.6f, y, x + s * 0.6f, y + s * 0.8f); break;
    case IC_CROSS: {
        float t = s * 0.22f;
        tri(x - s - t, y - s + t, x - s + t, y - s - t, x + s + t, y + s - t); tri(x - s - t, y - s + t, x + s + t, y + s - t, x + s - t, y + s + t);
        tri(x + s - t, y - s - t, x + s + t, y - s + t, x - s + t, y + s + t); tri(x + s - t, y - s - t, x - s + t, y + s + t, x - s - t, y + s - t);
        break;
    }
    }
}

/* 3x5 pixel font for the readout: each glyph is 5 rows of 3 bits */
static const char *FONT_CH = "0123456789.FPSGAMEWLC:UNIT";
static const unsigned char FONT[][5] = {
    {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},{7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},{7,5,7,5,7},{7,5,7,1,7},
    {0,0,0,0,2},{7,4,6,4,4},{7,5,7,4,4},{7,4,7,1,7},{7,4,5,5,7},{7,5,7,5,5},{5,7,7,5,5},{7,4,6,4,7},{5,5,7,7,5},{4,4,4,4,7},
    {7,4,4,4,7},{0,2,0,2,0},{5,5,5,5,7},{5,7,7,7,5},{7,2,2,2,7},{7,2,2,2,2},
};
static void text(float x, float y, float px, const char *str) {
    for (const char *c = str; *c; c++, x += px * 4) {
        const char *f = strchr(FONT_CH, *c);
        if (!f || !*c) continue;
        const unsigned char *g = FONT[f - FONT_CH];
        for (int r = 0; r < 5; r++) for (int b = 0; b < 3; b++)
            if (g[r] & (4 >> b)) quad(x + b * px, y + r * px, x + (b + 1) * px, y + (r + 1) * px);
    }
}

void touch_draw(void) {
    if (!visible && !perf_on) return;
    if (!prog) {
        prog = glCreateProgram();
        glAttachShader(prog, sh(GL_VERTEX_SHADER, VS)); glAttachShader(prog, sh(GL_FRAGMENT_SHADER, FS));
        glLinkProgram(prog);
        u_sc = glGetUniformLocation(prog, "sc");
        glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo);
        glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 24, 0);
        glEnableVertexAttribArray(1); glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 24, (void *)8);
    }
    layout();
    int w, h; plat_drawable_size(&w, &h);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(1, 1, 1, 1);
    glUseProgram(prog);
    glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glUniform2f(u_sc, 2.0f / scr_a, 2.0f);
    int race = glide_in_3d();
    if (perf_on) {
        float fps, game, gl, sw; glide_perf(&fps, &game, &gl, &sw);
        char line[96];
        char cap[16]; if (plat_fps_cap()) snprintf(cap, sizeof cap, "%d", plat_fps_cap()); else snprintf(cap, sizeof cap, "UNLIMIT");
        snprintf(line, sizeof line, "FPS %.0f  CAP %s  GAME %.1f  GL %.1f  SWAP %.1f", fps, cap, game, gl, sw);
        quad(0.01f, 0.005f, 0.01f + strlen(line) * 0.016f + 0.01f, 0.06f); flushdraw(0, 0, 0, 0.6f);
        text(0.02f, 0.015f, 0.008f, line); flushdraw(1, 1, 0.3f, 1);
    }
    for (int i = 0; i < NB && visible; i++) {
        const Btn *b = &btns[i];
        if (b->race_only && !race) continue;
        float a = held[i] ? 0.55f : 0.28f;
        disc(b->x, b->y, b->r, 0); flushdraw(0.05f, 0.05f, 0.05f, a * 0.8f);
        disc(b->x, b->y, b->r, b->r * 0.9f); flushdraw(b->col[0], b->col[1], b->col[2], a + 0.25f);
        icon(b); flushdraw(b->col[0], b->col[1], b->col[2], a + 0.35f);
    }
    if (nbatch) {
        glBufferData(GL_ARRAY_BUFFER, nbatch * 4, batch, GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, nbatch / 6);
        nbatch = 0;
    }
    glDisable(GL_BLEND);
}
