#!/usr/bin/env python3
"""Differential test: original functions in Unicorn (ARM32, ARM and Thumb-2) against their translations compiled
for the host.

The whole library is translated and built into one DLL, so a tested function's callees run translated too.
Both sides start from the same game memory (relocated image, scratch buffers, stack, heap) and the same CPU
state. Imports run the models in models.py on both sides; kernel helpers run in rc_kuser on the host and in
an equivalent hook in Unicorn. After the function returns, every core register, the flags, FPSCR's NZCV,
the VFP registers and every writable byte are compared. NaNs compare equal to any NaN: x86's default NaN
has the opposite sign to ARM's, and arm64 behaves like ARM here.

Thumb functions are entered in Thumb state (CPSR.T, LR = return address | 1) with a clean IT state; the
translated side does not care about the mode. CPSR is rewritten in full before every run (mode, E, T, IT, GE
and Q included), so a run that executed garbage cannot leak state into the next one. r15 is compared without
its Thumb bit (the bit belongs to the return address, not to PC).

  py -3 tools/recomp/difftest.py --sample 1000 --seed 1 --runs 16                    # random sample
  py -3 tools/recomp/difftest.py --only 2297c9c _ZN11MenuControl13GetTextHeightEPKc  # link addresses or names
  py -3 tools/recomp/difftest.py --kind leaf --sample 500 --isa all                  # Thumb included
  py -3 tools/recomp/difftest.py --mutate mixed --sample 300                         # check the checker
  py -3 tools/recomp/difftest.py --unicorn-only --sample 300                         # no lifter, no DLL

Statuses: pass (at least 4 runs completed and agreed), fail, no-coverage (fewer than 4 runs completed in Unicorn:
a random input crashed, an import has no model, ...), untranslatable (the lifter refused the function; counted over
the whole library, and the function is not sampled), untranslated-callee (reached a function the lifter refused).
Outputs under --out: results.csv, summary.json and report.json (every failure with its input, the first differing
register or address, the instruction mnemonics of the function, and a per-mnemonic cluster table).
"""
from __future__ import annotations

import argparse
import collections
import concurrent.futures
import csv
import ctypes
import gzip
import hashlib
import json
import os
import pickle
import random
import re
import struct
import subprocess
import sys
import time
from array import array
from pathlib import Path

from capstone import CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB, Cs
from capstone import arm as CA
from unicorn import UC_ARCH_ARM, UC_HOOK_CODE, UC_MODE_ARM, UC_PROT_EXEC, UC_PROT_READ, Uc, UcError
from unicorn import arm_const as U

sys.path.insert(0, str(Path(__file__).parent))
if "--lift-dir" in sys.argv:  # a folder holding another lift.py (a snapshot, or a candidate), used instead of ours
    sys.path.insert(0, str(Path(sys.argv[sys.argv.index("--lift-dir") + 1]).resolve()))
from image import IMAGE_BASE, IMPORT_STUB_BASE, Image  # noqa: E402
import lift as liftmod  # noqa: E402
from lift import Lifter, entries  # noqa: E402
import models  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SO = ROOT / "original/apk/lib/armeabi-v7a/librainbowsix.so"
DEFAULT_WORK = ROOT / "build/difftest"

SCRATCH, SCRATCH_SIZE = 0x02000000, 0x100000
STACK, STACK_SIZE = 0x03000000, 0x100000
SP = STACK + 0xF0000
SENTINEL = 0x04000000
KUSER = 0xFFFF0000
HOST_SIZE = models.HEAP + models.HEAP_SIZE
RUNTIME = Path(__file__).parent / "runtime"
BX_LR = (0xE12FFF1E).to_bytes(4, "little")
SHARDS = 48
CPSR_T, CPSR_E = 0x20, 0x200
CPSR_IT = 0x0600FC00  # IT[7:2] in bits 15:10, IT[1:0] in bits 26:25
CPSR_GE_Q = 0x080F0000  # GE[3:0] bits 19:16, Q bit 27


class Cpu(ctypes.Structure):
    _fields_ = [("r", ctypes.c_uint32 * 16), ("n", ctypes.c_uint32), ("z", ctypes.c_uint32), ("c", ctypes.c_uint32),
                ("v", ctypes.c_uint32), ("fpscr", ctypes.c_uint32), ("fault", ctypes.c_uint32),
                ("budget", ctypes.c_uint32), ("memsize", ctypes.c_uint32), ("depth", ctypes.c_uint32),
                ("tls", ctypes.c_uint32), ("pad", ctypes.c_uint32), ("d", ctypes.c_uint64 * 32),
                ("trace", ctypes.c_uint32 * 64)]


IMPORT_HANDLER = ctypes.CFUNCTYPE(None, ctypes.POINTER(Cpu), ctypes.c_void_p, ctypes.c_uint32)


# ---- inputs ------------------------------------------------------------------------------------
def rand_float_bits(rng) -> int:
    x = rng.choice((rng.uniform(-1000, 1000), rng.uniform(-1, 1), float(rng.randint(-100, 100)), 0.0))
    return struct.unpack("<I", struct.pack("<f", x))[0]


def rand_double_bits(rng) -> int:
    x = rng.choice((rng.uniform(-1000, 1000), rng.uniform(-1, 1), float(rng.randint(-100, 100)), 0.0))
    return struct.unpack("<Q", struct.pack("<d", x))[0]


def scratch_image(rng, data_lo, data_hi) -> bytes:
    words = array("I")
    for _ in range(SCRATCH_SIZE // 4):
        p = rng.random()
        if p < 0.40:
            w = SCRATCH + (rng.randrange(0, 0x40000) & ~3)
        elif p < 0.60:
            w = rng.randrange(0, 65)
        elif p < 0.75:
            w = 0
        elif p < 0.85:
            w = rand_float_bits(rng)
        elif p < 0.90:
            w = data_lo + (rng.randrange(0, data_hi - data_lo) & ~3)
        else:
            w = rng.getrandbits(32)
        words.append(w)
    return words.tobytes()


def reg_value(rng, data_lo, data_hi) -> int:
    p = rng.random()
    if p < 0.45:
        return SCRATCH + (rng.randrange(0, 0x40000) & ~3)
    if p < 0.65:
        return rng.randrange(0, 65)
    if p < 0.75:
        return 0
    if p < 0.90:
        return rng.getrandbits(32)
    return data_lo + (rng.randrange(0, data_hi - data_lo) & ~3)


# ---- comparison --------------------------------------------------------------------------------
def is_nan32(b: bytes) -> bool:
    u = int.from_bytes(b, "little")
    return (u & 0x7F800000) == 0x7F800000 and (u & 0x007FFFFF) != 0


def is_nan64(b: bytes) -> bool:
    if len(b) != 8:
        return False
    u = int.from_bytes(b, "little")
    return (u & 0x7FF0000000000000) == 0x7FF0000000000000 and (u & 0x000FFFFFFFFFFFFF) != 0


def same_vfp(a: int, b: int) -> bool:
    if a == b:
        return True
    if is_nan64(a.to_bytes(8, "little")) and is_nan64(b.to_bytes(8, "little")):
        return True
    for sh in (0, 32):
        x, y = (a >> sh) & 0xFFFFFFFF, (b >> sh) & 0xFFFFFFFF
        if x != y and not (is_nan32(x.to_bytes(4, "little")) and is_nan32(y.to_bytes(4, "little"))):
            return False
    return True


def compare(u, h):
    """None when the two final states agree, else a dict describing the first difference:
    {"where": "r3" | "nzcv" | "fpscr" | "d5" | "memory" | "host-fault", "unicorn", "host", "address"?, "text"}."""
    if h["fault"]:
        return {"where": "host-fault", "unicorn": None, "host": h["fault"], "text": f"host fault {h['fault']}"}
    for k in range(16):
        x, y = u["r"][k], h["r"][k]
        if k == 15:
            x, y = x & ~1, y & ~1  # the Thumb bit is part of the return address, not of PC
        # softfp passes floats in core registers, so the x86 NaN sign shows up here too
        if x != y and not (is_nan32(x.to_bytes(4, "little")) and is_nan32(y.to_bytes(4, "little"))):
            return {"where": f"r{k}", "unicorn": x, "host": y, "text": f"r{k}: unicorn {x:#x} host {y:#x}"}
    if u["f"] != h["f"]:
        return {"where": "nzcv", "unicorn": u["f"], "host": h["f"],
                "text": f"nzcv: unicorn {u['f']:04b} host {h['f']:04b}"}
    if u["fp"] != h["fp"]:
        return {"where": "fpscr", "unicorn": u["fp"], "host": h["fp"],
                "text": f"fpscr nzcv: unicorn {u['fp']:04b} host {h['fp']:04b}"}
    for k in range(32):
        if not same_vfp(u["d"][k], h["d"][k]):
            return {"where": f"d{k}", "unicorn": u["d"][k], "host": h["d"][k],
                    "text": f"d{k}: unicorn {u['d'][k]:#018x} host {h['d'][k]:#018x}"}
    for a, data in u["mem"].items():
        other = h["mem"][a]
        if data == other:
            continue
        for j in range(0, len(data), 4):
            if data[j:j + 4] == other[j:j + 4]:
                continue
            if is_nan32(data[j:j + 4]) and is_nan32(other[j:j + 4]):
                continue
            k = j & ~7
            if is_nan64(data[k:k + 8]) and is_nan64(other[k:k + 8]):
                continue
            return {"where": "memory", "address": a + j, "unicorn": data[j:j + 4].hex(), "host": other[j:j + 4].hex(),
                    "text": f"memory {a + j:#x}: unicorn {data[j:j + 4].hex()} host {other[j:j + 4].hex()}"}
    return None


# ---- build -------------------------------------------------------------------------------------
X86_TEST = ("x86_64-windows-gnu", ["-O1", "-DRECOMP_CHECKED"], "recomp.dll")
# zig ships no Android libc headers; musl's are enough because the translated code calls no libc (only builtins),
# and the library links with -nostdlib, so the result has no libc dependency and loads on Android as well
ARM64_ANDROID = ("aarch64-linux-musl", ["-O2", "-fPIC", "-g0"], "librainbowsix_recomp.so")


HOSTFN_BASE, HOSTFN_MAX = 0x00E00000, 0x10000  # runtime/rt.h: RT_HOSTFN_BASE, RT_HOSTFN_MAX


def compile_objects(image: Image, ents: dict, sources: dict, workdir: Path, mutate=None, target=X86_TEST,
                    variant="test", mutants=None) -> list:
    """Compile every entry (translated, or a stub that faults) plus the dispatcher. Object files are cached by
    content, so a rebuild recompiles only the shards that changed. The x86 test build is checked (bounds, budget);
    the arm64 build is the optimized one. variant "test" adds the comparison harness's glue (an import handler
    set from Python, rc_run); variant "runtime" leaves rc_import and rc_hostcall to runtime/.
    mutate: (old, new) text replaced in every translation (a blunt break-everything switch).
    mutants: {address: C source} compiled as fm_<addr> next to the intact f_<addr> in one extra file, with
    rc_run_mut(addr, cpu, mem) to run one of them; the intact shards stay cached."""
    triple, opt, out_name = target[:3]
    compiler = list(target[3]) if len(target) > 3 else [sys.executable, "-m", "ziglang", "cc"]
    workdir.mkdir(parents=True, exist_ok=True)
    protos = "".join(f"void f_{a:07x}(cpu_t *c, uint8_t *m);\n" for a in sorted(ents))
    (workdir / "protos.h").write_text(protos, encoding="utf-8")
    head = ('#include "recomp.h"\n#include "protos.h"\n#pragma clang diagnostic ignored "-Wunused-label"\n'
            '#pragma clang diagnostic ignored "-Wunused-variable"\n#pragma clang diagnostic ignored "-Wparentheses-equality"\n'
            '#pragma clang diagnostic ignored "-Wunused-but-set-variable"\n')
    addrs = sorted(ents)
    files = []
    for k in range(SHARDS):
        part = addrs[k::SHARDS]
        body = [head]
        for a in part:
            src = sources.get(a)
            if src is None:
                src = (f"void f_{a:07x}(cpu_t *c, uint8_t *m) {{ (void)m; c->fault = RC_FAULT_UNLIFTED; "
                       f"c->r[15] = {a:#x}u; }}")  # r15: which function was missing
            elif mutate:
                src = src.replace(*mutate)
            body.append(src)
        files.append((workdir / f"shard{k:02d}.c", "\n".join(body) + "\n"))
    disp = [head, "#ifdef _WIN32", "#define RC_EXPORT __declspec(dllexport)", "#else",
            "#define RC_EXPORT __attribute__((visibility(\"default\")))", "#endif"]
    if variant == "test":
        disp += ["static void (*handler)(cpu_t *, uint8_t *, uint32_t);",
                 "RC_EXPORT void rc_set_import_handler(void *h) { handler = (void (*)(cpu_t *, uint8_t *, uint32_t))h; }",
                 "void rc_import(cpu_t *c, uint8_t *m, uint32_t slot) { handler(c, m, slot); c->r[15] = c->r[14]; }",
                 "void rc_hostcall(cpu_t *c, uint8_t *m, uint32_t slot) { (void)m; (void)slot; c->fault = RC_FAULT_DISPATCH; }",
                 "RC_EXPORT void rc_run(uint32_t addr, cpu_t *c, uint8_t *m) { rc_dispatch(c, m, addr); }",
                 # the lifter's setjmp (RC_SETJMP, see recomp.h): longjmp is not modelled, so a host jmp_buf is never used
                 "void *rt_setjmp_prepare(cpu_t *c, uint32_t g) { static jmp_buf b; (void)c; (void)g; return &b; }",
                 "RC_EXPORT uint32_t rc_layout(uint32_t what) { return what == 0 ? (uint32_t)sizeof(cpu_t) : "
                 "what == 1 ? (uint32_t)__builtin_offsetof(cpu_t, vfp) : what == 2 ? (uint32_t)__builtin_offsetof(cpu_t, trace) : "
                 "(uint32_t)__builtin_offsetof(cpu_t, fault); }"]
    disp += ["int rc_is_entry(uint32_t t) {",
             f"  if (t >= {HOSTFN_BASE:#x}u && t < {HOSTFN_BASE + 4 * HOSTFN_MAX:#x}u) return 1;",
             f"  if (t >= {IMPORT_STUB_BASE:#x}u && t < {IMPORT_STUB_BASE + 4 * len(image.imports):#x}u) return 1;",
             "  switch (t & ~1u) {"]
    disp += [f"  case {IMAGE_BASE + a:#x}u:" for a in addrs]
    disp += [f"  case {IMAGE_BASE + a:#x}u:" for a in sorted(image.plt)]
    disp += ["    return 1;", "  }", "  return 0;", "}"]
    disp += ["void rc_dispatch(cpu_t *c, uint8_t *m, uint32_t t) {",
             f"  if (t >= {HOSTFN_BASE:#x}u && t < {HOSTFN_BASE + 4 * HOSTFN_MAX:#x}u) "
             f"{{ rc_hostcall(c, m, (t - {HOSTFN_BASE:#x}u) / 4); return; }}",
             f"  if (t >= {IMPORT_STUB_BASE:#x}u && t < {IMPORT_STUB_BASE + 4 * len(image.imports):#x}u) "
             f"{{ rc_import(c, m, (t - {IMPORT_STUB_BASE:#x}u) / 4); return; }}",
             "  if (rc_kuser(c, m, t)) return;",
             "  switch (t & ~1u) {  /* a Thumb address has its low bit set */"]
    for a in addrs:
        disp.append(f"  case {IMAGE_BASE + a:#x}u: f_{a:07x}(c, m); return;")
    slots = {name: k for k, name in enumerate(image.imports)}
    for a, (name, _) in sorted(image.plt.items()):
        disp.append(f"  case {IMAGE_BASE + a:#x}u: rc_import(c, m, {slots[name]}); return;  /* {name} via the PLT */")
    disp += ["  }", "  c->fault = RC_FAULT_DISPATCH; c->r[15] = t;", "}"]
    files.append((workdir / f"dispatch-{variant}.c", "\n".join(disp) + "\n"))
    if mutants:
        body = [head]
        for a, src in sorted(mutants.items()):
            name = f"f_{a:07x}(cpu_t *c, uint8_t *m)"
            if name not in src:
                raise RuntimeError(f"cannot rename the translation of {a:#x}")
            body.append(src.replace(name, f"fm_{a:07x}(cpu_t *c, uint8_t *m)", 1))
        body.append("#ifdef _WIN32\n#define RC_EXPORT __declspec(dllexport)\n#else\n"
                    "#define RC_EXPORT __attribute__((visibility(\"default\")))\n#endif")
        body.append("RC_EXPORT int rc_run_mut(uint32_t addr, cpu_t *c, uint8_t *m) {\n  switch (addr & ~1u) {")
        body += [f"  case {IMAGE_BASE + a:#x}u: fm_{a:07x}(c, m); return 1;" for a in sorted(mutants)]
        body.append("  }\n  return 0;\n}")
        files.append((workdir / "mutants.c", "\n".join(body) + "\n"))

    flags = [opt[0], "-target", triple, "-fwrapv", "-fno-strict-aliasing", "-ffp-contract=off", *opt[1:],
             f"-I{RUNTIME}", f"-I{workdir}"]
    key_extra = (RUNTIME / "recomp.h").read_text() + protos + " ".join(flags)

    def compile_one(item):
        path, text = item
        digest = hashlib.sha256((text + key_extra).encode()).hexdigest()[:16]
        obj = path.with_suffix(f".{digest}.o")
        if not obj.exists():
            tmp = path.with_suffix(f".{digest}.c")
            tmp.write_text(text, encoding="utf-8")
            r = subprocess.run([*compiler, "-c", *flags, str(tmp), "-o", str(obj) + ".part"], capture_output=True, text=True)
            tmp.unlink(missing_ok=True)
            if r.returncode:
                raise RuntimeError(f"compile of {path.name} failed:\n{r.stderr[-4000:]}")
            os.replace(str(obj) + ".part", obj)
        return obj

    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        return list(pool.map(compile_one, files))


def build(image: Image, ents: dict, sources: dict, workdir: Path, mutate=None, target=X86_TEST, mutants=None,
          out_name=None) -> Path:
    """The test variant of compile_objects, linked into one shared library."""
    triple, opt, default_name = target[:3]
    objs = compile_objects(image, ents, sources, workdir, mutate, target, "test", mutants)
    lib = workdir / (out_name or default_name)
    link = ["-nostdlib", "-Wl,--no-undefined", "-s"] if triple.startswith("aarch64") else []
    r = subprocess.run([sys.executable, "-m", "ziglang", "cc", "-shared", "-target", triple, *link,
                        *map(str, objs), "-o", str(lib)], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f"link failed:\n{r.stderr[-4000:]}")
    return lib


# ---- the two machines --------------------------------------------------------------------------
class UcMachine(models.Machine):
    def __init__(self, uc, writable):
        self.uc = uc
        self.regs = [getattr(U, f"UC_ARM_REG_R{k}") for k in range(13)] + [U.UC_ARM_REG_SP, U.UC_ARM_REG_LR]
        super().__init__(writable)

    def reg(self, k):
        return self.uc.reg_read(self.regs[k])

    def set_reg(self, k, v):
        self.uc.reg_write(self.regs[k], v)

    def read(self, a, n):
        return bytes(self.uc.mem_read(a, n))

    def write(self, a, data):
        self.uc.mem_write(a, data)


class HostMachine(models.Machine):
    def __init__(self, host, writable):
        self.base = ctypes.addressof(host)
        self.cpu = None
        super().__init__(writable)

    def reg(self, k):
        return self.cpu.r[k]

    def set_reg(self, k, v):
        self.cpu.r[k] = v

    def check(self, a, n):
        if a < 0x00100000 or a + n > HOST_SIZE:
            raise models.Unmodelled(f"host access {a:#x}")

    def read(self, a, n):
        self.check(a, n)
        return ctypes.string_at(self.base + a, n)

    def write(self, a, data):
        self.check(a, len(data))
        ctypes.memmove(self.base + a, data, len(data))


class Harness:
    """dll=None runs the originals in Unicorn alone (--unicorn-only)."""

    def __init__(self, image: Image, dll, seed: int, ents=None):
        self.image = image
        self.dll = dll
        self.import_names = list(image.imports)
        rw_start = IMAGE_BASE + (image.writable[0][0] & ~0xFFF)
        self.writable = [(rw_start, IMAGE_BASE + image.size)]
        self.data_lo, self.data_hi = IMAGE_BASE + image.writable[0][0], IMAGE_BASE + image.writable[-1][1]
        self.scratch = [scratch_image(random.Random(seed * 100 + k), self.data_lo, self.data_hi) for k in range(8)]
        self.regions = self.writable + [(SCRATCH, SCRATCH + SCRATCH_SIZE), (STACK, STACK + STACK_SIZE),
                                        (models.HEAP, models.HEAP + models.HEAP_SIZE)]
        self.base_image = bytes(image.memory)

        uc = self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        uc.mem_map(IMAGE_BASE, rw_start - IMAGE_BASE, UC_PROT_READ | UC_PROT_EXEC)
        uc.mem_map(rw_start, IMAGE_BASE + image.size - rw_start)
        uc.mem_map(SCRATCH, SCRATCH_SIZE)
        uc.mem_map(STACK, STACK_SIZE)
        uc.mem_map(models.HEAP, models.HEAP_SIZE)
        uc.mem_write(IMAGE_BASE, self.base_image)  # mem_write ignores the protection
        # import stubs and kernel helpers: `bx lr` at each address, the work is done in a code hook
        uc.mem_map(IMPORT_STUB_BASE, 0x1000, UC_PROT_READ | UC_PROT_EXEC)
        uc.mem_write(IMPORT_STUB_BASE, BX_LR * 0x400)
        uc.mem_map(KUSER, 0x1000, UC_PROT_READ | UC_PROT_EXEC)
        uc.mem_write(KUSER, BX_LR * 0x400)
        uc.hook_add(UC_HOOK_CODE, self._uc_import, begin=IMPORT_STUB_BASE, end=IMPORT_STUB_BASE + 0xFFF)
        uc.hook_add(UC_HOOK_CODE, self._uc_kuser, begin=KUSER, end=KUSER + 0xFFF)
        try:
            uc.reg_write(U.UC_ARM_REG_C1_C0_2, uc.reg_read(U.UC_ARM_REG_C1_C0_2) | (0xF << 20))
        except (UcError, AttributeError):
            pass
        uc.reg_write(U.UC_ARM_REG_FPEXC, 0x40000000)
        self.ucm = UcMachine(uc, self.regions)
        # CPSR as Unicorn starts it (ARM state, little-endian, privileged mode kept), with the flags, Q, GE, IT, T and
        # E bits cleared; written in full before every run, so a run that executed garbage (SETEND BE, a mode
        # change, a stray IT state) cannot leak into the next one. The T bit is added per run for Thumb functions.
        self.cpsr0 = uc.reg_read(U.UC_ARM_REG_CPSR) & 0x0FFFFFFF & ~(CPSR_T | CPSR_E | CPSR_IT | CPSR_GE_Q)
        self.core = [getattr(U, f"UC_ARM_REG_R{k}") for k in range(13)] + [U.UC_ARM_REG_SP, U.UC_ARM_REG_LR]
        self.dregs = [getattr(U, f"UC_ARM_REG_D{k}") for k in range(32)]
        self.host_error = None
        self.abort = None
        self.ents = ents if ents is not None else {}

        if dll is not None:
            self.host = ctypes.create_string_buffer(HOST_SIZE)
            ctypes.memmove(ctypes.addressof(self.host) + IMAGE_BASE, self.base_image, len(self.base_image))
            # the same stub words as in Unicorn: code may read through a pointer to an import
            ctypes.memmove(ctypes.addressof(self.host) + IMPORT_STUB_BASE, BX_LR * 0x400, 0x1000)
            self.hm = HostMachine(self.host, self.regions)
            self._handler = IMPORT_HANDLER(self._host_import)  # keep a reference: the DLL holds the pointer
            dll.rc_set_import_handler(self._handler)
            dll.rc_layout.restype = ctypes.c_uint32
            dll.rc_layout.argtypes = [ctypes.c_uint32]
            mine = (ctypes.sizeof(Cpu), Cpu.d.offset, Cpu.trace.offset, Cpu.fault.offset)
            theirs = tuple(dll.rc_layout(k) for k in range(4))
            if mine != theirs:
                raise RuntimeError(f"cpu_t in runtime/recomp.h changed: (size, vfp, trace, fault) offsets are {theirs} "
                                   f"in C but {mine} in difftest.Cpu; update the Cpu class")
            if hasattr(dll, "rc_run_mut"):
                dll.rc_run_mut.restype = ctypes.c_int
                dll.rc_run_mut.argtypes = [ctypes.c_uint32, ctypes.c_void_p, ctypes.c_void_p]

    # hooks
    def _uc_import(self, uc, address, size, _):
        name = self.import_names[(address - IMPORT_STUB_BASE) // 4]
        try:
            models.run_import(self.ucm, name)
        except Exception as e:  # noqa: BLE001 - an exception must not escape into Unicorn's callback machinery
            self.abort = f"import {name}" if isinstance(e, models.Unmodelled) and str(e) == name else f"import {name}: {e}"
            uc.emu_stop()

    def _uc_kuser(self, uc, address, size, _):
        m = self.ucm
        if address == 0xFFFF0FC0:
            ok = m.u32(m.reg(2)) == m.reg(0)
            if ok:
                try:
                    m.store(m.reg(2), m.reg(1).to_bytes(4, "little"))
                except models.Unmodelled as e:
                    self.abort = f"kernel helper: {e}"
                    uc.emu_stop()
                    return
            m.set_reg(0, 0 if ok else 1)
            cpsr = uc.reg_read(U.UC_ARM_REG_CPSR) & ~(0x6 << 28)
            uc.reg_write(U.UC_ARM_REG_CPSR, cpsr | ((0x6 << 28) if ok else 0))
        elif address == 0xFFFF0FE0:
            m.set_reg(0, models.TLS)
        elif address != 0xFFFF0FA0:
            self.abort = f"kernel helper {address:#x}"
            uc.emu_stop()

    def _host_import(self, cpu, m, slot):
        self.hm.cpu = cpu.contents
        try:
            models.run_import(self.hm, self.import_names[slot])
        except Exception as e:  # noqa: BLE001 - an exception must not cross the C frames
            self.host_error = f"import {self.import_names[slot]}: {e!r}"[:120]
            cpu.contents.fault = 8

    # runs
    def snapshot(self, variant: int):
        mem = {a: self.base_image[a - IMAGE_BASE:b - IMAGE_BASE] for a, b in self.writable}
        mem[SCRATCH] = self.scratch[variant]
        mem[STACK] = self.scratch[(variant + 3) % 8]
        mem[models.HEAP] = bytes(models.HEAP_SIZE)
        return mem

    def state(self, rng, thumb=False):
        regs = [reg_value(rng, self.data_lo, self.data_hi) for _ in range(13)] + [SP, SENTINEL | int(thumb)]
        d = []
        for _ in range(32):
            p = rng.random()
            if p < 0.45:
                d.append(rand_double_bits(rng))
            elif p < 0.9:
                d.append(rand_float_bits(rng) | (rand_float_bits(rng) << 32))
            else:
                d.append(rng.getrandbits(64))
        return regs, rng.getrandbits(4), rng.getrandbits(4) << 28, d

    def run_unicorn(self, addr, mem, regs, flags, fpscr, d, thumb=False, until=SENTINEL):
        uc = self.uc
        for a, data in mem.items():
            uc.mem_write(a, data)
        for k, r in enumerate(self.core):
            uc.reg_write(r, regs[k])
        cpsr = self.cpsr0 | (flags << 28) | (CPSR_T if thumb else 0)
        uc.reg_write(U.UC_ARM_REG_CPSR, cpsr)
        if uc.reg_read(U.UC_ARM_REG_CPSR) & CPSR_IT:
            raise RuntimeError("harness: IT state is not clean at function entry")
        uc.reg_write(U.UC_ARM_REG_FPSCR, fpscr)
        for k, r in enumerate(self.dregs):
            uc.reg_write(r, d[k])
        self.ucm.reset()
        self.abort = None
        try:
            uc.emu_start((IMAGE_BASE + addr) | int(thumb), until & ~1, count=2_000_000)
        except UcError as e:
            return None, f"unicorn: {e}"
        if self.abort:
            return None, self.abort
        pc = uc.reg_read(U.UC_ARM_REG_PC)
        if pc != until & ~1:
            return None, "unicorn stopped"
        pc = until  # the Thumb bit of a return address is not part of PC
        return {
            "r": [uc.reg_read(r) for r in self.core] + [pc],
            "f": uc.reg_read(U.UC_ARM_REG_CPSR) >> 28,
            "fp": uc.reg_read(U.UC_ARM_REG_FPSCR) >> 28,
            "d": [uc.reg_read(r) & 0xFFFFFFFFFFFFFFFF for r in self.dregs],
            "mem": {a: bytes(uc.mem_read(a, b - a)) for a, b in self.regions},
        }, None

    def run_host(self, addr, mem, regs, flags, fpscr, d, mutant=False):
        base = ctypes.addressof(self.host)
        # The translation runs first, so a run the original later rejects (a write to code) must not leave its
        # writes behind: restore the read-only image and the import stub words, not only the snapshot regions.
        ctypes.memmove(base + IMAGE_BASE, self.base_image, len(self.base_image))
        ctypes.memmove(base + IMPORT_STUB_BASE, BX_LR * 0x400, 0x1000)
        for a, data in mem.items():
            ctypes.memmove(base + a, data, len(data))
        c = Cpu()
        for k in range(15):
            c.r[k] = regs[k]
        c.n, c.z, c.c, c.v = (flags >> 3) & 1, (flags >> 2) & 1, (flags >> 1) & 1, flags & 1
        c.fpscr, c.budget, c.memsize, c.tls = fpscr, 2_000_000, HOST_SIZE, models.TLS
        for k in range(32):
            c.d[k] = d[k]
        self.hm.reset()
        if mutant:
            self.dll.rc_run_mut(ctypes.c_uint32(IMAGE_BASE + addr), ctypes.byref(c), self.host)
        else:
            self.dll.rc_run(ctypes.c_uint32(IMAGE_BASE + addr), ctypes.byref(c), self.host)
        return {
            "r": list(c.r), "f": (c.n << 3) | (c.z << 2) | (c.c << 1) | c.v, "fp": c.fpscr >> 28, "d": list(c.d),
            "mem": {a: ctypes.string_at(base + a, b - a) for a, b in self.regions}, "fault": c.fault,
        }

    def test_unicorn(self, addr, runs, thumb=False):
        """The original alone: how many random inputs run to completion, and why the others stop."""
        completed, notes = 0, {}
        for k in range(runs):
            rng = random.Random(addr * 1000 + k)
            mem = self.snapshot(k % 8)
            regs, flags, fpscr, d = self.state(rng, thumb)
            u, why = self.run_unicorn(addr, mem, regs, flags, fpscr, d, thumb)
            if u is None:
                key = re.sub(r"0x[0-9a-fA-F]+", "0x", why.split(" at ")[0].split(" (")[0])[:60]
                notes[key] = notes.get(key, 0) + 1
                continue
            completed += 1
        return ("unicorn-ok" if completed >= 4 else "no-coverage"), completed, json.dumps(notes), None

    def test(self, addr, runs, thumb=False, mutant=False):
        """Returns (status, completed runs, detail text, failure record or None)."""
        completed, notes = 0, {}
        for k in range(runs):
            rng = random.Random(addr * 1000 + k)
            mem = self.snapshot(k % 8)
            regs, flags, fpscr, d = self.state(rng, thumb)
            # The translation runs first. A function entered with random state may return to an address other
            # than the sentinel (a mid-function entry pops a code address); the translation stops there, so the
            # original is stopped at the same address. A wrong translation still shows: the original then never
            # reaches that address, or arrives with other registers.
            self.host_error = None
            h = self.run_host(addr, mem, regs, flags, fpscr, d, mutant)
            if h["fault"] == 8 and self.host_error and "Unmodelled" in self.host_error:
                # the import has no model, or the model refused an access: the original would stop there too
                key = self.host_error.split(":")[0]
                notes[key] = notes.get(key, 0) + 1
                continue
            until = h["r"][15] if not h["fault"] and (h["r"][15] & ~1) != SENTINEL else SENTINEL | int(thumb)
            u, why = self.run_unicorn(addr, mem, regs, flags, fpscr, d, thumb, until)
            if u is None:
                key = re.sub(r"0x[0-9a-fA-F]+", "0x", why.split(" at ")[0].split(" (")[0])[:60]
                notes[key] = notes.get(key, 0) + 1
                continue
            if (until & ~1) != SENTINEL:
                notes["returned elsewhere"] = notes.get("returned elsewhere", 0) + 1
            if h["fault"] == 4 and ((h["r"][15] - IMAGE_BASE) & ~1) in self.image.by_address \
                    and ((h["r"][15] - IMAGE_BASE) & ~1) not in self.ents:
                # a real function the build does not contain (Thumb code with an ARM-only lifter)
                return "untranslated-callee", completed, \
                    f"run {k}: called {h['r'][15]:#x}, which is not part of the build", None
            if h["fault"] == 7:
                return "untranslated-callee", completed, \
                    f"run {k}: reached a function the lifter could not translate ({h['r'][15]:#x})", None
            diff = compare(u, h)
            if diff:
                text = diff["text"] + (f" ({self.host_error})" if h["fault"] == 8 else "")
                rec = {"run": k, "seed": addr * 1000 + k, "scratch_variant": k % 8, "first_diff": diff,
                       "input": {"regs": [f"{x:#010x}" for x in regs], "nzcv": f"{flags:04b}",
                                 "fpscr_nzcv": f"{fpscr >> 28:04b}", "thumb": thumb,
                                 "d": [f"{x:#018x}" for x in d]},
                       "returned_to": f"{until:#x}", "host_fault": h["fault"], "completed_before": completed}
                return "fail", completed, f"run {k}: {text}", rec
            completed += 1
        if completed < 4:
            return "no-coverage", completed, json.dumps(notes), None
        return "pass", completed, "", None


# ---- the library: entries, translation, classification ---------------------------------------------
class ImageView:
    """The image with only some of its functions, for entries(): the ARM-mode lifter must not decode Thumb code."""

    def __init__(self, base, functions):
        self.__dict__["_base"] = base
        self.__dict__["functions"] = functions

    def __getattr__(self, name):
        return getattr(self._base, name)


def lifter_supports_thumb() -> bool:
    try:
        return "CS_MODE_THUMB" in Path(liftmod.__file__).read_text(encoding="utf-8")
    except OSError:
        return False


def lift_all(image, ents, work: Path, isa: str, verbose=True):
    """Translate every entry. The result (sources, failures) is cached under `work`, keyed by the text of lift.py
    and image.py, the library and the ISA mode, because lifting 20,000 functions in Python takes minutes."""
    key = hashlib.sha256(b"".join(Path(p).read_bytes() for p in (liftmod.__file__, ROOT / "tools/recomp/image.py"))
                         + f"{image.path.stat().st_size}:{image.path.stat().st_mtime_ns}:{isa}:{len(ents)}".encode()).hexdigest()
    cache = work / "lift-cache.pkl.gz"
    if cache.exists():
        try:
            with gzip.open(cache, "rb") as fh:
                k, sources, failed = pickle.load(fh)
            if k == key:
                if verbose:
                    print(f"translations from cache ({cache.name})")
                return sources, failed
        except Exception:  # noqa: BLE001 - a bad cache is just a cache miss
            pass
    lifter = Lifter(image, ents)
    sources, failed = {}, {}
    for a, f in ents.items():
        try:
            sources[a] = lifter.lift(f)
        except Exception as e:  # noqa: BLE001 - any lifter failure makes the function untranslatable
            failed[a] = (f"{type(e).__name__}: " if not isinstance(e, liftmod.Unsupported) else "") + str(e)
    work.mkdir(parents=True, exist_ok=True)
    tmp = cache.with_suffix(".part")
    with gzip.open(tmp, "wb", compresslevel=3) as fh:
        pickle.dump((key, sources, failed), fh, protocol=4)
    os.replace(tmp, cache)
    return sources, failed


def reason_key(why: str) -> str:
    return re.sub(r"0x[0-9a-fA-F]+", "0x", why.split(" at ")[0].split(" to 0x")[0])[:70]


CALL_SRC = re.compile(r"rc_dispatch|rc_import|\bf_[0-9a-f]{7}\(c, m\)")
NORMAL_CC = {CA.ARM_CC_EQ: "eq", CA.ARM_CC_NE: "ne", CA.ARM_CC_HS: "hs", CA.ARM_CC_LO: "lo", CA.ARM_CC_MI: "mi",
             CA.ARM_CC_PL: "pl", CA.ARM_CC_VS: "vs", CA.ARM_CC_VC: "vc", CA.ARM_CC_HI: "hi", CA.ARM_CC_LS: "ls",
             CA.ARM_CC_GE: "ge", CA.ARM_CC_LT: "lt", CA.ARM_CC_GT: "gt", CA.ARM_CC_LE: "le"}


class Disasm:
    """Instruction mnemonics of a function (condition and flag-setting suffixes stripped), for clustering failures."""

    def __init__(self, image):
        self.image = image
        self.md = {t: Cs(CS_ARCH_ARM, CS_MODE_THUMB if t else CS_MODE_ARM) for t in (False, True)}
        for m in self.md.values():
            m.detail = True
        self.cache = {}

    @staticmethod
    def normal(i) -> str:
        mn = i.mnemonic
        for suf in (".w", ".n"):
            if mn.endswith(suf):
                mn = mn[:-2]
        if re.fullmatch(r"it[te]{0,3}", mn):
            return "it"
        try:
            cc = NORMAL_CC.get(i.cc)
            if cc and mn.endswith(cc) and len(mn) > len(cc):
                mn = mn[:-len(cc)]
            if i.update_flags and mn.endswith("s") and len(mn) > 1 and mn not in ("bics",):
                mn = mn[:-1]
        except Exception:  # noqa: BLE001
            pass
        return mn

    def of(self, f) -> collections.Counter:
        key = (f.address, f.size)
        if key in self.cache:
            return self.cache[key]
        thumb = bool(getattr(f, "thumb", False))
        cnt = collections.Counter()
        try:
            for a, n in self.image.instructions(f):
                for i in self.md[thumb].disasm(bytes(self.image.memory[a:a + n]), a):
                    cnt[self.normal(i)] += 1
        except Exception:  # noqa: BLE001
            cnt["<undecodable>"] += 1
        self.cache[key] = cnt
        return cnt

    def calls(self, f) -> bool:
        c = self.of(f)
        return "bl" in c or "blx" in c


# ---- mutation ------------------------------------------------------------------------------------
COND_RE = r"(?:!?(?:z|cf|n|v)|\((?:cf && !z|!cf \|\| z|n == v|n != v|!z && n == v|z \|\| n != v)\))"
MUTATIONS = {
    # invert the carry a flag-setting instruction produced (an assignment to cf), or the carry-in of adc/sbc
    "carry": [(re.compile(r"(?<![\w>])cf = (?!c->c)([^;,{}]+);"), r"cf = !(\1);"),
              (re.compile(r"(addc\([^,;]+, [^,;]+, )cf,"), r"\1!cf,")],
    # read another register: the first source register of an expression that computes a value
    "reg": [(re.compile(r"(?m)^(?!.*c->r\[)(?!.*RC_)(.*?\b(?:res|ea|val|tgt) = [^;]*?)\br(\d{1,2})\b"),
             lambda mt: f"{mt.group(1)}r{(int(mt.group(2)) + 1) % 13}")],
    # drop the condition of a predicated instruction (an IT-block member, or an ARM conditional instruction)
    "itcond": [(re.compile(r"\bif \(" + COND_RE + r"\) \{(?! ?goto)"), "if (1) {")],
    # take a conditional branch the wrong way
    "branch": [(re.compile(r"(\bif \()(" + COND_RE + r")(\) \{? ?goto)"), r"\1!(\2)\3")],
}


def mutate_source(src: str, kind: str, rotate: int = 0):
    """(mutated source, kind used) or (None, None) when no pattern of the kind(s) applies. kind "mixed" tries the kinds
    starting at a rotating position, so a sample gets a spread of defects. Only the first match is changed."""
    kinds = list(MUTATIONS) if kind == "mixed" else [kind]
    if kind == "mixed":
        kinds = kinds[rotate % len(kinds):] + kinds[:rotate % len(kinds)]
    for kd in kinds:
        for rx, rep in MUTATIONS[kd]:
            new, n = rx.subn(rep, src, count=1)
            if n and new != src:
                return new, kd
    return None, None


# ---- driver ----------------------------------------------------------------------------------------
def parse_only(items, ents, image):
    """Link addresses (hex, with or without 0x, with or without IMAGE_BASE or the Thumb bit) or function names."""
    by_name = {}
    for a, f in ents.items():
        by_name.setdefault(f.name, []).append(a)
        for n in getattr(f, "aliases", []) or []:
            by_name.setdefault(n, []).append(a)
    picks = []
    for it in items:
        if it in by_name:
            picks += by_name[it]
            continue
        try:
            v = int(it, 16)
        except ValueError:
            raise SystemExit(f"--only: {it!r} is neither a function name nor a hex address")
        for cand in (v, v - IMAGE_BASE, v & ~1, (v - IMAGE_BASE) & ~1):
            if cand in ents:
                picks.append(cand)
                break
        else:
            raise SystemExit(f"--only: no entry at {it}")
    return sorted(set(picks))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--so", type=Path, default=DEFAULT_SO)
    ap.add_argument("--lift-dir", type=Path, help="import lift.py from this folder instead of tools/recomp")
    ap.add_argument("--out", type=Path, default=DEFAULT_WORK / "out", help="results.csv, summary.json, report.json")
    ap.add_argument("--work", type=Path, default=DEFAULT_WORK, help="object cache and the test DLL")
    ap.add_argument("--report", type=Path, help="failure report (default: <out>/report.json)")
    ap.add_argument("--sample", type=int, default=1000, help="functions to test; 0 tests every function of the pool")
    ap.add_argument("--runs", type=int, default=16, help="random inputs per function")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--kind", choices=("all", "leaf", "calls"), default="all", help="which functions to sample")
    ap.add_argument("--isa", choices=("auto", "arm", "all"), default="auto",
                    help="arm: only ARM-mode functions (the ARM lifter must not see Thumb code); all: ARM and Thumb; "
                         "auto: all when lift.py handles Thumb (it mentions CS_MODE_THUMB), else arm")
    ap.add_argument("--mode", choices=("any", "arm", "thumb"), default="any", help="restrict the sample to one ISA")
    ap.add_argument("--only", nargs="*", help="test these functions only: link addresses (hex) or names")
    ap.add_argument("--mutate", metavar="KIND|OLD=>NEW",
                    help="break translations on purpose and check that the run reports failures. KIND is one of "
                         f"{', '.join(MUTATIONS)} or mixed; each sampled function gets one defect and runs from a "
                         "separate copy (fm_<addr>), so the cached build stays valid. OLD=>NEW replaces that text "
                         "in every translation instead (slow: rebuilds all shards)")
    ap.add_argument("--unicorn-only", action="store_true",
                    help="run the originals in Unicorn alone (no lifter, no build): checks the Thumb entry path")
    args = ap.parse_args()

    t0 = time.time()
    image = Image(args.so)
    print(f"image loaded in {time.time() - t0:.0f}s: {sum(1 for f in image.functions if f.thumb)} Thumb and "
          f"{sum(1 for f in image.functions if not f.thumb)} ARM functions")
    dis = Disasm(image)
    args.out.mkdir(parents=True, exist_ok=True)
    report_path = args.report or (args.out / "report.json")

    sources, failed, dll, mutants, mutant_kind = {}, {}, None, {}, {}
    isa = args.isa
    if isa == "auto":
        isa = "all" if lifter_supports_thumb() else "arm"
    if args.unicorn_only:
        ents = {f.address: f for f in image.functions}
        pool_all = sorted(ents)
    else:
        print(f"isa {isa}: lift.py {'handles' if lifter_supports_thumb() else 'does not handle'} Thumb")
        ents = entries(image) if isa == "all" else entries(ImageView(image, [f for f in image.functions if not f.thumb]))
        sources, failed = lift_all(image, ents, args.work, isa)
        print(f"translated {len(sources)} of {len(ents)} entries in {time.time() - t0:.0f}s")
        pool_all = sorted(sources)

    def is_thumb(a):
        return bool(getattr(ents[a], "thumb", False))

    def calls_of(a):
        if a in sources:
            return bool(CALL_SRC.search(sources[a]))
        return dis.calls(ents[a])

    pool = pool_all
    if args.mode != "any":
        pool = [a for a in pool if is_thumb(a) == (args.mode == "thumb")]
    if args.kind != "all":
        pool = [a for a in pool if calls_of(a) == (args.kind == "calls")]

    # choose the functions
    rng = random.Random(args.seed)
    custom = None
    if args.mutate:
        if "=>" in args.mutate:
            custom = tuple(args.mutate.split("=>", 1))
        else:
            if args.mutate not in MUTATIONS and args.mutate != "mixed":
                raise SystemExit(f"--mutate: unknown kind {args.mutate}")
            if args.unicorn_only:
                raise SystemExit("--mutate needs the translations")
    if args.only:
        picks = parse_only(args.only, ents, image)
    else:
        want = args.sample if args.sample > 0 else len(pool)
        if args.mutate and not custom:
            # sample from the functions the mutation applies to
            order = pool[:]
            rng.shuffle(order)
            picks = []
            for a in order:
                new, kd = mutate_source(sources[a], args.mutate, len(picks))
                if new:
                    mutants[a], mutant_kind[a] = new, kd
                    picks.append(a)
                    if len(picks) >= want:
                        break
            picks.sort()
        else:
            picks = sorted(rng.sample(pool, min(want, len(pool))))
    print(f"pool {len(pool)} ({isa}/{args.mode}/{args.kind}), testing {len(picks)}")
    if args.mutate and not custom and args.only:
        for a in picks:
            if a in sources:
                new, kd = mutate_source(sources[a], args.mutate, len(mutants))
                if new:
                    mutants[a], mutant_kind[a] = new, kd

    if not args.unicorn_only:
        work = args.work if not args.mutate else args.work / "mutate"
        for old in work.glob("recomp-*.dll"):  # DLLs of earlier runs (best effort: one still loaded cannot be removed)
            try:
                old.unlink()
            except OSError:
                pass
        # one DLL per process, so two runs sharing --work cannot overwrite a library the other has loaded
        lib = build(image, ents, sources, work, custom, X86_TEST, mutants or None, f"recomp-{os.getpid()}.dll")
        dll = ctypes.CDLL(str(lib))
        print(f"built in {time.time() - t0:.0f}s")
    harness = Harness(image, dll, args.seed, ents)

    rows, counts, failures = [], collections.Counter(), []
    per_mn = collections.defaultdict(lambda: collections.Counter())  # mnemonic -> status -> functions
    tested_status = {}
    mut_stats = collections.defaultdict(collections.Counter)  # mutation kind -> outcome
    for a in picks:
        f = ents[a]
        thumb = is_thumb(a)
        kind = "calls" if calls_of(a) else "leaf"
        if args.mutate and not custom and a not in mutants:
            continue  # --only named a function the mutation kind does not apply to
        if not args.unicorn_only and a not in sources:
            status, completed, detail, rec = "untranslatable", 0, failed.get(a, "?"), None
        elif args.unicorn_only:
            status, completed, detail, rec = harness.test_unicorn(a, args.runs, thumb)
        else:
            status, completed, detail, rec = harness.test(a, args.runs, thumb, a in mutants)
        counts[status] += 1
        tested_status[a] = status
        mn = dis.of(f)
        for k in mn:
            per_mn[k][status] += 1
        if args.mutate:
            mut_stats[mutant_kind.get(a, "custom")][status] += 1
        rows.append({"address": f"0x{a:x}", "size": f.size, "kind": kind, "mode": "thumb" if thumb else "arm",
                     "status": status, "runs": completed, "detail": detail, "name": f.name})
        if status == "fail":
            print(f"fail 0x{a:x} {f.name[:60]}: {detail}")
            failures.append({"function": f.name, "address": f"0x{a:x}", "size": f.size,
                             "mode": "thumb" if thumb else "arm", "kind": kind, "mutation": mutant_kind.get(a),
                             "detail": detail, **(rec or {}), "mnemonics": dict(sorted(mn.items()))})
    if rows:
        with (args.out / "results.csv").open("w", newline="", encoding="utf-8") as fh:
            w = csv.DictWriter(fh, fieldnames=list(rows[0]), lineterminator="\n")
            w.writeheader()
            w.writerows(rows)
    nocov = collections.Counter()
    for r in rows:
        if r["status"] == "no-coverage":
            for k, n in json.loads(r["detail"]).items():
                nocov[k] += n

    # clustering: which instruction kinds are over-represented in the failing functions
    failing = {fl["address"] for fl in failures}
    n_fail = len(failures)
    n_run = sum(1 for r in rows if r["status"] in ("pass", "fail"))
    clusters = []
    for k, st in per_mn.items():
        fails, passes = st["fail"], st["pass"]
        if fails:
            clusters.append({"mnemonic": k, "failing_functions": fails, "passing_functions": passes,
                             "share_of_failures": round(fails / n_fail, 3),
                             "fail_rate": round(fails / (fails + passes), 3)})
    clusters.sort(key=lambda c: (-c["share_of_failures"], -c["fail_rate"]))
    common = None
    if failures:
        common = sorted(set.intersection(*(set(fl["mnemonics"]) for fl in failures)))
    untrans = collections.Counter(reason_key(w) for w in failed.values())
    summary = {"library": args.so.name, "isa": "unicorn-only" if args.unicorn_only else isa,
               "lifter_handles_thumb": lifter_supports_thumb(), "entries": len(ents), "translated": len(sources),
               "untranslatable_library_wide": len(failed),
               "untranslatable_reasons": dict(untrans.most_common(25)), "tested": len(picks),
               "pool": len(pool), "kind": args.kind, "mode": args.mode, "runs_per_function": args.runs,
               "seed": args.seed, "status_counts": dict(counts),
               "by_mode_kind": {f"{r['mode']}/{r['kind']}": 0 for r in rows},
               "incomplete_runs_by_reason": dict(nocov.most_common(25)), "seconds": round(time.time() - t0)}
    for r in rows:
        summary["by_mode_kind"][f"{r['mode']}/{r['kind']}"] += 1
    if args.mutate:
        tot = collections.Counter()
        for kd, st in mut_stats.items():
            tot.update(st)
        summary["mutation"] = {"mode": args.mutate, "by_kind": {k: dict(v) for k, v in mut_stats.items()},
                               "caught": tot["fail"], "missed_pass": tot["pass"], "no_coverage": tot["no-coverage"],
                               "caught_of_covered": round(tot["fail"] / max(1, tot["fail"] + tot["pass"]), 3)}
    (args.out / "summary.json").write_text(json.dumps(summary, indent=1) + "\n", encoding="utf-8")
    report = {"summary": summary, "failures": failures, "failing_mnemonic_clusters": clusters[:40],
              "mnemonics_in_every_failure": common}
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")

    line = (f"RESULT pass {counts['pass'] + counts['unicorn-ok']} fail {counts['fail']} no-coverage {counts['no-coverage']} "
            f"untranslatable {counts['untranslatable']} (library-wide {len(failed)} of {len(ents)}) "
            f"untranslated-callee {counts['untranslated-callee']}; tested {len(picks)}")
    if args.unicorn_only:
        line = f"RESULT unicorn-ok {counts['unicorn-ok']} no-coverage {counts['no-coverage']}; tested {len(picks)}"
    print(line)
    if args.mutate:
        m = summary["mutation"]
        print(f"MUTATION caught {m['caught']} of {m['caught'] + m['missed_pass']} covered mutants "
              f"({m['no_coverage']} without coverage): {json.dumps(m['by_kind'])}")
    print(f"report: {report_path}")


if __name__ == "__main__":
    main()
