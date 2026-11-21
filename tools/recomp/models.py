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

    def ret(self, v):
        self.set_reg(0, v & 0xFFFFFFFF)

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


def run_import(m: Machine, name: str):
    model = MODELS.get(name)
    if model is None:
        raise Unmodelled(name)
    model(m)
