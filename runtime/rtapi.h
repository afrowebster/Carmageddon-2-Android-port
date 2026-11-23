#pragma once
/* The interface between the runtime and the recompiled game code (libgame).
 * The game code is built separately (on the user's machine, from their own copy of the game) and is
 * freestanding: everything it needs from the runtime or the C library comes through this table. */
#include "cpu.h"

#define C2_RTAPI_VERSION 1

typedef struct { uint32_t addr; GuestFn fn; } FuncEntry;
typedef struct { uint32_t iat; const char *dll; const char *name; } ImportEntry;
typedef struct { const char *file; uint32_t base; int nfuncs; const FuncEntry *funcs; const ImportEntry *imports; int nimports; } LiftedModule;

typedef struct RtApi {
    uint32_t version;
    uint8_t *mem;
    void (*call)(CPU *c, uint32_t target);
    void (*badjump)(CPU *c, uint32_t at, uint32_t target);
    void (*trap)(CPU *c, uint32_t at);
    void (*unimpl)(CPU *c, uint32_t at, const char *what);
    void (*fallthrough)(CPU *c, uint32_t at);
    void (*divzero)(CPU *c, uint32_t at);
    void (*cpuid)(CPU *c);
    void (*hook)(CPU *c, uint32_t at);
    uint64_t (*rdtsc)(void);
    void (*fpu_fxam)(CPU *c);
    void (*fpu_fxtract)(CPU *c);
    void (*fpu_init)(CPU *c);
    void (*fpu_fnstenv)(CPU *c, uint32_t a);
    void (*fpu_fldenv)(CPU *c, uint32_t a);
    void (*fpu_fnsave)(CPU *c, uint32_t a);
    void (*fpu_frstor)(CPU *c, uint32_t a);
    double (*fpu_ld80)(const uint8_t *p);
    void (*fpu_st80)(uint8_t *p, double v);
    double (*fpu_round)(CPU *c, double v);
    uint32_t (*fpu_toint32)(CPU *c, double v);
    uint16_t (*fpu_toint16)(CPU *c, double v);
    uint64_t (*fpu_toint64)(CPU *c, double v);
    void *(*memmove)(void *d, const void *s, size_t n);
    void *(*memset)(void *d, int v, size_t n);
    double (*sin)(double), (*cos)(double), (*tan)(double), (*atan2)(double, double), (*ldexp)(double, int),
           (*log2)(double), (*fmod)(double, double), (*exp2)(double), (*remainder)(double, double), (*trunc)(double);
} RtApi;

/* libgame's entry point: checks the runtime version, keeps the table, returns the modules
   (exe, bdd) or NULL on a version mismatch */
typedef const LiftedModule *const *(*C2GameInit)(const RtApi *rt);
const LiftedModule *const *c2_game_init(const RtApi *rt);
