/* Calling conventions, calls into game code, host functions, import dispatch, diagnostics. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"

int rt_verbose;

#ifdef __ANDROID__
#include <android/log.h>
void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, "nova3", fmt, ap);
    va_end(ap);
}

void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_FATAL, "nova3", fmt, ap);
    va_end(ap);
    abort();
}
#else
void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("fatal: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
    abort();
}
#endif

/* ---- path aliases: the game's own folders under /data/data/<package> ------------------------------ */
static struct { char *from, *to; size_t n; } aliases[8];
static int nalias;

void rt_path_alias(const char *from, const char *to) {
    if (nalias >= 8) return;
    aliases[nalias].from = strdup(from);
    aliases[nalias].to = strdup(to);
    aliases[nalias].n = strlen(from);
    nalias++;
}

const char *rt_host_path(const char *path) {
    static __thread char buf[1024];
    for (int k = 0; k < nalias; k++)
        if (!strncmp(path, aliases[k].from, aliases[k].n)) {
            snprintf(buf, sizeof buf, "%s%s", aliases[k].to, path + aliases[k].n);
            return buf;
        }
    return path;
}

/* ---- arguments (ARM softfp): words 0-3 in r0-r3, then the caller's stack ----------------------- */
uint32_t rt_arg(cpu_t *c, int k) { return k < 4 ? c->r[k] : rt_ld32(c->r[13] + 4 * (k - 4)); }

double rt_darg(cpu_t *c, int k) {
    uint64_t u = (uint64_t)rt_arg(c, k) | ((uint64_t)rt_arg(c, k + 1) << 32);
    double d;
    memcpy(&d, &u, 8);
    return d;
}

float rt_farg(cpu_t *c, int k) {
    uint32_t u = rt_arg(c, k);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

void rt_dret(cpu_t *c, double v) {
    uint64_t u;
    memcpy(&u, &v, 8);
    c->r[0] = (uint32_t)u;
    c->r[1] = (uint32_t)(u >> 32);
}

/* ---- calls into game code ----------------------------------------------------------------------
 * The current state is saved, the arguments are placed per the ABI, LR points at the return sentinel, and
 * the translated function runs as an ordinary C call. Afterwards the caller's state comes back. */
uint32_t rt_call(cpu_t *c, uint32_t fn, int nargs, ...) {
    cpu_t saved = *c;
    uint32_t words[16];
    va_list ap;
    va_start(ap, nargs);
    for (int k = 0; k < nargs && k < 16; k++) words[k] = va_arg(ap, uint32_t);
    va_end(ap);
    uint32_t sp = (c->r[13] - 64) & ~7u; /* leave the caller's red zone alone */
    if (nargs > 4) {
        sp -= ((nargs - 4) * 4 + 7) & ~7u;
        for (int k = 4; k < nargs; k++) rt_st32(sp + 4 * (k - 4), words[k]);
    }
    for (int k = 0; k < 4 && k < nargs; k++) c->r[k] = words[k];
    c->r[13] = sp;
    c->r[14] = RT_RETURN_SENTINEL;
    rc_dispatch(c, rt_mem, fn);
    uint32_t result = c->r[0];
    uint32_t fault = c->fault;
    if (c->r[15] != RT_RETURN_SENTINEL && !fault)
        rt_log("call to %#x came back to %#x", fn, c->r[15]);
    *c = saved;
    c->fault = fault;
    return result;
}

/* ---- host functions ------------------------------------------------------------------------------ */
static rt_hostfn hostfns[RT_HOSTFN_MAX];
static const char *hostfn_names[RT_HOSTFN_MAX];
static uint32_t hostfn_count = 1; /* slot 0 is the return sentinel */

uint32_t rt_hostfn_add(rt_hostfn fn, const char *name) {
    if (hostfn_count >= RT_HOSTFN_MAX) rt_fatal("too many host functions");
    hostfns[hostfn_count] = fn;
    hostfn_names[hostfn_count] = name;
    return RT_HOSTFN_BASE + 4 * hostfn_count++;
}

__thread uint32_t rt_current_hostfn;

void rc_hostcall(cpu_t *c, uint8_t *m, uint32_t slot) {
    if (slot == 0) { c->r[15] = RT_RETURN_SENTINEL; return; } /* a computed jump to the sentinel: just return */
    if (slot >= hostfn_count) { rt_log("call to unknown host function slot %u", slot); c->fault = RC_FAULT_DISPATCH; return; }
    if (rt_verbose > 1) rt_log("hostfn %s", hostfn_names[slot]);
    uint32_t outer = rt_current_hostfn;
    rt_current_hostfn = RT_HOSTFN_BASE + 4 * slot;
    hostfns[slot](c, m);
    rt_current_hostfn = outer;
    c->r[15] = c->r[14];
}

/* ---- imports ------------------------------------------------------------------------------------- */
static rt_import_fn imports[1024];
static uint8_t warned[1024];

void rt_import_register(const char *name, rt_import_fn fn) {
    for (uint32_t k = 0; k < rt_import_count; k++)
        if (!strcmp(rt_import_names[k], name)) { imports[k] = fn; return; }
}

void rt_imports_init(void) {
    rt_libc_register();
    rt_pthread_register();
    rt_gl_register();
    rt_sles_register();
    uint32_t missing = 0;
    for (uint32_t k = 0; k < rt_import_count; k++) missing += !imports[k];
    rt_log("imports: %u of %u implemented", rt_import_count - missing, rt_import_count);
}

void rc_import(cpu_t *c, uint8_t *m, uint32_t slot) {
    (void)m;
    if (slot < rt_import_count && imports[slot]) {
        if (rt_verbose > 2) rt_log("import %s", rt_import_names[slot]);
        imports[slot](c);
    } else {
        if (slot < 1024 && !warned[slot]) {
            warned[slot] = 1;
            rt_log("unimplemented import %s (returns 0)", slot < rt_import_count ? rt_import_names[slot] : "?");
        }
        c->r[0] = 0;
    }
    c->r[15] = c->r[14];
}

uint32_t rt_data_symbol(const char *name) {
    for (uint32_t k = 0; k < rt_data_symbol_count; k++)
        if (!strcmp(rt_data_symbols[k].name, name)) return rt_data_symbols[k].addr;
    rt_fatal("no data symbol %s", name);
    return 0;
}

uint32_t rt_export(const char *name) {
    for (uint32_t k = 0; k < rt_export_count; k++)
        if (!strcmp(rt_exports[k].name, name)) return rt_exports[k].addr;
    rt_fatal("no export %s", name);
    return 0;
}

/* ---- threads: one CPU state per host thread ------------------------------------------------------ */
static __thread cpu_t *tl_cpu;

cpu_t *rt_thread_cpu(void) {
    if (!tl_cpu) {
        tl_cpu = calloc(1, sizeof(cpu_t));
        uint32_t stack = rt_malloc(1u << 20);
        tl_cpu->r[13] = stack + (1u << 20) - 16;
        tl_cpu->memsize = 0xFFFFFFFFu;
        tl_cpu->budget = 0xFFFFFFFFu;
        tl_cpu->tls = rt_calloc(1, 4096);
    }
    return tl_cpu;
}

/* ---- function names and the game call chain ------------------------------------------------------ */
const char *rt_func_name(uint32_t addr, uint32_t *offset) {
    uint32_t lo = 0, hi = rt_func_count;
    while (lo < hi) { /* last function starting at or below addr */
        uint32_t mid = (lo + hi) / 2;
        if (rt_func_addrs[mid] <= addr) lo = mid + 1; else hi = mid;
    }
    if (!lo) { if (offset) *offset = addr; return "?"; }
    if (offset) *offset = addr - rt_func_addrs[lo - 1];
    return rt_func_names[lo - 1];
}

void rt_print_trace(cpu_t *c) {
    if (!c) return;
    uint32_t d = c->depth;
    rt_log("game call chain (innermost first), depth %u:", d);
    for (uint32_t k = 0; k < 24 && d - k >= 1; k++) {
        uint32_t fn = c->trace[(d - k) & 63];
        rt_log("  %#010x %s", fn, rt_func_name(fn, NULL));
    }
}
