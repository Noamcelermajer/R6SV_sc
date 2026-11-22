/* Host unit tests for the libc stream: sockets, fd_set translation, wide formatting, division helpers, setjmp
 * contract, paths, data imports, time, and "every non-GL import of librainbowsix.so is registered".
 *
 *   py -m ziglang cc -O1 -target x86_64-windows-gnu -Iruntime -Itools/recomp/runtime runtime/test/libc_test.c \
 *       -o build/libc/libc_test.exe && build/libc/libc_test.exe
 *
 * It includes the runtime sources directly (one translation unit) so it can call the static import handlers. */
#define WIN32_LEAN_AND_MEAN
#define FD_SETSIZE 1024
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "../core.c"
#include "../mem.c"
#include "../printf.c"
#include "../libc.c"
#include "../pthread.c"

/* ---- stubs for what the generated image_meta.c and the translated code provide ------------------------------ */
const uint32_t rt_image_base = 0x01000000u, rt_image_size = 0x10000u, rt_import_stub_base = 0x01100000u;
const uint32_t rt_init_array[1] = {0}, rt_init_array_count = 0;
const char *const rt_import_names[] = {
#include "r6_imports.inc"
    "~extras~", "poll", "inet_aton", "inet_pton", "inet_ntop", "wcsncmp", "wcsstr", "htons", "htonl", "wctype", "iswctype",
    "mbrtowc", "wcrtomb", "stat", "opendir", "readdir", "closedir", "unlink", "clock", "div",
};
const uint32_t rt_import_count = sizeof rt_import_names / sizeof *rt_import_names;
const rt_named_addr rt_data_import_slots[] = {
    {"_ctype_", 0x01000100u}, {"__sF", 0x01000104u}, {"_toupper_tab_", 0x01000108u}, {"_tolower_tab_", 0x0100010Cu},
    {"__dso_handle", 0x01000110u}, {"_ZSt7nothrow", 0x01000114u}, {"__stack_chk_guard", 0x01000118u},
    {"mystery_data", 0x0100011Cu}};
const uint32_t rt_data_import_slot_count = 8;
const rt_named_addr rt_exports[1] = {{"x", 0}}, rt_data_symbols[1] = {{"x", 0}};
const uint32_t rt_export_count = 0, rt_data_symbol_count = 0;
const uint32_t rt_func_addrs[1] = {0}, rt_func_count = 0;
const char *const rt_func_names[1] = {"?"};
void rt_gl_register(void) {}
void rt_sles_register(void) {}
#define CMP_FN 0x01000800u
void rc_dispatch(cpu_t *c, uint8_t *m, uint32_t target) { /* the "translated" qsort comparator: ints, ascending */
    (void)m;
    if (target == CMP_FN) {
        int32_t a = (int32_t)rt_ld32(c->r[0]), b = (int32_t)rt_ld32(c->r[1]);
        c->r[0] = (uint32_t)(a < b ? -1 : a > b ? 1 : 0);
    }
    c->r[15] = c->r[14];
}

/* ---- harness ------------------------------------------------------------------------------------------------ */
static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static cpu_t *C;
static uint32_t gstr(const char *s) { return rt_strdup_guest(s); }
static uint32_t gwide(const uint32_t *w) { /* zero-terminated UTF-32 to game memory */
    uint32_t n = 0;
    while (w[n]) n++;
    uint32_t p = rt_malloc(4 * (n + 1));
    memcpy(G(p), w, 4 * (n + 1));
    return p;
}
static uint32_t slot_of(const char *name) {
    for (uint32_t k = 0; k < rt_import_count; k++) if (!strcmp(rt_import_names[k], name)) return k;
    return 0xFFFFFFFFu;
}
/* call an import by name through rc_import, like translated code: up to 4 register args, the rest on the stack */
static uint32_t callv(const char *name, int n, const uint32_t *args) {
    uint32_t slot = slot_of(name);
    if (slot == 0xFFFFFFFFu) { printf("no import slot for %s\n", name); fails++; return 0; }
    uint32_t saved_sp = C->r[13];
    uint32_t sp = (saved_sp - 64) & ~7u;
    for (int k = 0; k < n; k++) { if (k < 4) C->r[k] = args[k]; else rt_st32(sp + 4 * (k - 4), args[k]); }
    C->r[13] = sp;
    C->r[14] = RT_RETURN_SENTINEL;
    rc_import(C, rt_mem, slot);
    C->r[13] = saved_sp;
    return C->r[0];
}
#define CALL(name, ...) ({ uint32_t a_[] = {__VA_ARGS__}; callv(name, (int)(sizeof a_ / sizeof *a_), a_); })
static uint32_t guest_errno(void) { return rt_ld32(errno_addr(C)); }
static int32_t S32(uint32_t v) { return (int32_t)v; }
static uint64_t R64(void) { return ((uint64_t)C->r[1] << 32) | C->r[0]; }
static void dargs(double a, double b, uint32_t out[4]) { uint64_t x, y; memcpy(&x, &a, 8); memcpy(&y, &b, 8); out[0] = (uint32_t)x; out[1] = (uint32_t)(x >> 32); out[2] = (uint32_t)y; out[3] = (uint32_t)(y >> 32); }

/* ---- tests ---------------------------------------------------------------------------------------------------- */
static void test_registration(void) {
    static const char *const data_names[] = {"_toupper_tab_", "__dso_handle", "__sF", "_ctype_", "_tolower_tab_", "__stack_chk_guard", "_ZSt7nothrow"};
    int missing = 0;
    for (uint32_t k = 0; k < rt_import_count && strcmp(rt_import_names[k], "~extras~"); k++) {
        int is_data = 0;
        for (size_t d = 0; d < sizeof data_names / sizeof *data_names; d++) is_data |= !strcmp(data_names[d], rt_import_names[k]);
        if (!is_data && !imports[k]) { printf("  not registered: %s\n", rt_import_names[k]); missing++; }
    }
    for (uint32_t k = 0; k < rt_import_count; k++) if (!imports[k]) printf("  (no handler: %s)\n", rt_import_names[k]);
    CHECK(missing == 0, "%d imports of the game are not registered", missing);
    printf("registration: %u non-GL imports, %d missing\n", rt_import_count, missing);
}

static void test_data_imports(void) {
    rt_data_imports_init();
    uint32_t ct = rt_ld32(rt_ld32(0x01000100u));
    CHECK(rt_mem[ct + 1 + 'Q'] == 0x01 && rt_mem[ct + 1 + 'q'] == 0x02 && rt_mem[ct + 1 + '7'] == 0x04, "_ctype_ letters/digits");
    CHECK(rt_mem[ct + 1 + ' '] == (0x08 | 0x80) && rt_mem[ct + 1 + 'f'] == (0x02 | 0x40) && rt_mem[ct + 1 + '!'] == 0x10, "_ctype_ space/xdigit/punct");
    CHECK(rt_mem[ct] == 0 && rt_mem[ct + 1 + 200] == 0, "_ctype_ EOF and high bytes");
    uint32_t up = rt_ld32(rt_ld32(0x01000108u)), lo = rt_ld32(rt_ld32(0x0100010Cu));
    CHECK(*(int16_t *)G(up + 2 * (1 + 'q')) == 'Q' && *(int16_t *)G(up + 2 * (1 + 'Q')) == 'Q' && *(int16_t *)G(up) == -1, "_toupper_tab_");
    CHECK(*(int16_t *)G(lo + 2 * (1 + 'Q')) == 'q' && *(int16_t *)G(lo + 2 * (1 + '9')) == '9', "_tolower_tab_");
    uint32_t sf = rt_ld32(0x01000104u);
    CHECK(*(int16_t *)G(sf + 14) == 0 && *(int16_t *)G(sf + 84 + 14) == 1 && *(int16_t *)G(sf + 168 + 14) == 2, "__sF _file fields (fileno macro)");
    CHECK(rt_ld32(rt_ld32(0x01000118u)) != 0, "__stack_chk_guard");
    CHECK(rt_ld32(0x01000110u) != 0 && rt_ld32(0x01000114u) != 0 && rt_ld32(rt_ld32(0x01000110u)) == 0, "__dso_handle, nothrow: addresses of zero objects");
    CHECK(rt_ld32(0x0100011Cu) != 0, "unknown data import gets a zeroed block");
}

static void test_arith(void) {
    uint32_t a[4];
    CHECK(S32(CALL("__aeabi_idiv", (uint32_t)-7, 2)) == -3, "idiv -7/2");
    CHECK(CALL("__aeabi_idiv", 0x80000000u, (uint32_t)-1) == 0x80000000u, "idiv INT_MIN/-1");
    CHECK(CALL("__aeabi_idiv", 5, 0) == 0, "idiv by zero");
    CHECK(CALL("__aeabi_uidiv", 0xFFFFFFFFu, 16) == 0x0FFFFFFFu, "uidiv");
    CALL("__aeabi_idivmod", (uint32_t)-7, 2);
    CHECK(S32(C->r[0]) == -3 && S32(C->r[1]) == -1, "idivmod -7/2 = -3 r -1, got %d r %d", S32(C->r[0]), S32(C->r[1]));
    CALL("__aeabi_uidivmod", 100, 7);
    CHECK(C->r[0] == 14 && C->r[1] == 2, "uidivmod");
    int64_t n = -10000000000ll;
    CALL("__aeabi_ldivmod", (uint32_t)n, (uint32_t)((uint64_t)n >> 32), 3, 0);
    CHECK((int64_t)R64() == -3333333333ll && (int64_t)(((uint64_t)C->r[3] << 32) | C->r[2]) == -1, "ldivmod");
    CALL("__aeabi_uldivmod", 0xFFFFFFFFu, 0xFFFFFFFFu, 10, 0);
    CHECK(R64() == 1844674407370955161ull && C->r[2] == 5 && C->r[3] == 0, "uldivmod");
    CALL("__aeabi_ldivmod", 0, 0x80000000u, (uint32_t)-1, (uint32_t)-1);
    CHECK(R64() == 0x8000000000000000ull, "ldivmod INT64_MIN/-1");
    uint32_t d[4];
    dargs(1.5, 2.25, d); CALL("__aeabi_dadd", d[0], d[1], d[2], d[3]);
    CHECK(R64() == 0x400E000000000000ull, "dadd 3.75 got %llx", (unsigned long long)R64());
    dargs(1e300, 1e10, d); CALL("__aeabi_dmul", d[0], d[1], d[2], d[3]);
    CHECK(R64() == 0x7FF0000000000000ull, "dmul overflow to +inf");
    dargs(0.0, 0.0, d); CALL("__aeabi_ddiv", d[0], d[1], d[2], d[3]);
    CHECK(R64() == 0x7FF8000000000000ull, "0/0 is the ARM default NaN, got %llx", (unsigned long long)R64());
    dargs(7.0, 2.0, d); CALL("__aeabi_ddiv", d[0], d[1], d[2], d[3]);
    CHECK(R64() == 0x400C000000000000ull, "ddiv 3.5");
    dargs(-1.9, 0, a); CHECK(S32(CALL("__aeabi_d2iz", a[0], a[1])) == -1, "d2iz truncates");
    dargs(3e10, 0, a); CHECK(CALL("__aeabi_d2iz", a[0], a[1]) == 0x7FFFFFFFu, "d2iz saturates high");
    dargs(-3e10, 0, a); CHECK(CALL("__aeabi_d2iz", a[0], a[1]) == 0x80000000u, "d2iz saturates low");
    dargs(NAN, 0, a); CHECK(CALL("__aeabi_d2iz", a[0], a[1]) == 0, "d2iz NaN is 0");
    dargs(-1e19, 0, a); CALL("__aeabi_d2lz", a[0], a[1]); CHECK(R64() == 0x8000000000000000ull, "d2lz saturates");
    dargs(-123456789012.9, 0, a); CALL("__aeabi_d2lz", a[0], a[1]); CHECK((int64_t)R64() == -123456789012ll, "d2lz");
    CALL("__aeabi_ul2d", 0xFFFFFFFFu, 0xFFFFFFFFu); CHECK(R64() == 0x43F0000000000000ull, "ul2d 2^64 got %llx", (unsigned long long)R64());
    CALL("__aeabi_i2d", (uint32_t)-5); CHECK(R64() == 0xC014000000000000ull, "i2d -5");
    /* div: the result goes through a hidden pointer */
    uint32_t res = rt_malloc(16);
    CALL("div", res, (uint32_t)-7, 2);
    CHECK(C->r[0] == res && S32(rt_ld32(res)) == -3 && S32(rt_ld32(res + 4)) == -1, "div sret");
    /* floats in r0/r1 */
    float f = 2.0f; uint32_t fb; memcpy(&fb, &f, 4);
    CALL("sqrtf", fb); memcpy(&f, &C->r[0], 4); CHECK(fabsf(f - 1.41421356f) < 1e-6f, "sqrtf");
    float g = 3.0f, h; uint32_t gb; memcpy(&gb, &g, 4);
    CALL("powf", fb, gb); memcpy(&h, &C->r[0], 4); CHECK(h == 8.0f, "powf 2^3");
    float neg = -1.0f; uint32_t nb; memcpy(&nb, &neg, 4);
    CALL("sqrtf", nb); CHECK(C->r[0] == 0x7FC00000u, "sqrtf(-1) is the positive default NaN, got %x", C->r[0]);
}

static void test_fdset(void) {
    uint8_t set[128] = {0};
    int bits[] = {0, 1, 31, 32, 63, 64, 513, 1023};
    for (size_t k = 0; k < sizeof bits / sizeof *bits; k++) gfdset_set(set, bits[k]);
    for (int fd = 0; fd < 1024; fd++) {
        int expect = 0;
        for (size_t k = 0; k < sizeof bits / sizeof *bits; k++) expect |= bits[k] == fd;
        if (gfdset_test(set, fd) != expect) { CHECK(0, "fd_set bit %d", fd); break; }
    }
    uint32_t w0, w1; memcpy(&w0, set, 4); memcpy(&w1, set + 4, 4);
    CHECK(w0 == 0x80000003u && w1 == 0x80000001u, "fd_set word layout: %x %x", w0, w1);
    /* regular-file descriptors are always ready; select with no sockets returns them */
    uint32_t rs = rt_calloc(1, 128);
    rt_mem[rs] = 1 << 3;
    CHECK(S32(CALL("select", 5, rs, 0, 0, 0)) == 1 && (rt_mem[rs] & (1 << 3)), "select over a file descriptor");
    /* a zero timeout on an empty select returns at once */
    uint32_t tv = rt_calloc(1, 8);
    CHECK(S32(CALL("select", 0, 0, 0, 0, tv)) == 0, "select(0, timeout 0)");
}

static uint32_t sockaddr_in(uint32_t ip_be_bytes, uint16_t port_host) {
    uint32_t sa = rt_calloc(1, 16);
    rt_mem[sa] = 2;
    rt_mem[sa + 2] = (uint8_t)(port_host >> 8);
    rt_mem[sa + 3] = (uint8_t)port_host;
    rt_mem[sa + 4] = (uint8_t)(ip_be_bytes >> 24); rt_mem[sa + 5] = (uint8_t)(ip_be_bytes >> 16);
    rt_mem[sa + 6] = (uint8_t)(ip_be_bytes >> 8); rt_mem[sa + 7] = (uint8_t)ip_be_bytes;
    return sa;
}

static void test_sockets(void) {
    uint32_t one = rt_calloc(1, 4); rt_st32(one, 1);
    int32_t ls = S32(CALL("socket", 2, 1, 0));
    CHECK(ls >= 512 && ls < 1024, "socket fd %d is in the socket range", ls);
    CHECK(CALL("setsockopt", (uint32_t)ls, 1, 2, one, 4) == 0, "setsockopt SO_REUSEADDR");
    uint32_t sa = sockaddr_in(0x7F000001u, 0);
    CHECK(CALL("bind", (uint32_t)ls, sa, 16) == 0, "bind 127.0.0.1:0 (errno %u)", guest_errno());
    uint32_t out = rt_calloc(1, 16), len = rt_calloc(1, 4); rt_st32(len, 16);
    CHECK(CALL("getsockname", (uint32_t)ls, out, len) == 0 && rt_ld32(len) == 16 && rt_mem[out] == 2, "getsockname");
    uint16_t port = (uint16_t)((rt_mem[out + 2] << 8) | rt_mem[out + 3]);
    CHECK(port != 0 && rt_mem[out + 4] == 127, "bound port %u", port);
    CHECK(CALL("listen", (uint32_t)ls, 4) == 0, "listen");

    /* non-blocking connect to the listener: EINPROGRESS or success, then writable, SO_ERROR 0 */
    int32_t cs = S32(CALL("socket", 2, 1 | 0x800, 0));
    uint32_t dst = sockaddr_in(0x7F000001u, port);
    int32_t r = S32(CALL("connect", (uint32_t)cs, dst, 16));
    CHECK(r == 0 || (r == -1 && guest_errno() == 115), "non-blocking connect: %d errno %u", r, guest_errno());
    uint32_t ws = rt_calloc(1, 128), tv = rt_calloc(1, 8);
    rt_st32(tv, 5);
    gfdset_set(rt_mem + ws, cs);
    CHECK(S32(CALL("select", (uint32_t)cs + 1, 0, ws, 0, tv)) == 1 && gfdset_test(rt_mem + ws, cs), "select reports the connecting socket writable");
    uint32_t err = rt_calloc(1, 4), elen = rt_calloc(1, 4); rt_st32(elen, 4); rt_st32(err, 99);
    CHECK(CALL("getsockopt", (uint32_t)cs, 1, 4, err, elen) == 0 && rt_ld32(err) == 0, "SO_ERROR after connect: %u", rt_ld32(err));
    int32_t as = S32(CALL("accept", (uint32_t)ls, 0, 0));
    CHECK(as >= 512 && as != cs, "accept -> %d", as);

    uint32_t hello = gstr("hello"), buf = rt_calloc(1, 64);
    CHECK(S32(CALL("send", (uint32_t)cs, hello, 5, 0)) == 5, "send 5 bytes");
    memset(G(ws), 0, 128);
    gfdset_set(rt_mem + ws, as);
    CHECK(S32(CALL("select", (uint32_t)as + 1, ws, 0, 0, tv)) == 1 && gfdset_test(rt_mem + ws, as), "select reports data readable");
    CHECK(S32(CALL("recv", (uint32_t)as, buf, 64, 0)) == 5 && !memcmp(G(buf), "hello", 5), "recv hello");
    CHECK(S32(CALL("recv", (uint32_t)cs, buf, 64, 0)) == -1 && guest_errno() == 11, "recv on a non-blocking socket with no data: EAGAIN (errno %u)", guest_errno());
    CHECK(S32(CALL("write", (uint32_t)as, gstr("back"), 4)) == 4, "write() on a socket descriptor");
    CHECK(S32(CALL("ioctl", (uint32_t)cs, 0x541B, one)) == 0, "FIONREAD");
    /* SO_RCVTIMEO round trip and a blocking recv that times out */
    uint32_t t2 = rt_calloc(1, 8); rt_st32(t2, 0); rt_st32(t2 + 4, 50000);
    CHECK(CALL("setsockopt", (uint32_t)as, 1, 20, t2, 8) == 0, "SO_RCVTIMEO");
    CALL("read", (uint32_t)cs, buf, 64); /* drains "back" */
    uint64_t t0 = GetTickCount64();
    CHECK(S32(CALL("recv", (uint32_t)as, buf, 64, 0)) == -1 && guest_errno() == 11 && GetTickCount64() - t0 < 1000, "blocking recv honours SO_RCVTIMEO");
    /* poll */
    uint32_t pfd = rt_calloc(1, 8); rt_st32(pfd, (uint32_t)cs); *(uint16_t *)G(pfd + 4) = 1 | 4;
    CHECK(S32(CALL("poll", pfd, 1, 1000)) == 1 && (*(uint16_t *)G(pfd + 6) & 4), "poll: writable");
    /* fcntl */
    CHECK(S32(CALL("fcntl", (uint32_t)cs, 3, 0)) == (2 | 04000), "F_GETFL shows O_NONBLOCK");
    CALL("fcntl", (uint32_t)cs, 4, 2);
    CHECK(S32(CALL("fcntl", (uint32_t)cs, 3, 0)) == 2, "F_SETFL clears O_NONBLOCK");
    CHECK(CALL("close", (uint32_t)cs) == 0 && CALL("close", (uint32_t)as) == 0 && CALL("close", (uint32_t)ls) == 0, "close");
    CHECK(S32(CALL("recv", (uint32_t)cs, buf, 4, 0)) == -1 && guest_errno() == 88, "recv on a closed fd: ENOTSOCK");

    /* a server that is gone: connect fails at once, blocking or not */
    int32_t bs = S32(CALL("socket", 2, 1, 0));
    uint32_t faraway = sockaddr_in(0x08080808u, 53);
    uint64_t t1 = GetTickCount64();
    CHECK(S32(CALL("connect", (uint32_t)bs, faraway, 16)) == -1 && guest_errno() == 101, "blocking connect to 8.8.8.8: ENETUNREACH (errno %u)", guest_errno());
    CHECK(GetTickCount64() - t1 < 500, "connect failed quickly");
    CALL("close", (uint32_t)bs);
    int32_t us = S32(CALL("socket", 2, 2, 0));
    CHECK(S32(CALL("sendto", (uint32_t)us, hello, 5, 0, faraway, 16)) == -1 && guest_errno() == 101, "sendto 8.8.8.8: ENETUNREACH");
    CALL("close", (uint32_t)us);
    CHECK(S32(CALL("socket", 10, 1, 0)) == -1 && guest_errno() == 97, "AF_INET6: EAFNOSUPPORT");
    /* a refused local connect (nothing listens on the port that was just closed) reports failure via SO_ERROR */
    int32_t rs = S32(CALL("socket", 2, 1 | 0x800, 0));
    r = S32(CALL("connect", (uint32_t)rs, dst, 16));
    if (r == -1 && guest_errno() == 115) {
        memset(G(ws), 0, 128); gfdset_set(rt_mem + ws, rs);
        uint32_t ws2 = rt_calloc(1, 128); gfdset_set(rt_mem + ws2, rs);
        CALL("select", (uint32_t)rs + 1, ws2, ws, 0, tv);
        rt_st32(elen, 4);
        CALL("getsockopt", (uint32_t)rs, 1, 4, err, elen);
        CHECK(rt_ld32(err) == 111, "refused connect: SO_ERROR is ECONNREFUSED (111), got %u", rt_ld32(err));
    } else CHECK(r == -1 && guest_errno() == 111, "refused connect: %d errno %u", r, guest_errno());
    CALL("close", (uint32_t)rs);
}

static void test_resolver(void) {
    uint32_t res = rt_calloc(1, 4);
    uint32_t hints = rt_calloc(1, 32);
    CHECK(CALL("getaddrinfo", gstr("127.0.0.1"), gstr("8080"), 0, res) == 0, "getaddrinfo numeric");
    uint32_t ai = rt_ld32(res);
    CHECK(ai && rt_ld32(ai + 4) == 2 && rt_ld32(ai + 8) == 1 && rt_ld32(ai + 12) == 6 && rt_ld32(ai + 16) == 16, "addrinfo fields (family, socktype, protocol, addrlen)");
    uint32_t sa = rt_ld32(ai + 24);
    CHECK(sa == ai + 32 && rt_mem[sa] == 2 && rt_mem[sa + 2] == 0x1F && rt_mem[sa + 3] == 0x90 && rt_mem[sa + 4] == 127 && rt_mem[sa + 7] == 1, "sockaddr in addrinfo at +24 (bionic order)");
    uint32_t next = rt_ld32(ai + 28);
    CHECK(next && rt_ld32(next + 8) == 2 && rt_ld32(next + 12) == 17, "second entry is the datagram one");
    CALL("freeaddrinfo", ai);
    rt_st32(hints + 8, 2); /* SOCK_DGRAM */
    CHECK(CALL("getaddrinfo", gstr("localhost"), 0, hints, res) == 0 && rt_ld32(rt_ld32(res) + 28) == 0 && rt_ld32(rt_ld32(res) + 8) == 2, "hints socktype gives one entry");
    CALL("freeaddrinfo", rt_ld32(res));
    uint64_t t0 = GetTickCount64();
    CHECK(CALL("getaddrinfo", gstr("master.gameloft.example"), gstr("80"), 0, res) == 8, "unknown host: EAI_NONAME");
    CHECK(GetTickCount64() - t0 < 500, "name resolution failed quickly");
    CHECK(CALL("getaddrinfo", gstr("10.0.0.1"), gstr("bogus-service"), 0, res) == 9, "unknown service: EAI_SERVICE");
    uint32_t he = CALL("gethostbyname", gstr("localhost"));
    CHECK(he && rt_ld32(he + 8) == 2 && rt_ld32(he + 12) == 4, "gethostbyname hostent");
    uint32_t list = rt_ld32(he + 16);
    CHECK(rt_mem[rt_ld32(list)] == 127 && rt_ld32(list + 4) == 0, "h_addr_list");
    CHECK(CALL("gethostbyname", gstr("nothing.invalid")) == 0, "gethostbyname failure is NULL");
    CHECK(CALL("inet_addr", gstr("192.168.1.20")) == 0x1401A8C0u, "inet_addr is network order");
    CHECK(CALL("inet_addr", gstr("300.1.1.1")) == 0xFFFFFFFFu && CALL("inet_addr", gstr("abc")) == 0xFFFFFFFFu, "inet_addr rejects");
    CHECK(CALL("inet_addr", gstr("0x7f.1")) == 0x0100007Fu, "inet_addr a.b form");
    uint32_t s = CALL("inet_ntoa", 0x1401A8C0u);
    CHECK(!strcmp((char *)G(s), "192.168.1.20"), "inet_ntoa");
    CHECK(CALL("htons", 0x1F90) == 0x901F && CALL("htonl", 0x01020304) == 0x04030201, "byte order");
}

static void test_wchar(void) {
    static const uint32_t fmt[] = {'%', 'l', 's', '=', '%', 'd', '|', '%', '5', '.', '2', 'f', '|', '%', 's', '|', '%', 'c', '|', '%', '-', '4', 'd', '|', 0};
    static const uint32_t ne[] = {'n', 0xE9, 0};
    uint32_t buf = rt_calloc(1, 4 * 64), f = gwide(fmt), w = gwide(ne), narrow = gstr("ab");
    double d = 3.14159; uint64_t du; memcpy(&du, &d, 8);
    uint32_t stack[8] = {42, 0, (uint32_t)du, (uint32_t)(du >> 32), narrow, 'z', 7, 0};
    uint32_t sp = (C->r[13] - 64) & ~7u;
    memcpy(G(sp), stack, sizeof stack);
    uint32_t saved = C->r[13];
    C->r[13] = sp; C->r[0] = buf; C->r[1] = 64; C->r[2] = f; C->r[3] = w; C->r[14] = RT_RETURN_SENTINEL;
    rc_import(C, rt_mem, slot_of("swprintf"));
    C->r[13] = saved;
    CHECK(S32(C->r[0]) == 22, "swprintf returned %d, want 22", S32(C->r[0]));
    static const uint32_t want[] = {'n', 0xE9, '=', '4', '2', '|', ' ', '3', '.', '1', '4', '|', 'a', 'b', '|', 'z', '|', '7', ' ', ' ', ' ', '|', 0};
    CHECK(!memcmp(G(buf), want, sizeof want), "swprintf output differs");
    /* truncation: -1, and the buffer holds n-1 characters and a terminator */
    static const uint32_t abc[] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 0};
    CHECK(S32(CALL("swprintf", buf, 5, gwide(abc))) == -1, "swprintf truncation returns -1");
    CHECK(rt_ld32(buf) == 'a' && rt_ld32(buf + 12) == 'd' && rt_ld32(buf + 16) == 0, "truncated output is terminated");
    CHECK(S32(CALL("swprintf", buf, 9, gwide(abc))) == 8, "exact fit returns 8 only when n is 9");
    /* classification and case */
    CHECK(CALL("towupper", 0xE9) == 0xC9 && CALL("towlower", 0x416) == 0x436 && CALL("towupper", 'q') == 'Q' && CALL("towlower", 0xC0) == 0xE0, "towupper/towlower");
    CHECK(CALL("towupper", 0xF7) == 0xF7 && CALL("towlower", 0xD7) == 0xD7 && CALL("towupper", 0x3B1) == 0x391, "case maps skip x and divide, Greek");
    CHECK(CALL("iswalpha", 0x4E2D) && CALL("iswalpha", 0xE9) && !CALL("iswalpha", '1') && !CALL("iswalpha", 0xD7), "iswalpha");
    CHECK(CALL("iswdigit", '7') && !CALL("iswdigit", 'a') && CALL("iswxdigit", 'F') && !CALL("iswxdigit", 'G'), "iswdigit/iswxdigit");
    CHECK(CALL("iswspace", 0x3000) && CALL("iswspace", ' ') && !CALL("iswspace", 'x') && CALL("iswpunct", 0x3002) && CALL("iswpunct", '!') && !CALL("iswpunct", 'a'), "iswspace/iswpunct");
    CHECK(CALL("iswupper", 0xC9) && !CALL("iswupper", 0xE9) && CALL("iswlower", 0xE9) && CALL("iswcntrl", 7) && CALL("iswprint", 'a') && !CALL("iswprint", 7), "iswupper/lower/cntrl/print");
    CHECK(CALL("iswctype", 'a', CALL("wctype", gstr("alpha"))) == 1 && CALL("iswctype", 'a', CALL("wctype", gstr("digit"))) == 0, "wctype");
    /* strings */
    static const uint32_t ab[] = {'a', 'b', 0};
    uint32_t dst = rt_calloc(1, 32);
    memset(G(dst), 0xFF, 32);
    CALL("wcsncpy", dst, gwide(ab), 5);
    CHECK(rt_ld32(dst) == 'a' && rt_ld32(dst + 4) == 'b' && rt_ld32(dst + 8) == 0 && rt_ld32(dst + 12) == 0 && rt_ld32(dst + 16) == 0 && rt_ld32(dst + 20) == 0xFFFFFFFFu, "wcsncpy pads with zeros");
    CHECK(CALL("wcslen", gwide(abc)) == 8 && S32(CALL("wcscmp", gwide(ab), gwide(abc))) == -1 && CALL("wcscmp", gwide(ab), gwide(ab)) == 0, "wcslen/wcscmp");
    CALL("wmemset", dst, 0x1F600, 3);
    CHECK(rt_ld32(dst + 8) == 0x1F600 && S32(CALL("wmemcmp", dst, dst + 4, 2)) == 0, "wmemset/wmemcmp with code points above 16 bits");
    uint32_t mb = rt_calloc(1, 8);
    rt_mem[mb] = 0xC3; rt_mem[mb + 1] = 0xA9;
    CHECK(CALL("mbrtowc", dst, mb, 2, 0) == 2 && rt_ld32(dst) == 0xE9, "mbrtowc decodes UTF-8");
    CHECK(CALL("wcrtomb", mb, 0x20AC, 0) == 3 && rt_mem[mb] == 0xE2 && rt_mem[mb + 1] == 0x82 && rt_mem[mb + 2] == 0xAC, "wcrtomb encodes UTF-8");
}

static int jmp_stage;
static void do_longjmp(cpu_t *c, uint32_t buf, uint32_t val) { c->r[0] = buf; c->r[1] = val; imp_longjmp(c); }
static void test_setjmp(void) {
    cpu_t *c = C;
    uint32_t buf = rt_malloc(256);
    for (int k = 4; k <= 11; k++) c->r[k] = 0x40 + (uint32_t)k;
    c->r[13] = 0x01234560u; c->r[14] = 0x01000123u; c->depth = 5;
    for (int k = 8; k < 16; k++) c->vfp.d[k] = 0x1111111100000000ull + (uint64_t)k;
    jmp_stage = 0;
    /* what the lifter's setjmp macro does: save the registers, run the host setjmp in this frame, reload on return 2 */
    if (setjmp(*(jmp_buf *)rt_setjmp_prepare(c, buf)) == 0) {
        jmp_stage = 1;
        for (int k = 4; k <= 11; k++) c->r[k] = 0xDEAD;
        c->r[13] = 0x999; c->r[14] = 0x888; c->depth = 9;
        for (int k = 8; k < 16; k++) c->vfp.d[k] = 0;
        do_longjmp(c, buf, 0);
        jmp_stage = 99;
    } else {
        CHECK(jmp_stage == 1, "longjmp returned to the setjmp frame (stage %d)", jmp_stage);
        CHECK(c->r[0] == 1, "longjmp(buf, 0) makes setjmp return 1, got %u", c->r[0]);
        int ok = 1;
        for (int k = 4; k <= 11; k++) ok &= c->r[k] == 0x40 + (uint32_t)k;
        CHECK(ok, "r4-r11 restored");
        CHECK(c->r[13] == 0x01234560u && c->r[14] == 0x01000123u && c->r[15] == 0x01000123u && c->depth == 5, "sp, lr, call depth restored");
        ok = 1;
        for (int k = 8; k < 16; k++) ok &= c->vfp.d[k] == 0x1111111100000000ull + (uint64_t)k;
        CHECK(ok, "d8-d15 restored");
    }
    /* a second jump with a value */
    if (setjmp(*(jmp_buf *)rt_setjmp_prepare(c, buf)) == 0) do_longjmp(c, buf, 7);
    else CHECK(c->r[0] == 7, "longjmp(buf, 7)");
}

static void test_paths_and_files(void) {
    char base[512];
    snprintf(base, sizeof base, "%s/r6sv_libc_test", getenv("TEMP") ? getenv("TEMP") : ".");
    char p1[600], p2[600];
    host_mkdir(base, 0755);
    snprintf(p1, sizeof p1, "%s/Textures", base); host_mkdir(p1, 0755);
    snprintf(p2, sizeof p2, "%s/Textures/Foo.PNG", base);
    FILE *f = fopen(p2, "wb"); fputs("abcdef", f); fclose(f);
    rt_libc_set_data_dir(base);
    rt_libc_add_data_prefix("/sdcard/gameloft/games/GloftR6HP");
    char out[1024];
    CHECK(rt_map_path("/sdcard/gameloft/games/GloftR6HP/Textures/Foo.PNG", out, sizeof out) == 1, "mapped path exists: %s", out);
    CHECK(rt_map_path("/SDCARD/Gameloft/games/gloftr6hp/textures/foo.png", out, sizeof out) == 1, "prefix and file name match ignoring case: %s", out);
    CHECK(strstr(out, "r6sv_libc_test") != NULL, "mapped into the data folder");
    CHECK(rt_map_path("/sdcard/gameloft/games/GloftR6HP/textures/missing.png", out, sizeof out) == 0, "missing stays missing");
    CHECK(rt_map_path("/sdcard/gameloft/games/GloftR6HP2/x", out, sizeof out) == 0 && !strstr(out, "r6sv_libc_test"), "a longer folder name is not the prefix");
    /* through the imports */
    uint32_t fp = CALL("fopen", gstr("/sdcard/gameloft/games/GloftR6HP/TEXTURES/foo.png"), gstr("r"));
    CHECK(fp != 0, "fopen through the mapped, case-insensitive path");
    if (fp) {
        uint32_t b = rt_calloc(1, 16);
        CHECK(CALL("fread", b, 1, 6, fp) == 6 && !memcmp(G(b), "abcdef", 6), "fread");
        CHECK(CALL("feof", fp) == 0, "feof before the end");
        CALL("fseek", fp, 0, 2);
        uint32_t pos = rt_calloc(1, 4);
        CHECK(CALL("fgetpos", fp, pos) == 0 && rt_ld32(pos) == 6, "fgetpos");
        CALL("fseek", fp, 2, 0);
        CHECK(CALL("ftell", fp) == 2 && CALL("fsetpos", fp, pos) == 0 && CALL("ftell", fp) == 6, "fsetpos");
        CALL("fread", b, 1, 4, fp);
        CHECK(CALL("feof", fp) != 0, "feof after reading past the end");
        CALL("clearerr", fp);
        CHECK(CALL("feof", fp) == 0, "clearerr");
        CHECK(*(int16_t *)G(fp + 14) >= 3, "FILE._file holds the descriptor");
        CALL("fclose", fp);
    }
    uint32_t st = rt_calloc(1, 104);
    CHECK(CALL("stat", gstr("/sdcard/gameloft/games/GloftR6HP/textures/foo.png"), st) == 0 && rt_ld32(st + 48) == 6, "stat size at +48");
    int32_t fd = S32(CALL("open", gstr("/sdcard/gameloft/games/GloftR6HP/Textures/Foo.PNG"), 0));
    CHECK(fd >= 3 && fd < 512, "open fd %d", fd);
    CALL("close", (uint32_t)fd);
    CHECK(S32(CALL("open", gstr("/sdcard/gameloft/games/GloftR6HP/nothing"), 0)) == -1 && guest_errno() == 2, "open of a missing file: ENOENT");
    /* directories */
    uint32_t dir = CALL("opendir", gstr("/sdcard/gameloft/games/GloftR6HP/textures"));
    int seen = 0;
    CHECK(dir != 0, "opendir through the mapped path");
    for (uint32_t e; dir && (e = CALL("readdir", dir)) != 0;) seen += !strcmp((char *)G(e + 19), "Foo.PNG");
    CHECK(seen == 1, "readdir lists Foo.PNG");
    if (dir) CALL("closedir", dir);
    CHECK(CALL("unlink", gstr("/sdcard/gameloft/games/GloftR6HP/Textures/foo.png")) == 0, "unlink through a case-insensitive path");
    rmdir(p1);
    rmdir(base);
}

static void test_time_and_misc(void) {
    uint32_t tp = rt_calloc(1, 4), tm;
    rt_st32(tp, 951782400); /* 2000-02-29 00:00:00 UTC */
    tm = CALL("gmtime", tp);
    CHECK(S32(rt_ld32(tm + 20)) == 100 && S32(rt_ld32(tm + 16)) == 1 && S32(rt_ld32(tm + 12)) == 29 && S32(rt_ld32(tm + 24)) == 2 && S32(rt_ld32(tm + 28)) == 59, "gmtime 2000-02-29: year %d mon %d mday %d wday %d yday %d",
          S32(rt_ld32(tm + 20)), S32(rt_ld32(tm + 16)), S32(rt_ld32(tm + 12)), S32(rt_ld32(tm + 24)), S32(rt_ld32(tm + 28)));
    CHECK(rt_ld32(tm + 36) == 0 && !strcmp((char *)G(rt_ld32(tm + 40)), "UTC"), "tm_gmtoff and tm_zone of gmtime");
    uint32_t t2 = rt_calloc(1, 44);
    rt_st32(t2 + 12, 32); rt_st32(t2 + 16, 0); rt_st32(t2 + 20, 100); rt_st32(t2 + 32, 0); /* 32 January 2000 */
    uint32_t r = CALL("mktime", t2);
    CHECK(r != 0xFFFFFFFFu && S32(rt_ld32(t2 + 16)) == 1 && S32(rt_ld32(t2 + 12)) == 1, "mktime normalises the guest struct: mon %d mday %d", S32(rt_ld32(t2 + 16)), S32(rt_ld32(t2 + 12)));
    uint32_t lt = CALL("localtime", tp);
    int32_t off = S32(rt_ld32(lt + 36));
    CHECK(abs(off) <= 14 * 3600, "localtime tm_gmtoff %d is plausible", off);
    uint32_t cl = CALL("clock");
    CHECK(cl < 0x7FFFFFFFu, "clock() in microseconds");
    CHECK(CALL("sysconf", 0x27) == 4096 && CALL("sysconf", 0x61) == 4 && S32(CALL("sysconf", 0x7777)) == -1, "sysconf");
    CHECK(CALL("setlocale", 6, gstr("C")) != 0 && CALL("setlocale", 6, gstr("de_DE")) == 0 && CALL("setlocale", 6, 0) != 0, "setlocale");
    CHECK(CALL("calloc", 10, 8) != 0 && rt_ld32(CALL("calloc", 4, 4)) == 0, "calloc zeroes");
    /* sigaction records and hands back the handler */
    uint32_t act = rt_calloc(1, 16), old = rt_calloc(1, 16);
    rt_st32(act, 0x01000400u);
    CHECK(CALL("sigaction", 11, act, 0) == 0 && CALL("sigaction", 11, 0, old) == 0 && rt_ld32(old) == 0x01000400u, "sigaction records the handler");
    /* ARM EABI guard: acquire once, then not */
    uint32_t guard = rt_calloc(1, 8);
    CHECK(CALL("__cxa_guard_acquire", guard) == 1, "guard acquire runs the initialiser");
    CALL("__cxa_guard_release", guard);
    CHECK(CALL("__cxa_guard_acquire", guard) == 0 && rt_mem[guard] == 1, "guard acquire after release: initialised");
    uint32_t nw = CALL("_Znwj", 24);
    CHECK(nw && CALL("_Znaj", 8) && CALL("_ZnajRKSt9nothrow_t", 8, 0), "operator new forms");
    CALL("_ZdlPv", nw); CALL("_ZdaPv", CALL("_Znaj", 8));
    /* qsort: a stable sort through the guest comparator */
    uint32_t arr = rt_malloc(8 * 9);
    int keys[9] = {5, 3, 9, 3, 1, 5, 3, 0, 9};
    for (int k = 0; k < 9; k++) { rt_st32(arr + 8 * k, (uint32_t)keys[k]); rt_st32(arr + 8 * k + 4, (uint32_t)k); }
    CALL("qsort", arr, 9, 8, CMP_FN);
    int ok = 1, stable = 1;
    for (int k = 1; k < 9; k++) {
        ok &= S32(rt_ld32(arr + 8 * (k - 1))) <= S32(rt_ld32(arr + 8 * k));
        if (rt_ld32(arr + 8 * (k - 1)) == rt_ld32(arr + 8 * k)) stable &= rt_ld32(arr + 8 * (k - 1) + 4) < rt_ld32(arr + 8 * k + 4);
    }
    CHECK(ok && stable, "qsort sorts and is stable");
    CHECK(CALL("strcoll", gstr("a"), gstr("b")) != 0 && CALL("strcoll", gstr("a"), gstr("a")) == 0, "strcoll");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    rt_mem_init_from(calloc(1, rt_image_size), rt_image_size);
    rt_imports_init();
    C = rt_thread_cpu();
    test_registration();
    printf("-- data_imports\n");
    test_data_imports();
    printf("-- arith\n");
    test_arith();
    printf("-- fdset\n");
    test_fdset();
    printf("-- sockets\n");
    test_sockets();
    printf("-- resolver\n");
    test_resolver();
    printf("-- wchar\n");
    test_wchar();
    printf("-- setjmp\n");
    test_setjmp();
    printf("-- paths_and_files\n");
    test_paths_and_files();
    printf("-- time_and_misc\n");
    test_time_and_misc();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
