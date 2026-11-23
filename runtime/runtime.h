#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"

#include "rtapi.h"
/* guest-side modules (lifted DLLs loaded by our loader) */
uint32_t rt_module_export(const char *dll, const char *name);
uint32_t rt_module_handle(const char *dll);
int rt_is_code(uint32_t a);

/* ---- memory layout ---- */
#define GUEST_TEB      0x00010000u
#define GUEST_PEB      0x00011000u
#define GUEST_MISC     0x00012000u   /* command line, env strings, ... */
#define GUEST_STACK_LO 0x00100000u
#define GUEST_STACK_HI 0x00300000u
#define GUEST_HEAP_LO  0x30000000u
#define GUEST_HEAP_HI  0xE0000000u
#define THUNK_BASE     0xF0000000u
#define CB_RETURN      0xEFFFFFF0u   /* magic return address for host->guest calls */

/* ---- host API thunks ---- */
typedef uint32_t (*HostFn)(CPU *c);
/* register a host function; argbytes popped on return (stdcall); returns guest address */
uint32_t rt_host(const char *name, HostFn fn, int argbytes);
#define ARG(i) gr32(c->esp + 4 + 4 * (i))
#define ARGF(i) grf(c->esp + 4 + 4 * (i))

/* invoke guest function from host (stdcall or cdecl both fine) */
uint32_t rt_invoke(CPU *c, uint32_t fn, int n, ...);

/* guest heap */
uint32_t gmalloc(uint32_t size);
uint32_t gcalloc(uint32_t size);
void gfree(uint32_t p);
uint32_t grealloc(uint32_t p, uint32_t size);
uint32_t gmsize(uint32_t p);
uint32_t gstrdup(const char *s);

/* logging */
extern int g_trace;
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rt_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
#define LOG(...) rt_log(__VA_ARGS__)
#define TRACE(...) do { if (g_trace) rt_log(__VA_ARGS__); } while (0)

/* image */
extern uint32_t g_image_base, g_image_size;
extern char g_game_dir[1024];
extern char g_save_dir[1024];

/* threads */
CPU *rt_new_cpu(uint32_t stack_size);
extern __thread CPU *t_cpu;

/* win32 layer init */
void win32_init(void);
void win32_register_imports(void);
uint32_t win32_resolve(const char *dll, const char *name);
/* path translation */
int host_path(const char *winpath, char *out, size_t outsz, int for_create);

/* platform */
void plat_init(void);
void plat_pump_events(void);
uint32_t plat_ticks(void);
void plat_sleep(uint32_t ms);

/* settings (platform/settings.c) */
const char *setting_str(const char *key, const char *def);
int setting_int(const char *key, int def);
void setting_set(const char *key, const char *val);
void setting_set_int(const char *key, int v);
