/* DirectInput (DX5 interface): system keyboard and mouse. Game controllers are mapped onto
   keyboard keys by the platform layer, so the game sees no joystick devices. */
#include <SDL.h>
#include "win32.h"

static uint8_t dik_state[256];
static struct { uint8_t ofs; uint8_t down; uint32_t t; } kq[256];
static int kq_head, kq_tail;
static uint32_t kq_seq;
static uint8_t mouse_btn[4];
static int32_t mouse_dx, mouse_dy;

/* refcounted so several sources (keyboard, controller) can hold the same key */
static uint8_t dik_refs[256];
/* a press that is released before the game has polled the keyboard is held until the next poll,
   so quick taps (touch buttons, adb, fast fingers at a low frame rate) are never lost */
static uint8_t dik_seen[256], dik_pending_up[256];
void dinput_key(int dik, int down) {
    dik &= 0xff;
    if (!dik) return;
    int was = dik_refs[dik] > 0;
    if (down) dik_refs[dik]++; else if (dik_refs[dik]) dik_refs[dik]--;
    int now = dik_refs[dik] > 0;
    if (now && !was) { dik_seen[dik] = 0; dik_pending_up[dik] = 0; }
    if (!now && was && !dik_seen[dik]) dik_pending_up[dik] = 1;
    else dik_state[dik] = now ? 0x80 : 0;
    if (was != now) {
        int n = (kq_tail + 1) & 255;
        if (n != kq_head) { kq[kq_tail].ofs = dik; kq[kq_tail].down = now; kq[kq_tail].t = plat_ticks(); kq_tail = n; }
    }
}
int dinput_key_held(int dik) { return dik_state[dik & 0xff] != 0; }
void dinput_mouse(int dx, int dy, int btn, int down) {
    mouse_dx += dx; mouse_dy += dy;
    if (btn >= 0 && btn < 4) mouse_btn[btn] = down ? 0x80 : 0;
}

int dinput_from_sdl(int sc) {
    switch (sc) {
    case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_AC_BACK: return 0x01;
    case SDL_SCANCODE_1: return 0x02; case SDL_SCANCODE_2: return 0x03; case SDL_SCANCODE_3: return 0x04;
    case SDL_SCANCODE_4: return 0x05; case SDL_SCANCODE_5: return 0x06; case SDL_SCANCODE_6: return 0x07;
    case SDL_SCANCODE_7: return 0x08; case SDL_SCANCODE_8: return 0x09; case SDL_SCANCODE_9: return 0x0A;
    case SDL_SCANCODE_0: return 0x0B; case SDL_SCANCODE_MINUS: return 0x0C; case SDL_SCANCODE_EQUALS: return 0x0D;
    case SDL_SCANCODE_BACKSPACE: return 0x0E; case SDL_SCANCODE_TAB: return 0x0F;
    case SDL_SCANCODE_Q: return 0x10; case SDL_SCANCODE_W: return 0x11; case SDL_SCANCODE_E: return 0x12;
    case SDL_SCANCODE_R: return 0x13; case SDL_SCANCODE_T: return 0x14; case SDL_SCANCODE_Y: return 0x15;
    case SDL_SCANCODE_U: return 0x16; case SDL_SCANCODE_I: return 0x17; case SDL_SCANCODE_O: return 0x18;
    case SDL_SCANCODE_P: return 0x19; case SDL_SCANCODE_LEFTBRACKET: return 0x1A; case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
    case SDL_SCANCODE_RETURN: return 0x1C; case SDL_SCANCODE_LCTRL: return 0x1D;
    case SDL_SCANCODE_A: return 0x1E; case SDL_SCANCODE_S: return 0x1F; case SDL_SCANCODE_D: return 0x20;
    case SDL_SCANCODE_F: return 0x21; case SDL_SCANCODE_G: return 0x22; case SDL_SCANCODE_H: return 0x23;
    case SDL_SCANCODE_J: return 0x24; case SDL_SCANCODE_K: return 0x25; case SDL_SCANCODE_L: return 0x26;
    case SDL_SCANCODE_SEMICOLON: return 0x27; case SDL_SCANCODE_APOSTROPHE: return 0x28; case SDL_SCANCODE_GRAVE: return 0x29;
    case SDL_SCANCODE_LSHIFT: return 0x2A; case SDL_SCANCODE_BACKSLASH: return 0x2B;
    case SDL_SCANCODE_Z: return 0x2C; case SDL_SCANCODE_X: return 0x2D; case SDL_SCANCODE_C: return 0x2E;
    case SDL_SCANCODE_V: return 0x2F; case SDL_SCANCODE_B: return 0x30; case SDL_SCANCODE_N: return 0x31;
    case SDL_SCANCODE_M: return 0x32; case SDL_SCANCODE_COMMA: return 0x33; case SDL_SCANCODE_PERIOD: return 0x34;
    case SDL_SCANCODE_SLASH: return 0x35; case SDL_SCANCODE_RSHIFT: return 0x36; case SDL_SCANCODE_KP_MULTIPLY: return 0x37;
    case SDL_SCANCODE_LALT: return 0x38; case SDL_SCANCODE_SPACE: return 0x39; case SDL_SCANCODE_CAPSLOCK: return 0x3A;
    case SDL_SCANCODE_F1: return 0x3B; case SDL_SCANCODE_F2: return 0x3C; case SDL_SCANCODE_F3: return 0x3D;
    case SDL_SCANCODE_F4: return 0x3E; case SDL_SCANCODE_F5: return 0x3F; case SDL_SCANCODE_F6: return 0x40;
    case SDL_SCANCODE_F7: return 0x41; case SDL_SCANCODE_F8: return 0x42; case SDL_SCANCODE_F9: return 0x43;
    case SDL_SCANCODE_F10: return 0x44; case SDL_SCANCODE_NUMLOCKCLEAR: return 0x45; case SDL_SCANCODE_SCROLLLOCK: return 0x46;
    case SDL_SCANCODE_KP_7: return 0x47; case SDL_SCANCODE_KP_8: return 0x48; case SDL_SCANCODE_KP_9: return 0x49;
    case SDL_SCANCODE_KP_MINUS: return 0x4A; case SDL_SCANCODE_KP_4: return 0x4B; case SDL_SCANCODE_KP_5: return 0x4C;
    case SDL_SCANCODE_KP_6: return 0x4D; case SDL_SCANCODE_KP_PLUS: return 0x4E; case SDL_SCANCODE_KP_1: return 0x4F;
    case SDL_SCANCODE_KP_2: return 0x50; case SDL_SCANCODE_KP_3: return 0x51; case SDL_SCANCODE_KP_0: return 0x52;
    case SDL_SCANCODE_KP_PERIOD: return 0x53; case SDL_SCANCODE_F11: return 0x57; case SDL_SCANCODE_F12: return 0x58;
    case SDL_SCANCODE_KP_ENTER: return 0x9C; case SDL_SCANCODE_RCTRL: return 0x9D; case SDL_SCANCODE_KP_DIVIDE: return 0xB5;
    case SDL_SCANCODE_RALT: return 0xB8; case SDL_SCANCODE_PAUSE: return 0xC5; case SDL_SCANCODE_HOME: return 0xC7;
    case SDL_SCANCODE_UP: return 0xC8; case SDL_SCANCODE_PAGEUP: return 0xC9; case SDL_SCANCODE_LEFT: return 0xCB;
    case SDL_SCANCODE_RIGHT: return 0xCD; case SDL_SCANCODE_END: return 0xCF; case SDL_SCANCODE_DOWN: return 0xD0;
    case SDL_SCANCODE_PAGEDOWN: return 0xD1; case SDL_SCANCODE_INSERT: return 0xD2; case SDL_SCANCODE_DELETE: return 0xD3;
    }
    return 0;
}

typedef struct { int kind; uint32_t buffer; } Dev;   /* kind: 1 keyboard, 2 mouse */

API(di_QueryInterface) { gw32(ARG(2), 0); return 0x80004002u; }
API(di_AddRef) { return 2; }
API(di_Release) { return 0; }
API(dev_GetCapabilities) {
    uint32_t p = ARG(1);
    Dev *d = COM_HOST(Dev);
    gw32(p + 4, 7); gw32(p + 8, d->kind == 1 ? 0x13 : 0x12);
    gw32(p + 12, d->kind == 1 ? 0 : 2); gw32(p + 16, d->kind == 1 ? 128 : 3); gw32(p + 20, 0);
    return 0;
}
API(dev_ok) { return 0; }
API(dev_SetProperty) {
    Dev *d = COM_HOST(Dev);
    if (ARG(1) == 1) d->buffer = gr32(ARG(2) + 16);   /* DIPROP_BUFFERSIZE */
    return 0;
}
API(dev_GetDeviceState) {
    Dev *d = COM_HOST(Dev);
    uint32_t n = ARG(1), p = ARG(2);
    if (d->kind == 1) {
        memcpy(GPTR(p), dik_state, n < 256 ? n : 256);
        for (int i = 0; i < 256; i++) {
            dik_seen[i] = 1;
            if (dik_pending_up[i]) { dik_pending_up[i] = 0; if (!dik_refs[i]) dik_state[i] = 0; }
        }
        return 0;
    }
    memset(GPTR(p), 0, n);
    if (n >= 16) { gw32(p, mouse_dx); gw32(p + 4, mouse_dy); for (int i = 0; i < 4; i++) gw8(p + 12 + i, mouse_btn[i]); }
    mouse_dx = mouse_dy = 0;
    return 0;
}
API(dev_GetDeviceData) {
    Dev *d = COM_HOST(Dev);
    uint32_t sz = ARG(1), out = ARG(2), pn = ARG(3), fl = ARG(4);
    uint32_t want = gr32(pn), got = 0;
    if (d->kind != 1) { gw32(pn, 0); return 0; }
    int h = kq_head;
    while (h != kq_tail && got < want) {
        if (out) {
            uint32_t o = out + got * sz;
            memset(GPTR(o), 0, sz);
            gw32(o, kq[h].ofs); gw32(o + 4, kq[h].down ? 0x80 : 0); gw32(o + 8, kq[h].t); gw32(o + 12, ++kq_seq);
        }
        h = (h + 1) & 255; got++;
    }
    if (!(fl & 1)) kq_head = h;   /* DIGDD_PEEK leaves the queue */
    gw32(pn, got);
    return 0;
}

static uint32_t dev_vtbl;
API(di_CreateDevice) {
    uint32_t g = gr32(ARG(1));
    Dev *d = calloc(1, sizeof *d);
    d->kind = g == 0x6F1D2B61u ? 1 : g == 0x6F1D2B60u ? 2 : 0;
    LOG("DirectInput CreateDevice(%08x) kind=%d", g, d->kind);
    if (!d->kind) { free(d); gw32(ARG(2), 0); return 0x80040154u; }
    if (!dev_vtbl) {
        static const ComMethod m[] = {
            {"QueryInterface", api_di_QueryInterface, 3}, {"AddRef", api_di_AddRef, 1}, {"Release", api_di_Release, 1},
            {"GetCapabilities", api_dev_GetCapabilities, 2}, {"EnumObjects", api_dev_ok, 4}, {"GetProperty", api_dev_ok, 3},
            {"SetProperty", api_dev_SetProperty, 3}, {"Acquire", api_dev_ok, 1}, {"Unacquire", api_dev_ok, 1},
            {"GetDeviceState", api_dev_GetDeviceState, 3}, {"GetDeviceData", api_dev_GetDeviceData, 5},
            {"SetDataFormat", api_dev_ok, 2}, {"SetEventNotification", api_dev_ok, 2},
            {"SetCooperativeLevel", api_dev_ok, 3}, {"GetObjectInfo", api_dev_ok, 4}, {"GetDeviceInfo", api_dev_ok, 2},
            {"RunControlPanel", api_dev_ok, 3}, {"Initialize", api_dev_ok, 4},
        };
        dev_vtbl = com_make_vtbl("IDirectInputDevice", m, sizeof m / sizeof *m);
    }
    gw32(ARG(2), com_new(dev_vtbl, 0, d));
    return 0;
}
API(di_EnumDevices) { return 0; }   /* no joysticks */
API(di_GetDeviceStatus) { return 1; /* DI_NOTATTACHED */ }
API(di_ok) { return 0; }

API(DirectInputCreateA) {
    static uint32_t vt, obj;
    if (!vt) {
        static const ComMethod m[] = {
            {"QueryInterface", api_di_QueryInterface, 3}, {"AddRef", api_di_AddRef, 1}, {"Release", api_di_Release, 1},
            {"CreateDevice", api_di_CreateDevice, 4}, {"EnumDevices", api_di_EnumDevices, 5},
            {"GetDeviceStatus", api_di_GetDeviceStatus, 2}, {"RunControlPanel", api_di_ok, 3}, {"Initialize", api_di_ok, 3},
        };
        vt = com_make_vtbl("IDirectInput", m, sizeof m / sizeof *m);
        obj = com_new(vt, 0, NULL);
    }
    LOG("DirectInputCreateA(ver=%x)", ARG(1));
    gw32(ARG(2), obj);
    return 0;
}

const ApiDef dinput_apis[] = { DEF("dinput.dll", DirectInputCreateA, 4), { 0 } };
