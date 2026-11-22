"""Deterministic models of the library's imports, used by the differential test on both sides.

Unicorn (the original code) and the host DLL (the translation) call the same model functions through a
`Machine`, so an import behaves identically on both sides and the comparison tests only the translation.
The models follow the ARM softfp ABI: integer and pointer arguments in r0-r3 then the stack, doubles in
an even/odd register pair, results in r0 (r0:r1 for a double). They are not the bionic implementations
(math results may differ from bionic in the last bit), only stand-ins that are the same on both sides.
"""
from __future__ import annotations

import math
import struct

HEAP, HEAP_SIZE = 0x05000000, 0x100000
ERRNO = HEAP + HEAP_SIZE - 0x10       # __errno() cell
TLS = HEAP + HEAP_SIZE - 0x1000       # __kuser_get_tls() block
STACK_GUARD = 0x5AFE6A2D


class Unmodelled(Exception):
    pass


class Machine:
    """Register and memory access for one side of the comparison. Subclasses implement the five primitives."""

    def reg(self, k: int) -> int: ...
    def set_reg(self, k: int, v: int): ...
    def read(self, a: int, n: int) -> bytes: ...
    def write(self, a: int, data: bytes): ...

    def __init__(self, writable=()):
        self.writable = list(writable)  # (start, end): a model may write only here, like code on the device
        self.reset()

    def store(self, a: int, data: bytes):
        if self.writable and not any(s <= a and a + len(data) <= e for s, e in self.writable):
            raise Unmodelled(f"write to read-only memory at {a:#x}")
        self.write(a, data)

    def reset(self):
        self.brk = HEAP
        self.rand = 0x1234ABCD330E

    # helpers
    def u32(self, a):
        return int.from_bytes(self.read(a, 4), "little")

    def arg(self, k):
        return self.reg(k) if k < 4 else self.u32(self.reg(13) + 4 * (k - 4))

    def darg(self, k):
        """Double in argument registers k, k+1 (k even)."""
        lo, hi = self.arg(k), self.arg(k + 1)
        return struct.unpack("<d", struct.pack("<II", lo, hi))[0]

    def arg64(self, k):
        """64-bit integer in argument registers k, k+1 (k even), low word first."""
        return self.arg(k) | (self.arg(k + 1) << 32)

    def ret(self, v):
        self.set_reg(0, v & 0xFFFFFFFF)

    def ret64(self, v, k=0):
        """A 64-bit integer into registers k, k+1 (k even)."""
        self.set_reg(k, v & 0xFFFFFFFF)
        self.set_reg(k + 1, (v >> 32) & 0xFFFFFFFF)

    def dret(self, x):
        lo, hi = struct.unpack("<II", struct.pack("<d", x))
        self.set_reg(0, lo)
        self.set_reg(1, hi)

    def cstr(self, a, limit=1 << 16):
        out = bytearray()
        while len(out) < limit:
            b = self.read(a + len(out), 1)
            if b == b"\0":
                return bytes(out)
            out += b
        raise Unmodelled("unterminated string")

    def alloc(self, n):
        n = (n + 7) & ~7
        if self.brk + 8 + n > HEAP + HEAP_SIZE - 0x1000:
            return 0
        self.write(self.brk, n.to_bytes(4, "little") + b"\0\0\0\0")
        p = self.brk + 8
        self.brk = p + n
        return p


def _memcpy(m):
    d, s, n = m.arg(0), m.arg(1), m.arg(2)
    if n:
        m.store(d, m.read(s, n))
    m.ret(d)


def _memset(m):
    d, c, n = m.arg(0), m.arg(1), m.arg(2)
    if n:
        m.store(d, bytes([c & 0xFF]) * n)
    m.ret(d)


def _memcmp(m):
    a, b, n = m.read(m.arg(0), m.arg(2)), m.read(m.arg(1), m.arg(2)), m.arg(2)
    for x, y in zip(a, b):
        if x != y:
            return m.ret(x - y)
    m.ret(0)


def _memchr(m):
    p, c, n = m.arg(0), m.arg(1) & 0xFF, m.arg(2)
    j = m.read(p, n).find(bytes([c])) if n else -1
    m.ret(p + j if j >= 0 else 0)


def _strlen(m):
    m.ret(len(m.cstr(m.arg(0))))


def _strcmp(m):
    a, b = m.cstr(m.arg(0)) + b"\0", m.cstr(m.arg(1)) + b"\0"
    for x, y in zip(a, b):
        if x != y:
            return m.ret(x - y)
    m.ret(0)


def _strncmp(m):
    n = m.arg(2)
    a, b = (m.cstr(m.arg(0)) + b"\0")[:n], (m.cstr(m.arg(1)) + b"\0")[:n]
    for x, y in zip(a, b):
        if x != y:
            return m.ret(x - y)
        if x == 0:
            break
    m.ret(0)


def _strcasecmp(m, n=None):
    a, b = (m.cstr(m.arg(0)) + b"\0").lower(), (m.cstr(m.arg(1)) + b"\0").lower()
    if n is not None:
        a, b = a[:n], b[:n]
    for x, y in zip(a, b):
        if x != y:
            return m.ret(x - y)
        if x == 0:
            break
    m.ret(0)


def _strcpy(m):
    d = m.arg(0)
    m.store(d, m.cstr(m.arg(1)) + b"\0")
    m.ret(d)


def _strncpy(m):
    d, n = m.arg(0), m.arg(2)
    s = m.cstr(m.arg(1))[:n]
    m.store(d, s + b"\0" * (n - len(s)))
    m.ret(d)


def _strcat(m):
    d = m.arg(0)
    m.store(d + len(m.cstr(d)), m.cstr(m.arg(1)) + b"\0")
    m.ret(d)


def _strchr(m, last=False):
    p, c = m.arg(0), m.arg(1) & 0xFF
    s = m.cstr(p) + b"\0"
    j = s.rfind(bytes([c])) if last else s.find(bytes([c]))
    m.ret(p + j if j >= 0 else 0)


def _strstr(m):
    p = m.arg(0)
    j = m.cstr(p).find(m.cstr(m.arg(1)))
    m.ret(p + j if j >= 0 else 0)


def _strdup(m):
    s = m.cstr(m.arg(0)) + b"\0"
    p = m.alloc(len(s))
    if p:
        m.store(p, s)
    m.ret(p)


def _malloc(m):
    m.ret(m.alloc(m.arg(0)))


def _realloc(m):
    old, n = m.arg(0), m.arg(1)
    p = m.alloc(n)
    if p and old:
        oldn = m.u32(old - 8)
        m.store(p, m.read(old, min(oldn, n)))
    m.ret(p)


def _math1(fn):
    def model(m):
        try:
            m.dret(fn(m.darg(0)))
        except (ValueError, OverflowError):
            m.dret(float("nan"))
    return model


def _math2(fn):
    def model(m):
        try:
            m.dret(fn(m.darg(0), m.darg(2)))
        except (ValueError, OverflowError, ZeroDivisionError):
            m.dret(float("nan"))
    return model


def _ldexp(m):
    e = m.arg(2)
    try:
        m.dret(math.ldexp(m.darg(0), e - (1 << 32) if e & 0x80000000 else e))
    except OverflowError:
        m.dret(math.copysign(math.inf, m.darg(0)))


def _lrand48(m):
    m.rand = (m.rand * 0x5DEECE66D + 0xB) & ((1 << 48) - 1)
    m.ret(m.rand >> 17)


def _srand48(m):
    m.rand = ((m.arg(0) << 16) | 0x330E) & ((1 << 48) - 1)


def _ctype(pred):
    def model(m):
        c = m.arg(0)
        m.ret(1 if c < 128 and pred(chr(c)) else 0)
    return model


def _gettimeofday(m):
    tv = m.arg(0)
    if tv:
        m.store(tv, (1382600000).to_bytes(4, "little") + (500000).to_bytes(4, "little"))
    m.ret(0)


def _const(v):
    return lambda m: m.ret(v)


def _atoi(m):
    s = m.cstr(m.arg(0)).lstrip(b" \t\n\v\f\r")
    j = 1 if s[:1] in (b"+", b"-") else 0
    while j < len(s) and 48 <= s[j] <= 57:
        j += 1
    digits = s[:j]
    m.ret(int(digits) if digits not in (b"", b"+", b"-") else 0)


def _abort(m):
    raise Unmodelled("abort")


MODELS = {
    "memcpy": _memcpy, "memmove": _memcpy, "memset": _memset, "memcmp": _memcmp, "memchr": _memchr,
    "strlen": _strlen, "strcmp": _strcmp, "strncmp": _strncmp, "strcasecmp": _strcasecmp,
    "strncasecmp": lambda m: _strcasecmp(m, m.arg(2)), "strcpy": _strcpy, "strncpy": _strncpy, "strcat": _strcat,
    "strchr": _strchr, "strrchr": lambda m: _strchr(m, True), "strstr": _strstr, "strdup": _strdup,
    "malloc": _malloc, "realloc": _realloc, "free": _const(0),
    "sqrt": _math1(math.sqrt), "sin": _math1(math.sin), "cos": _math1(math.cos), "tan": _math1(math.tan),
    "asin": _math1(math.asin), "acos": _math1(math.acos), "atan": _math1(math.atan), "exp": _math1(math.exp),
    "log": _math1(math.log), "log10": _math1(math.log10), "floor": _math1(math.floor), "ceil": _math1(math.ceil),
    "sinh": _math1(math.sinh), "cosh": _math1(math.cosh), "tanh": _math1(math.tanh),
    "atan2": _math2(math.atan2), "pow": _math2(math.pow), "fmod": _math2(math.fmod), "ldexp": _ldexp,
    "lrand48": _lrand48, "srand48": _srand48,
    "isalpha": _ctype(str.isalpha), "isalnum": _ctype(str.isalnum), "isspace": _ctype(lambda c: c in " \t\n\v\f\r"),
    "isupper": _ctype(str.isupper), "islower": _ctype(str.islower), "isxdigit": _ctype(lambda c: c in "0123456789abcdefABCDEF"),
    "toupper": lambda m: m.ret(m.arg(0) - 32 if 97 <= m.arg(0) <= 122 else m.arg(0)),
    "__errno": _const(ERRNO), "pthread_self": _const(0x00001000),
    "pthread_mutex_lock": _const(0), "pthread_mutex_unlock": _const(0), "pthread_mutex_trylock": _const(0),
    "pthread_mutex_init": _const(0), "pthread_mutex_destroy": _const(0), "pthread_cond_signal": _const(0),
    "pthread_cond_broadcast": _const(0), "sched_yield": _const(0), "usleep": _const(0),
    "pthread_mutexattr_init": _const(0), "pthread_mutexattr_settype": _const(0),
    "pthread_mutexattr_destroy": _const(0), "pthread_cond_init": _const(0), "pthread_cond_destroy": _const(0),
    "atoi": _atoi, "atol": _atoi,
    "__android_log_print": _const(0), "__cxa_atexit": _const(0), "time": _const(1382600000),
    "gettimeofday": _gettimeofday, "clock": _const(123456), "abort": _abort, "__stack_chk_fail": _abort,
}


# ---- Android/ARM EABI additions (Rainbow Six: Shadow Vanguard) ----------------------------------------------------
# The library imports the libgcc integer division and double helpers (__aeabi_*), single-precision math, the C++
# allocation operators, printf-style formatting and the wide-character functions. Every model is a pure function of
# the arguments and guest memory, so both sides of the comparison compute exactly the same thing.
def _s32(x):
    return x - (1 << 32) if x & 0x80000000 else x


def _s64(x):
    return x - (1 << 64) if x & (1 << 63) else x


def _tdiv(a, b):
    """Integer division truncating toward zero (C semantics); a division by zero gives 0 as libgcc's __div0 path does."""
    if b == 0:
        return 0
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def _idiv(signed, wide, both):
    """__aeabi_idiv/uidiv (r0 = quotient), __aeabi_idivmod/uidivmod (r0 = quotient, r1 = remainder) and the 64-bit
    ldivmod/uldivmod (r0:r1 = quotient, r2:r3 = remainder). The remainder is a - q*b (a, when b is 0)."""
    def model(m):
        if wide:
            a, b = m.arg64(0), m.arg64(2)
            if signed:
                a, b = _s64(a), _s64(b)
        else:
            a, b = m.arg(0), m.arg(1)
            if signed:
                a, b = _s32(a), _s32(b)
        q = _tdiv(a, b)
        r = a - q * b
        if wide:
            m.ret64(q, 0)
            m.ret64(r, 2)
        else:
            m.ret(q)
            if both:
                m.set_reg(1, r & 0xFFFFFFFF)
    return model


def _dadd(m):
    m.dret(m.darg(0) + m.darg(2))


def _dsub(m):
    m.dret(m.darg(0) - m.darg(2))


def _drsub(m):
    m.dret(m.darg(2) - m.darg(0))


def _dmul(m):
    m.dret(m.darg(0) * m.darg(2))


def _ddiv(m):
    a, b = m.darg(0), m.darg(2)
    if b == 0.0:
        if a != a or a == 0.0:
            m.dret(float("nan"))
        else:
            neg = (a < 0) != (math.copysign(1.0, b) < 0)
            m.dret(-math.inf if neg else math.inf)
    else:
        m.dret(a / b)


def _d2int(bits, signed):
    """double to integer, truncating, saturating at the range ends, NaN to 0 (libgcc on ARM)."""
    def model(m):
        x = m.darg(0)
        hi = (1 << (bits - 1)) - 1 if signed else (1 << bits) - 1
        lo = -(1 << (bits - 1)) if signed else 0
        if x != x:
            v = 0
        elif x == math.inf:
            v = hi
        elif x == -math.inf:
            v = lo
        else:
            v = max(lo, min(hi, int(x)))
        if bits == 64:
            m.ret64(v & ((1 << 64) - 1))
        else:
            m.ret(v)
    return model


def _i2d(signed, wide):
    def model(m):
        v = m.arg64(0) if wide else m.arg(0)
        if signed:
            v = _s64(v) if wide else _s32(v)
        m.dret(float(v))
    return model


def _f32bits(x):
    try:
        return struct.unpack("<I", struct.pack("<f", x))[0]
    except OverflowError:
        return 0xFF800000 if x < 0 else 0x7F800000


def _farg(m, k):
    return struct.unpack("<f", m.arg(k).to_bytes(4, "little"))[0]


def _fmath1(fn):
    def model(m):
        try:
            y = fn(_farg(m, 0))
        except (ValueError, OverflowError):
            y = float("nan")
        m.ret(_f32bits(float(y)))
    return model


def _fmath2(fn):
    def model(m):
        try:
            y = fn(_farg(m, 0), _farg(m, 1))
        except (ValueError, OverflowError, ZeroDivisionError):
            y = float("nan")
        m.ret(_f32bits(float(y)))
    return model


def _ldexpf(m):
    e = _s32(m.arg(1))
    try:
        y = math.ldexp(_farg(m, 0), max(-4000, min(4000, e)))
    except OverflowError:
        y = math.copysign(math.inf, _farg(m, 0))
    m.ret(_f32bits(y))


def _f2d(m):
    m.dret(_farg(m, 0))


def _d2f(m):
    m.ret(_f32bits(m.darg(0)))


def _div_t(m):  # div_t div(int, int) returns the 8-byte struct through a hidden pointer in r0
    p, a, b = m.arg(0), _s32(m.arg(1)), _s32(m.arg(2))
    q = _tdiv(a, b)
    m.store(p, (q & 0xFFFFFFFF).to_bytes(4, "little") + ((a - q * b) & 0xFFFFFFFF).to_bytes(4, "little"))
    m.ret(p)


class _RegArgs:
    """Variadic arguments in the AAPCS argument registers and stack; 64-bit values start on an even slot."""
    def __init__(self, m, k):
        self.m, self.k = m, k

    def u32(self):
        v = self.m.arg(self.k)
        self.k += 1
        return v

    def u64(self):
        self.k += self.k & 1
        v = self.m.arg(self.k) | (self.m.arg(self.k + 1) << 32)
        self.k += 2
        return v


class _VaArgs:
    """A bionic va_list on ARM is a pointer to the next argument word on the stack; 64-bit values are 8-aligned."""
    def __init__(self, m, ptr):
        self.m, self.p = m, ptr

    def u32(self):
        v = self.m.u32(self.p)
        self.p += 4
        return v

    def u64(self):
        self.p = (self.p + 7) & ~7
        v = self.m.u32(self.p) | (self.m.u32(self.p + 4) << 32)
        self.p += 8
        return v


def _format(m, fmt_addr, args):
    """printf-style formatter. Flags, width, precision, h/hh/l/ll/z/t/j length modifiers, d i u x X o c s p f e g %."""
    import re
    fmt = m.cstr(fmt_addr).decode("latin-1")
    out, pos = [], 0
    for mt in re.finditer(r"%([-+ #0]*)(\*|\d+)?(?:\.(\*|\d+))?(hh|h|ll|l|z|t|j|L)?([diuxXocspfFeEgG%])", fmt):
        out.append(fmt[pos:mt.start()])
        pos = mt.end()
        flags, width, prec, ln, conv = mt.groups()
        if conv == "%":
            out.append("%")
            continue
        if width == "*":
            width = _s32(args.u32())
            if width < 0:
                flags, width = (flags or "") + "-", -width
        if prec == "*":
            prec = _s32(args.u32())
            if prec < 0:
                prec = None
        spec = "%" + (flags or "") + (str(width) if width else "") + (("." + str(prec)) if prec is not None else "")
        if conv in "fFeEgG":
            val = struct.unpack("<d", struct.pack("<Q", args.u64()))[0]
            out.append((spec + conv) % val)
        elif conv == "s":
            p = args.u32()
            out.append((spec + "s") % (m.cstr(p).decode("latin-1") if p else "(null)"))
        elif conv == "c":
            out.append((spec + "c") % chr(args.u32() & 0xFF))
        elif conv == "p":
            out.append("0x%x" % args.u32())
        else:
            if ln in ("ll", "j"):
                v, bits = args.u64(), 64
            else:
                v, bits = args.u32(), 32
                if ln == "h":
                    v, bits = v & 0xFFFF, 16
                elif ln == "hh":
                    v, bits = v & 0xFF, 8
            if conv in "di" and v >> (bits - 1):
                v -= 1 << bits
            out.append((spec + ("d" if conv in "diu" else conv)) % v)
    out.append(fmt[pos:])
    return "".join(out).encode("latin-1")


def _safe_format(m, fmt_addr, args):
    try:
        return _format(m, fmt_addr, args)
    except (ValueError, TypeError, OverflowError) as e:
        raise Unmodelled(f"format: {e!r}")


def _sprintf(m):
    s = _safe_format(m, m.arg(1), _RegArgs(m, 2))
    m.store(m.arg(0), s + b"\0")
    m.ret(len(s))


def _snprintf(m):
    n = m.arg(1)
    s = _safe_format(m, m.arg(2), _RegArgs(m, 3))
    if n:
        m.store(m.arg(0), s[:n - 1] + b"\0")
    m.ret(len(s))


def _vsprintf(m):
    s = _safe_format(m, m.arg(1), _VaArgs(m, m.arg(2)))
    m.store(m.arg(0), s + b"\0")
    m.ret(len(s))


def _vsnprintf(m):
    n = m.arg(1)
    s = _safe_format(m, m.arg(2), _VaArgs(m, m.arg(3)))
    if n:
        m.store(m.arg(0), s[:n - 1] + b"\0")
    m.ret(len(s))


def _printf(m):
    m.ret(len(_safe_format(m, m.arg(0), _RegArgs(m, 1))))


def _fprintf(m):
    m.ret(len(_safe_format(m, m.arg(1), _RegArgs(m, 2))))


def _puts(m):
    m.ret(len(m.cstr(m.arg(0))) + 1)


def _guard_acquire(m):
    m.ret(1 if m.read(m.arg(0), 1)[0] == 0 else 0)


def _guard_release(m):
    m.store(m.arg(0), bytes([1]))


def _strlcpy(m):
    d, s, n = m.arg(0), m.cstr(m.arg(1)), m.arg(2)
    if n:
        m.store(d, s[:n - 1] + b"\0")
    m.ret(len(s))


def _strncat(m):
    d, n = m.arg(0), m.arg(2)
    m.store(d + len(m.cstr(d)), m.cstr(m.arg(1))[:n] + b"\0")
    m.ret(d)


def _basename(m):
    p = m.arg(0)
    s = m.cstr(p)
    if not s or s.endswith(b"/"):
        raise Unmodelled("basename of an empty path or one with a trailing slash")
    m.ret(p + s.rfind(b"/") + 1)


def _wstr(m, a, limit=1 << 14):
    out = []
    while len(out) < limit:
        w = m.u32(a + 4 * len(out))
        if w == 0:
            return out
        out.append(w)
    raise Unmodelled("unterminated wide string")


def _wcslen(m):
    m.ret(len(_wstr(m, m.arg(0))))


def _wcscpy(m):
    d = m.arg(0)
    m.store(d, b"".join(w.to_bytes(4, "little") for w in _wstr(m, m.arg(1)) + [0]))
    m.ret(d)


def _wcsncpy(m):
    d, n = m.arg(0), m.arg(2)
    s = _wstr(m, m.arg(1))[:n]
    m.store(d, b"".join(w.to_bytes(4, "little") for w in s + [0] * (n - len(s))))
    m.ret(d)


def _wcscmp(m):
    a, b = _wstr(m, m.arg(0)) + [0], _wstr(m, m.arg(1)) + [0]
    for x, y in zip(a, b):
        if x != y:
            return m.ret(1 if x > y else -1)
    m.ret(0)


def _wmemcpy(m):
    d, s, n = m.arg(0), m.arg(1), m.arg(2)
    if n:
        m.store(d, m.read(s, 4 * n))
    m.ret(d)


def _wmemset(m):
    d, c, n = m.arg(0), m.arg(1), m.arg(2)
    if n:
        m.store(d, c.to_bytes(4, "little") * n)
    m.ret(d)


def _wmemcmp(m):
    for k in range(m.arg(2)):
        x, y = m.u32(m.arg(0) + 4 * k), m.u32(m.arg(1) + 4 * k)
        if x != y:
            return m.ret(1 if x > y else -1)
    m.ret(0)


def _pthread_key_create(m):
    m.store(m.arg(0), (1).to_bytes(4, "little"))
    m.ret(0)


MODELS.update({
    "__aeabi_idiv": _idiv(True, False, False), "__aeabi_uidiv": _idiv(False, False, False),
    "__aeabi_idivmod": _idiv(True, False, True), "__aeabi_uidivmod": _idiv(False, False, True),
    "__aeabi_ldivmod": _idiv(True, True, True), "__aeabi_uldivmod": _idiv(False, True, True),
    "__aeabi_dadd": _dadd, "__aeabi_dsub": _dsub, "__aeabi_drsub": _drsub, "__aeabi_dmul": _dmul,
    "__aeabi_ddiv": _ddiv, "__aeabi_d2iz": _d2int(32, True), "__aeabi_d2uiz": _d2int(32, False),
    "__aeabi_d2lz": _d2int(64, True), "__aeabi_d2ulz": _d2int(64, False),
    "__aeabi_i2d": _i2d(True, False), "__aeabi_ui2d": _i2d(False, False), "__aeabi_l2d": _i2d(True, True),
    "__aeabi_ul2d": _i2d(False, True), "__aeabi_f2d": _f2d, "__aeabi_d2f": _d2f,
    "__aeabi_atexit": _const(0), "__cxa_guard_acquire": _guard_acquire, "__cxa_guard_release": _guard_release,
    "__cxa_guard_abort": _const(0), "__cxa_pure_virtual": _abort,
    "_Znwj": _malloc, "_Znaj": _malloc, "_ZnwjRKSt9nothrow_t": _malloc, "_ZnajRKSt9nothrow_t": _malloc,
    "_ZdlPv": _const(0), "_ZdaPv": _const(0),
    "sinf": _fmath1(math.sin), "cosf": _fmath1(math.cos), "tanf": _fmath1(math.tan), "asinf": _fmath1(math.asin),
    "acosf": _fmath1(math.acos), "atanf": _fmath1(math.atan), "expf": _fmath1(math.exp), "logf": _fmath1(math.log),
    "log10f": _fmath1(math.log10), "sqrtf": _fmath1(math.sqrt), "floorf": _fmath1(math.floor),
    "ceilf": _fmath1(math.ceil), "atan2f": _fmath2(math.atan2), "powf": _fmath2(math.pow),
    "fmodf": _fmath2(math.fmod), "ldexpf": _ldexpf,
    "div": _div_t, "strlcpy": _strlcpy, "strncat": _strncat, "strcoll": _strcmp, "basename": _basename,
    "sprintf": _sprintf, "snprintf": _snprintf, "vsprintf": _vsprintf, "vsnprintf": _vsnprintf,
    "printf": _printf, "fprintf": _fprintf, "puts": _puts,
    "wcslen": _wcslen, "wcscpy": _wcscpy, "wcsncpy": _wcsncpy, "wcscmp": _wcscmp, "wmemcpy": _wmemcpy,
    "wmemmove": _wmemcpy, "wmemset": _wmemset, "wmemcmp": _wmemcmp,
    "towlower": lambda m: m.ret(m.arg(0) + 32 if 65 <= m.arg(0) <= 90 else m.arg(0)),
    "towupper": lambda m: m.ret(m.arg(0) - 32 if 97 <= m.arg(0) <= 122 else m.arg(0)),
    "iswalpha": _ctype(str.isalpha), "iswdigit": _ctype(str.isdigit),
    "iswspace": _ctype(lambda c: c in " \t\n\v\f\r"),
    "iswupper": _ctype(str.isupper), "iswlower": _ctype(str.islower),
    "iswxdigit": _ctype(lambda c: c in "0123456789abcdefABCDEF"),
    "iswcntrl": _ctype(lambda c: ord(c) < 32 or ord(c) == 127), "iswprint": _ctype(lambda c: 32 <= ord(c) < 127),
    "iswpunct": _ctype(lambda c: 32 < ord(c) < 127 and not c.isalnum()),
    "pthread_key_create": _pthread_key_create, "pthread_getspecific": _const(0), "pthread_setspecific": _const(0),
    "__android_log_write": _const(0), "getenv": _const(0),
})


def run_import(m: Machine, name: str):
    model = MODELS.get(name)
    if model is None and name.startswith("gl") and name[2:3].isupper():
        model = _const(0)  # OpenGL ES calls: no effect on the CPU state that is compared
    if model is None:
        raise Unmodelled(name)
    try:
        model(m)
    except Unmodelled:
        raise
    except (ValueError, OverflowError, ZeroDivisionError, struct.error) as e:
        raise Unmodelled(f"{name}: {e!r}")
