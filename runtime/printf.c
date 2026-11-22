/* Guest printf and scanf for the runtime. The game's printf-family calls pass their format and arguments in
 * the emulated CPU state (r0-r3, then the stack) or in an ARM va_list in game memory. The arguments are fetched
 * here with the ARM softfp rules (64-bit values and doubles start on an even word), the conversions are done
 * by the host snprintf and strtod, and strings are game addresses read from or written to game memory.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"

#define uc(ch) ((unsigned char)(ch))

/* ---- arguments ---------------------------------------------------------------------------------- */

uint32_t rt_args_u32(rt_args *a) {
    if (a->va) {
        uint32_t v = rt_ld32(a->va);
        a->va += 4;
        return v;
    }
    int k = a->next++;
    if (k < 4) return a->c->r[k];
    return rt_ld32(a->c->r[13] + 4u * (uint32_t)(k - 4));
}

uint64_t rt_args_u64(rt_args *a) {
    uint32_t lo, hi;
    if (a->va) {
        a->va = (a->va + 7) & ~7u;
        lo = rt_ld32(a->va);
        hi = rt_ld32(a->va + 4);
        a->va += 8;
    } else {
        a->next = (a->next + 1) & ~1;
        lo = rt_args_u32(a);
        hi = rt_args_u32(a);
    }
    return ((uint64_t)hi << 32) | lo;
}

double rt_args_double(rt_args *a) {
    uint64_t bits = rt_args_u64(a);
    double d;
    memcpy(&d, &bits, 8);
    return d;
}

/* ---- shared helpers ----------------------------------------------------------------------------- */

static int parse_num(const char **p) {
    int v = 0;
    while (isdigit(uc(**p))) {
        if (v < (1 << 24)) v = v * 10 + (**p - '0');
        (*p)++;
    }
    return v;
}

/* Length modifier: 0 none, 1 hh, 2 h, 3 l/z/t (32 bits here), 4 ll/q/j (64 bits), 5 L (a double on ARM). */
static int parse_len(const char **p) {
    switch (**p) {
    case 'h': (*p)++; if (**p == 'h') { (*p)++; return 1; } return 2;
    case 'l': (*p)++; if (**p == 'l') { (*p)++; return 4; } return 3;
    case 'L': (*p)++; return 5;
    case 'q': case 'j': (*p)++; return 4;
    case 'z': case 't': (*p)++; return 3;
    default: return 0;
    }
}

/* Bytes an integer of this length modifier occupies in game memory (little-endian host). */
static int int_size(int lenmod) {
    return lenmod == 1 ? 1 : lenmod == 2 ? 2 : (lenmod == 4 || lenmod == 5) ? 8 : 4;
}

static int digit_val(int ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    ch |= 0x20;
    return ch >= 'a' && ch <= 'z' ? ch - 'a' + 10 : -1;
}

static int utf8_enc(uint32_t cp, char *b) {
    if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) cp = 0xFFFDu;
    if (cp < 0x80u) { b[0] = (char)cp; return 1; }
    if (cp < 0x800u) {
        b[0] = (char)(0xC0u | (cp >> 6));
        b[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        b[0] = (char)(0xE0u | (cp >> 12));
        b[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        b[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    b[0] = (char)(0xF0u | (cp >> 18));
    b[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    b[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    b[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

/* ---- formatting --------------------------------------------------------------------------------- */

/* Output sink: counts every byte produced, keeps the first cap-1 of them. */
typedef struct { char *out; size_t cap, len; } sink;

static void put(sink *s, const char *p, size_t n) {
    size_t room = s->cap > s->len + 1 ? s->cap - s->len - 1 : 0;
    size_t k = n < room ? n : room;
    if (k) memcpy(s->out + s->len, p, k);
    s->len += n;
}

static void fill(sink *s, char ch, size_t n) {
    while (n--) put(s, &ch, 1);
}

static void put_pad(sink *s, const char *p, size_t n, int width, int left) {
    size_t pad = width > 0 && (size_t)width > n ? (size_t)width - n : 0;
    if (!left) fill(s, ' ', pad);
    put(s, p, n);
    if (left) fill(s, ' ', pad);
}

/* Host conversions: build the host format from the guest's flags, width and precision. */
static void mkfmt(char *hf, size_t hsz, const char *flags, int w, int p, const char *len, char conv) {
    int n = snprintf(hf, hsz, "%%%s", flags);
    if (w >= 0) n += snprintf(hf + n, hsz - (size_t)n, "%d", w);
    if (p >= 0) n += snprintf(hf + n, hsz - (size_t)n, ".%d", p);
    snprintf(hf + n, hsz - (size_t)n, "%s%c", len, conv);
}

static void put_ll(sink *s, const char *hf, long long v) {
    int n = snprintf(NULL, 0, hf, v);
    char *t = n > 0 ? malloc((size_t)n + 1) : NULL;
    if (!t) return;
    snprintf(t, (size_t)n + 1, hf, v);
    put(s, t, (size_t)n);
    free(t);
}

static void put_ull(sink *s, const char *hf, unsigned long long v) {
    int n = snprintf(NULL, 0, hf, v);
    char *t = n > 0 ? malloc((size_t)n + 1) : NULL;
    if (!t) return;
    snprintf(t, (size_t)n + 1, hf, v);
    put(s, t, (size_t)n);
    free(t);
}

static void put_dbl(sink *s, const char *hf, double v) {
    int n = snprintf(NULL, 0, hf, v);
    char *t = n > 0 ? malloc((size_t)n + 1) : NULL;
    if (!t) return;
    snprintf(t, (size_t)n + 1, hf, v);
    put(s, t, (size_t)n);
    free(t);
}

/* %s and %ls-style strings. A NULL argument prints "(null)". */
static void put_narrow(sink *s, uint32_t addr, int prec, int width, int left) {
    const char *str = "(null)";
    size_t n = 6;
    if (addr) {
        str = (const char *)rt_mem + addr;
        n = 0;
        while ((prec < 0 || n < (size_t)prec) && str[n]) n++;
    } else if (prec >= 0 && prec < 6) {
        n = (size_t)prec;
    }
    put_pad(s, str, n, width, left);
}

/* Wide string of UTF-32 code units, written as UTF-8. The precision limits the output bytes. */
static void put_wide(sink *s, uint32_t addr, int prec, int width, int left) {
    char b[4];
    size_t n = 0, pad;
    if (!addr) {
        put_pad(s, "(null)", prec >= 0 && prec < 6 ? (size_t)prec : 6, width, left);
        return;
    }
    for (uint32_t i = 0; rt_ld32(addr + 4 * i) != 0; i++) {
        size_t l = (size_t)utf8_enc(rt_ld32(addr + 4 * i), b);
        if (prec >= 0 && n + l > (size_t)prec) break;
        n += l;
    }
    pad = width > 0 && (size_t)width > n ? (size_t)width - n : 0;
    if (!left) fill(s, ' ', pad);
    for (uint32_t i = 0, got = 0; got < n; i++) {
        size_t l = (size_t)utf8_enc(rt_ld32(addr + 4 * i), b);
        put(s, b, l);
        got += l;
    }
    if (left) fill(s, ' ', pad);
}

int rt_vformat(char *out, size_t cap, const char *fmt, rt_args *a) {
    sink s = { out, cap, 0 };
    const char *p = fmt;
    while (*p) {
        if (*p != '%') {
            const char *q = p;
            while (*q && *q != '%') q++;
            put(&s, p, (size_t)(q - p));
            p = q;
            continue;
        }
        p++;

        char flags[8];
        int nf = 0, width = -1, prec = -1;
        while (*p && strchr("-+ #0", *p)) {
            if (nf < 7) flags[nf++] = *p;
            p++;
        }
        if (*p == '*') {
            int w = (int)rt_args_u32(a);
            p++;
            if (w < 0) {
                w = -w;
                if (nf < 7) flags[nf++] = '-';
            }
            width = w;
        } else if (isdigit(uc(*p))) {
            width = parse_num(&p);
        }
        if (*p == '.') {
            p++;
            if (*p == '*') {
                int pr = (int)rt_args_u32(a);
                p++;
                prec = pr < 0 ? -1 : pr;
            } else {
                prec = parse_num(&p);
            }
        }
        flags[nf] = 0;
        int lenmod = parse_len(&p);
        int is64 = lenmod == 4 || lenmod == 5;
        int left = strchr(flags, '-') != NULL;

        char c = *p;
        if (c) p++;
        char hf[64];
        switch (c) {
        case '%':
            put(&s, "%", 1);
            break;
        case 'd': case 'i': {
            long long v = is64 ? (long long)rt_args_u64(a) : (long long)(int32_t)rt_args_u32(a);
            if (lenmod == 1) v = (signed char)v;
            else if (lenmod == 2) v = (short)v;
            mkfmt(hf, sizeof hf, flags, width, prec, "ll", 'd');
            put_ll(&s, hf, v);
            break;
        }
        case 'u': case 'o': case 'x': case 'X': {
            unsigned long long v = is64 ? rt_args_u64(a) : rt_args_u32(a);
            if (lenmod == 1) v = (unsigned char)v;
            else if (lenmod == 2) v = (unsigned short)v;
            mkfmt(hf, sizeof hf, flags, width, prec, "ll", c);
            put_ull(&s, hf, v);
            break;
        }
        case 'c': {
            uint32_t v = rt_args_u32(a);
            if (lenmod == 3) {
                char b[4];
                size_t n = (size_t)utf8_enc(v, b);
                put_pad(&s, b, n, width, left);
            } else {
                char b = (char)uc(v);
                put_pad(&s, &b, 1, width, left);
            }
            break;
        }
        case 's':
            if (lenmod == 3) put_wide(&s, rt_args_u32(a), prec, width, left);
            else put_narrow(&s, rt_args_u32(a), prec, width, left);
            break;
        case 'S':
            put_wide(&s, rt_args_u32(a), prec, width, left);
            break;
        case 'p': {
            char b[16];
            int n = snprintf(b, sizeof b, "0x%x", (unsigned)rt_args_u32(a));
            put_pad(&s, b, (size_t)n, width, left);
            break;
        }
        case 'n': {
            uint32_t addr = rt_args_u32(a);
            if (addr) {
                if (lenmod == 1) {
                    rt_mem[addr] = (uint8_t)s.len;
                } else if (lenmod == 2) {
                    uint16_t v = (uint16_t)s.len;
                    memcpy(rt_mem + addr, &v, 2);
                } else if (is64) {
                    uint64_t v = s.len;
                    memcpy(rt_mem + addr, &v, 8);
                } else {
                    rt_st32(addr, (uint32_t)s.len);
                }
            }
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double v = rt_args_double(a);
            mkfmt(hf, sizeof hf, flags, width, prec, "", c);
            put_dbl(&s, hf, v);
            break;
        }
        default: /* unknown conversion: copy it through */
            put(&s, "%", 1);
            if (c) put(&s, &c, 1);
            break;
        }
    }
    if (cap) out[s.len < cap ? s.len : cap - 1] = 0;
    return (int)s.len;
}

/* ---- scanning ----------------------------------------------------------------------------------- */

static void store_bytes(uint32_t addr, unsigned long long v, int size) {
    memcpy(rt_mem + addr, &v, (size_t)size); /* little-endian host, as the rest of the runtime */
}

int rt_vscan(const char *in, const char *fmt, rt_args *a) {
    const char *ip = in, *f = fmt;
    int count = 0;
    while (*f) {
        if (isspace(uc(*f))) {
            while (isspace(uc(*f))) f++;
            while (isspace(uc(*ip))) ip++;
            continue;
        }
        if (*f != '%') {
            if (!*ip) return count ? count : -1;
            if (*ip != *f) return count;
            ip++;
            f++;
            continue;
        }
        f++;
        if (*f == '%') {
            while (isspace(uc(*ip))) ip++;
            if (!*ip) return count ? count : -1;
            if (*ip != '%') return count;
            ip++;
            f++;
            continue;
        }

        int suppress = *f == '*';
        if (suppress) f++;
        size_t wid = isdigit(uc(*f)) ? (size_t)parse_num(&f) : 0;
        size_t lim = wid ? wid : (size_t)-1;
        int lenmod = parse_len(&f);
        char c = *f;
        if (!c) return count;
        f++;

        if (c != 'c' && c != '[' && c != 'n') {
            while (isspace(uc(*ip))) ip++;
        }
        if (c != 'n' && !*ip) return count ? count : -1;
        uint32_t dst = suppress ? 0 : rt_args_u32(a);
        size_t k = 0;

        switch (c) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
            int base = c == 'd' || c == 'u' ? 10 : c == 'o' ? 8 : c == 'i' ? 0 : 16;
            int neg = 0, d;
            unsigned long long v = 0;
            size_t nd = 0;
            if (k < lim && (ip[k] == '+' || ip[k] == '-')) neg = ip[k++] == '-';
            if ((base == 0 || base == 16) && k + 2 < lim && ip[k] == '0' && (ip[k + 1] | 0x20) == 'x'
                && isxdigit(uc(ip[k + 2]))) {
                k += 2;
                base = 16;
            } else if (base == 0) {
                base = ip[k] == '0' ? 8 : 10;
            }
            while (k < lim && ip[k] && (d = digit_val(uc(ip[k]))) >= 0 && d < base) {
                v = v * (unsigned)base + (unsigned)d;
                nd++;
                k++;
            }
            if (!nd) return count;
            if (neg) v = 0 - v;
            if (!suppress) {
                store_bytes(dst, v, int_size(lenmod));
                count++;
            }
            ip += k;
            break;
        }
        case 'f': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            int hex = 0;
            size_t nd = 0, e;
            if (k < lim && (ip[k] == '+' || ip[k] == '-')) k++;
            if (k + 1 < lim && ip[k] == '0' && (ip[k + 1] | 0x20) == 'x') {
                hex = 1;
                k += 2;
            }
            while (k < lim && ip[k] && (hex ? isxdigit(uc(ip[k])) : isdigit(uc(ip[k])))) { k++; nd++; }
            if (k < lim && ip[k] == '.') {
                k++;
                while (k < lim && ip[k] && (hex ? isxdigit(uc(ip[k])) : isdigit(uc(ip[k])))) { k++; nd++; }
            }
            if (!nd) return count;
            if (k < lim && ip[k] && (ip[k] | 0x20) == (hex ? 'p' : 'e')) {
                e = k + 1;
                if (e < lim && (ip[e] == '+' || ip[e] == '-')) e++;
                if (e < lim && isdigit(uc(ip[e]))) {
                    while (e < lim && isdigit(uc(ip[e]))) e++;
                    k = e;
                }
            }
            char *buf = malloc(k + 1);
            if (!buf) return count;
            memcpy(buf, ip, k);
            buf[k] = 0;
            double v = strtod(buf, NULL);
            free(buf);
            ip += k;
            if (!suppress) {
                if (lenmod == 3 || lenmod == 4 || lenmod == 5) {
                    memcpy(rt_mem + dst, &v, 8);
                } else {
                    float fv = (float)v;
                    memcpy(rt_mem + dst, &fv, 4);
                }
                count++;
            }
            break;
        }
        case 's': {
            while (k < lim && ip[k] && !isspace(uc(ip[k]))) k++;
            if (!k) return count;
            if (!suppress) {
                memcpy(rt_mem + dst, ip, k);
                rt_mem[dst + k] = 0;
                count++;
            }
            ip += k;
            break;
        }
        case 'c': {
            size_t n = wid ? wid : 1;
            while (k < n && ip[k]) k++;
            if (!suppress) {
                memcpy(rt_mem + dst, ip, k);
                count++;
            }
            ip += k;
            break;
        }
        case '[': {
            unsigned char set[256];
            int neg = *f == '^';
            if (neg) f++;
            memset(set, 0, sizeof set);
            if (*f == ']') {
                set[uc(']')] = 1;
                f++;
            }
            while (*f && *f != ']') {
                if (f[1] == '-' && f[2] && f[2] != ']') {
                    for (int ch = uc(f[0]); ch <= uc(f[2]); ch++) set[ch] = 1;
                    f += 3;
                } else {
                    set[uc(*f)] = 1;
                    f++;
                }
            }
            if (*f == ']') f++;
            while (k < lim && ip[k] && ((set[uc(ip[k])] != 0) != neg)) k++;
            if (!k) return count;
            if (!suppress) {
                memcpy(rt_mem + dst, ip, k);
                rt_mem[dst + k] = 0;
                count++;
            }
            ip += k;
            break;
        }
        case 'n':
            if (!suppress) store_bytes(dst, (unsigned long long)(ip - in), int_size(lenmod));
            break;
        default:
            return count;
        }
    }
    return count;
}

/* ---- wide formatting (swprintf) ------------------------------------------------------------------
 * The wide format (UTF-32 in game memory) is converted to UTF-8 and run through rt_vformat, whose %s, %ls, %c
 * and %lc already print UTF-8; the result is converted back to UTF-32. Width and precision therefore count
 * bytes, not characters, for non-ASCII text (identical for ASCII). A byte that is not valid UTF-8 (a narrow %s
 * argument holding Latin-1) becomes the code point of the same value. */
static uint32_t utf8_dec(const uint8_t *s, size_t n, int *used) {
    uint8_t b = s[0];
    int need = (b >= 0xC2 && b < 0xE0) ? 1 : (b >= 0xE0 && b < 0xF0) ? 2 : (b >= 0xF0 && b < 0xF5) ? 3 : 0;
    if (!need || (size_t)need >= n) { *used = 1; return b; }
    uint32_t cp = b & (0x3Fu >> need);
    for (int k = 1; k <= need; k++) {
        if ((s[k] & 0xC0) != 0x80) { *used = 1; return b; }
        cp = (cp << 6) | (s[k] & 0x3Fu);
    }
    *used = need + 1;
    return cp;
}

/* Returns the number of characters swprintf would produce; writes at most cap-1 characters and a terminator.
 * (The caller turns a result >= cap into -1, as swprintf does.) */
int rt_vformat_w(uint32_t dst, uint32_t cap, uint32_t wfmt, rt_args *a) {
    size_t fl = 0, k = 0;
    while (rt_ld32(wfmt + 4 * (uint32_t)fl)) fl++;
    char *fmt = malloc(fl * 4 + 1);
    if (!fmt) return -1;
    for (uint32_t i = 0; i < fl; i++) k += (size_t)utf8_enc(rt_ld32(wfmt + 4 * i), fmt + k);
    fmt[k] = 0;
    rt_args first = *a;
    int n = rt_vformat(NULL, 0, fmt, a);
    char *buf = malloc((size_t)n + 1);
    if (!buf) { free(fmt); return -1; }
    rt_vformat(buf, (size_t)n + 1, fmt, &first);
    free(fmt);
    uint32_t chars = 0;
    for (size_t i = 0; i < (size_t)n;) {
        int used;
        uint32_t cp = utf8_dec((const uint8_t *)buf + i, (size_t)n - i, &used);
        i += (size_t)used;
        if (cap && chars + 1 < cap) rt_st32(dst + 4 * chars, cp);
        chars++;
    }
    if (cap) rt_st32(dst + 4 * (chars < cap ? chars : cap - 1), 0);
    free(buf);
    return (int)chars;
}
