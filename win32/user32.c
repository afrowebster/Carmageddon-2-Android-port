#include <pthread.h>
#include <strings.h>
#include <ctype.h>
#include "win32.h"

/* ---------------- classes & windows ---------------- */
typedef struct { char name[64]; uint32_t proc; uint32_t style; } WClass;
static WClass classes[32];
static int nclasses;

typedef struct Ctl { uint32_t id; int cls; uint32_t style; char text[256]; int check;
    char items[64][128]; uint32_t itemdata[64]; int nitems; int cursel; } Ctl;

typedef struct {
    int used;
    uint32_t proc, style, exstyle, userdata, parent, id;
    int x, y, w, h;
    char title[128];
    /* dialog */
    int is_dialog, dlg_ended; uint32_t dlg_result;
    Ctl ctls[32]; int nctls;
} Wnd;
#define MAXW 32
#define HWBASE 0x00010010u
static Wnd wins[MAXW];
static uint32_t main_hwnd;

static Wnd *w_get(uint32_t h) {
    if (h < HWBASE || h >= HWBASE + MAXW * 4) return NULL;
    Wnd *w = &wins[(h - HWBASE) / 4];
    return w->used ? w : NULL;
}
static uint32_t w_alloc(void) {
    for (int i = 0; i < MAXW; i++) if (!wins[i].used) { memset(&wins[i], 0, sizeof wins[i]); wins[i].used = 1; return HWBASE + i * 4; }
    rt_fatal("too many windows");
}
uint32_t user32_main_hwnd(void) { return main_hwnd; }

/* ---------------- message queue ---------------- */
typedef struct { uint32_t hwnd, msg, wp, lp, time; int x, y; } Msg;
#define QN 1024
static Msg q[QN];
static int qh, qt;
static pthread_mutex_t qmx = PTHREAD_MUTEX_INITIALIZER;
static int quit_posted; static uint32_t quit_code;
static int mouse_x, mouse_y;

void user32_post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    pthread_mutex_lock(&qmx);
    int n = (qt + 1) % QN;
    if (n != qh) { q[qt] = (Msg){hwnd, msg, wp, lp, plat_ticks(), mouse_x, mouse_y}; qt = n; }
    pthread_mutex_unlock(&qmx);
}
static int q_pop(Msg *m, int remove, uint32_t lo, uint32_t hi) {
    int r = 0;
    pthread_mutex_lock(&qmx);
    for (int i = qh; i != qt; i = (i + 1) % QN) {
        if ((lo || hi) && (q[i].msg < lo || q[i].msg > hi)) continue;
        *m = q[i]; r = 1;
        if (remove) {
            /* remove element i */
            for (int j = i; j != qt; j = (j + 1) % QN) { int k = (j + 1) % QN; if (k != qt) q[j] = q[k]; }
            qt = (qt - 1 + QN) % QN;
        }
        break;
    }
    pthread_mutex_unlock(&qmx);
    return r;
}

uint32_t user32_send(CPU *c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    Wnd *w = w_get(hwnd);
    if (!w || !w->proc) return 0;
    return rt_invoke(c, w->proc, 4, hwnd, msg, wp, lp);
}

/* keyboard: translate to WM_KEYDOWN/UP + WM_CHAR */
void user32_key_event(int vk, int down) {
    uint32_t h = main_hwnd;
    if (!h) return;
    static int state[256];
    uint32_t lp = 1 | ((uint32_t)(vk & 0xff) << 16);
    if (down) {
        if (state[vk & 0xff]) lp |= 1u << 30;
        state[vk & 0xff] = 1;
        user32_post(h, vk == 0x12 ? WM_SYSKEYDOWN : WM_KEYDOWN, vk, lp);
    } else {
        state[vk & 0xff] = 0;
        user32_post(h, vk == 0x12 ? WM_SYSKEYUP : WM_KEYUP, vk, lp | 0xC0000000u);
    }
}
void user32_char_event(int ch) { if (main_hwnd) user32_post(main_hwnd, WM_CHAR, ch, 1); }
void user32_mouse_event(int x, int y, int buttons, int what) {
    mouse_x = x; mouse_y = y;
    if (!main_hwnd) return;
    uint32_t lp = ((uint32_t)(y & 0xffff) << 16) | (x & 0xffff);
    uint32_t msg = what == 0 ? WM_MOUSEMOVE : what == 1 ? WM_LBUTTONDOWN : what == 2 ? WM_LBUTTONUP : what == 3 ? WM_RBUTTONDOWN : WM_RBUTTONUP;
    user32_post(main_hwnd, msg, buttons, lp);
}
void user32_activate(int active) {
    if (!main_hwnd) return;
    user32_post(main_hwnd, WM_ACTIVATEAPP, active, 0);
    user32_post(main_hwnd, WM_ACTIVATE, active ? 1 : 0, 0);
}
void user32_quit_request(void) { if (main_hwnd) user32_post(main_hwnd, WM_CLOSE, 0, 0); else exit(0); }

/* ---------------- timers ---------------- */
typedef struct { int used; uint32_t hwnd, id, period, next, proc; } Timer;
static Timer timers[16];
static void run_timers(void) {
    uint32_t now = plat_ticks();
    for (int i = 0; i < 16; i++) if (timers[i].used && (int32_t)(now - timers[i].next) >= 0) {
        timers[i].next = now + timers[i].period;
        /* don't flood: only one pending WM_TIMER per timer */
        Msg m; int dup = 0;
        pthread_mutex_lock(&qmx);
        for (int j = qh; j != qt; j = (j + 1) % QN) if (q[j].msg == WM_TIMER && q[j].wp == timers[i].id) dup = 1;
        pthread_mutex_unlock(&qmx);
        (void)m;
        if (!dup) user32_post(timers[i].hwnd, WM_TIMER, timers[i].id, timers[i].proc);
    }
}
API(SetTimer) {
    uint32_t hwnd = ARG(0), id = ARG(1), ms = ARG(2), proc = ARG(3);
    for (int i = 0; i < 16; i++) if (timers[i].used && timers[i].hwnd == hwnd && timers[i].id == id) timers[i].used = 0;
    for (int i = 0; i < 16; i++) if (!timers[i].used) {
        timers[i] = (Timer){1, hwnd, id ? id : (uint32_t)(i + 1), ms ? ms : 1, plat_ticks() + ms, proc};
        LOG("SetTimer(hwnd=%x id=%u %ums)", hwnd, id, ms);
        return timers[i].id;
    }
    return 0;
}

static void write_msg(uint32_t p, Msg *m) {
    gw32(p, m->hwnd); gw32(p + 4, m->msg); gw32(p + 8, m->wp); gw32(p + 12, m->lp);
    gw32(p + 16, m->time); gw32(p + 20, m->x); gw32(p + 24, m->y);
}

API(PeekMessageA) {
    uint32_t pm = ARG(0), lo = ARG(2), hi = ARG(3), rm = ARG(4);
    k32_check_kill(c);
    plat_pump_events();
    run_timers();
    Msg m;
    if (q_pop(&m, rm & 1, lo, hi)) { write_msg(pm, &m); return 1; }
    if (quit_posted) {
        m = (Msg){0, WM_QUIT, quit_code, 0, plat_ticks(), 0, 0};
        if (rm & 1) quit_posted = 0;
        write_msg(pm, &m); return 1;
    }
    return 0;
}
API(GetMessageA) {
    uint32_t pm = ARG(0), lo = ARG(2), hi = ARG(3);
    for (;;) {
        plat_pump_events();
        run_timers();
        Msg m;
        if (q_pop(&m, 1, lo, hi)) { write_msg(pm, &m); return m.msg != WM_QUIT; }
        if (quit_posted) { quit_posted = 0; m = (Msg){0, WM_QUIT, quit_code, 0, plat_ticks(), 0, 0}; write_msg(pm, &m); return 0; }
        plat_sleep(2);
    }
}
API(WaitMessage) {
    for (int i = 0; i < 50; i++) {
        plat_pump_events(); run_timers();
        if (qh != qt || quit_posted) break;
        plat_sleep(2);
    }
    return 1;
}
API(TranslateMessage) {
    uint32_t pm = ARG(0);
    uint32_t msg = gr32(pm + 4), vk = gr32(pm + 8);
    if (msg == WM_KEYDOWN) {
        int ch = 0;
        if (vk >= 'A' && vk <= 'Z') ch = vk;
        else if (vk >= '0' && vk <= '9') ch = vk;
        else if (vk == 0x20 || vk == 0x0d || vk == 0x08 || vk == 0x1b) ch = vk;
        else if (vk >= 0x60 && vk <= 0x69) ch = '0' + vk - 0x60;
        if (ch) user32_post(gr32(pm), WM_CHAR, ch, gr32(pm + 12));
    }
    return 1;
}
API(DispatchMessageA) {
    uint32_t pm = ARG(0);
    uint32_t hwnd = gr32(pm), msg = gr32(pm + 4), wp = gr32(pm + 8), lp = gr32(pm + 12);
    if (msg == WM_TIMER && lp) return rt_invoke(c, lp, 4, hwnd, msg, wp, plat_ticks());
    Wnd *w = w_get(hwnd);
    if (!w) w = w_get(main_hwnd), hwnd = main_hwnd;
    if (!w || !w->proc) return 0;
    return rt_invoke(c, w->proc, 4, hwnd, msg, wp, lp);
}
API(PostMessageA) { user32_post(ARG(0), ARG(1), ARG(2), ARG(3)); return 1; }
API(PostQuitMessage) { quit_posted = 1; quit_code = ARG(0); LOG("PostQuitMessage(%u)", ARG(0)); return 0; }
API(DefWindowProcA) {
    uint32_t hwnd = ARG(0), msg = ARG(1);
    switch (msg) {
    case WM_CLOSE: user32_send(c, hwnd, WM_DESTROY, 0, 0); return 0;
    case WM_NCCREATE: return 1;
    case WM_SETCURSOR: return 1;
    case WM_ERASEBKGND: return 1;
    case WM_NCACTIVATE: return 1;
    }
    return 0;
}

/* ---------------- class / window creation ---------------- */
static uint32_t reg_class(const char *name, uint32_t proc, uint32_t style) {
    if (nclasses >= 32) return 0;
    snprintf(classes[nclasses].name, 64, "%s", name);
    classes[nclasses].proc = proc; classes[nclasses].style = style;
    LOG("RegisterClass(%s, proc=%08x)", name, proc);
    return 0xC000 + nclasses++;
}
static WClass *find_class(uint32_t nm) {
    for (int i = 0; i < nclasses; i++) {
        if (nm < 0x10000 ? (nm == (uint32_t)(0xC000 + i)) : !strcasecmp(classes[i].name, gstr(nm))) return &classes[i];
    }
    return NULL;
}
API(RegisterClassA) { uint32_t p = ARG(0); return reg_class(gstr(gr32(p + 36)) ? gstr(gr32(p + 36)) : "?", gr32(p + 4), gr32(p)); }
API(RegisterClassExA) { uint32_t p = ARG(0); return reg_class(gstr(gr32(p + 40)) ? gstr(gr32(p + 40)) : "?", gr32(p + 8), gr32(p + 4)); }
API(UnregisterClassA) { return 1; }
API(GetClassInfoA) {
    WClass *k = find_class(ARG(1));
    if (!k) return 0;
    uint32_t p = ARG(2);
    memset(GPTR(p), 0, 40);
    gw32(p, k->style); gw32(p + 4, k->proc);
    return 1;
}
API(CreateWindowExA) {
    uint32_t ex = ARG(0), cls = ARG(1), title = ARG(2), style = ARG(3);
    int x = ARG(4), y = ARG(5), w = ARG(6), h = ARG(7);
    uint32_t parent = ARG(8), param = ARG(11);
    WClass *k = find_class(cls);
    uint32_t hwnd = w_alloc();
    Wnd *wn = w_get(hwnd);
    wn->proc = k ? k->proc : 0;
    wn->style = style; wn->exstyle = ex; wn->parent = parent;
    wn->x = x == (int)0x80000000 ? 0 : x; wn->y = y == (int)0x80000000 ? 0 : y;
    wn->w = w == (int)0x80000000 ? 640 : w; wn->h = h == (int)0x80000000 ? 480 : h;
    if (title) snprintf(wn->title, sizeof wn->title, "%s", gstr(title));
    LOG("CreateWindowExA(class=%s title=%s style=%x %dx%d) -> %x", k ? k->name : "?", wn->title, style, wn->w, wn->h, hwnd);
    if (!main_hwnd && !parent) main_hwnd = hwnd;
    /* CREATESTRUCT */
    uint32_t cs = gcalloc(48);
    gw32(cs, param); gw32(cs + 12, parent); gw32(cs + 16, h); gw32(cs + 20, w); gw32(cs + 24, y); gw32(cs + 28, x);
    gw32(cs + 32, style); gw32(cs + 36, title); gw32(cs + 40, cls); gw32(cs + 44, ex);
    if (wn->proc) {
        rt_invoke(c, wn->proc, 4, hwnd, WM_NCCREATE, 0, cs);
        uint32_t r = rt_invoke(c, wn->proc, 4, hwnd, WM_CREATE, 0, cs);
        if (r == 0xffffffffu) { wn->used = 0; return 0; }
    }
    if (style & 0x10000000u) { /* WS_VISIBLE */
        user32_send(c, hwnd, WM_SHOWWINDOW, 1, 0);
        user32_send(c, hwnd, WM_ACTIVATEAPP, 1, 0);
        user32_send(c, hwnd, WM_ACTIVATE, 1, 0);
        user32_send(c, hwnd, WM_SETFOCUS, 0, 0);
    }
    return hwnd;
}
API(DestroyWindow) {
    uint32_t hwnd = ARG(0);
    Wnd *w = w_get(hwnd);
    if (!w) return 0;
    LOG("DestroyWindow(%x)", hwnd);
    user32_send(c, hwnd, WM_DESTROY, 0, 0);
    user32_send(c, hwnd, WM_NCDESTROY, 0, 0);
    w->used = 0;
    if (hwnd == main_hwnd) main_hwnd = 0;
    return 1;
}
API(ShowWindow) {
    uint32_t hwnd = ARG(0), cmd = ARG(1);
    Wnd *w = w_get(hwnd);
    if (!w) return 0;
    int was = (w->style & 0x10000000u) != 0;
    if (cmd) {
        w->style |= 0x10000000u;
        if (!was) {
            user32_send(c, hwnd, WM_SHOWWINDOW, 1, 0);
            user32_send(c, hwnd, WM_ACTIVATEAPP, 1, 0);
            user32_send(c, hwnd, WM_ACTIVATE, 1, 0);
            user32_send(c, hwnd, WM_SETFOCUS, 0, 0);
        }
    } else w->style &= ~0x10000000u;
    return was;
}
API(UpdateWindow) { return 1; }
API(SetFocus) { return main_hwnd; }
API(GetParent) { Wnd *w = w_get(ARG(0)); return w ? w->parent : 0; }
API(GetMenu) { return 0; }
API(GetWindowLongA) {
    Wnd *w = w_get(ARG(0));
    if (!w) return 0;
    switch ((int32_t)ARG(1)) {
    case -4: return w->proc;
    case -16: return w->style;
    case -20: return w->exstyle;
    case -21: return w->userdata;
    case -12: return w->id;
    }
    return 0;
}
API(SetWindowTextA) {
    Wnd *w = w_get(ARG(0));
    if (w) snprintf(w->title, sizeof w->title, "%s", gstr(ARG(1)) ? gstr(ARG(1)) : "");
    return 1;
}
API(GetClientRect) {
    Wnd *w = w_get(ARG(0));
    uint32_t r = ARG(1);
    gw32(r, 0); gw32(r + 4, 0); gw32(r + 8, w ? w->w : 640); gw32(r + 12, w ? w->h : 480);
    return 1;
}
API(ClientToScreen) { return 1; }
API(AdjustWindowRectEx) { return 1; }
API(GetWindowPlacement) {
    uint32_t p = ARG(1); Wnd *w = w_get(ARG(0));
    memset(GPTR(p + 4), 0, 40); gw32(p, 44); gw32(p + 8, 1);
    if (w) { gw32(p + 28, w->x); gw32(p + 32, w->y); gw32(p + 36, w->x + w->w); gw32(p + 40, w->y + w->h); }
    return 1;
}
API(SetWindowPlacement) { return 1; }
API(GetSystemMetrics) {
    switch (ARG(0)) {
    case 0: return 640;   /* SM_CXSCREEN */
    case 1: return 480;   /* SM_CYSCREEN */
    case 4: return 19;    /* caption */
    case 5: case 6: return 1;
    case 32: case 33: return 4;
    }
    return 0;
}
API(GetCursorPos) { uint32_t p = ARG(0); gw32(p, mouse_x); gw32(p + 4, mouse_y); return 1; }
API(SetCursorPos) { mouse_x = ARG(0); mouse_y = ARG(1); return 1; }
API(SetCursor) { return 0; }
API(LoadIconA) { return 0x00020001u; }
API(LoadCursorA) { return 0x00020002u; }
API(MessageBeep) { return 1; }
API(MessageBoxA) {
    LOG("MessageBox [%s]: %s", gstr(ARG(2)) ? gstr(ARG(2)) : "", gstr(ARG(1)) ? gstr(ARG(1)) : "");
    uint32_t t = ARG(3) & 0xf;
    return t == 4 || t == 3 ? 6 /* IDYES */ : 1;
}
API(GetDC) { return 0x00030001u; }
API(ReleaseDC) { return 1; }
API(DrawTextA) { TRACE("DrawText %s", gstr(ARG(1))); return 16; }

/* ---------------- dialogs ---------------- */
static uint32_t rd_sz(uint32_t *o, char *out, int n) {
    uint32_t p = *o;
    if (gr16(p) == 0xffff) { uint32_t id = gr16(p + 2); *o = p + 4; if (out) snprintf(out, n, "#%u", id); return id | 0x10000; }
    int i = 0;
    while (gr16(p)) { if (out && i < n - 1) out[i++] = (char)gr16(p); p += 2; }
    if (out) out[i] = 0;
    *o = p + 2;
    return 0;
}
static void parse_dialog(Wnd *w, uint32_t t) {
    int ex = gr16(t) == 1 && gr16(t + 2) == 0xffff;
    uint32_t style, o; int cnt;
    if (ex) { style = gr32(t + 12); cnt = gr16(t + 16); o = t + 26; }
    else { style = gr32(t); cnt = gr16(t + 8); o = t + 18; }
    rd_sz(&o, NULL, 0); rd_sz(&o, NULL, 0); rd_sz(&o, w->title, sizeof w->title);
    if (style & 0x40) { o += ex ? 6 : 2; rd_sz(&o, NULL, 0); }
    for (int i = 0; i < cnt && i < 32; i++) {
        o = (o + 3) & ~3u;
        Ctl *k = &w->ctls[w->nctls++];
        memset(k, 0, sizeof *k);
        if (ex) { k->style = gr32(o + 8); k->id = gr32(o + 20); o += 24; }
        else { k->style = gr32(o); k->id = gr16(o + 16); o += 18; }
        uint32_t cl = rd_sz(&o, NULL, 0);
        k->cls = cl ? (int)(cl & 0xffff) : 0;
        rd_sz(&o, k->text, sizeof k->text);
        o += 2 + gr16(o);
        k->cursel = -1;
    }
}
static Ctl *dlg_ctl(uint32_t hdlg, uint32_t id) {
    Wnd *w = w_get(hdlg);
    if (!w) return NULL;
    for (int i = 0; i < w->nctls; i++) if ((w->ctls[i].id & 0xffff) == (id & 0xffff)) return &w->ctls[i];
    return NULL;
}
static void dump_dialog(Wnd *w) {
    for (int i = 0; i < w->nctls; i++) {
        Ctl *k = &w->ctls[i];
        char buf[512] = "";
        int n = 0;
        for (int j = 0; j < k->nitems && n < 400; j++) n += snprintf(buf + n, sizeof buf - n, "%s%s", j ? " | " : "", k->items[j]);
        LOG("  ctl %u cls=%x '%s' check=%d sel=%d items=[%s]", k->id & 0xffff, k->cls, k->text, k->check, k->cursel, buf);
    }
}
/* which button does the "user" press? overridable with C2_DLG_<name>=id */
static uint32_t pick_button(Wnd *w, const char *name) {
    char env[96]; snprintf(env, sizeof env, "C2_DLG_%s", name);
    if (getenv(env)) return atoi(getenv(env));
    for (int i = 0; i < w->nctls; i++) if (w->ctls[i].cls == 0x80 && (w->ctls[i].style & 0xf) == 1) return w->ctls[i].id & 0xffff;
    return 1;
}
API(DialogBoxParamA) {
    uint32_t tmpl = ARG(1), parent = ARG(2), proc = ARG(3), param = ARG(4);
    char name[64];
    if (tmpl < 0x10000) snprintf(name, sizeof name, "%u", tmpl); else snprintf(name, sizeof name, "%s", gstr(tmpl));
    uint32_t de = res_find(5, tmpl, NULL);
    if (!de) { LOG("DialogBoxParamA: template %s not found", name); return 0xffffffffu; }
    uint32_t hdlg = w_alloc();
    Wnd *w = w_get(hdlg);
    w->is_dialog = 1; w->proc = proc; w->parent = parent;
    parse_dialog(w, g_image_base + gr32(de));
    LOG("DialogBoxParamA(%s '%s')", name, w->title);
    rt_invoke(c, proc, 4, hdlg, WM_INITDIALOG, 0, param);
    dump_dialog(w);
    int guard = 0;
    while (!w->dlg_ended && guard++ < 8) {
        uint32_t id = pick_button(w, name);
        LOG("  dialog %s: pressing %u", name, id);
        rt_invoke(c, proc, 4, hdlg, WM_COMMAND, id, 0);
        if (!w->dlg_ended && guard >= 2) { w->dlg_ended = 1; w->dlg_result = id; }
    }
    uint32_t r = w->dlg_result;
    LOG("  dialog %s result %u", name, r);
    dump_dialog(w);
    w->used = 0;
    return r;
}
API(EndDialog) {
    Wnd *w = w_get(ARG(0));
    if (w) { w->dlg_ended = 1; w->dlg_result = ARG(1); }
    return 1;
}
API(GetDlgItemTextA) {
    Ctl *k = dlg_ctl(ARG(0), ARG(1));
    gstrcpy(ARG(2), k ? k->text : "", ARG(3));
    return k ? strlen(k->text) : 0;
}
API(SendDlgItemMessageA) {
    uint32_t id = ARG(1), msg = ARG(2), wp = ARG(3), lp = ARG(4);
    Ctl *k = dlg_ctl(ARG(0), id);
    if (!k) { TRACE("SendDlgItemMessage: no ctl %u", id); return 0; }
    switch (msg) {
    case 0x00F0: return k->check;                     /* BM_GETCHECK */
    case 0x00F1: k->check = wp; return 0;             /* BM_SETCHECK */
    case 0x000C: snprintf(k->text, sizeof k->text, "%s", gstr(lp) ? gstr(lp) : ""); return 1; /* WM_SETTEXT */
    case 0x000D: gstrcpy(lp, k->text, wp); return strlen(k->text); /* WM_GETTEXT */
    case 0x0143: case 0x0180:                         /* CB_ADDSTRING / LB_ADDSTRING */
        if (k->nitems < 64) { snprintf(k->items[k->nitems], 128, "%s", gstr(lp) ? gstr(lp) : ""); k->itemdata[k->nitems] = 0; return k->nitems++; }
        return 0xffffffffu;
    case 0x014B: case 0x0184: k->nitems = 0; k->cursel = -1; return 0;   /* RESETCONTENT */
    case 0x014E: case 0x0186: k->cursel = (int32_t)wp; return wp;        /* SETCURSEL */
    case 0x0147: case 0x0188: return (uint32_t)k->cursel;               /* GETCURSEL */
    case 0x0146: case 0x018B: return k->nitems;                         /* GETCOUNT */
    case 0x0148: case 0x0189:                                            /* GETLBTEXT */
        if ((int)wp < k->nitems) { gstrcpy(lp, k->items[wp], 128); return strlen(k->items[wp]); }
        return 0xffffffffu;
    case 0x0149: case 0x018A: return (int)wp < k->nitems ? strlen(k->items[wp]) : 0xffffffffu;
    case 0x0151: case 0x019A: if ((int)wp < 64) k->itemdata[wp] = lp; return 1;  /* SETITEMDATA */
    case 0x0150: case 0x0199: return (int)wp < 64 ? k->itemdata[wp] : 0;     /* GETITEMDATA */
    case 0x0172: case 0x0170: return 0;                                     /* STM_SETIMAGE/SETICON */
    }
    TRACE("SendDlgItemMessage(%u, %x) unhandled", id, msg);
    return 0;
}

/* ---------------- wsprintfA (cdecl) ---------------- */
static int fmt_guest(char *out, int cap, const char *f, uint32_t argp) {
    int n = 0;
    #define PUT(ch) do { if (n < cap - 1) out[n] = (ch); n++; } while (0)
    for (; *f; f++) {
        if (*f != '%') { PUT(*f); continue; }
        f++;
        char spec[32]; int sn = 0;
        spec[sn++] = '%';
        while (*f && strchr("-+ #0", *f)) spec[sn++] = *f++;
        while (*f && isdigit((unsigned char)*f)) spec[sn++] = *f++;
        if (*f == '.') { spec[sn++] = *f++; while (*f && isdigit((unsigned char)*f)) spec[sn++] = *f++; }
        while (*f == 'l' || *f == 'h') f++;
        char t = *f;
        char tmp[512];
        if (t == '%') { PUT('%'); continue; }
        if (t == 's') { spec[sn++] = 's'; spec[sn] = 0; uint32_t s = gr32(argp); argp += 4; snprintf(tmp, sizeof tmp, spec, s ? gstr(s) : "(null)"); }
        else if (t == 'c') { spec[sn++] = 'c'; spec[sn] = 0; snprintf(tmp, sizeof tmp, spec, (int)gr32(argp)); argp += 4; }
        else if (t == 'd' || t == 'i') { spec[sn++] = 'd'; spec[sn] = 0; snprintf(tmp, sizeof tmp, spec, (int32_t)gr32(argp)); argp += 4; }
        else if (t == 'u' || t == 'x' || t == 'X' || t == 'o') { spec[sn++] = t; spec[sn] = 0; snprintf(tmp, sizeof tmp, spec, gr32(argp)); argp += 4; }
        else if (!t) break;
        else { tmp[0] = t; tmp[1] = 0; }
        for (char *p = tmp; *p; p++) PUT(*p);
    }
    if (cap) out[n < cap ? n : cap - 1] = 0;
    return n;
    #undef PUT
}
API(wsprintfA) {
    char buf[1025];
    int n = fmt_guest(buf, sizeof buf, gstr(ARG(1)), c->esp + 12);
    if (n > 1024) n = 1024;
    memcpy(GPTR(ARG(0)), buf, n + 1);
    return n;
}

const ApiDef user32_apis[] = {
    DEF("user32.dll", SetTimer, 4), DEF("user32.dll", PeekMessageA, 5), DEF("user32.dll", GetMessageA, 4),
    DEF("user32.dll", WaitMessage, 0), DEF("user32.dll", TranslateMessage, 1), DEF("user32.dll", DispatchMessageA, 1),
    DEF("user32.dll", PostMessageA, 4), DEF("user32.dll", PostQuitMessage, 1), DEF("user32.dll", DefWindowProcA, 4),
    DEF("user32.dll", RegisterClassA, 1), DEF("user32.dll", RegisterClassExA, 1), DEF("user32.dll", UnregisterClassA, 2),
    DEF("user32.dll", GetClassInfoA, 3), DEF("user32.dll", CreateWindowExA, 12), DEF("user32.dll", DestroyWindow, 1),
    DEF("user32.dll", ShowWindow, 2), DEF("user32.dll", UpdateWindow, 1), DEF("user32.dll", SetFocus, 1),
    DEF("user32.dll", GetParent, 1), DEF("user32.dll", GetMenu, 1), DEF("user32.dll", GetWindowLongA, 2),
    DEF("user32.dll", SetWindowTextA, 2), DEF("user32.dll", GetClientRect, 2), DEF("user32.dll", ClientToScreen, 2),
    DEF("user32.dll", AdjustWindowRectEx, 4), DEF("user32.dll", GetWindowPlacement, 2), DEF("user32.dll", SetWindowPlacement, 2),
    DEF("user32.dll", GetSystemMetrics, 1), DEF("user32.dll", GetCursorPos, 1), DEF("user32.dll", SetCursorPos, 2),
    DEF("user32.dll", SetCursor, 1), DEF("user32.dll", LoadIconA, 2), DEF("user32.dll", LoadCursorA, 2),
    DEF("user32.dll", MessageBeep, 1), DEF("user32.dll", MessageBoxA, 4), DEF("user32.dll", GetDC, 1),
    DEF("user32.dll", ReleaseDC, 2), DEF("user32.dll", DrawTextA, 5), DEF("user32.dll", DialogBoxParamA, 5),
    DEF("user32.dll", EndDialog, 2), DEF("user32.dll", GetDlgItemTextA, 4), DEF("user32.dll", SendDlgItemMessageA, 5),
    DEF("user32.dll", wsprintfA, 0),
    { 0 }
};

/* ---------------- gdi32 ---------------- */
API(SetBkMode) { return 1; }
API(SetTextColor) { return 0; }
API(GetStockObject) { return 0x00040000u + ARG(0); }
API(GetPaletteEntries) { return 0; }
API(StretchBlt) { return 1; }
const ApiDef gdi32_apis[] = {
    DEF("gdi32.dll", SetBkMode, 2), DEF("gdi32.dll", SetTextColor, 2), DEF("gdi32.dll", GetStockObject, 1),
    DEF("gdi32.dll", GetPaletteEntries, 4), DEF("gdi32.dll", StretchBlt, 11),
    { 0 }
};

/* ---------------- advapi32 / shell32 / winmm ---------------- */
API(RegOpenKeyExA) {
    const char *k = gstr(ARG(1));
    LOG("RegOpenKeyExA(%x, %s)", ARG(0), k ? k : "");
    if (k && (!strcasecmp(k, "Software\\Microsoft\\DirectX") || !strcasecmp(k, "Software\\Microsoft\\Windows\\CurrentVersion"))) {
        gw32(ARG(4), !strcasecmp(k, "Software\\Microsoft\\DirectX") ? 0x80000101u : 0x80000102u);
        return 0;
    }
    return 2;
}
API(RegQueryValueExA) {
    uint32_t key = ARG(0); const char *v = gstr(ARG(1));
    uint32_t type = ARG(3), data = ARG(4), pcb = ARG(5);
    const char *val = NULL;
    if (key == 0x80000101u && v && !strcasecmp(v, "Version")) val = "4.07.00.0700";
    if (key == 0x80000102u && v && !strcasecmp(v, "ProductName")) val = "Microsoft Windows";
    if (key == 0x80000102u && v && !strcasecmp(v, "Version")) val = "5.1";
    if (!val) return 2;
    if (type) gw32(type, 1);
    uint32_t n = strlen(val) + 1;
    if (pcb) { if (data && gr32(pcb) >= n) gstrcpy(data, val, n); gw32(pcb, n); }
    return 0;
}
API(ShellExecuteA) { LOG("ShellExecuteA(%s, %s)", gstr(ARG(1)) ? gstr(ARG(1)) : "", gstr(ARG(2)) ? gstr(ARG(2)) : ""); return 42; }

/* joystick: report none (touch / keyboard instead) */
API(joyGetNumDevs) { return 0; }
API(joyGetDevCapsA) { return 165; /* JOYERR_UNPLUGGED */ }
API(joyGetPosEx) { return 167; }

const ApiDef misc_apis[] = {
    DEF("advapi32.dll", RegOpenKeyExA, 5), DEF("advapi32.dll", RegQueryValueExA, 6),
    DEF("shell32.dll", ShellExecuteA, 6),
    DEF("winmm.dll", joyGetNumDevs, 0), DEF("winmm.dll", joyGetDevCapsA, 3), DEF("winmm.dll", joyGetPosEx, 2),
    { 0 }
};
