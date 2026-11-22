/* BSD sockets and name resolution for the game's 32-bit bionic ABI. This file is part of the libc.c translation unit
 * (it is #included there and uses its macros); built on its own it is empty, so it is harmless in a build list.
 *
 * Design. Game socket descriptors are 512..1023 (so they fit the 1024-bit fd_set and never collide with file
 * descriptors) and index a table of host sockets. Every host socket is non-blocking; blocking behaviour is done
 * here with select, so a call never hangs on a dead peer: connect gives up after 5 s, and name resolution and
 * connections outside the local network are refused at once. The game's servers are gone, so by default
 * (R6SV_NET unset) only loopback, private (RFC 1918), link-local, multicast and broadcast addresses are
 * reachable; anything else fails immediately with ENETUNREACH, and a name other than "localhost" or a dotted
 * address fails with EAI_NONAME. R6SV_NET=real allows the real network (real DNS through the host resolver),
 * R6SV_NET=off makes socket() itself fail with ENETDOWN. On Windows ws2_32.dll is loaded at run time, so no
 * link flag is needed. IPv4 only; AF_INET6 and AF_UNIX fail with EAFNOSUPPORT. */
#ifdef RT_LIBC_UNITY

#define SOCK_BASE 512
#define SOCK_MAX 512
enum { NET_LIMITED = 0, NET_REAL = 1, NET_OFF = 2 };

/* ---- the host side ---------------------------------------------------------------------------------- */
#ifdef _WIN32
typedef SOCKET hsock;
#define HS_BAD INVALID_SOCKET
#define WS_FUNCS(X)                                                                                      \
    X(int, WSAStartup, (WORD, LPWSADATA))                                                                 \
    X(int, WSAGetLastError, (void))                                                                       \
    X(SOCKET, socket, (int, int, int))                                                                    \
    X(int, closesocket, (SOCKET))                                                                         \
    X(int, connect, (SOCKET, const struct sockaddr *, int))                                               \
    X(int, bind, (SOCKET, const struct sockaddr *, int))                                                  \
    X(int, listen, (SOCKET, int))                                                                         \
    X(SOCKET, accept, (SOCKET, struct sockaddr *, int *))                                                 \
    X(int, shutdown, (SOCKET, int))                                                                       \
    X(int, send, (SOCKET, const char *, int, int))                                                        \
    X(int, recv, (SOCKET, char *, int, int))                                                              \
    X(int, sendto, (SOCKET, const char *, int, int, const struct sockaddr *, int))                        \
    X(int, recvfrom, (SOCKET, char *, int, int, struct sockaddr *, int *))                                \
    X(int, getsockopt, (SOCKET, int, int, char *, int *))                                                 \
    X(int, setsockopt, (SOCKET, int, int, const char *, int))                                             \
    X(int, getsockname, (SOCKET, struct sockaddr *, int *))                                               \
    X(int, getpeername, (SOCKET, struct sockaddr *, int *))                                               \
    X(int, ioctlsocket, (SOCKET, long, u_long *))                                                         \
    X(int, select, (int, fd_set *, fd_set *, fd_set *, const struct timeval *))                           \
    X(int, getaddrinfo, (const char *, const char *, const struct addrinfo *, struct addrinfo **))        \
    X(void, freeaddrinfo, (struct addrinfo *))
#define X(ret, name, args) typedef ret(WSAAPI *fn_##name) args; static fn_##name p_##name;
WS_FUNCS(X)
#undef X
#define hs_socket p_socket
#define hs_close p_closesocket
#define hs_connect p_connect
#define hs_bind p_bind
#define hs_listen p_listen
#define hs_accept p_accept
#define hs_shutdown p_shutdown
#define hs_send p_send
#define hs_recv p_recv
#define hs_sendto p_sendto
#define hs_recvfrom p_recvfrom
#define hs_getsockopt p_getsockopt
#define hs_setsockopt p_setsockopt
#define hs_getsockname p_getsockname
#define hs_getpeername p_getpeername
#define hs_select p_select
#define hs_getaddrinfo p_getaddrinfo
#define hs_freeaddrinfo p_freeaddrinfo
#define hs_errno() p_WSAGetLastError()
static volatile int ws_state; /* 0 not tried, 1 ready, -1 unavailable */
static int net_host_init(void) {
    if (ws_state) return ws_state > 0;
    HMODULE h = LoadLibraryA("ws2_32.dll");
    int ok = h != NULL;
#define X(ret, name, args) if (ok) { p_##name = (fn_##name)(void *)GetProcAddress(h, #name); if (!p_##name) ok = 0; }
    WS_FUNCS(X)
#undef X
    if (ok) { WSADATA wsa; ok = p_WSAStartup(0x0202, &wsa) == 0; }
    ws_state = ok ? 1 : -1;
    return ok;
}
static void hs_set_nonblock(hsock s) { u_long m = 1; p_ioctlsocket(s, FIONBIO, &m); }
static int hs_avail(hsock s) { u_long n = 0; p_ioctlsocket(s, FIONREAD, &n); return (int)n; }
#else
typedef int hsock;
#define HS_BAD (-1)
#define hs_socket socket
#define hs_close close
#define hs_connect connect
#define hs_bind bind
#define hs_listen listen
#define hs_accept accept
#define hs_shutdown shutdown
#define hs_send send
#define hs_recv recv
#define hs_sendto sendto
#define hs_recvfrom recvfrom
#define hs_getsockopt getsockopt
#define hs_setsockopt setsockopt
#define hs_getsockname getsockname
#define hs_getpeername getpeername
#define hs_select select
#define hs_getaddrinfo getaddrinfo
#define hs_freeaddrinfo freeaddrinfo
#define hs_errno() errno
static int net_host_init(void) { return 1; }
static void hs_set_nonblock(hsock s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
static int hs_avail(hsock s) { int n = 0; ioctl(s, FIONREAD, &n); return n; }
#endif

/* a host error code as the game's (Linux) errno */
static uint32_t net_map_err(int e) {
#ifdef _WIN32
    switch (e) {
    case WSAEINTR: return G_EINTR; case WSAEBADF: return G_EBADF; case WSAEACCES: return G_EACCES;
    case WSAEINVAL: return G_EINVAL; case WSAEWOULDBLOCK: return G_EAGAIN; case WSAEINPROGRESS: return G_EINPROGRESS;
    case WSAEALREADY: return G_EALREADY; case WSAENOTSOCK: return G_ENOTSOCK; case WSAEDESTADDRREQ: return G_EDESTADDRREQ;
    case WSAEMSGSIZE: return G_EMSGSIZE; case WSAENOPROTOOPT: return G_ENOPROTOOPT;
    case WSAEPROTONOSUPPORT: return G_EPROTONOSUPPORT; case WSAEOPNOTSUPP: return G_EOPNOTSUPP;
    case WSAEAFNOSUPPORT: return G_EAFNOSUPPORT; case WSAEADDRINUSE: return G_EADDRINUSE;
    case WSAEADDRNOTAVAIL: return G_EADDRNOTAVAIL; case WSAENETDOWN: return G_ENETDOWN;
    case WSAENETUNREACH: return G_ENETUNREACH; case WSAECONNABORTED: return G_ECONNABORTED;
    case WSAECONNRESET: return G_ECONNRESET; case WSAENOBUFS: return G_ENOBUFS; case WSAEISCONN: return G_EISCONN;
    case WSAENOTCONN: return G_ENOTCONN; case WSAESHUTDOWN: return G_EPIPE; case WSAETIMEDOUT: return G_ETIMEDOUT;
    case WSAECONNREFUSED: return G_ECONNREFUSED; case WSAEHOSTUNREACH: return G_EHOSTUNREACH;
    default: return e ? G_EIO : 0;
    }
#else
    switch (e) { /* the host is Linux or Android: the numbers already agree; map the few that can differ */
    case EAGAIN: return G_EAGAIN;
    case EINPROGRESS: return G_EINPROGRESS;
    case ECONNREFUSED: return G_ECONNREFUSED;
    case ETIMEDOUT: return G_ETIMEDOUT;
    case ENETUNREACH: return G_ENETUNREACH;
    case EHOSTUNREACH: return G_EHOSTUNREACH;
    case EPIPE: return G_EPIPE;
    default: return (uint32_t)e;
    }
#endif
}
static int net_would_block(int e) {
#ifdef _WIN32
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY;
#else
    return e == EAGAIN || e == EWOULDBLOCK || e == EINPROGRESS || e == EALREADY;
#endif
}
static int net_in_progress(int e) {
#ifdef _WIN32
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return e == EINPROGRESS || e == EAGAIN;
#endif
}

/* ---- host fd sets and waiting ------------------------------------------------------------------------ */
typedef struct { fd_set fs; } hfd;
static void hfd_zero(hfd *h) { FD_ZERO(&h->fs); }
static void hfd_add(hfd *h, hsock s) {
#ifdef _WIN32
    if (h->fs.fd_count < FD_SETSIZE) h->fs.fd_array[h->fs.fd_count++] = s;
#else
    if (s >= 0 && s < FD_SETSIZE) FD_SET(s, &h->fs);
#endif
}
static int hfd_has(hfd *h, hsock s) {
#ifdef _WIN32
    for (u_int k = 0; k < h->fs.fd_count; k++) if (h->fs.fd_array[k] == s) return 1;
    return 0;
#else
    return s >= 0 && s < FD_SETSIZE && FD_ISSET(s, &h->fs);
#endif
}
/* select over up to three sets; timeout_ms < 0 waits for ever; returns the host's result */
static int hs_select3(int maxfd, hfd *r, hfd *w, hfd *e, int timeout_ms) {
    struct timeval tv, *tp = NULL;
    if (timeout_ms >= 0) { tv.tv_sec = timeout_ms / 1000; tv.tv_usec = (timeout_ms % 1000) * 1000; tp = &tv; }
    return hs_select(maxfd + 1, r ? &r->fs : NULL, w ? &w->fs : NULL, e ? &e->fs : NULL, tp);
}

/* ---- the socket table ---------------------------------------------------------------------------------- */
typedef struct { volatile int used; hsock s; int type, nonblock, rcv_ms, snd_ms; } gsock;
static gsock socks[SOCK_MAX];
static int net_mode = -1;

static int net_mode_get(void) {
    if (net_mode < 0) {
        const char *e = getenv("R6SV_NET");
        net_mode = e && !strcmp(e, "real") ? NET_REAL : e && !strcmp(e, "off") ? NET_OFF : NET_LIMITED;
    }
    return net_mode;
}
static gsock *sock_of(int gfd) {
    if (gfd < SOCK_BASE || gfd >= SOCK_BASE + SOCK_MAX) return NULL;
    gsock *g = &socks[gfd - SOCK_BASE];
    return __atomic_load_n(&g->used, __ATOMIC_ACQUIRE) == 1 ? g : NULL;
}
static int sock_alloc(hsock s, int type, int nonblock) {
    for (int k = 0; k < SOCK_MAX; k++) {
        int zero = 0;
        if (__atomic_compare_exchange_n(&socks[k].used, &zero, 2, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            socks[k].s = s; socks[k].type = type; socks[k].nonblock = nonblock; socks[k].rcv_ms = socks[k].snd_ms = 0;
            __atomic_store_n(&socks[k].used, 1, __ATOMIC_RELEASE);
            return SOCK_BASE + k;
        }
    }
    return -1;
}
static int net_is_sock(int fd) { return sock_of(fd) != NULL; }
static int net_close(cpu_t *c, int fd) {
    gsock *g = sock_of(fd);
    if (!g) { set_errno(c, G_EBADF); return -1; }
    hs_close(g->s);
    __atomic_store_n(&g->used, 0, __ATOMIC_RELEASE);
    return 0;
}

/* wait until the socket is readable/writable: 1 ready, 0 timed out (timeout_ms < 0: for ever), -1 error */
static int wait_sock(hsock s, int writable, int timeout_ms) {
    int waited = 0;
    for (;;) {
        hfd r, w, e;
        hfd_zero(&r); hfd_zero(&w); hfd_zero(&e);
        hfd_add(writable ? &w : &r, s);
#ifdef _WIN32
        hfd_add(&e, s); /* a failed non-blocking connect is reported as an exception on Windows */
#endif
        int slice = timeout_ms < 0 ? 200 : (timeout_ms - waited < 200 ? timeout_ms - waited : 200);
        if (slice < 0) slice = 0;
        int n = hs_select3((int)s, &r, &w, &e, slice);
        if (n < 0) return -1;
        if (n > 0) return 1;
        waited += slice;
        if (timeout_ms >= 0 && waited >= timeout_ms) return 0;
    }
}

/* ---- addresses -------------------------------------------------------------------------------------------- */
/* may the game talk to this address (4 bytes in network order)? */
static int addr_allowed(const uint8_t *b) {
    if (net_mode_get() == NET_REAL) return 1;
    return b[0] == 127 || b[0] == 10 || b[0] == 0 || (b[0] == 172 && b[1] >= 16 && b[1] <= 31) ||
           (b[0] == 192 && b[1] == 168) || (b[0] == 169 && b[1] == 254) || (b[0] >= 224 && b[0] <= 239) ||
           (b[0] == 255 && b[1] == 255 && b[2] == 255 && b[3] == 255);
}

/* inet_aton: a, a.b, a.b.c or a.b.c.d with decimal, octal (0) or hex (0x) parts; the result is in network order */
static int parse_inet(const char *s, uint8_t out[4]) {
    uint32_t part[4];
    int n = 0;
    for (;;) {
        char *end;
        if (!isdigit((unsigned char)*s)) return 0;
        unsigned long v = strtoul(s, &end, 0);
        if (end == s || n >= 4) return 0;
        part[n++] = (uint32_t)v;
        s = end;
        if (*s == '.') { s++; continue; }
        break;
    }
    if (*s && !isspace((unsigned char)*s)) return 0;
    uint32_t addr;
    for (int k = 0; k < n - 1; k++) if (part[k] > 255) return 0;
    if (part[n - 1] > (n == 1 ? 0xFFFFFFFFu : n == 2 ? 0xFFFFFFu : n == 3 ? 0xFFFFu : 0xFFu)) return 0;
    switch (n) {
    case 1: addr = part[0]; break;
    case 2: addr = part[0] << 24 | part[1]; break;
    case 3: addr = part[0] << 24 | part[1] << 16 | part[2]; break;
    default: addr = part[0] << 24 | part[1] << 16 | part[2] << 8 | part[3]; break;
    }
    out[0] = (uint8_t)(addr >> 24); out[1] = (uint8_t)(addr >> 16); out[2] = (uint8_t)(addr >> 8); out[3] = (uint8_t)addr;
    return 1;
}

/* a host name to an IPv4 address: localhost always; anything else only in R6SV_NET=real mode */
static int resolve_name(const char *name, uint8_t out[4]) {
    if (parse_inet(name, out)) return 1;
    if (!strcasecmp(name, "localhost")) { out[0] = 127; out[1] = out[2] = 0; out[3] = 1; return 1; }
    if (net_mode_get() != NET_REAL || !net_host_init()) return 0;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (hs_getaddrinfo(name, NULL, &hints, &res) != 0 || !res) return 0;
    memcpy(out, &((struct sockaddr_in *)res->ai_addr)->sin_addr, 4);
    hs_freeaddrinfo(res);
    return 1;
}

/* guest sockaddr_in: family (2 bytes), port and address in network order, 8 zero bytes */
static int get_sa(cpu_t *c, uint32_t addr, uint32_t len, struct sockaddr_in *out) {
    if (!addr || len < 8) { set_errno(c, G_EINVAL); return -1; }
    uint16_t fam;
    memcpy(&fam, G(addr), 2);
    if (fam != 2) { set_errno(c, G_EAFNOSUPPORT); return -1; }
    memset(out, 0, sizeof *out);
    out->sin_family = AF_INET;
    memcpy(&out->sin_port, G(addr + 2), 2);
    memcpy(&out->sin_addr, G(addr + 4), 4);
    return 0;
}
static void put_sa(uint32_t addr, uint32_t lenp, const struct sockaddr_in *sa) {
    if (!addr || !lenp) return;
    uint8_t img[16] = {2, 0};
    memcpy(img + 2, &sa->sin_port, 2);
    memcpy(img + 4, &sa->sin_addr, 4);
    uint32_t room = rt_ld32(lenp);
    memcpy(G(addr), img, room < 16 ? room : 16);
    rt_st32(lenp, 16);
}

/* ---- guest fd_set (bionic: 1024 bits in 32 words) -------------------------------------------------------- */
static int gfdset_test(const uint8_t *set, int fd) { uint32_t w; memcpy(&w, set + 4 * (fd >> 5), 4); return (int)((w >> (fd & 31)) & 1); }
static void gfdset_set(uint8_t *set, int fd) { uint32_t w; memcpy(&w, set + 4 * (fd >> 5), 4); w |= 1u << (fd & 31); memcpy(set + 4 * (fd >> 5), &w, 4); }

typedef struct { uint8_t r[128], w[128], e[128]; } gsets;

/* The select engine on guest bit sets. Sockets go to the host select; other descriptors (regular files, which are
 * always ready) count as ready for reading and writing. Fills the output sets and returns the number of ready
 * entries, or -1 with *err set to a guest errno. */
static int net_select_sets(int nfds, const gsets *in, gsets *out, int timeout_ms, uint32_t *err) {
    hfd hr, hw, he;
    hfd_zero(&hr); hfd_zero(&hw); hfd_zero(&he);
    memset(out, 0, sizeof *out);
    int nsock = 0, ready = 0;
    int maxfd = -1;
    for (int fd = 0; fd < nfds; fd++) {
        int ir = gfdset_test(in->r, fd), iw = gfdset_test(in->w, fd), ie = gfdset_test(in->e, fd);
        if (!ir && !iw && !ie) continue;
        gsock *g = sock_of(fd);
        if (!g) {
            if (ir) { gfdset_set(out->r, fd); ready++; }
            if (iw) { gfdset_set(out->w, fd); ready++; }
            continue;
        }
        nsock++;
        if ((int)g->s > maxfd) maxfd = (int)g->s;
        if (ir) hfd_add(&hr, g->s);
        if (iw) { hfd_add(&hw, g->s);
#ifdef _WIN32
            hfd_add(&he, g->s);
#endif
        }
        if (ie) hfd_add(&he, g->s);
    }
    if (!nsock) {
        if (!ready && timeout_ms != 0) {
#ifdef _WIN32
            Sleep(timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms);
#else
            struct timespec ts = {timeout_ms < 0 ? 3600 : timeout_ms / 1000, timeout_ms < 0 ? 0 : (long)(timeout_ms % 1000) * 1000000L};
            nanosleep(&ts, NULL);
#endif
        }
        return ready;
    }
    int n = hs_select3(maxfd, &hr, &hw, &he, ready ? 0 : timeout_ms);
    if (n < 0) { *err = net_map_err(hs_errno()); return -1; }
    for (int fd = 0; fd < nfds; fd++) {
        gsock *g = sock_of(fd);
        if (!g) continue;
        int ir = gfdset_test(in->r, fd), iw = gfdset_test(in->w, fd), ie = gfdset_test(in->e, fd);
        if (ir && hfd_has(&hr, g->s)) { gfdset_set(out->r, fd); ready++; }
        if (iw) {
            int wr = hfd_has(&hw, g->s);
#ifdef _WIN32
            wr = wr || hfd_has(&he, g->s); /* a failed connect: writable, and SO_ERROR tells why */
#endif
            if (wr) { gfdset_set(out->w, fd); ready++; }
        }
#ifdef _WIN32
        if (ie && !iw && hfd_has(&he, g->s)) { gfdset_set(out->e, fd); ready++; }
#else
        if (ie && hfd_has(&he, g->s)) { gfdset_set(out->e, fd); ready++; }
#endif
    }
    return ready;
}

IMP(select) { /* (nfds, readfds, writefds, exceptfds, timeval *) with 32-bit fd_set and timeval */
    int nfds = (int)A(0);
    uint32_t sets[3] = {A(1), A(2), A(3)};
    if (nfds < 0 || nfds > 1024) { set_errno(c, G_EINVAL); RET(-1); return; }
    int nbytes = ((nfds + 31) / 32) * 4;
    gsets in, out;
    memset(&in, 0, sizeof in);
    uint8_t *dst[3] = {in.r, in.w, in.e};
    for (int k = 0; k < 3; k++) if (sets[k] && nbytes) memcpy(dst[k], G(sets[k]), (size_t)nbytes);
    int timeout_ms = -1;
    if (A(4)) {
        int64_t ms = (int64_t)(int32_t)rt_ld32(A(4)) * 1000 + ((int32_t)rt_ld32(A(4) + 4) + 999) / 1000;
        timeout_ms = ms < 0 ? 0 : ms > 0x7FFFFFF ? 0x7FFFFFF : (int)ms;
    }
    uint32_t err = 0;
    int n = net_select_sets(nfds, &in, &out, timeout_ms, &err);
    if (n < 0) { set_errno(c, err); RET(-1); return; }
    uint8_t *res[3] = {out.r, out.w, out.e};
    for (int k = 0; k < 3; k++) if (sets[k] && nbytes) memcpy(G(sets[k]), res[k], (size_t)nbytes);
    RET(n);
}

IMP(poll) { /* (struct pollfd *, nfds, timeout ms): pollfd is {int fd; short events, revents} */
    uint32_t base = A(0), n = A(1);
    int timeout_ms = (int)A(2);
    gsets in, out;
    memset(&in, 0, sizeof in);
    int maxfd = -1;
    for (uint32_t k = 0; k < n; k++) {
        int fd = (int)rt_ld32(base + 8 * k);
        uint16_t ev;
        memcpy(&ev, G(base + 8 * k + 4), 2);
        if (fd < 0 || fd >= 1024) continue;
        if (ev & 1) gfdset_set(in.r, fd);
        if (ev & 4) gfdset_set(in.w, fd);
        gfdset_set(in.e, fd);
        if (fd > maxfd) maxfd = fd;
    }
    uint32_t err = 0;
    int r = net_select_sets(maxfd + 1, &in, &out, timeout_ms, &err);
    if (r < 0) { set_errno(c, err); RET(-1); return; }
    int count = 0;
    for (uint32_t k = 0; k < n; k++) {
        int fd = (int)rt_ld32(base + 8 * k);
        uint16_t rev = 0;
        if (fd >= 0 && fd < 1024) {
            if (gfdset_test(out.r, fd)) rev |= 1;
            if (gfdset_test(out.w, fd)) rev |= 4;
            if (gfdset_test(out.e, fd)) rev |= 8;
            if (fd >= SOCK_BASE && !sock_of(fd)) rev = 0x20; /* POLLNVAL */
        }
        memcpy(G(base + 8 * k + 6), &rev, 2);
        count += rev != 0;
    }
    RET(count);
}

/* ---- socket calls ---------------------------------------------------------------------------------------- */
IMP(socket) {
    uint32_t domain = A(0), type = A(1);
    if (net_mode_get() == NET_OFF || !net_host_init()) { set_errno(c, G_ENETDOWN); RET(-1); return; }
    if (domain != 2) { set_errno(c, G_EAFNOSUPPORT); RET(-1); return; }
    uint32_t base = type & 0xF;
    if (base != 1 && base != 2) { set_errno(c, G_EPROTONOSUPPORT); RET(-1); return; }
    hsock s = hs_socket(AF_INET, base == 1 ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (s == HS_BAD) { set_errno(c, net_map_err(hs_errno())); RET(-1); return; }
    hs_set_nonblock(s);
    int fd = sock_alloc(s, (int)base, (type & 0x800) != 0);
    if (fd < 0) { hs_close(s); set_errno(c, G_EMFILE); RET(-1); return; }
    RET(fd);
}

IMP(bind) {
    gsock *g = sock_of((int)A(0));
    struct sockaddr_in sa;
    if (!g) { set_errno(c, G_ENOTSOCK); RET(-1); return; }
    if (get_sa(c, A(1), A(2), &sa)) { RET(-1); return; }
    if (hs_bind(g->s, (struct sockaddr *)&sa, sizeof sa)) { set_errno(c, net_map_err(hs_errno())); RET(-1); return; }
    RET(0);
}

IMP(listen) {
    gsock *g = sock_of((int)A(0));
    if (!g) { set_errno(c, G_ENOTSOCK); RET(-1); return; }
    if (hs_listen(g->s, (int)A(1))) { set_errno(c, net_map_err(hs_errno())); RET(-1); return; }
    RET(0);
}

IMP(accept) {
    gsock *g = sock_of((int)A(0));
    if (!g) { set_errno(c, G_ENOTSOCK); RET(-1); return; }
    for (;;) {
        struct sockaddr_in sa;
        socklen_t sl = sizeof sa;
        hsock s = hs_accept(g->s, (struct sockaddr *)&sa, &sl);
        if (s != HS_BAD) {
            hs_set_nonblock(s);
            int fd = sock_alloc(s, g->type, 0);
            if (fd < 0) { hs_close(s); set_errno(c, G_EMFILE); RET(-1); return; }
            put_sa(A(1), A(2), &sa);
            RET(fd);
            return;
        }
        int e = hs_errno();
        if (!net_would_block(e)) { set_errno(c, net_map_err(e)); RET(-1); return; }
        if (g->nonblock) { set_errno(c, G_EAGAIN); RET(-1); return; }
        if (wait_sock(g->s, 0, g->rcv_ms > 0 ? g->rcv_ms : -1) <= 0 && g->rcv_ms > 0) { set_errno(c, G_EAGAIN); RET(-1); return; }
    }
}

IMP(connect) {
    gsock *g = sock_of((int)A(0));
    struct sockaddr_in sa;
    if (!g) { set_errno(c, G_ENOTSOCK); RET(-1); return; }
    if (get_sa(c, A(1), A(2), &sa)) { RET(-1); return; }
    if (!addr_allowed((const uint8_t *)&sa.sin_addr)) { /* the servers are gone: fail at once, blocking or not */
        if (rt_verbose) {
            const uint8_t *b = (const uint8_t *)&sa.sin_addr;
            rt_log("connect to %u.%u.%u.%u refused (R6SV_NET)", b[0], b[1], b[2], b[3]);
        }
        set_errno(c, G_ENETUNREACH);
        RET(-1);
        return;
    }
    if (hs_connect(g->s, (struct sockaddr *)&sa, sizeof sa) == 0) { RET(0); return; }
    int e = hs_errno();
    if (!net_in_progress(e)) { set_errno(c, net_map_err(e)); RET(-1); return; }
    if (g->nonblock) { set_errno(c, G_EINPROGRESS); RET(-1); return; }
    int w = wait_sock(g->s, 1, g->snd_ms > 0 ? g->snd_ms : 5000); /* blocking connect: give up after 5 s */
    if (w <= 0) { set_errno(c, G_ETIMEDOUT); RET(-1); return; }
    int err = 0;
    socklen_t el = sizeof err;
    hs_getsockopt(g->s, SOL_SOCKET, SO_ERROR, (char *)&err, &el);
    if (err) { set_errno(c, net_map_err(err)); RET(-1); return; }
    RET(0);
}

IMP(shutdown) {
    gsock *g = sock_of((int)A(0));
    if (!g) { set_errno(c, G_ENOTSOCK); RET(-1); return; }
    if (hs_shutdown(g->s, (int)A(1))) { set_errno(c, net_map_err(hs_errno())); RET(-1); return; }
    RET(0);
}

static int sock_name(cpu_t *c, int peer, uint32_t fd, uint32_t addr, uint32_t lenp) {
    gsock *g = sock_of((int)fd);
    if (!g) { set_errno(c, G_ENOTSOCK); return -1; }
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    memset(&sa, 0, sizeof sa);
    if ((peer ? hs_getpeername(g->s, (struct sockaddr *)&sa, &sl) : hs_getsockname(g->s, (struct sockaddr *)&sa, &sl))) {
        set_errno(c, net_map_err(hs_errno()));
        return -1;
    }
    put_sa(addr, lenp, &sa);
    return 0;
}
IMP(getsockname) { RET(sock_name(c, 0, A(0), A(1), A(2))); }
IMP(getpeername) { RET(sock_name(c, 1, A(0), A(1), A(2))); }

/* guest MSG_* flags to host flags */
static int msg_flags(uint32_t gf) {
    int f = 0;
    if (gf & 1) f |= MSG_OOB;
    if (gf & 2) f |= MSG_PEEK;
#if defined(MSG_NOSIGNAL) && !defined(_WIN32)
    f |= MSG_NOSIGNAL;
#endif
    return f;
}

/* receive: 0x40 (MSG_DONTWAIT) and the socket's own flag make it non-blocking; otherwise wait (SO_RCVTIMEO bounds it) */
static int32_t do_recv(cpu_t *c, int gfd, uint32_t buf, uint32_t len, uint32_t gflags, uint32_t from, uint32_t fromlen) {
    gsock *g = sock_of(gfd);
    if (!g) { set_errno(c, G_ENOTSOCK); return -1; }
    int dontwait = g->nonblock || (gflags & 0x40);
    for (;;) {
        struct sockaddr_in sa;
        socklen_t sl = sizeof sa;
        int n;
        memset(&sa, 0, sizeof sa);
        if (from) n = (int)hs_recvfrom(g->s, (char *)G(buf), (int)len, msg_flags(gflags), (struct sockaddr *)&sa, &sl);
        else n = (int)hs_recv(g->s, (char *)G(buf), (int)len, msg_flags(gflags));
        if (n >= 0) { if (from) put_sa(from, fromlen, &sa); return n; }
        int e = hs_errno();
#ifdef _WIN32
        if (e == WSAEMSGSIZE && from) { put_sa(from, fromlen, &sa); return (int32_t)len; } /* datagram truncated */
#endif
        if (!net_would_block(e)) { set_errno(c, net_map_err(e)); return -1; }
        if (dontwait) { set_errno(c, G_EAGAIN); return -1; }
        if (wait_sock(g->s, 0, g->rcv_ms > 0 ? g->rcv_ms : -1) == 0 && g->rcv_ms > 0) { set_errno(c, G_EAGAIN); return -1; }
    }
}

static int32_t do_send(cpu_t *c, int gfd, uint32_t buf, uint32_t len, uint32_t gflags, uint32_t to, uint32_t tolen) {
    gsock *g = sock_of(gfd);
    struct sockaddr_in sa;
    if (!g) { set_errno(c, G_ENOTSOCK); return -1; }
    if (to) {
        if (get_sa(c, to, tolen, &sa)) return -1;
        if (!addr_allowed((const uint8_t *)&sa.sin_addr)) { set_errno(c, G_ENETUNREACH); return -1; }
    }
    int dontwait = g->nonblock || (gflags & 0x40);
    for (;;) {
        int n = to ? (int)hs_sendto(g->s, (const char *)G(buf), (int)len, msg_flags(gflags), (struct sockaddr *)&sa, sizeof sa)
                   : (int)hs_send(g->s, (const char *)G(buf), (int)len, msg_flags(gflags));
        if (n >= 0) return n;
        int e = hs_errno();
        if (!net_would_block(e)) { set_errno(c, net_map_err(e)); return -1; }
        if (dontwait) { set_errno(c, G_EAGAIN); return -1; }
        if (wait_sock(g->s, 1, g->snd_ms > 0 ? g->snd_ms : -1) == 0 && g->snd_ms > 0) { set_errno(c, G_EAGAIN); return -1; }
    }
}
static int32_t net_read(cpu_t *c, int fd, uint32_t buf, uint32_t len) { return do_recv(c, fd, buf, len, 0, 0, 0); }
static int32_t net_write(cpu_t *c, int fd, uint32_t buf, uint32_t len) { return do_send(c, fd, buf, len, 0, 0, 0); }
IMP(recv) { RET(do_recv(c, (int)A(0), A(1), A(2), A(3), 0, 0)); }
IMP(recvfrom) { RET(do_recv(c, (int)A(0), A(1), A(2), A(3), A(4), A(5))); }
IMP(send) { RET(do_send(c, (int)A(0), A(1), A(2), A(3), 0, 0)); }
IMP(sendto) { RET(do_send(c, (int)A(0), A(1), A(2), A(3), A(4), A(5))); }

/* ---- options, ioctl, fcntl ------------------------------------------------------------------------------- */
IMP(setsockopt) { /* (fd, level, optname, optval, optlen) with Linux level and option numbers */
    gsock *g = sock_of((int)A(0));
    uint32_t level = A(1), opt = A(2), val = A(3), len = A(4);
    if (!g) { set_errno(c, G_ENOTSOCK); RET(-1); return; }
    int v = len >= 4 && val ? (int)rt_ld32(val) : 0;
    int r = 0;
    if (level == 1) { /* SOL_SOCKET */
        switch (opt) {
        case 2: r = hs_setsockopt(g->s, SOL_SOCKET, SO_REUSEADDR, (char *)&v, sizeof v); break;
        case 6: r = hs_setsockopt(g->s, SOL_SOCKET, SO_BROADCAST, (char *)&v, sizeof v); break;
        case 7: r = hs_setsockopt(g->s, SOL_SOCKET, SO_SNDBUF, (char *)&v, sizeof v); break;
        case 8: r = hs_setsockopt(g->s, SOL_SOCKET, SO_RCVBUF, (char *)&v, sizeof v); break;
        case 9: r = hs_setsockopt(g->s, SOL_SOCKET, SO_KEEPALIVE, (char *)&v, sizeof v); break;
        case 13: { /* SO_LINGER: {int onoff, linger} */
            struct linger l;
            if (len < 8 || !val) { set_errno(c, G_EINVAL); RET(-1); return; }
            l.l_onoff = (__typeof__(l.l_onoff))rt_ld32(val);
            l.l_linger = (__typeof__(l.l_linger))rt_ld32(val + 4);
            r = hs_setsockopt(g->s, SOL_SOCKET, SO_LINGER, (char *)&l, sizeof l);
            break;
        }
        case 20: case 21: { /* SO_RCVTIMEO / SO_SNDTIMEO: a timeval, kept here (the host socket stays non-blocking) */
            int ms = 0;
            if (len >= 8 && val) ms = (int)rt_ld32(val) * 1000 + ((int)rt_ld32(val + 4) + 999) / 1000;
            if (opt == 20) g->rcv_ms = ms; else g->snd_ms = ms;
            break;
        }
        default: break; /* SO_REUSEPORT, SO_PRIORITY, ...: accepted and ignored */
        }
    } else if (level == 6 && opt == 1) { /* IPPROTO_TCP, TCP_NODELAY */
        r = hs_setsockopt(g->s, IPPROTO_TCP, TCP_NODELAY, (char *)&v, sizeof v);
    } else if (level == 0 && (opt == 1 || opt == 2 || opt == 33)) { /* IPPROTO_IP: IP_TOS, IP_TTL, IP_MULTICAST_TTL */
        int ho = opt == 1 ? IP_TOS : opt == 2 ? IP_TTL : IP_MULTICAST_TTL;
        r = hs_setsockopt(g->s, IPPROTO_IP, ho, (char *)&v, sizeof v);
    } else if (level == 0 && (opt == 35 || opt == 36) && len >= 8) { /* IP_ADD/DROP_MEMBERSHIP: ip_mreq, 8 bytes */
        struct ip_mreq m;
        memcpy(&m, G(val), 8);
        r = hs_setsockopt(g->s, IPPROTO_IP, opt == 35 ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP, (char *)&m, sizeof m);
    }
    if (r) { set_errno(c, net_map_err(hs_errno())); RET(-1); return; }
    RET(0);
}

IMP(getsockopt) {
    gsock *g = sock_of((int)A(0));
    uint32_t level = A(1), opt = A(2), val = A(3), lenp = A(4);
    if (!g) { set_errno(c, G_ENOTSOCK); RET(-1); return; }
    int out = 0, r = 0;
    socklen_t sl = sizeof out;
    if (level == 1) {
        switch (opt) {
        case 3: out = g->type; break; /* SO_TYPE */
        case 4: { /* SO_ERROR: the pending error, as a Linux errno, and cleared */
            int e = 0;
            socklen_t el = sizeof e;
            hs_getsockopt(g->s, SOL_SOCKET, SO_ERROR, (char *)&e, &el);
            out = (int)net_map_err(e);
            break;
        }
        case 2: r = hs_getsockopt(g->s, SOL_SOCKET, SO_REUSEADDR, (char *)&out, &sl); break;
        case 6: r = hs_getsockopt(g->s, SOL_SOCKET, SO_BROADCAST, (char *)&out, &sl); break;
        case 7: r = hs_getsockopt(g->s, SOL_SOCKET, SO_SNDBUF, (char *)&out, &sl); break;
        case 8: r = hs_getsockopt(g->s, SOL_SOCKET, SO_RCVBUF, (char *)&out, &sl); break;
        case 9: r = hs_getsockopt(g->s, SOL_SOCKET, SO_KEEPALIVE, (char *)&out, &sl); break;
        case 20: case 21: { /* a timeval of the stored timeout */
            int ms = opt == 20 ? g->rcv_ms : g->snd_ms;
            if (!val || !lenp || rt_ld32(lenp) < 8) { set_errno(c, G_EINVAL); RET(-1); return; }
            rt_st32(val, (uint32_t)(ms / 1000));
            rt_st32(val + 4, (uint32_t)(ms % 1000) * 1000);
            rt_st32(lenp, 8);
            RET(0);
            return;
        }
        default: set_errno(c, G_ENOPROTOOPT); RET(-1); return;
        }
    } else if (level == 6 && opt == 1) {
        r = hs_getsockopt(g->s, IPPROTO_TCP, TCP_NODELAY, (char *)&out, &sl);
    } else {
        set_errno(c, G_ENOPROTOOPT);
        RET(-1);
        return;
    }
    if (r) { set_errno(c, net_map_err(hs_errno())); RET(-1); return; }
    if (val && lenp) {
        uint32_t room = rt_ld32(lenp), n = room < 4 ? room : 4;
        memcpy(G(val), &out, n);
        rt_st32(lenp, 4);
    }
    RET(0);
}

IMP(ioctl) { /* (fd, request, arg): the Linux request numbers */
    gsock *g = sock_of((int)A(0));
    uint32_t req = A(1), arg = A(2);
    if (!g) { set_errno(c, G_ENOTTY); RET(-1); return; }
    switch (req) {
    case 0x5421: /* FIONBIO */
        g->nonblock = arg && rt_ld32(arg) != 0;
        RET(0);
        break;
    case 0x541B: /* FIONREAD */
        if (arg) rt_st32(arg, (uint32_t)hs_avail(g->s));
        RET(0);
        break;
    default: /* SIOCGIFCONF, SIOCGIFADDR, ...: no interfaces to report */
        set_errno(c, G_EINVAL);
        RET(-1);
    }
}

IMP(fcntl) { /* (fd, cmd, arg) */
    int fd = (int)A(0);
    uint32_t cmd = A(1), arg = A(2);
    gsock *g = sock_of(fd);
    switch (cmd) {
    case 1: RET(0); break;      /* F_GETFD */
    case 2: RET(0); break;      /* F_SETFD */
    case 3: RET(2 | (g && g->nonblock ? 04000 : 0)); break; /* F_GETFL: O_RDWR, O_NONBLOCK */
    case 4: if (g) g->nonblock = (arg & 04000) != 0; RET(0); break; /* F_SETFL */
    default: set_errno(c, G_EINVAL); RET(-1);
    }
}

/* ---- name resolution ---------------------------------------------------------------------------------------- */
IMP(gethostname) { snprintf(S(0), A(1), "localhost"); RET(0); }

static const struct { const char *name; uint16_t port; } services[] = {
    {"ftp", 21}, {"ssh", 22}, {"telnet", 23}, {"smtp", 25}, {"domain", 53}, {"http", 80}, {"pop3", 110},
    {"ntp", 123}, {"imap", 143}, {"https", 443}, {"echo", 7}};

/* guest addrinfo (bionic order): flags, family, socktype, protocol, addrlen, canonname, addr, next = 32 bytes,
 * then the 16-byte sockaddr_in and the canonical name in the same block */
IMP(getaddrinfo) { /* (node, service, hints, res) */
    const char *node = A(0) ? S(0) : NULL, *serv = A(1) ? S(1) : NULL;
    uint32_t hints = A(2), res = A(3);
    uint32_t flags = 0, family = 0, socktype = 0, protocol = 0;
    if (hints) { flags = rt_ld32(hints); family = rt_ld32(hints + 4); socktype = rt_ld32(hints + 8); protocol = rt_ld32(hints + 12); }
    if (!res || (!node && !serv)) { RET(8); return; } /* EAI_NONAME */
    if (family != 0 && family != 2) { RET(5); return; } /* EAI_FAMILY */
    if (socktype != 0 && socktype != 1 && socktype != 2) { RET(10); return; } /* EAI_SOCKTYPE */
    uint16_t port = 0;
    if (serv) {
        char *end;
        unsigned long v = strtoul(serv, &end, 10);
        if (*serv && !*end && v <= 65535) port = (uint16_t)v;
        else if (flags & 0x400) { RET(8); return; } /* AI_NUMERICSERV */
        else {
            int found = 0;
            for (size_t k = 0; k < sizeof services / sizeof *services && !found; k++)
                if (!strcmp(serv, services[k].name)) { port = services[k].port; found = 1; }
            if (!found) { RET(9); return; } /* EAI_SERVICE */
        }
    }
    uint8_t ip[4];
    if (!node) { memset(ip, 0, 4); if (!(flags & 1)) ip[0] = 127, ip[3] = 1; } /* AI_PASSIVE: the wildcard */
    else if (!parse_inet(node, ip)) {
        if (flags & 4) { RET(8); return; } /* AI_NUMERICHOST */
        if (!resolve_name(node, ip)) { RET(8); return; }
    }
    uint32_t head = 0, tail = 0;
    int types[2] = {1, 2}, ntypes = 2;
    if (socktype) { types[0] = (int)socktype; ntypes = 1; }
    size_t cn = node && (flags & 2) ? strlen(node) + 1 : 0;
    for (int k = 0; k < ntypes; k++) {
        uint32_t b = rt_calloc(1, (uint32_t)(48 + (k == 0 ? cn : 0)));
        if (!b) { RET(6); return; } /* EAI_MEMORY */
        rt_st32(b + 4, 2);
        rt_st32(b + 8, (uint32_t)types[k]);
        rt_st32(b + 12, protocol ? protocol : types[k] == 1 ? 6u : 17u);
        rt_st32(b + 16, 16);
        rt_st32(b + 24, b + 32);
        uint8_t *sa = rt_mem + b + 32;
        sa[0] = 2;
        memcpy(sa + 2, (uint8_t[]){(uint8_t)(port >> 8), (uint8_t)port}, 2);
        memcpy(sa + 4, ip, 4);
        if (k == 0 && cn) { memcpy(rt_mem + b + 48, node, cn); rt_st32(b + 20, b + 48); }
        if (tail) rt_st32(tail + 28, b); else head = b;
        tail = b;
    }
    rt_st32(res, head);
    RET(0);
}
IMP(freeaddrinfo) {
    for (uint32_t p = A(0); p;) {
        uint32_t next = rt_ld32(p + 28);
        rt_free(p);
        p = next;
    }
}

IMP(gethostbyname) { /* a hostent in a per-thread block: h_name, h_aliases, h_addrtype, h_length, h_addr_list */
    uint8_t ip[4];
    static __thread uint32_t hb;
    if (!A(0) || !resolve_name(S(0), ip)) { RET(0); return; }
    if (!hb) hb = rt_calloc(1, 512);
    memset(G(hb), 0, 512);
    rt_st32(hb, hb + 64);
    rt_st32(hb + 4, hb + 32);
    rt_st32(hb + 8, 2);
    rt_st32(hb + 12, 4);
    rt_st32(hb + 16, hb + 40);
    rt_st32(hb + 40, hb + 56);
    memcpy(G(hb + 56), ip, 4);
    snprintf((char *)G(hb + 64), 400, "%s", S(0));
    RET(hb);
}

IMP(inet_addr) { uint8_t b[4]; if (!parse_inet(S(0), b)) { RET(0xFFFFFFFFu); return; } uint32_t v; memcpy(&v, b, 4); RET(v); }
IMP(inet_aton) { uint8_t b[4]; if (!parse_inet(S(0), b)) { RET(0); return; } if (A(1)) memcpy(G(A(1)), b, 4); RET(1); }
IMP(inet_ntoa) { /* struct in_addr is passed by value in r0 */
    static __thread uint32_t buf;
    if (!buf) buf = rt_calloc(1, 32);
    uint32_t a = A(0);
    snprintf((char *)G(buf), 32, "%u.%u.%u.%u", a & 255, (a >> 8) & 255, (a >> 16) & 255, a >> 24);
    RET(buf);
}
IMP(inet_pton) { uint8_t b[4]; if (A(0) != 2) { set_errno(c, G_EAFNOSUPPORT); RET(-1); return; } if (!parse_inet(S(1), b) || strchr(S(1), 'x')) { RET(0); return; } memcpy(G(A(2)), b, 4); RET(1); }
IMP(inet_ntop) {
    if (A(0) != 2) { set_errno(c, G_EAFNOSUPPORT); RET(0); return; }
    const uint8_t *b = rt_mem + A(1);
    char t[16];
    int n = snprintf(t, sizeof t, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    if ((uint32_t)n + 1 > A(3)) { set_errno(c, 28 /* ENOSPC */); RET(0); return; }
    memcpy(G(A(2)), t, (size_t)n + 1);
    RET(A(2));
}
IMP(htons) { RET(((A(0) & 0xFF) << 8) | ((A(0) >> 8) & 0xFF)); }
IMP(htonl) { uint32_t v = A(0); RET((v << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24)); }

static void net_register(void) {
    REG(socket); REG(bind); REG(listen); REG(accept); REG(connect); REG(shutdown); REG(getsockname); REG(getpeername);
    REG(recv); REG(recvfrom); REG(send); REG(sendto); REG(setsockopt); REG(getsockopt); REG(ioctl); REG(fcntl);
    REG(select); REG(poll); REG(gethostname); REG(getaddrinfo); REG(freeaddrinfo); REG(gethostbyname);
    REG(inet_addr); REG(inet_aton); REG(inet_ntoa); REG(inet_pton); REG(inet_ntop); REG(htons); REG(htonl);
    REGAS(ntohs, imp_htons); REGAS(ntohl, imp_htonl);
}

#endif /* RT_LIBC_UNITY */
