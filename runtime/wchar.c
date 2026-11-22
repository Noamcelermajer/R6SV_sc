/* Wide-character imports: wchar_t is 4 bytes (UTF-32) on Android. This file is part of the libc.c translation unit
 * (it is #included there and uses its macros); built on its own it is empty, so it is harmless in a build list.
 *
 * Character classes follow Unicode for the scripts a game's text uses (Latin, Greek, Cyrillic, CJK, Hangul, kana,
 * fullwidth forms) and are ASCII-only elsewhere; the case maps cover ASCII, Latin-1, Latin Extended-A, Greek,
 * Cyrillic and the fullwidth letters. */
#ifdef RT_LIBC_UNITY

#define W(p, i) rt_ld32((p) + 4u * (uint32_t)(i))
#define WSET(p, i, v) rt_st32((p) + 4u * (uint32_t)(i), (v))

/* ---- classification ---------------------------------------------------------------------------------- */
static int w_isdigit(uint32_t c) { return c >= '0' && c <= '9'; }
static int w_isxdigit(uint32_t c) { return w_isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

static uint32_t w_tolower(uint32_t c) {
    if (c < 0x80) return c >= 'A' && c <= 'Z' ? c + 32 : c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if (c == 0x178) return 0xFF;
    if (c >= 0x100 && c <= 0x17F) {
        if ((c >= 0x100 && c <= 0x137 && c != 0x130 && c != 0x131) || (c >= 0x14A && c <= 0x177)) return c & 1 ? c : c + 1;
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return c & 1 ? c + 1 : c;
        return c;
    }
    if (c >= 0x391 && c <= 0x3AB && c != 0x3A2) return c + 32;
    if (c == 0x386) return 0x3AC;
    if (c >= 0x388 && c <= 0x38A) return c + 37;
    if (c == 0x38C) return 0x3CC;
    if (c == 0x38E || c == 0x38F) return c + 63;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    if (c >= 0xFF21 && c <= 0xFF3A) return c + 32;
    return c;
}
static uint32_t w_toupper(uint32_t c) {
    if (c < 0x80) return c >= 'a' && c <= 'z' ? c - 32 : c;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c == 0xFF) return 0x178;
    if (c >= 0x100 && c <= 0x17F) {
        if ((c >= 0x100 && c <= 0x137 && c != 0x130 && c != 0x131) || (c >= 0x14A && c <= 0x177)) return c & 1 ? c - 1 : c;
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return c & 1 ? c : c - 1;
        return c;
    }
    if (c >= 0x3B1 && c <= 0x3CB && c != 0x3C2) return c - 32;
    if (c == 0x3C2) return 0x3A3;
    if (c == 0x3AC) return 0x386;
    if (c >= 0x3AD && c <= 0x3AF) return c - 37;
    if (c == 0x3CC) return 0x38C;
    if (c == 0x3CD || c == 0x3CE) return c - 63;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    if (c >= 0xFF41 && c <= 0xFF5A) return c - 32;
    return c;
}
static int w_isupper(uint32_t c) { return w_tolower(c) != c; }
static int w_islower(uint32_t c) { return w_toupper(c) != c || c == 0xDF; }
static int w_isalpha(uint32_t c) {
    if (c < 0x80) return (c | 0x20) >= 'a' && (c | 0x20) <= 'z';
    return c == 0xAA || c == 0xB5 || c == 0xBA || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) ||
           (c >= 0x370 && c <= 0x3FF && c != 0x37E && c != 0x387) || (c >= 0x400 && c <= 0x481) ||
           (c >= 0x48A && c <= 0x52F) || (c >= 0x5D0 && c <= 0x5EA) || (c >= 0x620 && c <= 0x64A) ||
           (c >= 0x3041 && c <= 0x3096) || (c >= 0x30A1 && c <= 0x30FA) || (c >= 0x3400 && c <= 0x4DBF) ||
           (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xFF21 && c <= 0xFF3A) ||
           (c >= 0xFF41 && c <= 0xFF5A);
}
static int w_isspace(uint32_t c) {
    return (c >= 9 && c <= 13) || c == 32 || c == 0x1680 || (c >= 0x2000 && c <= 0x2006) || (c >= 0x2008 && c <= 0x200A) ||
           c == 0x2028 || c == 0x2029 || c == 0x205F || c == 0x3000;
}
static int w_iscntrl(uint32_t c) { return c < 0x20 || (c >= 0x7F && c <= 0x9F) || c == 0x2028 || c == 0x2029; }
static int w_isprint(uint32_t c) {
    return c >= 0x20 && c <= 0x10FFFF && !(c >= 0x7F && c <= 0x9F) && !(c >= 0xD800 && c <= 0xDFFF) && c != 0x2028 &&
           c != 0x2029 && (c & 0xFFFE) != 0xFFFE;
}
static int w_isgraph(uint32_t c) { return w_isprint(c) && !w_isspace(c); }
static int w_isalnum(uint32_t c) { return w_isalpha(c) || w_isdigit(c); }
static int w_ispunct(uint32_t c) { return w_isgraph(c) && !w_isalnum(c); }
static int w_isblank(uint32_t c) { return c == ' ' || c == '\t' || c == 0x1680 || (c >= 0x2000 && c <= 0x2006) || (c >= 0x2008 && c <= 0x200A) || c == 0x205F || c == 0x3000; }

#define WIS(name) IMP(isw##name) { RET(w_is##name(A(0))); }
WIS(alpha) WIS(digit) WIS(lower) WIS(upper) WIS(space) WIS(print) WIS(punct) WIS(xdigit) WIS(cntrl) WIS(graph)
WIS(alnum) WIS(blank)
IMP(towlower) { RET(w_tolower(A(0))); }
IMP(towupper) { RET(w_toupper(A(0))); }

/* wctype/iswctype: a class handle is a small number */
static const char *const wclass_names[] = {"alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower", "print", "punct", "space", "upper", "xdigit"};
IMP(wctype) {
    const char *n = A(0) ? S(0) : "";
    for (uint32_t k = 0; k < sizeof wclass_names / sizeof *wclass_names; k++)
        if (!strcmp(n, wclass_names[k])) { RET(k + 1); return; }
    RET(0);
}
IMP(iswctype) {
    uint32_t ch = A(0);
    switch (A(1)) {
    case 1: RET(w_isalnum(ch)); break; case 2: RET(w_isalpha(ch)); break; case 3: RET(w_isblank(ch)); break;
    case 4: RET(w_iscntrl(ch)); break; case 5: RET(w_isdigit(ch)); break; case 6: RET(w_isgraph(ch)); break;
    case 7: RET(w_islower(ch)); break; case 8: RET(w_isprint(ch)); break; case 9: RET(w_ispunct(ch)); break;
    case 10: RET(w_isspace(ch)); break; case 11: RET(w_isupper(ch)); break; case 12: RET(w_isxdigit(ch)); break;
    default: RET(0);
    }
}

/* ---- strings and memory ------------------------------------------------------------------------------ */
static uint32_t w_len(uint32_t p) { uint32_t n = 0; while (W(p, n)) n++; return n; }
IMP(wcslen) { RET(w_len(A(0))); }
IMP(wcsnlen) { uint32_t n = 0; while (n < A(1) && W(A(0), n)) n++; RET(n); }
IMP(wcscmp) {
    uint32_t a = A(0), b = A(1);
    for (;; a += 4, b += 4) {
        uint32_t x = rt_ld32(a), y = rt_ld32(b);
        if (x != y) { RET(x < y ? -1 : 1); return; }
        if (!x) { RET(0); return; }
    }
}
IMP(wcsncmp) {
    for (uint32_t k = 0; k < A(2); k++) {
        uint32_t x = W(A(0), k), y = W(A(1), k);
        if (x != y) { RET(x < y ? -1 : 1); return; }
        if (!x) break;
    }
    RET(0);
}
IMP(wcscoll) { imp_wcscmp(c); } /* the C locale collates by code point */
IMP(wcscpy) { uint32_t d = A(0), s = A(1), v; do { v = rt_ld32(s); rt_st32(d, v); d += 4; s += 4; } while (v); RET(A(0)); }
IMP(wcsncpy) { /* pads with zeros up to n, like strncpy */
    uint32_t n = A(2), k = 0, v = 1;
    for (; k < n && v; k++) { v = W(A(1), k); WSET(A(0), k, v); }
    for (; k < n; k++) WSET(A(0), k, 0);
    RET(A(0));
}
IMP(wcscat) { uint32_t d = A(0) + 4 * w_len(A(0)), s = A(1), v; do { v = rt_ld32(s); rt_st32(d, v); d += 4; s += 4; } while (v); RET(A(0)); }
IMP(wcsncat) {
    uint32_t d = A(0) + 4 * w_len(A(0)), k = 0;
    for (; k < A(2) && W(A(1), k); k++) WSET(d, k, W(A(1), k));
    WSET(d, k, 0);
    RET(A(0));
}
IMP(wcschr) { for (uint32_t k = 0;; k++) { uint32_t v = W(A(0), k); if (v == A(1)) { RET(A(0) + 4 * k); return; } if (!v) break; } RET(0); }
IMP(wcsrchr) { uint32_t r = 0; for (uint32_t k = 0;; k++) { uint32_t v = W(A(0), k); if (v == A(1)) r = A(0) + 4 * k; if (!v) break; } RET(r); }
IMP(wcsstr) {
    uint32_t nl = w_len(A(1));
    if (!nl) { RET(A(0)); return; }
    for (uint32_t k = 0; W(A(0), k); k++) {
        uint32_t j = 0;
        while (j < nl && W(A(0), k + j) == W(A(1), j)) j++;
        if (j == nl) { RET(A(0) + 4 * k); return; }
        if (!W(A(0), k + j)) break;
    }
    RET(0);
}
IMP(wcsdup) {
    uint32_t n = w_len(A(0)), p = rt_malloc(4 * (n + 1));
    if (p) memcpy(G(p), G(A(0)), 4 * (n + 1));
    RET(p);
}
IMP(wcsxfrm) {
    uint32_t n = w_len(A(1));
    if (A(2) > n) memcpy(G(A(0)), G(A(1)), 4 * (n + 1));
    RET(n);
}
IMP(wmemcpy) { memcpy(G(A(0)), G(A(1)), 4 * (size_t)A(2)); RET(A(0)); }
IMP(wmemmove) { memmove(G(A(0)), G(A(1)), 4 * (size_t)A(2)); RET(A(0)); }
IMP(wmemset) { for (uint32_t k = 0; k < A(2); k++) WSET(A(0), k, A(1)); RET(A(0)); }
IMP(wmemcmp) {
    for (uint32_t k = 0; k < A(2); k++) {
        uint32_t x = W(A(0), k), y = W(A(1), k);
        if (x != y) { RET(x < y ? -1 : 1); return; }
    }
    RET(0);
}
IMP(wmemchr) { for (uint32_t k = 0; k < A(2); k++) if (W(A(0), k) == A(1)) { RET(A(0) + 4 * k); return; } RET(0); }

/* ---- conversions to and from the multibyte (UTF-8) encoding ---------------------------------------------- */
IMP(btowc) { RET(A(0) < 0x80 ? A(0) : (uint32_t)-1); }
IMP(wctob) { RET(A(0) < 0x80 ? A(0) : (uint32_t)-1); }
IMP(mbrtowc) { /* (pwc, s, n, ps): a complete UTF-8 sequence; an incomplete one is reported as (size_t)-2 */
    if (!A(1)) { RET(0); return; }
    const uint8_t *s = (const uint8_t *)G(A(1));
    uint32_t n = A(2);
    if (!n) { RET((uint32_t)-2); return; }
    uint8_t b = s[0];
    int need = b < 0x80 ? 0 : (b >= 0xC2 && b < 0xE0) ? 1 : (b >= 0xE0 && b < 0xF0) ? 2 : (b >= 0xF0 && b < 0xF5) ? 3 : -1;
    if (need < 0) { set_errno(c, 84 /* EILSEQ */); RET((uint32_t)-1); return; }
    if ((uint32_t)need >= n) { RET((uint32_t)-2); return; }
    uint32_t cp = need ? b & (0x3Fu >> need) : b;
    for (int k = 1; k <= need; k++) {
        if ((s[k] & 0xC0) != 0x80) { set_errno(c, 84); RET((uint32_t)-1); return; }
        cp = (cp << 6) | (s[k] & 0x3Fu);
    }
    if (A(0)) rt_st32(A(0), cp);
    RET(cp ? (uint32_t)need + 1 : 0);
}
IMP(wcrtomb) { /* (s, wc, ps) */
    uint32_t cp = A(1);
    if (!A(0)) { RET(1); return; }
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { set_errno(c, 84); RET((uint32_t)-1); return; }
    uint8_t *d = (uint8_t *)G(A(0));
    if (cp < 0x80) { d[0] = (uint8_t)cp; RET(1); }
    else if (cp < 0x800) { d[0] = (uint8_t)(0xC0 | (cp >> 6)); d[1] = (uint8_t)(0x80 | (cp & 0x3F)); RET(2); }
    else if (cp < 0x10000) { d[0] = (uint8_t)(0xE0 | (cp >> 12)); d[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)); d[2] = (uint8_t)(0x80 | (cp & 0x3F)); RET(3); }
    else { d[0] = (uint8_t)(0xF0 | (cp >> 18)); d[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F)); d[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)); d[3] = (uint8_t)(0x80 | (cp & 0x3F)); RET(4); }
}

/* narrow text (the C-locale view) of a wide string, for the conversion functions that go through the host */
static size_t w_narrow(uint32_t p, char *out, size_t cap) {
    size_t k = 0;
    for (; W(p, k) && k + 1 < cap; k++) out[k] = W(p, k) < 0x80 ? (char)W(p, k) : '?';
    out[k] = 0;
    return k;
}
static void w_endptr(cpu_t *c, uint32_t endp, uint32_t base, size_t consumed) { if (endp) rt_st32(endp, base + 4 * (uint32_t)consumed); }
IMP(wcstol) {
    char buf[256], *e;
    w_narrow(A(0), buf, sizeof buf);
    long long v = strtoll(buf, &e, (int)A(2));
    w_endptr(c, A(1), A(0), (size_t)(e - buf));
    RET(v > 0x7FFFFFFFLL ? 0x7FFFFFFF : v < -0x80000000LL ? 0x80000000u : (uint32_t)v);
}
IMP(wcstoul) {
    char buf[256], *e;
    w_narrow(A(0), buf, sizeof buf);
    unsigned long long v = strtoull(buf, &e, (int)A(2));
    w_endptr(c, A(1), A(0), (size_t)(e - buf));
    RET(v > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)v);
}
IMP(wcstod) {
    char buf[256], *e;
    w_narrow(A(0), buf, sizeof buf);
    double v = strtod(buf, &e);
    w_endptr(c, A(1), A(0), (size_t)(e - buf));
    rt_dret(c, v);
}

/* ---- formatting ---------------------------------------------------------------------------------------- */
IMP(swprintf) { /* (buf, n, fmt, ...): the number of wide characters stored, or -1 when n was too small */
    rt_args a = {c, 3, 0};
    int n = rt_vformat_w(A(0), A(1), A(2), &a);
    RET(n < 0 || (uint32_t)n >= A(1) ? (uint32_t)-1 : (uint32_t)n);
}
IMP(vswprintf) { /* (buf, n, fmt, va_list) */
    rt_args a = {c, 0, A(3)};
    int n = rt_vformat_w(A(0), A(1), A(2), &a);
    RET(n < 0 || (uint32_t)n >= A(1) ? (uint32_t)-1 : (uint32_t)n);
}
IMP(wcsftime) {
    struct tm t;
    tm_from_guest(&t, A(3));
    char fmt[512], out[1024];
    w_narrow(A(2), fmt, sizeof fmt);
    size_t n = strftime(out, sizeof out, fmt, &t);
    if (n >= A(1)) { RET(0); return; }
    for (size_t j = 0; j <= n; j++) WSET(A(0), j, (uint8_t)out[j]);
    RET(n);
}

static void wchar_register(void) {
    REG(iswalpha); REG(iswdigit); REG(iswlower); REG(iswupper); REG(iswspace); REG(iswprint); REG(iswpunct);
    REG(iswxdigit); REG(iswcntrl); REG(iswgraph); REG(iswalnum); REG(iswblank); REG(towlower); REG(towupper);
    REG(wctype); REG(iswctype); REG(wcslen); REG(wcsnlen); REG(wcscmp); REG(wcsncmp); REG(wcscoll); REG(wcscpy);
    REG(wcsncpy); REG(wcscat); REG(wcsncat); REG(wcschr); REG(wcsrchr); REG(wcsstr); REG(wcsdup); REG(wcsxfrm);
    REG(wmemcpy); REG(wmemmove); REG(wmemset); REG(wmemcmp); REG(wmemchr); REG(btowc); REG(wctob); REG(mbrtowc);
    REG(wcrtomb); REG(wcstol); REG(wcstoul); REG(wcstod); REG(swprintf); REG(vswprintf); REG(wcsftime);
}

#endif /* RT_LIBC_UNITY */
