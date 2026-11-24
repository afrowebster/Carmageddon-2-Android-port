/* 3dfx Glide 2.x on OpenGL ES 3.
 *
 * The game's BRender driver (hardware_3dfx.bdd) talks to glide2x.dll. This file is that DLL:
 *  - triangles are batched and drawn with one shader that implements Glide's colour/alpha
 *    combine units, texture combine (one TMU), chroma key, alpha test, table fog and W/Z buffering
 *  - TMU memory is emulated as a byte array; textures are decoded to RGBA8 on first use and cached
 *    by (address, format, size, palette generation)
 *  - the frame is rendered into an FBO at a multiple of the game resolution, then scaled to the
 *    screen; linear-frame-buffer writes are overlaid as a texture (unwritten pixels keep the 3D image)
 */
#include <math.h>
#include "win32.h"
#include "../platform/plat.h"

/* ------------------------------------------------------------------ state */
#define TMU_MEM (4u << 20)
#define MAXV 65536
#define VBO_BYTES (8u << 20)

typedef struct { float x, y, z, r, g, b, ooz, a, oow, sow, tow, oow0; } GVtx;   /* first 12 floats of GrVertex */
typedef struct { float x, y, d, oow, r, g, b, a, s, t, q, rawoow; } HVtx;   /* rawoow: the vertex's 1/w as the game gave it */

typedef struct {
    uint32_t addr; int fmt, small_lod, large_lod, aspect;
    uint32_t palgen;
    GLuint tex; int w, h;
    uint32_t last_use;
    GLint p_min, p_mag, p_ws, p_wt;   /* sampler state last applied to this texture */
} TexEnt;
#define NTEX 2048

static struct {
    int inited, open;
    int gw, gh;                  /* game resolution */
    int scale, fw, fh;           /* FBO size */
    GLuint fbo, fbtex, fbdepth;
    GLuint prog, vao, vbo;
    GLuint progs[2];   /* [0] without discard (keeps the GPU's early depth / hidden-surface removal), [1] with alpha test + chroma key */
    GLuint blit_prog, blit_vao, blit_vbo;
    GLuint lfbtex; uint16_t *lfbbuf; uint16_t *lfbfill;
    GLuint fogtex;
    int color_fmt;
    /* combine */
    int cc_func, cc_fact, cc_local, cc_other, cc_inv;
    int ac_func, ac_fact, ac_local, ac_other, ac_inv;
    int tc_mode;                 /* 0 zero, 1 one, 2 texel */
    float constant[4];
    /* raster ops */
    int blend_s, blend_d, blend_as, blend_ad;
    int depth_mode, depth_func, depth_mask;
    int alpha_func; float alpha_ref;
    int cull;
    int color_mask, alpha_mask;
    int chroma_on; uint32_t chroma;
    int fog_mode; float fog_color[3]; uint8_t fogtab[64]; int fog_dirty;
    int filter_min, filter_mag, clamp_s, clamp_t, mipmap;
    /* textures */
    uint8_t *tmu;
    uint32_t pal[256]; uint32_t palgen;
    uint32_t src_addr; int src_fmt, src_small, src_large, src_aspect; int src_valid;
    TexEnt tc[NTEX]; uint32_t tick;
    TexEnt *cur;
    /* batch */
    HVtx v[MAXV]; int nv; GLenum prim;
    uint32_t frames;
    int dirty;
    uint32_t stw_hint;
    int tri3d, in3d;
    int frame_3d;
    /* a 3D-scene batch has been drawn in this frame (what comes before it is backdrop) */
    size_t vbo_off;
    uint32_t st_draws, st_uploads, st_downloads, st_pal, st_tris;   /* per-second statistics */
    /* widescreen: the render target is vw x gh game pixels; 2D is drawn 4:3 in the middle (xoff),
       the race's 3D scene (rendered by the game with a wider camera aspect) is stretched by wide_k */
    int vw; float xoff, wide_k; int race_wide;
} G;

/* ------------------------------------------------------------------ shaders */
static const char *VS =
"#version 300 es\n"
"precision highp float;\n"
"layout(location=0) in vec4 a_pos;\n"     /* x, y, depth(0..1), oow */
"layout(location=1) in vec4 a_col;\n"
"layout(location=2) in vec3 a_tex;\n"     /* s, t, vertex 1/w (fog) */
"uniform vec2 u_scr; uniform vec2 u_xf;\n"
"out vec4 v_col; out vec2 v_tex; out float v_w;\n"
"void main() {\n"
"  float w = a_pos.w > 0.0 ? 1.0 / a_pos.w : 1.0;\n"
"  vec2 n = vec2((a_pos.x * u_xf.x + u_xf.y) / u_scr.x * 2.0 - 1.0, 1.0 - a_pos.y / u_scr.y * 2.0);\n"
"  gl_Position = vec4(n * w, (a_pos.z * 2.0 - 1.0) * w, w);\n"
"  v_col = a_col / 255.0; v_tex = a_tex.xy; v_w = a_tex.z > 0.0 ? 1.0 / a_tex.z : w;\n"
"}\n";

static const char *FS =
"#version 300 es\n"
"precision highp float;\n"
"in vec4 v_col; in vec2 v_tex; in float v_w;\n"
"uniform sampler2D u_tex; uniform sampler2D u_fogt;\n"
"uniform ivec4 u_cc; uniform int u_ccinv;\n"   /* func, factor, local, other */
"uniform ivec4 u_ac; uniform int u_acinv;\n"
"uniform int u_tc; uniform vec2 u_tscale;\n"
"uniform vec4 u_const;\n"
"uniform int u_afunc; uniform float u_aref;\n"
"uniform int u_chroma; uniform vec3 u_ckey;\n"
"uniform int u_fog; uniform vec3 u_fogc;\n"
"out vec4 o;\n"
"vec3 cfactor(int f, vec3 loc, float loca, float otha, float texa) {\n"
"  if (f == 0) return vec3(0.0); if (f == 1) return loc; if (f == 2) return vec3(otha); if (f == 3) return vec3(loca);\n"
"  if (f == 4) return vec3(texa); if (f == 8) return vec3(1.0); if (f == 9) return 1.0 - loc;\n"
"  if (f == 10) return vec3(1.0 - otha); if (f == 11) return vec3(1.0 - loca); if (f == 12) return vec3(1.0 - texa);\n"
"  return vec3(0.0); }\n"
"vec3 comb(int fn, vec3 f, vec3 loc, float loca, vec3 oth) {\n"
"  if (fn == 0) return vec3(0.0); if (fn == 1) return loc; if (fn == 2) return vec3(loca);\n"
"  if (fn == 3) return f * oth; if (fn == 4) return f * oth + loc; if (fn == 5) return f * oth + loca;\n"
"  if (fn == 6) return f * (oth - loc); if (fn == 7) return f * (oth - loc) + loc; if (fn == 8) return f * (oth - loc) + loca;\n"
"  if (fn == 9) return f * (-loc) + loc; if (fn == 16) return f * (-loc) + loca;\n"
"  return loc; }\n"
"bool cmpf(int fn, float a, float b) {\n"
"  if (fn == 0) return false; if (fn == 1) return a < b; if (fn == 2) return a == b; if (fn == 3) return a <= b;\n"
"  if (fn == 4) return a > b; if (fn == 5) return a != b; if (fn == 6) return a >= b; return true; }\n"
"void main() {\n"
"  vec4 vc = clamp(v_col, 0.0, 1.0);\n"
"  vec4 tx = vec4(0.0);\n"
"  if (u_tc == 1) tx = vec4(1.0); else if (u_tc == 2) tx = texture(u_tex, v_tex * u_tscale);\n"
"#ifdef ALLOW_DISCARD\n"
"  if (u_chroma != 0 && u_tc == 2 && all(lessThan(abs(tx.rgb - u_ckey), vec3(0.5 / 255.0)))) discard;\n"
"#endif\n"
"  vec3 cl = u_cc.z == 1 ? u_const.rgb : vc.rgb; float cla = u_ac.z == 1 ? u_const.a : vc.a;\n"
"  vec3 co = u_cc.w == 1 ? tx.rgb : (u_cc.w == 2 ? u_const.rgb : vc.rgb);\n"
"  float aoth = u_ac.w == 1 ? tx.a : (u_ac.w == 2 ? u_const.a : vc.a);\n"
"  float aloc = u_ac.z == 1 ? u_const.a : vc.a;\n"
"  float coth_a = u_cc.w == 1 ? tx.a : (u_cc.w == 2 ? u_const.a : vc.a);\n"
"  vec3 rgb = comb(u_cc.x, cfactor(u_cc.y, cl, aloc, coth_a, tx.a), cl, aloc, co);\n"
"  float a = comb(u_ac.x, cfactor(u_ac.y, vec3(aloc), aloc, aoth, tx.a), vec3(aloc), aloc, vec3(aoth)).x;\n"
"  if (u_ccinv != 0) rgb = 1.0 - rgb; if (u_acinv != 0) a = 1.0 - a;\n"
"  rgb = clamp(rgb, 0.0, 1.0); a = clamp(a, 0.0, 1.0);\n"
"#ifdef ALLOW_DISCARD\n"
"  if (!cmpf(u_afunc, floor(a * 255.0 + 0.5), u_aref)) discard;\n"
"#endif\n"
"  if (u_fog == 1) {\n"
"    float w = max(v_w, 1.0); float k = floor(log2(w)); float m = w / exp2(k);\n"
"    float idx = clamp(4.0 * k + 8.0 - 8.0 / m, 0.0, 63.0);\n"
"    float f = texture(u_fogt, vec2((idx + 0.5) / 64.0, 0.5)).r;\n"
"    rgb = mix(rgb, u_fogc, f);\n"
"  } else if (u_fog == 2) rgb = mix(rgb, u_fogc, vc.a);\n"
"  o = vec4(rgb, a);\n"
"}\n";

static const char *BVS =
"#version 300 es\n"
"layout(location=0) in vec4 a; out vec2 t;\n"
"void main() { gl_Position = vec4(a.xy, 0.0, 1.0); t = a.zw; }\n";
static const char *BFS =
"#version 300 es\n"
"precision mediump float; in vec2 t; uniform sampler2D s; uniform int u_key; out vec4 o;\n"
"void main() { vec4 c = texture(s, t); if (u_key != 0 && c.a < 0.5) discard; o = vec4(c.rgb, 1.0); }\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[4096]; glGetShaderInfoLog(s, sizeof log, NULL, log); rt_fatal("shader: %s", log); }
    return s;
}
static GLuint link(const char *vs, const char *fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p);
    GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[4096]; glGetProgramInfoLog(p, sizeof log, NULL, log); rt_fatal("link: %s", log); }
    return p;
}
/* uniform location, looked up once per call site (lookups by name are slow on some GL drivers) */
typedef struct { GLint scr, xf, cc, ccinv, ac, acinv, cnst, afunc, aref, chroma, ckey, fog, fogc, tex, fogt, tc, tscale; } Loc;
static Loc L[2];
#define U(p, n) ({ static GLint _l = -2; static GLuint _p; if (_l == -2 || _p != (p)) { _l = glGetUniformLocation(p, n); _p = (p); } _l; })

/* ------------------------------------------------------------------ helpers */
static void unpack_color(uint32_t c, float *out) {
    float a, r, g, b;
    switch (G.color_fmt) {
    case 1: a = c >> 24; b = (c >> 16) & 255; g = (c >> 8) & 255; r = c & 255; break;   /* ABGR */
    case 2: r = c >> 24; g = (c >> 16) & 255; b = (c >> 8) & 255; a = c & 255; break;   /* RGBA */
    case 3: b = c >> 24; g = (c >> 16) & 255; r = (c >> 8) & 255; a = c & 255; break;   /* BGRA */
    default: a = c >> 24; r = (c >> 16) & 255; g = (c >> 8) & 255; b = c & 255; break;  /* ARGB */
    }
    out[0] = r / 255.f; out[1] = g / 255.f; out[2] = b / 255.f; out[3] = a / 255.f;
}

static void flush(void);
static void gs_reset(void);
static void submit(void);
/* record pending batch and draw everything recorded so far */
static void sync_gl(void) { flush(); submit(); }
void glide_screenshot(const char *path);
/* C2_PROF: log frame stages that take longer than 20 ms */
static int prof = -1;
/* frame timing for the on-screen performance readout */
#include <time.h>
static inline uint64_t now_us(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000; }
static uint64_t perf_gl_us, perf_swap_us;            /* accumulated this frame */
static float perf_fps, perf_game_ms, perf_gl_ms, perf_swap_ms;  /* smoothed, for display */
void glide_perf(float *fps, float *game, float *gl, float *swap) { *fps = perf_fps; *game = perf_game_ms; *gl = perf_gl_ms; *swap = perf_swap_ms; }
void rt_prof_report_reset(void);
static uint32_t prof_t(void) { return plat_ticks(); }

static void setup_fbo(void) {
    int dw, dh; plat_drawable_size(&dw, &dh);
    int s = dh / G.gh; if (s < 1) s = 1; if (s > 4) s = 4;
    if (getenv("C2_SCALE")) s = atoi(getenv("C2_SCALE"));
    /* fill the screen: the target is as wide as the display's aspect ratio (never narrower than 4:3) */
    float asp = plat_screen_aspect();
    if (getenv("C2_ASPECT")) asp = (float)atof(getenv("C2_ASPECT"));
    float ga = (float)G.gw / G.gh;
    if (asp < ga) asp = ga;
    if (asp > 2.4f) asp = 2.4f;
    G.vw = ((int)(G.gh * asp + 0.5f) + 1) & ~1;
    G.xoff = (G.vw - G.gw) / 2.0f;
    G.wide_k = (float)G.vw / G.gw;
    G.scale = s; G.fw = G.vw * s; G.fh = G.gh * s;
    if (!G.fbo) { glGenFramebuffers(1, &G.fbo); glGenTextures(1, &G.fbtex); glGenRenderbuffers(1, &G.fbdepth); }
    glBindTexture(GL_TEXTURE_2D, G.fbtex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, G.fw, G.fh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindRenderbuffer(GL_RENDERBUFFER, G.fbdepth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, G.fw, G.fh);
    glBindFramebuffer(GL_FRAMEBUFFER, G.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, G.fbtex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, G.fbdepth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) rt_fatal("FBO incomplete");
    glViewport(0, 0, G.fw, G.fh);
    glClearColor(0, 0, 0, 1); glClearDepthf(1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    LOG("glide: game %dx%d, render target %dx%d", G.gw, G.gh, G.fw, G.fh);
}

static void gl_init(void) {
    if (G.inited) return;
    G.inited = 1;
    plat_create_gl(G.gw, G.gh);
    {
        /* the fragment shader's version line must come first: insert the define after it */
        const char *nl = strchr(FS, '\n') + 1;
        size_t n1 = nl - FS;
        char *fs1 = malloc(strlen(FS) + 64);
        memcpy(fs1, FS, n1); strcpy(fs1 + n1, "#define ALLOW_DISCARD 1\n"); strcat(fs1, nl);
        G.progs[0] = link(VS, FS);
        G.progs[1] = link(VS, fs1);
        free(fs1);
        for (int i = 0; i < 2; i++) {
            GLuint p = G.progs[i];
            Loc *l = &L[i];
            l->scr = glGetUniformLocation(p, "u_scr"); l->xf = glGetUniformLocation(p, "u_xf");
            l->cc = glGetUniformLocation(p, "u_cc"); l->ccinv = glGetUniformLocation(p, "u_ccinv");
            l->ac = glGetUniformLocation(p, "u_ac"); l->acinv = glGetUniformLocation(p, "u_acinv");
            l->cnst = glGetUniformLocation(p, "u_const"); l->afunc = glGetUniformLocation(p, "u_afunc");
            l->aref = glGetUniformLocation(p, "u_aref"); l->chroma = glGetUniformLocation(p, "u_chroma");
            l->ckey = glGetUniformLocation(p, "u_ckey"); l->fog = glGetUniformLocation(p, "u_fog");
            l->fogc = glGetUniformLocation(p, "u_fogc"); l->tex = glGetUniformLocation(p, "u_tex");
            l->fogt = glGetUniformLocation(p, "u_fogt"); l->tc = glGetUniformLocation(p, "u_tc");
            l->tscale = glGetUniformLocation(p, "u_tscale");
        }
        G.prog = G.progs[0];
    }
    glGenVertexArrays(1, &G.vao); glBindVertexArray(G.vao);
    glGenBuffers(1, &G.vbo); glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
    glBufferData(GL_ARRAY_BUFFER, VBO_BYTES, NULL, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(HVtx), (void *)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(HVtx), (void *)16);
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(HVtx), (void *)32);
    G.blit_prog = link(BVS, BFS);
    glGenVertexArrays(1, &G.blit_vao); glBindVertexArray(G.blit_vao);
    glGenBuffers(1, &G.blit_vbo); glBindBuffer(GL_ARRAY_BUFFER, G.blit_vbo);
    glBufferData(GL_ARRAY_BUFFER, 16 * sizeof(float), NULL, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 16, (void *)0);
    glGenTextures(1, &G.fogtex);
    glBindTexture(GL_TEXTURE_2D, G.fogtex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 64, 1, 0, GL_RED, GL_UNSIGNED_BYTE, G.fogtab);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glGenTextures(1, &G.lfbtex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
}

/* ------------------------------------------------------------------ textures */
static const int aspect_w[7] = { 8, 4, 2, 1, 1, 1, 1 }, aspect_h[7] = { 1, 1, 1, 1, 2, 4, 8 };
static void lod_dims(int lod, int aspect, int *w, int *h) {
    int big = 256 >> lod;
    *w = big; *h = big;
    if (aspect < 3) { *h = big / aspect_w[aspect]; if (*h < 1) *h = 1; }
    else if (aspect > 3) { *w = big / aspect_h[aspect]; if (*w < 1) *w = 1; }
}
static int fmt_bytes(int fmt) { return fmt >= 8 ? 2 : 1; }
static uint32_t mip_size(int small, int large, int aspect, int fmt) {
    uint32_t n = 0;
    for (int l = large; l <= small; l++) { int w, h; lod_dims(l, aspect, &w, &h); n += (uint32_t)w * h * fmt_bytes(fmt); }
    return n;
}

static void decode(const uint8_t *src, int fmt, int n, uint32_t *dst) {
    for (int i = 0; i < n; i++) {
        uint32_t r, g, b, a = 255;
        if (fmt_bytes(fmt) == 1) {
            uint8_t v = src[i];
            switch (fmt) {
            case 0: r = (v >> 5) * 255 / 7; g = ((v >> 2) & 7) * 255 / 7; b = (v & 3) * 255 / 3; break;
            case 2: r = g = b = 255; a = v; break;
            case 3: r = g = b = v; break;
            case 4: r = g = b = (v & 15) * 17; a = (v >> 4) * 17; break;
            case 5: { uint32_t p = G.pal[v]; r = (p >> 16) & 255; g = (p >> 8) & 255; b = p & 255; break; }
            default: r = g = b = v;
            }
        } else {
            uint16_t v = src[2 * i] | (src[2 * i + 1] << 8);
            switch (fmt) {
            case 8: { uint8_t q = v & 255; r = (q >> 5) * 255 / 7; g = ((q >> 2) & 7) * 255 / 7; b = (q & 3) * 255 / 3; a = v >> 8; break; }
            case 0xa: r = (v >> 11) * 255 / 31; g = ((v >> 5) & 63) * 255 / 63; b = (v & 31) * 255 / 31; break;
            case 0xb: r = ((v >> 10) & 31) * 255 / 31; g = ((v >> 5) & 31) * 255 / 31; b = (v & 31) * 255 / 31; a = (v >> 15) ? 255 : 0; break;
            case 0xc: a = (v >> 12) * 17; r = ((v >> 8) & 15) * 17; g = ((v >> 4) & 15) * 17; b = (v & 15) * 17; break;
            case 0xd: r = g = b = v & 255; a = v >> 8; break;
            case 0xe: { uint32_t p = G.pal[v & 255]; r = (p >> 16) & 255; g = (p >> 8) & 255; b = p & 255; a = v >> 8; break; }
            default: r = g = b = v & 255;
            }
        }
        dst[i] = r | (g << 8) | (b << 16) | (a << 24);
    }
}

static TexEnt *tex_get(void) {
    if (!G.src_valid) return NULL;
    int pal = G.src_fmt == 5 || G.src_fmt == 0xe;
    TexEnt *free_e = NULL, *lru = &G.tc[0];
    for (int i = 0; i < NTEX; i++) {
        TexEnt *e = &G.tc[i];
        if (!e->tex) { if (!free_e) free_e = e; continue; }
        if (e->addr == G.src_addr && e->fmt == G.src_fmt && e->large_lod == G.src_large && e->small_lod == G.src_small &&
            e->aspect == G.src_aspect && (!pal || e->palgen == G.palgen)) { e->last_use = G.tick; return e; }
        if (e->last_use < lru->last_use) lru = e;
    }
    TexEnt *e = free_e ? free_e : lru;
    G.st_uploads++;
    if (e->tex) submit();   /* recorded draws may still use the texture being replaced */
    if (!e->tex) glGenTextures(1, &e->tex);
    e->addr = G.src_addr; e->fmt = G.src_fmt; e->large_lod = G.src_large; e->small_lod = G.src_small;
    e->aspect = G.src_aspect; e->palgen = G.palgen; e->last_use = G.tick;
    glBindTexture(GL_TEXTURE_2D, e->tex);
    uint32_t off = G.src_addr;
    int levels = 0;
    for (int l = G.src_large; l <= G.src_small; l++, levels++) {
        int w, h; lod_dims(l, G.src_aspect, &w, &h);
        if (levels == 0) { e->w = w; e->h = h; }
        uint32_t n = (uint32_t)w * h;
        if (off + n * fmt_bytes(G.src_fmt) > TMU_MEM) break;
        uint32_t *buf = malloc(n * 4);
        decode(G.tmu + off, G.src_fmt, n, buf);
        glTexImage2D(GL_TEXTURE_2D, levels, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
        free(buf);
        off += n * fmt_bytes(G.src_fmt);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1);
    e->p_min = GL_NEAREST_MIPMAP_LINEAR; e->p_mag = GL_LINEAR; e->p_ws = e->p_wt = GL_REPEAT;   /* GL defaults */
    gs_reset();
    return e;
}

/* ------------------------------------------------------------------ drawing */
static const GLenum blendf_src[16] = { GL_ZERO, GL_SRC_ALPHA, GL_DST_COLOR, GL_DST_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
    GL_ONE_MINUS_DST_COLOR, GL_ONE_MINUS_DST_ALPHA, GL_ZERO, GL_ZERO, GL_ZERO, GL_ZERO, GL_ZERO, GL_ZERO, GL_ZERO, GL_SRC_ALPHA_SATURATE };
static const GLenum blendf_dst[16] = { GL_ZERO, GL_SRC_ALPHA, GL_SRC_COLOR, GL_DST_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
    GL_ONE_MINUS_SRC_COLOR, GL_ONE_MINUS_DST_ALPHA, GL_ZERO, GL_ZERO, GL_ZERO, GL_ZERO, GL_ZERO, GL_ZERO, GL_ZERO, GL_ZERO };
static const GLenum cmpf[8] = { GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS };

static int uses_texture(void) {
    return G.tc_mode == 2 && (G.cc_other == 1 || G.ac_other == 1 || G.cc_fact == 4 || G.cc_fact == 12 || G.ac_fact == 4 || G.ac_fact == 12);
}

/* GL state cache: mobile drivers are slow at redundant state changes, so only real changes are sent.
   gs_reset() must be called after code outside the batch path touches GL state. */
static struct {
    int valid;
    int blend; GLenum bs, bd, bas, bad;
    int depth; GLenum dfunc; int dmask;
    int cm, am;
    GLuint tex0;
    int pi;            /* current program (index into G.progs) */
} GS;
/* uniform values last sent to each program */
static struct {
    int valid;
    int u_cc[5], u_ac[5], u_afunc, u_chroma, u_fog, u_tc;
    float u_const[4], u_aref, u_ckey[3], u_fogc[3], u_tscale[2], u_xf[2];
} UC[2];
static void gs_reset(void) { GS.valid = 0; UC[0].valid = UC[1].valid = 0; }

#define SET_I(field, v, call) do { if (!GS.valid || GS.field != (v)) { GS.field = (v); call; } } while (0)
#define SET_U(field, v, call) do { if (!u->valid || u->field != (v)) { u->field = (v); call; } } while (0)

/* ---- recorded draws ----
   Mobile GL drivers stall when a vertex buffer is written while earlier draws may still read it, and
   this game issues hundreds of small draws per frame. So draws are recorded (vertices + a snapshot of
   the GL state they need) and submitted together: one buffer upload, then the draw calls. Anything
   that must see the drawn pixels or replaces a GL object in use (clears, frame-buffer overlays,
   texture replacement, read-backs, end of frame) submits first. */
typedef struct {
    int pi;
    GLuint tex; GLint minf, magf, ws, wt; float tscale[2];
    int tc, cc[5], ac[5], afunc, chroma, fog;
    float cnst[4], aref, ckey[3], fogc[3], xf[2];
    int blend; GLenum bs, bd, bas, bad;
    int depth; GLenum dfunc; int dmask, cm, am;
} DState;
typedef struct { DState st; GLenum prim; int first, count; int backdrop; float bx0, bx1; } Cmd;   /* backdrop: sky tiles, bx: their x range */
static Cmd *cmds; static int ncmds, capcmds;
static HVtx *fverts; static int nfverts, capfverts;
static void submit(void);

/* sampler parameters last set on each GL texture name */
#define TPMAX 65536
static GLint tp_min[TPMAX], tp_mag[TPMAX], tp_ws[TPMAX], tp_wt[TPMAX];

static void snapshot(DState *d) {
    memset(d, 0, sizeof *d);
    int tc = G.tc_mode;
    TexEnt *e = NULL;
    if (tc == 2) { e = uses_texture() ? tex_get() : NULL; if (!e) tc = 0; }
    d->tc = tc;
    d->pi = (G.alpha_func != 7 || (G.chroma_on && tc == 2)) ? 1 : 0;
    if (e) {
        d->tex = e->tex;
        int mx = e->w > e->h ? e->w : e->h;
        d->tscale[0] = 1.0f / (256.0f * e->w / mx); d->tscale[1] = 1.0f / (256.0f * e->h / mx);
        d->minf = G.filter_min ? GL_LINEAR : GL_NEAREST;
        if (G.mipmap && e->small_lod > e->large_lod) d->minf = G.filter_min ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
        d->magf = G.filter_mag ? GL_LINEAR : GL_NEAREST;
        d->ws = G.clamp_s ? GL_CLAMP_TO_EDGE : GL_REPEAT; d->wt = G.clamp_t ? GL_CLAMP_TO_EDGE : GL_REPEAT;
    }
    int cc[5] = {G.cc_func, G.cc_fact, G.cc_local, G.cc_other, G.cc_inv}; memcpy(d->cc, cc, sizeof cc);
    int ac[5] = {G.ac_func, G.ac_fact, G.ac_local, G.ac_other, G.ac_inv}; memcpy(d->ac, ac, sizeof ac);
    memcpy(d->cnst, G.constant, sizeof d->cnst);
    d->afunc = G.alpha_func; d->aref = G.alpha_ref;
    d->chroma = G.chroma_on;
    if (G.chroma_on) { float ck[4]; unpack_color(G.chroma, ck); memcpy(d->ckey, ck, 12); }
    d->fog = G.fog_mode; memcpy(d->fogc, G.fog_color, 12);
    d->blend = !(G.blend_s == 4 && G.blend_d == 0);
    if (d->blend) { d->bs = blendf_src[G.blend_s & 15]; d->bd = blendf_dst[G.blend_d & 15]; d->bas = blendf_src[G.blend_as & 15]; d->bad = blendf_dst[G.blend_ad & 15]; }
    d->depth = G.depth_mode != 0;
    d->dfunc = cmpf[G.depth_func & 7];
    d->dmask = G.depth_mode && G.depth_mask;
    d->cm = G.color_mask; d->am = G.alpha_mask;
}

static void apply(const DState *d) {
    if (!GS.valid) {
        glBindFramebuffer(GL_FRAMEBUFFER, G.fbo);
        glViewport(0, 0, G.fw, G.fh);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, G.fogtex);
        glActiveTexture(GL_TEXTURE0);
        glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
        glBindVertexArray(G.vao);
        glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
        memset(&GS, 0, sizeof GS);
        GS.tex0 = (GLuint)-1; GS.pi = -1;
        UC[0].valid = UC[1].valid = 0;
    }
    int pi = d->pi;
    if (GS.pi != pi) { glUseProgram(G.progs[pi]); GS.pi = pi; G.prog = G.progs[pi]; }
    Loc *l = &L[pi];
    typeof(&UC[0]) u = &UC[pi];
    if (!u->valid) {
        glUniform2f(l->scr, (float)G.vw, (float)G.gh);
        glUniform1i(l->tex, 0); glUniform1i(l->fogt, 1);
        u->u_xf[0] = -1e30f;
    }
    if (!u->valid || memcmp(u->u_cc, d->cc, sizeof d->cc)) { memcpy(u->u_cc, d->cc, sizeof d->cc); glUniform4i(l->cc, d->cc[0], d->cc[1], d->cc[2], d->cc[3]); glUniform1i(l->ccinv, d->cc[4]); }
    if (!u->valid || memcmp(u->u_ac, d->ac, sizeof d->ac)) { memcpy(u->u_ac, d->ac, sizeof d->ac); glUniform4i(l->ac, d->ac[0], d->ac[1], d->ac[2], d->ac[3]); glUniform1i(l->acinv, d->ac[4]); }
    if (!u->valid || memcmp(u->u_const, d->cnst, 16)) { memcpy(u->u_const, d->cnst, 16); glUniform4fv(l->cnst, 1, d->cnst); }
    if (pi == 1) {
        SET_U(u_afunc, d->afunc, glUniform1i(l->afunc, d->afunc));
        SET_U(u_aref, d->aref, glUniform1f(l->aref, d->aref));
        SET_U(u_chroma, d->chroma, glUniform1i(l->chroma, d->chroma));
        if (d->chroma && (!u->valid || memcmp(u->u_ckey, d->ckey, 12))) { memcpy(u->u_ckey, d->ckey, 12); glUniform3f(l->ckey, d->ckey[0], d->ckey[1], d->ckey[2]); }
    }
    SET_U(u_fog, d->fog, glUniform1i(l->fog, d->fog));
    if (d->fog && (!u->valid || memcmp(u->u_fogc, d->fogc, 12))) { memcpy(u->u_fogc, d->fogc, 12); glUniform3fv(l->fogc, 1, d->fogc); }
    if (d->tc == 2) {
        if (GS.tex0 != d->tex) { glBindTexture(GL_TEXTURE_2D, d->tex); GS.tex0 = d->tex; }
        if (!u->valid || memcmp(u->u_tscale, d->tscale, 8)) { memcpy(u->u_tscale, d->tscale, 8); glUniform2f(l->tscale, d->tscale[0], d->tscale[1]); }
        GLuint t = d->tex;
        if (t >= TPMAX || tp_min[t] != d->minf) { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, d->minf); if (t < TPMAX) tp_min[t] = d->minf; }
        if (t >= TPMAX || tp_mag[t] != d->magf) { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, d->magf); if (t < TPMAX) tp_mag[t] = d->magf; }
        if (t >= TPMAX || tp_ws[t] != d->ws) { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, d->ws); if (t < TPMAX) tp_ws[t] = d->ws; }
        if (t >= TPMAX || tp_wt[t] != d->wt) { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, d->wt); if (t < TPMAX) tp_wt[t] = d->wt; }
    }
    SET_U(u_tc, d->tc, glUniform1i(l->tc, d->tc));
    if (memcmp(u->u_xf, d->xf, 8)) { memcpy(u->u_xf, d->xf, 8); glUniform2f(l->xf, d->xf[0], d->xf[1]); }
    u->valid = 1;
    int v = GS.valid;
    SET_I(blend, d->blend, (d->blend ? glEnable : glDisable)(GL_BLEND));
    if (d->blend && (!v || GS.bs != d->bs || GS.bd != d->bd || GS.bas != d->bas || GS.bad != d->bad)) {
        GS.bs = d->bs; GS.bd = d->bd; GS.bas = d->bas; GS.bad = d->bad;
        glBlendFuncSeparate(d->bs, d->bd, d->bas, d->bad);
    }
    SET_I(depth, d->depth, (d->depth ? glEnable : glDisable)(GL_DEPTH_TEST));
    if (d->depth) SET_I(dfunc, d->dfunc, glDepthFunc(d->dfunc));
    SET_I(dmask, d->dmask, glDepthMask(d->dmask));
    if (!v || GS.cm != d->cm || GS.am != d->am) { GS.cm = d->cm; GS.am = d->am; glColorMask(d->cm, d->cm, d->cm, d->am); }
    GS.valid = 1;
}

/* upload the recorded vertices once and issue the recorded draws */
static void submit(void) {
    if (!ncmds) return;
    uint64_t t = now_us();
    if (!GS.valid) apply(&cmds[0].st);   /* binds the FBO, VAO and buffer */
    if (G.fog_dirty) {
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, G.fogtex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 64, 1, GL_RED, GL_UNSIGNED_BYTE, G.fogtab);
        glActiveTexture(GL_TEXTURE0);
        G.fog_dirty = 0;
    }
    /* the sky tiles don't always span the whole 4:3 width (their positions follow the camera); map their
       combined x range onto the full widescreen width so no stale pixels remain at the screen sides */
    {
        float ux0 = 1e9f, ux1 = -1e9f;
        for (int i = 0; i < ncmds; i++) if (cmds[i].backdrop) { if (cmds[i].bx0 < ux0) ux0 = cmds[i].bx0; if (cmds[i].bx1 > ux1) ux1 = cmds[i].bx1; }
        if (ux1 - ux0 > G.gw * 0.5f) {
            float k = G.vw / (ux1 - ux0);
            for (int i = 0; i < ncmds; i++) if (cmds[i].backdrop) { cmds[i].st.xf[0] = k; cmds[i].st.xf[1] = -ux0 * k; }
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)nfverts * sizeof(HVtx), fverts, GL_STREAM_DRAW);
    for (int i = 0; i < ncmds; i++) {
        apply(&cmds[i].st);
        glDrawArrays(cmds[i].prim, cmds[i].first, cmds[i].count);
    }
    G.st_draws += ncmds;
    ncmds = 0; nfverts = 0;
    perf_gl_us += now_us() - t;
}

static int race_dump;   /* C2_RACEDUMP: log every draw of one race frame */
static void flush_(void);
static void flush(void) {
    if (!G.nv) return;
    uint64_t t = now_us();
    flush_();
    perf_gl_us += now_us() - t;
}
static void flush_(void) {
    if (race_dump == 1) {
        float x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9, w0 = 1e9, w1 = -1e9;
        for (int i = 0; i < G.nv; i++) { HVtx *v = &G.v[i]; if (v->x < x0) x0 = v->x; if (v->x > x1) x1 = v->x; if (v->y < y0) y0 = v->y; if (v->y > y1) y1 = v->y;
            if (v->oow < w0) w0 = v->oow; if (v->oow > w1) w1 = v->oow; }
        static int seq;
        LOG("RD #%d n=%d prim=%x depth=%d/%d/%d blend=%d/%d/%d/%d tc=%d cc=%d/%d/%d/%d/%d ac=%d/%d/%d/%d/%d tex=%x fmt=%d lod=%d-%d asp=%d chroma=%d fog=%d afn=%d/%.0f bbox=%.0f,%.0f-%.0f,%.0f oow=%g..%g stw=%x rgba0=%.0f,%.0f,%.0f,%.0f",
            seq, G.nv, G.prim, G.depth_mode, G.depth_func, G.depth_mask, G.blend_s, G.blend_d, G.blend_as, G.blend_ad, G.tc_mode,
            G.cc_func, G.cc_fact, G.cc_local, G.cc_other, G.cc_inv, G.ac_func, G.ac_fact, G.ac_local, G.ac_other, G.ac_inv,
            G.src_addr, G.src_fmt, G.src_large, G.src_small, G.src_aspect, G.chroma_on, G.fog_mode, G.alpha_func, G.alpha_ref,
            x0, y0, x1, y1, w0, w1, G.stw_hint, G.v[0].r, G.v[0].g, G.v[0].b, G.v[0].a);
        for (int i = 0; i < G.nv && i < 3; i++)
            LOG("RD    v%d x=%.1f y=%.1f rgba=%.1f,%.1f,%.1f,%.1f oow=%g s=%g t=%g const=%.2f,%.2f,%.2f,%.2f", i, G.v[i].x, G.v[i].y, G.v[i].r, G.v[i].g, G.v[i].b, G.v[i].a, G.v[i].oow, G.v[i].s, G.v[i].t,
                G.constant[0], G.constant[1], G.constant[2], G.constant[3]);
        if (G.tc_mode == 2 && G.src_valid) {
            /* dump the texture as decoded */
            int w, h; lod_dims(G.src_large, G.src_aspect, &w, &h);
            uint32_t *buf = malloc((size_t)w * h * 4);
            decode(G.tmu + G.src_addr, G.src_fmt, w * h, buf);
            char pth[1200]; snprintf(pth, sizeof pth, "%s/rd_%03d_tex%06x_f%d.ppm", g_save_dir, seq, G.src_addr, G.src_fmt);
            FILE *f = fopen(pth, "wb");
            if (f) { fprintf(f, "P6\n%d %d\n255\n", w, h);
                for (int i = 0; i < w * h; i++) { uint32_t px = (buf[i] >> 24) >= 128 ? buf[i] : ((buf[i] >> 24) ? 0x00800080u : 0x00ff00ffu); fwrite(&px, 1, 3, f); }
                fclose(f); }
            if (G.src_fmt >= 8) {   /* raw 16-bit texels too */
                snprintf(pth, sizeof pth, "%s/rd_%03d_tex%06x_f%d.raw", g_save_dir, seq, G.src_addr, G.src_fmt);
                FILE *r = fopen(pth, "wb"); if (r) { fwrite(G.tmu + G.src_addr, 2, w * h, r); fclose(r); }
            }
            free(buf);
        }
        seq++;
    }
    static int dbg = -1, ndbg;
    if (dbg < 0) dbg = getenv("C2_GDBG") ? atoi(getenv("C2_GDBG")) : 0;
    static int dfrom = -1;
    if (dfrom < 0) dfrom = getenv("C2_GDBG_FROM") ? atoi(getenv("C2_GDBG_FROM")) : 0;
    if (ndbg < dbg && (int)G.frames >= dfrom) {
        ndbg++;
        HVtx *v = G.v;
        LOG("draw n=%d prim=%x cc=%d/%d/%d/%d/%d ac=%d/%d/%d/%d/%d tc=%d tex=%x fmt=%d lod=%d-%d asp=%d blend=%d/%d depth=%d/%d/%d chroma=%d fog=%d const=%.2f,%.2f,%.2f,%.2f",
            G.nv, G.prim, G.cc_func, G.cc_fact, G.cc_local, G.cc_other, G.cc_inv, G.ac_func, G.ac_fact, G.ac_local, G.ac_other, G.ac_inv,
            G.tc_mode, G.src_addr, G.src_fmt, G.src_large, G.src_small, G.src_aspect, G.blend_s, G.blend_d, G.depth_mode, G.depth_func, G.depth_mask,
            G.chroma_on, G.fog_mode, G.constant[0], G.constant[1], G.constant[2], G.constant[3]);
        for (int i = 0; i < 3 && i < G.nv; i++)
            LOG("   v x=%.1f y=%.1f d=%.4f oow=%g rgba=%.0f,%.0f,%.0f,%.0f st=%.1f,%.1f", v[i].x, v[i].y, v[i].d, v[i].oow, v[i].r, v[i].g, v[i].b, v[i].a, v[i].s, v[i].t);
    }
    DState st;
    snapshot(&st);   /* may create a texture, which may submit pending draws first */
    if (ncmds == capcmds) { capcmds = capcmds ? capcmds * 2 : 1024; cmds = realloc(cmds, capcmds * sizeof *cmds); }
    Cmd *cm = &cmds[ncmds];
    cm->st = st;
    {
        int stretch = 0;
        if (G.race_wide) {
            if (G.stw_hint & 4) {                /* 3D scene: perspective hint set and real 1/w on every vertex */
                stretch = 1;                     /* (2D fills leave the hint alone and w unset) */
                for (int i = 0; i < G.nv; i++) if (!(G.v[i].rawoow > 1e-5f && G.v[i].rawoow < 1e4f)) { stretch = 0; break; }
            }
            if (stretch) G.frame_3d = 1;
            else if (!G.frame_3d && G.depth_mode && G.depth_mask)
                stretch = 2;                     /* backdrop: 2D tiles drawn before the 3D scene (the sky, in any number of batches) */
            else {                               /* full-width 2D: underwater tint, screen flashes */
                float x0 = 1e9f, x1 = -1e9f;
                for (int i = 0; i < G.nv; i++) { if (G.v[i].x < x0) x0 = G.v[i].x; if (G.v[i].x > x1) x1 = G.v[i].x; }
                stretch = x0 <= 1.0f && x1 >= G.gw - 1.0f;
            }
        }
        cm->st.xf[0] = stretch ? G.wide_k : 1.0f;
        cm->st.xf[1] = stretch ? G.vw / 2.0f - G.gw / 2.0f * G.wide_k : G.xoff;
        cm->backdrop = stretch == 2;
        if (cm->backdrop) {
            cm->bx0 = 1e9f; cm->bx1 = -1e9f;
            for (int i = 0; i < G.nv; i++) { if (G.v[i].x < cm->bx0) cm->bx0 = G.v[i].x; if (G.v[i].x > cm->bx1) cm->bx1 = G.v[i].x; }
        }
    }
    if (nfverts + G.nv > capfverts) { while (nfverts + G.nv > capfverts) capfverts = capfverts ? capfverts * 2 : 65536; fverts = realloc(fverts, capfverts * sizeof *fverts); }
    memcpy(fverts + nfverts, G.v, G.nv * sizeof(HVtx));
    cm->prim = G.prim; cm->first = nfverts; cm->count = G.nv;
    nfverts += G.nv; ncmds++;
    G.st_tris += G.nv / 3;
    G.nv = 0;
}
#define STATE() flush()

/* 1/w as the GPU sees it: garbage values (the driver leaves w unset on 2D quads) become 1 */
static inline float sane(float v) { return (v > 1e-6f && v < 1e6f) ? v : 1.0f; }
static void push_vtx(uint32_t gp) {
    const GVtx *g = (const GVtx *)GPTR(gp);
    HVtx *h = &G.v[G.nv++];
    h->x = g->x; h->y = g->y;
    float oow = g->oow;
    h->rawoow = oow;
    if (G.depth_mode == 2 || G.depth_mode == 4) h->d = 1.0f - (oow < 1.0f ? (oow > 0 ? oow : 0) : 1.0f);
    else { float z = g->ooz / 65535.0f; h->d = z < 0 ? 0 : z > 1 ? 1 : z; }
    /* perspective comes from the TMU's 1/w when the STW hint says it differs from the vertex's */
    float o0 = sane((G.stw_hint & 2) ? g->oow0 : oow);
    /* w only matters for perspective-correct texturing; untextured draws (HUD bars, panels) often
       carry garbage in it, which would wreck clipping precision */
    if (!(G.tc_mode == 2 && uses_texture())) o0 = 1.0f;
    h->oow = o0;
    h->r = g->r; h->g = g->g; h->b = g->b; h->a = g->a;
    h->s = g->sow / o0; h->t = g->tow / o0; h->q = sane(oow);
}

static void cull_and_push(uint32_t a, uint32_t b, uint32_t cc) {
    if (G.cull) {
        const float *va = GPTR(a), *vb = GPTR(b), *vc = GPTR(cc);
        float area = (vb[0] - va[0]) * (vc[1] - va[1]) - (vc[0] - va[0]) * (vb[1] - va[1]);
        if (G.cull == 1 && area < 0) return;   /* GR_CULL_NEGATIVE */
        if (G.cull == 2 && area > 0) return;   /* GR_CULL_POSITIVE */
    }
    if (G.prim != GL_TRIANGLES || G.nv + 3 > MAXV) { flush(); G.prim = GL_TRIANGLES; }
    if (G.depth_mode && G.depth_mask) G.tri3d++;
    push_vtx(a); push_vtx(b); push_vtx(cc);
}

/* ------------------------------------------------------------------ presentation */
static int lfb_dirty;
static void lfb_overlay(void);

/* Hor+ widescreen: give the race cameras a wider aspect. The game keeps its camera actors in two
   globals; each actor's camera data (BRender br_camera) is at +0x5c, its aspect at +0x10. */
static int widen_cameras(void) {
    extern uint32_t gheap_top(void);
    static const uint32_t globals[] = { 0x74c7b0, 0x74c7b4 };
    int n = 0;
    float want = (float)G.gw / G.gh * G.wide_k;
    for (int i = 0; i < 2; i++) {
        uint32_t actor = gr32(globals[i]);
        if (actor < GUEST_HEAP_LO || actor + 0x60 > gheap_top()) continue;
        uint32_t cam = gr32(actor + 0x5c);
        if (cam < GUEST_HEAP_LO || cam + 0x20 > gheap_top()) continue;
        float hither = grf(cam + 8), yon = grf(cam + 0xc), asp = grf(cam + 0x10);
        if (!(hither > 0 && hither < 10 && yon > hither && asp > 1.0f && asp < 4.0f)) continue;
        if (asp != want) {
            gwf(cam + 0x10, want);
            /* the sky model (a slice of a cylinder) is built for the camera's field of view and aspect,
               and rebuilt only when the field of view or yon changes: invalidate its cached field of
               view so the game rebuilds it for the wider view */
            gw16(0x67c4ac, 0xffff);
        }
        n++;
    }
    return n > 0;
}

static void present_(void);
static void present(void) {
    sync_gl();
    G.frame_3d = 0;
    present_();
}
static void present_(void) {
    lfb_overlay();
    /* NB: the depth buffer must survive into the next frame (no glInvalidateFramebuffer): the game
       doesn't always clear it, and the sky backdrop relies on last frame's depth values */
    int dw, dh; plat_drawable_size(&dw, &dh);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, dw, dh);
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glDepthMask(GL_TRUE);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    float sa = (float)dw / dh, ga = (float)G.vw / G.gh;
    float sx = 1, sy = 1;
    if (sa > ga) sx = ga / sa; else sy = sa / ga;
    float q[16] = { -sx, -sy, 0, 0,  sx, -sy, 1, 0,  -sx, sy, 0, 1,  sx, sy, 1, 1 };
    glUseProgram(G.blit_prog);
    glUniform1i(U(G.blit_prog, "u_key"), 0);
    glBindVertexArray(G.blit_vao); glBindBuffer(GL_ARRAY_BUFFER, G.blit_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof q, q);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, G.fbtex);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gs_reset();
    /* a frame with plenty of depth-written triangles is a race (vs. menus / videos) */
    G.in3d = G.tri3d > 40;
    G.race_wide = G.in3d && G.wide_k > 1.001f && widen_cameras();
    {
        static int env = -1, n3d;
        if (env < 0) env = getenv("C2_RACEDUMP") != NULL;
        if (race_dump == 1) {
            race_dump = 2; LOG("RD end of frame");
            { char pth[1200]; snprintf(pth, sizeof pth, "%s/rd_frame.ppm", g_save_dir); glide_screenshot(pth); }
            /* camera data blocks (aspect 4/3 at +0x10) and who points at them, two levels deep */
            extern uint32_t gheap_top(void);
            uint32_t ranges[2][2] = { { 0x58f000, 0x7a2000 }, { 0x30000000, gheap_top() } };
            uint32_t cams[8]; int nc = 0;
            for (uint32_t a = 0x30000000; a + 4 <= gheap_top() && nc < 8; a += 4) {
                float f = grf(a);
                if (f > 1.3333f && f < 1.33337f && grf(a - 4) > 1 && grf(a - 8) > 0 && grf(a - 8) < 1) cams[nc++] = a - 0x10;
            }
            for (int k = 0; k < nc; k++) {
                LOG("RD camera data %08x", cams[k]);
                for (int r = 0; r < 2; r++) for (uint32_t a = ranges[r][0]; a + 4 <= ranges[r][1]; a += 4) if (gr32(a) == cams[k]) {
                    LOG("RD   ptr at %08x", a);
                    for (int r2 = 0; r2 < 2; r2++) for (uint32_t a2 = ranges[r2][0]; a2 + 4 <= ranges[r2][1]; a2 += 4) {
                        uint32_t v = gr32(a2);
                        if (v <= a && a - v < 0x100) LOG("RD     ptr to %08x(+%x) at %08x", v, a - v, a2);
                    }
                }
            }
        }
        if (env && !race_dump && G.in3d && ++n3d == (getenv("C2_RACEDUMP_AT") ? atoi(getenv("C2_RACEDUMP_AT")) : 50)) { race_dump = 1; LOG("RD frame %u", G.frames); }
    }
    G.tri3d = 0;
    {
        { extern void plat_vclock_frame(void); plat_vclock_frame(); }
        {   /* C2_SCANVARS=frame: at that frame and the next few, log small integers in the game's globals that are the same every frame (frame-period candidates) */
            static int at = -1; if (at < 0) at = getenv("C2_SCANVARS") ? atoi(getenv("C2_SCANVARS")) : 0;
            static uint32_t snap[4][0x220000 / 4]; static int ns;
            if (at && (int)G.frames >= at && ns < 4) {
                for (uint32_t a = 0x58f000; a < 0x7a2000; a += 4) snap[ns][(a - 0x58f000) / 4] = gr32(a);
                if (++ns == 4) {
                    for (uint32_t a = 0x58f000; a < 0x7a2000; a += 4) {
                        uint32_t i = (a - 0x58f000) / 4;
                        uint32_t d1 = snap[1][i] - snap[0][i], d2 = snap[2][i] - snap[1][i], d3 = snap[3][i] - snap[2][i];
                        if (d1 >= 3 && d1 <= 40 && d2 >= 3 && d2 <= 40 && d3 >= 3 && d3 <= 40) LOG("SCAN %08x += %u %u %u (now %u)", a, d1, d2, d3, snap[3][i]);
                        uint32_t e1 = -d1, e2 = -d2, e3 = -d3;   /* decreasing (count-down timers) */
                        if (e1 >= 3 && e1 <= 40 && e2 >= 3 && e2 <= 40 && e3 >= 3 && e3 <= 40) LOG("SCAN %08x -= %u %u %u (now %d)", a, e1, e2, e3, (int)snap[3][i]);
                    }
                }
            }
        }
        {   /* C2_VSHOT=ms,ms,...: screenshots at these game-clock times (ms since start) */
            static const char *vs = (const char *)1; static int vi;
            if (vs == (const char *)1) vs = getenv("C2_VSHOT");
            if (vs) {
                const char *q = vs; for (int k = 0; k < vi && q; k++) { q = strchr(q, ','); if (q) q++; }
                if (q && *q && plat_ticks() - 100000 >= (uint32_t)atoi(q)) {
                    char p[1200]; snprintf(p, sizeof p, "%s/v_%06d.ppm", g_save_dir, atoi(q)); glide_screenshot(p); vi++;
                }
            }
        }
        uint64_t ts = now_us();
        plat_swap();
        uint64_t te = now_us();
        perf_swap_us += te - ts;
        /* per-frame split: game code (everything else), GL submission, swap (waiting for the GPU / vsync) */
        static uint64_t last;
        if (last) {
            float frame = (te - last) / 1000.0f, gl = perf_gl_us / 1000.0f, sw = perf_swap_us / 1000.0f;
            float a = 0.1f;
            perf_fps += a * (1000.0f / (frame > 0.1f ? frame : 0.1f) - perf_fps);
            perf_gl_ms += a * (gl - perf_gl_ms);
            perf_swap_ms += a * (sw - perf_swap_ms);
            perf_game_ms += a * (frame - gl - sw - perf_game_ms);
        }
        last = te; perf_gl_us = perf_swap_us = 0;
    }
    { static int slow = -1; if (slow < 0) slow = getenv("C2_SLOW") ? atoi(getenv("C2_SLOW")) : 0; if (slow) plat_sleep(slow); }  /* test: emulate a slow device */
    G.frames++;
    { static uint32_t last; uint32_t now = prof_t(); extern void rt_prof_report(void);
      if (prof > 0 && last && now - last > 200) { static int n; if (n++ < 50) { LOG("prof: frame %u took %u ms", G.frames, now - last); rt_prof_report(); } }
      last = now; }
    {
        static uint32_t t0, f0;
        uint32_t now = plat_ticks();
        if (!t0) { t0 = now; f0 = G.frames; }
        if (now - t0 >= 5000) {
            uint32_t nf = G.frames - f0 ? G.frames - f0 : 1;
            extern double rt_prof_api_ms(void); extern int g_prof_api;
            double api = g_prof_api ? rt_prof_api_ms() / nf : 0;
            LOG("fps %.1f (per frame: %u draws, %u tris, %u texture decodes, %u downloads, %u palette changes%s%.1f ms in APIs, %.1f ms total)",
                (G.frames - f0) * 1000.0 / (now - t0), G.st_draws / nf, G.st_tris / nf, G.st_uploads / nf, G.st_downloads / nf, G.st_pal / nf,
                g_prof_api ? "; " : "", api, (now - t0) / (double)nf);
            G.st_draws = G.st_tris = G.st_uploads = G.st_downloads = G.st_pal = 0;
            t0 = now; f0 = G.frames;
        }
    }
}

int glide_in_3d(void) { return G.in3d; }

/* window coordinates (0..1) -> game pixel coordinates; 0 when outside the game picture */
int glide_to_game(float nx, float ny, int *gx, int *gy) {
    if (!G.open) return 0;
    int dw, dh; plat_drawable_size(&dw, &dh);
    if (dw <= 0 || dh <= 0) return 0;
    float sa = (float)dw / dh, ga = (float)G.vw / G.gh, sx = 1, sy = 1;
    if (sa > ga) sx = ga / sa; else sy = sa / ga;
    float x = ((nx * 2 - 1) / sx + 1) / 2 * G.vw - G.xoff, y = ((ny * 2 - 1) / sy + 1) / 2;
    if (x < 0 || x >= G.gw || y < 0 || y >= 1) return 0;
    *gx = (int)x; *gy = (int)(y * G.gh);
    return 1;
}

void glide_screenshot(const char *path) {
    sync_gl();
    uint8_t *b = malloc((size_t)G.fw * G.fh * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, G.fbo);
    glReadPixels(0, 0, G.fw, G.fh, GL_RGBA, GL_UNSIGNED_BYTE, b);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", G.fw, G.fh);
        for (int y = G.fh - 1; y >= 0; y--) for (int x = 0; x < G.fw; x++) fwrite(b + ((size_t)y * G.fw + x) * 4, 1, 3, f);
        fclose(f);
    }
    free(b);
    LOG("screenshot %s", path);
}

/* ------------------------------------------------------------------ LFB */
/* Writes go into a 16-bit shadow buffer that is pre-filled with a sentinel; on swap the written
   pixels (those differing from the sentinel) are drawn over the 3D image. */
#define LFB_KEY 0xF81Fu   /* magenta */
#define LFB_STRIDE 1024   /* pixels per LFB row: the Voodoo's frame buffer is always 1024 pixels (2048 bytes) wide */
#define LFB_AT(x, y) (lfb_guest + ((uint32_t)(y) * LFB_STRIDE + (uint32_t)(x)) * 2)
static int lfb_locked_write;
static uint32_t lfb_guest;    /* guest copy of the buffer handed out by grLfbLock */
static int lfb_wmode;
static int lfb_bgr = 0;

static void lfb_ensure(void) {
    if (lfb_guest) return;
    lfb_guest = gmalloc(LFB_STRIDE * 1024 * 2 + 64);
    G.lfbbuf = malloc(G.gw * G.gh * 4);
    for (int i = 0; i < LFB_STRIDE * 1024; i++) gw16(lfb_guest + i * 2, LFB_KEY);
}
static void lfb_overlay_(void);
static void lfb_overlay(void) {
    if (prof < 0) prof = getenv("C2_PROF") != NULL;
    uint32_t t = prof ? prof_t() : 0;
    lfb_overlay_();
    if (prof > 0 && prof_t() - t > 20) { static int n; if (n++ < 50) LOG("prof: lfb_overlay %u ms", prof_t() - t); }
}
static void lfb_overlay_(void) {
    if (!lfb_dirty) return;
    sync_gl();
    lfb_dirty = 0;
    uint32_t *rgba = (uint32_t *)G.lfbbuf;
    /* convert the written pixels and find the rows that changed */
    int y0 = G.gh, y1 = -1;
    for (int y = 0; y < G.gh; y++) {
        uint32_t row = LFB_AT(0, y);
        uint32_t *dst = rgba + (size_t)y * G.gw;
        int rowany = 0;
        for (int x = 0; x < G.gw; x++) {
            uint16_t v = gr16(row + x * 2);
            if (v == LFB_KEY) { dst[x] = 0; continue; }
            rowany = 1;
            uint32_t r = (v >> 11) * 255 / 31, g = ((v >> 5) & 63) * 255 / 63, b = (v & 31) * 255 / 31;
            dst[x] = (lfb_bgr ? b | (r << 16) : r | (b << 16)) | (g << 8) | 0xff000000u;
            gw16(row + x * 2, LFB_KEY);
        }
        if (rowany) { if (y < y0) y0 = y; y1 = y; }
    }
    if (y1 < 0) return;
    if (race_dump == 1) LOG("RD LFB overlay rows %d-%d", y0, y1);
    /* the texture keeps its contents between frames: upload only the rows written now */
    static int tex_ready;
    glBindTexture(GL_TEXTURE_2D, G.lfbtex);
    if (!tex_ready) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, G.gw, G.gh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        tex_ready = 1;
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y0, G.gw, y1 - y0 + 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba + (size_t)y0 * G.gw);
    glBindFramebuffer(GL_FRAMEBUFFER, G.fbo);
    glViewport(0, 0, G.fw, G.fh);
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glColorMask(1, 1, 1, 1);
    glUseProgram(G.blit_prog);
    glUniform1i(U(G.blit_prog, "u_key"), 1);
    glBindVertexArray(G.blit_vao); glBindBuffer(GL_ARRAY_BUFFER, G.blit_vbo);
    /* draw only the changed rows. In a widescreen race the right third of this layer (the head-up
       map in the bottom-right corner) moves to the right edge of the screen; the rest (e.g. the
       opponent's condition bar, part of the centred lower HUD panel) stays where it is */
    float v0 = (float)y0 / G.gh, v1 = (float)(y1 + 1) / G.gh;
    float top = 1 - v0 * 2, bot = 1 - v1 * 2;
    int parts = G.race_wide ? 2 : 1;
    for (int k = 0; k < parts; k++) {
        float u0 = parts == 1 ? 0 : (k ? 2 / 3.0f : 0), u1 = parts == 1 ? 1 : (k ? 1 : 2 / 3.0f);
        float off = parts == 1 || !k ? G.xoff : 2 * G.xoff;
        float l = (off + u0 * G.gw) / G.vw * 2 - 1, r = (off + u1 * G.gw) / G.vw * 2 - 1;
        float q[16] = { l, top, u0, v0,  r, top, u1, v0,  l, bot, u0, v1,  r, bot, u1, v1 };
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof q, q);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    gs_reset();
}

/* ------------------------------------------------------------------ API */
#define GAPI(name, n) static uint32_t gl_##name(CPU *c)
#define GDEF(name, n) { "glide2x.dll", "_" #name "@" #n, gl_##name, n }

GAPI(grGlideInit, 0) {
    G.tmu = calloc(1, TMU_MEM);
    G.blend_s = 4; G.blend_d = 0; G.blend_as = 4; G.blend_ad = 0;
    G.color_mask = G.alpha_mask = 1; G.depth_mask = 1; G.alpha_func = 7;
    G.cc_func = 1; G.cc_local = 0; G.ac_func = 1; G.tc_mode = 2;
    G.filter_min = G.filter_mag = 1;
    LOG("grGlideInit");
    return 0;
}
GAPI(grGlideShutdown, 0) { return 0; }
GAPI(grErrorSetCallback, 4) { return 0; }
GAPI(grSstQueryHardware, 4) {
    uint32_t p = ARG(0);
    /* GrHwConfiguration: num_sst, then SSTs: type (0 = Voodoo), VoodooConfig {fbRam, fbiRev, nTexelfx, sliDetect, tmuConfig[2]{tmuRev, tmuRam}} */
    memset(GPTR(p), 0, 4 + 4 * 36);   /* GrHwConfiguration: num_sst + 4 x GrSstConfig (36 bytes) */
    gw32(p, 1);
    gw32(p + 4, 0);          /* GR_SSTTYPE_VOODOO */
    gw32(p + 8, 4);          /* fbRam MB */
    gw32(p + 12, 2);         /* fbiRev */
    gw32(p + 16, 1);         /* nTexelfx */
    gw32(p + 20, 0);         /* sliDetect */
    gw32(p + 24, 1); gw32(p + 28, 4);   /* tmu0 rev, ram MB */
    return 1;
}
GAPI(grSstSelect, 4) { return 0; }
static const int res_w[] = { 320, 320, 400, 512, 640, 640, 640, 640, 800, 960, 856, 512, 1024, 1280, 1600, 400, 1152, 1280, 1600 };
static const int res_h[] = { 200, 240, 256, 384, 200, 350, 400, 480, 600, 720, 480, 256, 768, 1024, 1200, 300, 864, 960, 1280 };
GAPI(grSstWinOpen, 28) {
    uint32_t res = ARG(1);
    G.color_fmt = ARG(3);
    G.gw = res < 19 ? res_w[res] : 640; G.gh = res < 19 ? res_h[res] : 480;
    LOG("grSstWinOpen(res=%u -> %dx%d, refresh=%u, cfmt=%u, origin=%u, bufs=%u, aux=%u)", res, G.gw, G.gh, ARG(2), ARG(3), ARG(4), ARG(5), ARG(6));
    gl_init();
    setup_fbo();
    G.open = 1;
    return 1;
}
GAPI(grBufferClear, 12) {
    sync_gl();
    G.frame_3d = 0;
    float col[4]; unpack_color(ARG(0), col);
    glBindFramebuffer(GL_FRAMEBUFFER, G.fbo);
    glViewport(0, 0, G.fw, G.fh);
    glColorMask(G.color_mask, G.color_mask, G.color_mask, G.alpha_mask);
    glDepthMask(G.depth_mask);
    glClearColor(col[0], col[1], col[2], (ARG(1) & 255) / 255.f);
    float d = (ARG(2) & 0xffff) / 65535.f;
    glClearDepthf(d);
    glClear(GL_COLOR_BUFFER_BIT | (G.depth_mask ? GL_DEPTH_BUFFER_BIT : 0));
    gs_reset();
    return 0;
}
GAPI(grBufferSwap, 4) {
    present();
    G.tick++;
    {
        static int every = -1;
        if (every < 0) every = getenv("C2_SHOT_EVERY") ? atoi(getenv("C2_SHOT_EVERY")) : 0;
        if (every > 0 && G.frames % every == 0) { char p[1200]; snprintf(p, sizeof p, "%s/shot_%05u.ppm", g_save_dir, G.frames); glide_screenshot(p); }
    }
    plat_pump_events();
    return 0;
}
GAPI(grBufferNumPending, 0) { return 0; }
GAPI(grRenderBuffer, 4) { return 0; }
GAPI(grColorMask, 8) { STATE(); G.color_mask = ARG(0) != 0; G.alpha_mask = ARG(1) != 0; return 0; }
GAPI(grDepthMask, 4) { int v = ARG(0) != 0; if (v != G.depth_mask) { STATE(); G.depth_mask = v; } return 0; }
GAPI(grDepthBufferMode, 4) { STATE(); G.depth_mode = ARG(0); return 0; }
GAPI(grDepthBufferFunction, 4) { STATE(); G.depth_func = ARG(0); return 0; }
GAPI(grCullMode, 4) { STATE(); G.cull = ARG(0); return 0; }
GAPI(grAlphaBlendFunction, 16) {
    if (G.blend_s == (int)ARG(0) && G.blend_d == (int)ARG(1) && G.blend_as == (int)ARG(2) && G.blend_ad == (int)ARG(3)) return 0;
    STATE(); G.blend_s = ARG(0); G.blend_d = ARG(1); G.blend_as = ARG(2); G.blend_ad = ARG(3); return 0;
}
GAPI(grAlphaTestReferenceValue, 4) { STATE(); G.alpha_ref = ARG(0) & 255; return 0; }
GAPI(grAlphaCombine, 20) {
    if (G.ac_func == (int)ARG(0) && G.ac_fact == (int)ARG(1) && G.ac_local == (int)ARG(2) && G.ac_other == (int)ARG(3) && G.ac_inv == (ARG(4) != 0)) return 0;
    STATE();
    G.ac_func = ARG(0); G.ac_fact = ARG(1); G.ac_local = ARG(2); G.ac_other = ARG(3); G.ac_inv = ARG(4) != 0;
    return 0;
}
static void color_combine(int fn, int fact, int loc, int oth, int inv) {
    if (G.cc_func == fn && G.cc_fact == fact && G.cc_local == loc && G.cc_other == oth && G.cc_inv == inv) return;
    flush();
    G.cc_func = fn; G.cc_fact = fact; G.cc_local = loc; G.cc_other = oth; G.cc_inv = inv;
}
GAPI(guColorCombineFunction, 4) {
    { static int seen[32]; if (ARG(0) < 32 && !seen[ARG(0)]++) LOG("guColorCombineFunction(%u) first use", ARG(0)); }
    enum { Z = 0, LOC = 1, LOCA = 2, SO = 3, SOAL = 4, SOALA = 5, SOML = 6, SOMLAL = 7 };
    enum { FZ = 0, FL = 1, FLA = 3, FTA = 4, F1 = 8 };
    enum { IT = 0, CO = 1 };
    enum { OIT = 0, OTX = 1, OCO = 2 };
    switch (ARG(0)) {
    case 0: color_combine(Z, FZ, IT, OIT, 0); break;
    case 1: color_combine(LOC, FZ, CO, OIT, 0); break;
    case 2: color_combine(LOC, FZ, IT, OIT, 0); break;
    /* the DELTA0 presets don't iterate colour: the SST-1 takes it from the constant colour register */
    case 3: color_combine(LOC, FZ, CO, OIT, 0); break;
    case 4: color_combine(SO, F1, IT, OTX, 0); break;
    case 5: color_combine(SO, FL, CO, OTX, 0); break;
    case 6: color_combine(SO, FL, IT, OTX, 0); break;
    case 7: color_combine(SO, FL, CO, OTX, 0); break;
    case 8: color_combine(SOALA, FL, IT, OTX, 0); break;
    case 9: color_combine(SO, FLA, IT, OTX, 0); break;
    case 10: color_combine(SOAL, FLA, IT, OTX, 0); break;
    case 11: color_combine(SOAL, F1, IT, OTX, 0); break;
    case 12: color_combine(SOML, F1, IT, OTX, 0); break;
    case 13: color_combine(SOMLAL, FTA, IT, OCO, 0); break;
    case 14: color_combine(SOALA, FL, IT, OTX, 0); break;
    case 15: color_combine(SOAL, FLA, IT, OTX, 0); break;
    case 16: color_combine(Z, FZ, IT, OIT, 1); break;
    default: LOG("guColorCombineFunction(%u)?", ARG(0));
    }
    return 0;
}
GAPI(grTexCombineFunction, 8) {
    int tc;
    switch (ARG(1)) { case 0: tc = 0; break; case 10: tc = 1; break; default: tc = 2; }
    if (tc != G.tc_mode) { STATE(); G.tc_mode = tc; }
    return 0;
}
/* state setters flush the pending batch only when the value really changes */
static void set_constant(const float *v) { if (memcmp(v, G.constant, sizeof G.constant)) { STATE(); memcpy(G.constant, v, sizeof G.constant); } }
GAPI(grConstantColorValue, 4) { float v[4]; unpack_color(ARG(0), v); set_constant(v); return 0; }
GAPI(grConstantColorValue4, 16) {
    float v[4] = { ARGF(1) / 255.f, ARGF(2) / 255.f, ARGF(3) / 255.f, ARGF(0) / 255.f };
    set_constant(v);
    return 0;
}
GAPI(grChromakeyMode, 4) { STATE(); G.chroma_on = ARG(0) != 0; return 0; }
GAPI(grChromakeyValue, 4) { STATE(); G.chroma = ARG(0); return 0; }
GAPI(grFogMode, 4) {
    uint32_t m = ARG(0) & 0xff;
    /* GR_FOG_WITH_ITERATED_ALPHA = 1, GR_FOG_WITH_TABLE = 2; shader: 1 = table, 2 = iterated alpha */
    int fm = m == 2 ? 1 : m == 1 ? 2 : 0;
    if (fm != G.fog_mode) { STATE(); G.fog_mode = fm; }
    return 0;
}
GAPI(grFogColorValue, 4) { STATE(); float f[4]; unpack_color(ARG(0), f); memcpy(G.fog_color, f, 12); return 0; }
GAPI(grFogTable, 4) { sync_gl(); memcpy(G.fogtab, GPTR(ARG(0)), 64); G.fog_dirty = 1; return 0; }
GAPI(grGammaCorrectionValue, 4) { return 0; }
GAPI(grHints, 8) { if (ARG(0) == 0) { STATE(); G.stw_hint = ARG(1); } return 0; }
GAPI(grTexFilterMode, 12) { STATE(); G.filter_min = ARG(1) != 0; G.filter_mag = ARG(2) != 0; return 0; }
GAPI(grTexClampMode, 12) { STATE(); G.clamp_s = ARG(1) != 0; G.clamp_t = ARG(2) != 0; return 0; }
GAPI(grTexMipMapMode, 12) { STATE(); G.mipmap = ARG(1) != 0; return 0; }
GAPI(grTexLodBiasValue, 8) { return 0; }
GAPI(grTexDetailControl, 16) { return 0; }
GAPI(grTexMinAddress, 4) { return 0; }
GAPI(grTexMaxAddress, 4) { return TMU_MEM - 8; }
GAPI(grTexTextureMemRequired, 8) {
    uint32_t ti = ARG(1);
    uint32_t n = mip_size(gr32(ti), gr32(ti + 4), gr32(ti + 8), gr32(ti + 12));
    return (n + 7) & ~7u;
}
static void tex_invalidate(uint32_t lo, uint32_t hi) {
    for (int i = 0; i < NTEX; i++) {
        TexEnt *e = &G.tc[i];
        if (!e->tex) continue;
        uint32_t a = e->addr, b = a + mip_size(e->small_lod, e->large_lod, e->aspect, e->fmt);
        if (a < hi && lo < b) { submit(); glDeleteTextures(1, &e->tex); e->tex = 0; gs_reset(); }
    }
}
GAPI(grTexDownloadMipMap, 16) {
    STATE();
    uint32_t addr = ARG(1), ti = ARG(3);
    int small = gr32(ti), large = gr32(ti + 4), aspect = gr32(ti + 8), fmt = gr32(ti + 12);
    uint32_t data = gr32(ti + 16);
    uint32_t n = mip_size(small, large, aspect, fmt);
    if (addr + n > TMU_MEM) { LOG("grTexDownloadMipMap out of range %x+%x", addr, n); return 0; }
    memcpy(G.tmu + addr, GPTR(data), n);
    G.st_downloads++;
    tex_invalidate(addr, addr + n);
    return 0;
}
GAPI(grTexSource, 16) {
    STATE();
    uint32_t ti = ARG(3);
    G.src_addr = ARG(1); G.src_small = gr32(ti); G.src_large = gr32(ti + 4); G.src_aspect = gr32(ti + 8); G.src_fmt = gr32(ti + 12);
    G.src_valid = 1;
    return 0;
}
GAPI(guTexSource, 4) { static int n; if (n++ < 3) LOG("guTexSource(%x)", ARG(0)); STATE(); if (ARG(0) == 0xffffffffu) G.src_valid = 0; return 0; }
GAPI(grTexDownloadTable, 12) {
    STATE();
    if (ARG(1) == 2) {   /* GR_TEXTABLE_PALETTE */
        const uint32_t *p = GPTR(ARG(2));
        if (memcmp(G.pal, p, 1024)) { memcpy(G.pal, p, 1024); G.palgen++; G.st_pal++; }
    }
    return 0;
}
GAPI(grDrawTriangle, 12) {
    if (getenv("C2_SKYSTACK") && G.in3d && !G.frame_3d && !(G.stw_hint & 4) && G.depth_mask) {
        static int n;
        if (n++ < 2) {
            char line[1024]; int o = 0;
            for (int i = 0; i < 400 && o < 960; i++) { uint32_t v = gr32(c->esp + 4 * i); if ((v >= 0x401000 && v < 0x589000) || (v >= 0x10001000 && v < 0x1000b000)) o += snprintf(line + o, sizeof line - o, " %x", v); }
            LOG("sky stack:%s", line);
        }
    }
    cull_and_push(ARG(0), ARG(1), ARG(2)); return 0;
}
GAPI(grDrawLine, 8) {
    if (G.prim != GL_LINES || G.nv + 2 > MAXV) { flush(); G.prim = GL_LINES; }
    push_vtx(ARG(0)); push_vtx(ARG(1));
    return 0;
}
GAPI(grDrawPoint, 4) {
    if (G.prim != GL_POINTS || G.nv + 1 > MAXV) { flush(); G.prim = GL_POINTS; }
    push_vtx(ARG(0));
    return 0;
}

/* LFB */
GAPI(grLfbLock, 24) {
    uint32_t type = ARG(0), wmode = ARG(2), info = ARG(5);
    { static int n; if (n++ < 5) LOG("grLfbLock(type=%x buf=%x wmode=%x origin=%x pipe=%x)", type, ARG(1), wmode, ARG(3), ARG(4)); }
    sync_gl();
    lfb_ensure();
    int write = (type & 1) != 0;
    if (!write) {
        /* read: download the current image at game resolution */
        uint32_t *tmp = malloc((size_t)G.fw * G.fh * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, G.fbo);
        glReadPixels(0, 0, G.fw, G.fh, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
        for (int y = 0; y < G.gh; y++) for (int x = 0; x < G.gw; x++) {
            uint32_t p = tmp[(size_t)(G.fh - 1 - y * G.scale) * G.fw + (int)((x + G.xoff) * G.scale)];
            uint16_t v = (uint16_t)((((p & 255) >> 3) << 11) | ((((p >> 8) & 255) >> 2) << 5) | (((p >> 16) & 255) >> 3));
            gw16(LFB_AT(x, y), v);
        }
        free(tmp);
    } else {
        lfb_locked_write = 1;
        lfb_wmode = wmode;
        if (wmode != 0 && wmode != 0xff) LOG("grLfbLock: write mode %u not handled", wmode);
    }
    gw32(info + 4, lfb_guest);
    gw32(info + 8, LFB_STRIDE * 2);
    gw32(info + 12, 0);
    gw32(info + 16, 0);
    return 1;
}
GAPI(grLfbUnlock, 8) {
    if (ARG(0) & 1) { lfb_dirty = 1; lfb_locked_write = 0; flush(); lfb_overlay(); }
    else {
        /* restore the sentinel so a later write lock starts transparent */
        for (int y = 0; y < G.gh; y++) for (int x = 0; x < G.gw; x++) gw16(LFB_AT(x, y), LFB_KEY);
    }
    return 1;
}
GAPI(grLfbWriteRegion, 32) {
    uint32_t x0 = ARG(1), y0 = ARG(2), fmt = ARG(3), w = ARG(4), h = ARG(5), stride = ARG(6), data = ARG(7);
    { static int n; if (n++ < 5) LOG("grLfbWriteRegion(buf=%x %u,%u fmt=%x %ux%u stride=%u)", ARG(0), x0, y0, fmt, w, h, stride); }
    lfb_ensure();
    for (uint32_t y = 0; y < h; y++) {
        if (y0 + y >= (uint32_t)G.gh) break;
        for (uint32_t x = 0; x < w; x++) {
            if (x0 + x >= (uint32_t)G.gw) break;
            uint16_t v;
            if (fmt == 0) v = gr16(data + y * stride + x * 2);
            else if (fmt == 1 || fmt == 2) { uint16_t s = gr16(data + y * stride + x * 2); v = ((s & 0x7c00) << 1) | ((s & 0x3e0) << 1) | (s & 31); }
            else { uint32_t p = gr32(data + y * stride + x * 4); v = (uint16_t)((((p >> 16) & 255) >> 3) << 11 | (((p >> 8) & 255) >> 2) << 5 | ((p & 255) >> 3)); }
            if (v == LFB_KEY) v ^= 0x20;
            gw16(LFB_AT(x0 + x, y0 + y), v);
        }
    }
    lfb_dirty = 1;
    flush(); lfb_overlay();
    return 1;
}
GAPI(grLfbReadRegion, 28) {
    uint32_t x0 = ARG(1), y0 = ARG(2), w = ARG(3), h = ARG(4), stride = ARG(5), data = ARG(6);
    sync_gl();
    uint32_t *tmp = malloc((size_t)G.fw * G.fh * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, G.fbo);
    glReadPixels(0, 0, G.fw, G.fh, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
        int sx = (int)((x0 + x + G.xoff) * G.scale), sy = G.fh - 1 - (int)(y0 + y) * G.scale;
        if (sx >= G.fw || sy < 0) continue;
        uint32_t p = tmp[(size_t)sy * G.fw + sx];
        gw16(data + y * stride + x * 2, (uint16_t)((((p & 255) >> 3) << 11) | ((((p >> 8) & 255) >> 2) << 5) | (((p >> 16) & 255) >> 3)));
    }
    free(tmp);
    return 1;
}

const ApiDef glide_apis[] = {
    GDEF(grGlideInit, 0), GDEF(grGlideShutdown, 0), GDEF(grErrorSetCallback, 4), GDEF(grSstQueryHardware, 4),
    GDEF(grSstSelect, 4), GDEF(grSstWinOpen, 28), GDEF(grBufferClear, 12), GDEF(grBufferSwap, 4),
    GDEF(grBufferNumPending, 0), GDEF(grRenderBuffer, 4), GDEF(grColorMask, 8), GDEF(grDepthMask, 4),
    GDEF(grDepthBufferMode, 4), GDEF(grDepthBufferFunction, 4), GDEF(grCullMode, 4), GDEF(grAlphaBlendFunction, 16),
    GDEF(grAlphaTestReferenceValue, 4), GDEF(grAlphaCombine, 20), GDEF(guColorCombineFunction, 4),
    GDEF(grTexCombineFunction, 8), GDEF(grConstantColorValue, 4), GDEF(grConstantColorValue4, 16),
    GDEF(grChromakeyMode, 4), GDEF(grChromakeyValue, 4), GDEF(grFogMode, 4), GDEF(grFogColorValue, 4),
    GDEF(grFogTable, 4), GDEF(grGammaCorrectionValue, 4), GDEF(grHints, 8), GDEF(grTexFilterMode, 12),
    GDEF(grTexClampMode, 12), GDEF(grTexMipMapMode, 12), GDEF(grTexLodBiasValue, 8), GDEF(grTexDetailControl, 16),
    GDEF(grTexMinAddress, 4), GDEF(grTexMaxAddress, 4), GDEF(grTexTextureMemRequired, 8), GDEF(grTexDownloadMipMap, 16),
    GDEF(grTexSource, 16), GDEF(guTexSource, 4), GDEF(grTexDownloadTable, 12), GDEF(grDrawTriangle, 12),
    GDEF(grDrawLine, 8), GDEF(grDrawPoint, 4), GDEF(grLfbLock, 24), GDEF(grLfbUnlock, 8), GDEF(grLfbWriteRegion, 32),
    GDEF(grLfbReadRegion, 28),
    { 0 }
};
