#pragma once
/* Macros used by generated code */
#include <math.h>
#include <string.h>
#include "cpu.h"

void rt_call(CPU *c, uint32_t target);
void rt_badjump(CPU *c, uint32_t at, uint32_t target);
void rt_trap(CPU *c, uint32_t at);
void rt_unimpl(CPU *c, uint32_t at, const char *what);
void rt_fallthrough(CPU *c, uint32_t at);
void rt_divzero(CPU *c, uint32_t at);
void rt_cpuid(CPU *c);
void rt_hook(CPU *c, uint32_t at);
uint64_t rt_rdtsc(void);
void fpu_fxam(CPU *c);
void fpu_fxtract(CPU *c);
void fpu_init(CPU *c);
void fpu_fnstenv(CPU *c, uint32_t a);
void fpu_fldenv(CPU *c, uint32_t a);
void fpu_fnsave(CPU *c, uint32_t a);
void fpu_frstor(CPU *c, uint32_t a);
double fpu_ld80(const uint8_t *p);
void fpu_st80(uint8_t *p, double v);
double fpu_round(CPU *c, double v);
uint32_t fpu_toint32(CPU *c, double v);
uint16_t fpu_toint16(CPU *c, double v);
uint64_t fpu_toint64(CPU *c, double v);

static inline void rt_movs(uint8_t *M, uint32_t d, uint32_t s, uint32_t n) {
    /* byte-wise forward semantic: overlapping forward copies must replicate */
    if (d > s && d - s < n) { for (uint32_t i = 0; i < n; i++) M[(uint32_t)(d + i)] = M[(uint32_t)(s + i)]; }
    else memmove(M + d, M + s, n);
}
static inline void rt_stos32(uint8_t *M, uint32_t d, uint32_t v, uint32_t n) {
    if (v == 0) { memset(M + d, 0, (size_t)n * 4); return; }
    for (uint32_t i = 0; i < n; i++) *(u32u *)(M + d + i * 4) = v;
}

#define R8(a) (*(uint8_t *)(M + (uint32_t)(a)))
#define R16(a) (*(u16u *)(M + (uint32_t)(a)))
#define R32(a) (*(u32u *)(M + (uint32_t)(a)))
#define R64(a) (*(u64u *)(M + (uint32_t)(a)))
#define RF32(a) ((double)*(f32u *)(M + (uint32_t)(a)))
#define RF64(a) (*(f64u *)(M + (uint32_t)(a)))
#define W8(a, v) (*(uint8_t *)(M + (uint32_t)(a)) = (uint8_t)(v))
#define W16(a, v) (*(u16u *)(M + (uint32_t)(a)) = (uint16_t)(v))
#define W32(a, v) (*(u32u *)(M + (uint32_t)(a)) = (uint32_t)(v))
#define W64(a, v) (*(u64u *)(M + (uint32_t)(a)) = (uint64_t)(v))
#define WF32(a, v) (*(f32u *)(M + (uint32_t)(a)) = (float)(v))
#define WF64(a, v) (*(f64u *)(M + (uint32_t)(a)) = (double)(v))

#define PARITY(r) (!__builtin_parity((r) & 0xff))

#define DECLS() \
    uint8_t *const M = g_mem; \
    uint32_t eax = c->eax, ecx = c->ecx, edx = c->edx, ebx = c->ebx, esp = c->esp, ebp = c->ebp, esi = c->esi, edi = c->edi; \
    uint32_t cf = 0, zf = 0, sf = 0, of = 0, pf = 0, fa = 0, fb = 0, fr = 0; \
    (void)cf; (void)zf; (void)sf; (void)of; (void)pf; (void)fa; (void)fb; (void)fr
#define SAVE() do { c->eax = eax; c->ecx = ecx; c->edx = edx; c->ebx = ebx; c->esp = esp; c->ebp = ebp; c->esi = esi; c->edi = edi; } while (0)
#define LOAD() do { eax = c->eax; ecx = c->ecx; edx = c->edx; ebx = c->ebx; esp = c->esp; ebp = c->ebp; esi = c->esi; edi = c->edi; } while (0)
#define MKFLAGS() (cf | 2 | (pf << 2) | (zf << 6) | (sf << 7) | (c->df << 10) | (of << 11) | 0x200)
#define SETFLAGS(v) do { cf = (v) & 1; pf = ((v) >> 2) & 1; zf = ((v) >> 6) & 1; sf = ((v) >> 7) & 1; c->df = ((v) >> 10) & 1; of = ((v) >> 11) & 1; } while (0)

/* x87 */
#define ST(i) (c->fpr[(c->ftop + (i)) & 7])
#define FPUSH(v) do { double _v = (v); c->ftop = (c->ftop - 1) & 7; c->fpr[c->ftop] = _v; c->fiv[c->ftop] = 0; } while (0)
#define FI(i) (c->fi[(c->ftop + (i)) & 7])
#define FIV(i) (c->fiv[(c->ftop + (i)) & 7])
/* the register still holds exactly the integer fild loaded */
#define FIEXACT(i) (FIV(i) && (double)FI(i) == ST(i))
#define FPOP() (c->ftop = (c->ftop + 1) & 7)
#define FSW() ((uint16_t)((c->fsw & ~0x3800) | ((c->ftop & 7) << 11)))
#define FCOM(x, y) do { double _a = (x), _b = (y); c->fsw &= ~0x4500; \
    if (_a != _a || _b != _b) c->fsw |= 0x4500; else if (_a < _b) c->fsw |= 0x0100; else if (_a == _b) c->fsw |= 0x4000; } while (0)
