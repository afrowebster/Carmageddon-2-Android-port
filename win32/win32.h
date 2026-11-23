#pragma once
#include <stdarg.h>
#include "../runtime/runtime.h"

typedef struct ApiDef { const char *dll; const char *name; HostFn fn; int argbytes; } ApiDef;

/* each module defines a NULL-terminated table */
extern const ApiDef k32_apis[], user32_apis[], gdi32_apis[], misc_apis[], dsound_apis[], glide_apis[], misc2_apis[], dinput_apis[], cdaudio_apis[], smack_apis[];

#define API(name) static uint32_t api_##name(CPU *c)
#define DEF(dll, name, nargs) { dll, #name, api_##name, (nargs) * 4 }

/* guest string helpers */
static inline const char *gstr(uint32_t a) { return a ? (const char *)GPTR(a) : NULL; }
static inline void gstrcpy(uint32_t dst, const char *s, uint32_t max) {
    if (!max) return;
    uint32_t n = strlen(s);
    if (n >= max) n = max - 1;
    memcpy(GPTR(dst), s, n); gw8(dst + n, 0);
}

/* window messages */
#define WM_NULL 0x0000
#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_PAINT 0x000F
#define WM_CLOSE 0x0010
#define WM_QUIT 0x0012
#define WM_ERASEBKGND 0x0014
#define WM_SHOWWINDOW 0x0018
#define WM_ACTIVATEAPP 0x001C
#define WM_SETCURSOR 0x0020
#define WM_GETMINMAXINFO 0x0024
#define WM_WINDOWPOSCHANGED 0x0047
#define WM_NCCREATE 0x0081
#define WM_NCDESTROY 0x0082
#define WM_NCACTIVATE 0x0086
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_INITDIALOG 0x0110
#define WM_COMMAND 0x0111
#define WM_SYSCOMMAND 0x0112
#define WM_TIMER 0x0113
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205

/* user32 internals shared with others */
uint32_t user32_main_hwnd(void);
void user32_post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
uint32_t user32_send(CPU *c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
void user32_key_event(int vk, int down);
void user32_mouse_event(int x, int y, int buttons, int what);
void user32_activate(int active);
void user32_quit_request(void);

/* resources */
uint32_t res_find(uint32_t type, uint32_t name, uint32_t *size);  /* type/name: guest ptr to string or int id */
uint32_t res_find_host(int type_id, const char *type_name, int name_id, const char *name_name, uint32_t *size);

/* thread */
int k32_thread_killed(CPU *c);
void k32_check_kill(CPU *c);

/* COM helper: create guest object with vtable of host methods */
typedef struct ComMethod { const char *name; HostFn fn; int nargs; /* including this */ } ComMethod;
uint32_t com_make_vtbl(const char *iface, const ComMethod *m, int n);
uint32_t com_new(uint32_t vtbl, uint32_t extra_bytes, void *host);
void *com_host(uint32_t obj);
#define COM_HOST(T) ((T *)com_host(ARG(0)))
