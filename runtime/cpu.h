#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct CPU {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t df;
    uint32_t fs_base;
    /* x87 */
    double fpr[8];
    int64_t fi[8];      /* exact 64-bit integer loaded by fild qword (a double can't hold it): */
    uint8_t fiv[8];     /* valid while the register still holds that value (memcpy via fild/fistp) */
    uint32_t ftop;
    uint16_t fsw;   /* condition codes etc (TOP kept in ftop) */
    uint16_t fcw;
    /* thread bookkeeping */
    int thread_id;
    volatile int kill;
    void *exit_jmp;
    uint32_t last_error;
} CPU;

typedef void (*GuestFn)(CPU *c);


typedef uint8_t u8;
typedef uint16_t __attribute__((aligned(1))) u16u;
typedef uint32_t __attribute__((aligned(1))) u32u;
typedef uint64_t __attribute__((aligned(1))) u64u;
typedef float __attribute__((aligned(1))) f32u;
typedef double __attribute__((aligned(1))) f64u;

#ifndef C2_GEN
/* host-side access helpers (runtime only) */
extern uint8_t *g_mem;
#define GPTR(a) ((void *)(g_mem + (uint32_t)(a)))
static inline uint32_t gr32(uint32_t a) { return *(u32u *)(g_mem + a); }
static inline uint16_t gr16(uint32_t a) { return *(u16u *)(g_mem + a); }
static inline uint8_t gr8(uint32_t a) { return g_mem[a]; }
static inline void gw32(uint32_t a, uint32_t v) { *(u32u *)(g_mem + a) = v; }
static inline void gw16(uint32_t a, uint16_t v) { *(u16u *)(g_mem + a) = v; }
static inline void gw8(uint32_t a, uint8_t v) { g_mem[a] = v; }
static inline float grf(uint32_t a) { return *(f32u *)(g_mem + a); }
static inline void gwf(uint32_t a, float v) { *(f32u *)(g_mem + a) = v; }
#endif
