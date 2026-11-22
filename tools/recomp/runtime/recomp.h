/* Runtime for code translated from ARM32 (Thumb-2 and ARM mode) by tools/recomp/lift.py.
 *
 * Game memory is a byte array `m`; a game address is an offset into it. CPU state the translated
 * functions share lives in `cpu_t`. Build with -fwrapv -fno-strict-aliasing -ffp-contract=off.
 * RECOMP_CHECKED (test builds) bounds-checks every access and caps loop iterations, so a wrong
 * translation reports a fault instead of crashing the test process.
 */
#ifndef RECOMP_H
#define RECOMP_H

#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

typedef struct cpu {
    uint32_t r[16];
    uint32_t n, z, c, v;
    uint32_t fpscr;
    uint32_t fault;   /* set on an out-of-range access, an exhausted budget or an unknown jump target (see RC_FAULT_*) */
    uint32_t budget;  /* RECOMP_CHECKED: backward branches left */
    uint32_t memsize; /* RECOMP_CHECKED: size of m */
    uint32_t depth;   /* call depth of translated functions */
    uint32_t tls;     /* value __kuser_get_tls returns */
    uint32_t pad;
    union {
        uint64_t d[32];
        uint32_t u32[64];
        double f64[32];
        float s[64];
    } vfp;
    uint32_t trace[64]; /* RECOMP_CHECKED: game address of the function entered at each call depth (mod 64) */
} cpu_t;

enum {
    RC_FAULT_MEMORY = 1,   /* access outside m */
    RC_FAULT_BUDGET = 2,   /* loop budget exhausted */
    RC_FAULT_END = 3,      /* ran off the end of a function into no function */
    RC_FAULT_DISPATCH = 4, /* jump or call to an address that is no entry */
    RC_FAULT_RETURN = 5,   /* unused: a call that comes back elsewhere is a non-local return (RC_AFTER_CALL) */
    RC_FAULT_DEPTH = 6,    /* call depth limit */
    RC_FAULT_UNLIFTED = 7, /* entered a function the lifter could not translate */
    RC_FAULT_IMPORT = 8,   /* the import handler could not run the import */
};

/* After a call: if the callee came back to another address than the call's return address, it has already
 * returned for its caller too (libgcc's __muldf3 calls a helper with BL that pops __muldf3's own frame and
 * returns to __muldf3's caller). The state in c is final, so this function leaves at once without writing its
 * locals back; its caller does the same check. A wrong translation still shows: the outermost r15 is wrong. */
#ifdef RECOMP_CHECKED
#define RC_OK(c, a, n) ((uint64_t)(a) + (n) <= (c)->memsize)
#define RC_BUDGET(c) do { if ((c)->budget-- == 0) { (c)->fault = RC_FAULT_BUDGET; RC_TAIL(); } } while (0)
#define RC_ENTER(fn) do {                                                              \
        if (++c->depth > 2000) { c->fault = RC_FAULT_DEPTH; c->depth--; return; }   \
        c->trace[c->depth & 63] = (fn);                                              \
    } while (0)
#define RC_AFTER_CALL(ret) do { if (c->fault || c->r[15] != (ret)) RC_TAIL(); } while (0)
#else
#define RC_OK(c, a, n) 1
#define RC_BUDGET(c) do { } while (0)
#define RC_ENTER(fn) do { c->depth++; } while (0)
#define RC_AFTER_CALL(ret) do { if (c->r[15] != (ret)) RC_TAIL(); } while (0)
#endif

/* Calls: the caller's register locals go to c before a call and come back after it. */
#define RC_SAVE() do {                                                                         \
        c->r[0] = r0; c->r[1] = r1; c->r[2] = r2; c->r[3] = r3; c->r[4] = r4; c->r[5] = r5;   \
        c->r[6] = r6; c->r[7] = r7; c->r[8] = r8; c->r[9] = r9; c->r[10] = r10; c->r[11] = r11; \
        c->r[12] = r12; c->r[13] = r13; c->r[14] = r14;                                       \
        c->n = n; c->z = z; c->c = cf; c->v = v;                                               \
    } while (0)
#define RC_LOAD() do {                                                                         \
        r0 = c->r[0]; r1 = c->r[1]; r2 = c->r[2]; r3 = c->r[3]; r4 = c->r[4]; r5 = c->r[5];   \
        r6 = c->r[6]; r7 = c->r[7]; r8 = c->r[8]; r9 = c->r[9]; r10 = c->r[10]; r11 = c->r[11]; \
        r12 = c->r[12]; r13 = c->r[13]; r14 = c->r[14];                                       \
        n = c->n; z = c->z; cf = c->c; v = c->v;                                               \
    } while (0)
/* Leave after a tail call: the callee already left the final state (including r15) in c. */
#define RC_TAIL() do { c->depth--; return; } while (0)

/* Generated with the translated functions: call the function at a game address, or an import by slot. The dispatcher
 * names functions by their link address without the Thumb bit; translated code reaches it through RC_DISPATCH, which
 * clears bit 0 (a Thumb function pointer, or the target of bx/blx, carries it; return addresses in lr do too and are
 * never dispatched). The generated dispatcher may also accept the odd form itself. */
void rc_dispatch(struct cpu *c, uint8_t *m, uint32_t target);
#define RC_DISPATCH(c, m, t) rc_dispatch((c), (m), (t) & ~1u)
void rc_import(struct cpu *c, uint8_t *m, uint32_t slot);
void rc_hostcall(struct cpu *c, uint8_t *m, uint32_t slot); /* runtime functions called through a game address */

/* Loads and stores (little-endian host, unaligned allowed as on ARMv7). */
#define RC_LD(bits)                                                                         \
    static inline uint##bits##_t ld##bits(cpu_t *c, uint8_t *m, uint32_t a) {               \
        uint##bits##_t v = 0;                                                               \
        if (RC_OK(c, a, bits / 8)) memcpy(&v, m + a, bits / 8); else c->fault = 1;          \
        return v;                                                                           \
    }                                                                                       \
    static inline void st##bits(cpu_t *c, uint8_t *m, uint32_t a, uint##bits##_t v) {      \
        if (RC_OK(c, a, bits / 8)) memcpy(m + a, &v, bits / 8); else c->fault = 1;          \
    }
RC_LD(8)
RC_LD(16)
RC_LD(32)
RC_LD(64)

/* Shifter. kind: 1 ASR, 2 LSL, 3 LSR, 4 ROR, 5 RRX (capstone's numbering). n is the amount (0-255). */
static inline uint32_t shc(int kind, uint32_t x, uint32_t n, uint32_t cin, uint32_t *cout) {
    switch (kind) {
    case 2: /* LSL */
        if (n == 0) { *cout = cin; return x; }
        if (n < 32) { *cout = (x >> (32 - n)) & 1; return x << n; }
        *cout = n == 32 ? (x & 1) : 0;
        return 0;
    case 3: /* LSR */
        if (n == 0) { *cout = cin; return x; }
        if (n < 32) { *cout = (x >> (n - 1)) & 1; return x >> n; }
        *cout = n == 32 ? (x >> 31) : 0;
        return 0;
    case 1: /* ASR */
        if (n == 0) { *cout = cin; return x; }
        if (n < 32) { *cout = (x >> (n - 1)) & 1; return (uint32_t)((int32_t)x >> n); }
        *cout = x >> 31;
        return (uint32_t)((int32_t)x >> 31);
    case 4: /* ROR */
        if (n == 0) { *cout = cin; return x; }
        n &= 31;
        if (n == 0) { *cout = x >> 31; return x; }
        x = (x >> n) | (x << (32 - n));
        *cout = x >> 31;
        return x;
    default: /* RRX */
        *cout = x & 1;
        return (cin << 31) | (x >> 1);
    }
}

static inline uint32_t addc(uint32_t a, uint32_t b, uint32_t cin, uint32_t *co, uint32_t *vo) {
    uint64_t u = (uint64_t)a + b + cin;
    int64_t s = (int64_t)(int32_t)a + (int32_t)b + (int32_t)cin;
    uint32_t r = (uint32_t)u;
    *co = (uint32_t)(u >> 32) & 1;
    *vo = (int64_t)(int32_t)r != s;
    return r;
}

/* setjmp. A translated function that calls setjmp (through the PLT) must be resumable by a later longjmp from deeper C
 * frames, so the host setjmp runs in that function's own C frame. Contract with the runtime (runtime/libc.c):
 *  - the lifter emits `r14 = <return address>; r12 = <got slot>; RC_SAVE(); RC_SETJMP();` for a call to setjmp, _setjmp,
 *    sigsetjmp or __sigsetjmp, with r0 = the guest jmp_buf; execution continues with the next instruction;
 *  - rt_setjmp_prepare(c, guest_jmp_buf) (runtime) saves r4-r11, sp, lr, d8-d15 from c into the guest buffer, records
 *    c->depth and returns the host jmp_buf kept for that guest address; RC_SETJMP runs setjmp on it;
 *  - the first return makes r0 = 0; a later longjmp (runtime) fills c (r0 = value, r4-r11, sp, lr, d8-d15, r15 = lr),
 *    restores c->depth and host-longjmps here: the second return reloads every register local and the flags from c. */
void *rt_setjmp_prepare(struct cpu *c, uint32_t guest_jmp_buf);
#define RC_SETJMP() do { \
        if (setjmp(*(jmp_buf *)rt_setjmp_prepare(c, c->r[0])) == 0) { r0 = 0; } else { RC_LOAD(); } \
    } while (0)

/* Linux kernel user helpers at 0xffff0fxx. Returns 0 when `a` is not one. */
static inline int rc_kuser(cpu_t *c, uint8_t *m, uint32_t a) {
    switch (a) {
    case 0xffff0fc0u: { /* __kuser_cmpxchg(oldval r0, newval r1, ptr r2): r0 = 0 and C set when stored */
        uint32_t ok = 0, expected = c->r[0];
        if (RC_OK(c, c->r[2], 4) && (c->r[2] & 3) == 0)
            ok = __atomic_compare_exchange_n((uint32_t *)(m + c->r[2]), &expected, c->r[1], 0, __ATOMIC_SEQ_CST,
                                             __ATOMIC_SEQ_CST);
        else
            c->fault = RC_FAULT_MEMORY;
        c->r[0] = !ok; c->c = ok; c->z = ok;
        break;
    }
    case 0xffff0fa0u: /* __kuser_memory_barrier */
        break;
    case 0xffff0fe0u: /* __kuser_get_tls */
        c->r[0] = c->tls;
        break;
    default:
        return 0;
    }
    c->r[15] = c->r[14];
    return 1;
}

/* Exclusive stores (ldrex/strex family) with single-threaded semantics but a real atomic compare-exchange: the
 * translated function keeps the monitor in two locals (address `exa`, the value `exv` that ldrex read) and strex
 * stores only if the address matches and the memory still holds that value. Returns the strex status (0 = stored). */
#define RC_STREX(bits) \
    static inline uint32_t rc_strex##bits(cpu_t *c, uint8_t *m, uint32_t ea, uint32_t exa, uint##bits##_t exv, \
                                          uint##bits##_t val) { \
        if (ea != exa) return 1; \
        if (!RC_OK(c, ea, bits / 8) || (ea & (bits / 8 - 1))) { c->fault = RC_FAULT_MEMORY; return 1; } \
        uint##bits##_t e = exv; \
        return !__atomic_compare_exchange_n((uint##bits##_t *)(m + ea), &e, val, 0, __ATOMIC_SEQ_CST, \
                                            __ATOMIC_SEQ_CST); \
    }
RC_STREX(8)
RC_STREX(16)
RC_STREX(32)
RC_STREX(64)

static inline uint32_t ror32(uint32_t x, unsigned n) { n &= 31; return n ? (x >> n) | (x << (32 - n)) : x; }
static inline uint32_t clz32(uint32_t x) { return x ? (uint32_t)__builtin_clz(x) : 32; }
static inline uint32_t rev32(uint32_t x) { return __builtin_bswap32(x); }
static inline uint32_t rev16(uint32_t x) { return ((x & 0xff00ff00u) >> 8) | ((x & 0x00ff00ffu) << 8); }
static inline uint32_t revsh(uint32_t x) { return (uint32_t)(int32_t)(int16_t)(((x & 0xffu) << 8) | ((x >> 8) & 0xffu)); }
static inline uint32_t rbit32(uint32_t x) {
    x = ((x >> 1) & 0x55555555u) | ((x & 0x55555555u) << 1);
    x = ((x >> 2) & 0x33333333u) | ((x & 0x33333333u) << 2);
    x = ((x >> 4) & 0x0f0f0f0fu) | ((x & 0x0f0f0f0fu) << 4);
    return __builtin_bswap32(x);
}
/* udiv/sdiv as the ARMv7-A core does them: division by zero gives 0, INT_MIN / -1 gives INT_MIN */
static inline uint32_t udiv32(uint32_t a, uint32_t b) { return b ? a / b : 0; }
static inline uint32_t sdiv32(uint32_t a, uint32_t b) {
    if (b == 0) return 0;
    if ((int32_t)a == INT32_MIN && (int32_t)b == -1) return a;
    return (uint32_t)((int32_t)a / (int32_t)b);
}
/* Saturation. The Q flag is not modelled. */
static inline uint32_t rc_ssat(int32_t x, unsigned bits) {
    int64_t hi = (1ll << (bits - 1)) - 1, lo = -(1ll << (bits - 1));
    return (uint32_t)(int32_t)(x > hi ? hi : x < lo ? lo : x);
}
static inline uint32_t rc_usat(int32_t x, unsigned bits) {
    int64_t hi = (1ll << bits) - 1;
    return (uint32_t)(x < 0 ? 0 : x > hi ? hi : x);
}
static inline uint32_t rc_qadd(uint32_t a, uint32_t b) {
    int64_t s = (int64_t)(int32_t)a + (int32_t)b;
    return (uint32_t)(int32_t)(s > INT32_MAX ? INT32_MAX : s < INT32_MIN ? INT32_MIN : s);
}
static inline uint32_t rc_qsub(uint32_t a, uint32_t b) {
    int64_t s = (int64_t)(int32_t)a - (int32_t)b;
    return (uint32_t)(int32_t)(s > INT32_MAX ? INT32_MAX : s < INT32_MIN ? INT32_MIN : s);
}

/* VFP. Comparisons give the FPSCR NZCV nibble, conversions saturate like the hardware. */
static inline uint32_t fcmp(double a, double b) {
    if (a != a || b != b) return 0x3u << 28; /* unordered: C V */
    if (a == b) return 0x6u << 28;           /* Z C */
    if (a < b) return 0x8u << 28;            /* N */
    return 0x2u << 28;                       /* C */
}
static inline uint32_t f2s32(double x) {
    if (x != x) return 0;
    if (x >= 2147483647.0) return 0x7fffffffu;
    if (x <= -2147483648.0) return 0x80000000u;
    return (uint32_t)(int32_t)x;
}
static inline uint32_t f2u32(double x) {
    if (x != x || x <= 0.0) return 0;
    if (x >= 4294967295.0) return 0xffffffffu;
    return (uint32_t)x;
}
static inline float bf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t fb(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline double bd(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static inline uint64_t db(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

#endif
