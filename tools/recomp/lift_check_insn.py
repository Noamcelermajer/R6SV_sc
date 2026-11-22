"""Instruction-level differential fuzz of lift.ThumbLifter against Unicorn (independent of difftest.py).

Random encodings are wrapped as `[it] insn... ; bx lr` functions in a fake image, lifted, compiled with zig (checked build)
and run from the same random state in Unicorn (Thumb) and the C code; registers, flags, FPSCR NZCV, VFP registers and
scratch/stack memory are compared.

  py -I tools/recomp/lift_check_insn.py --seed 1 --count 3000 [--runs 4] [--it 0.3] [--only add,ldr] [--words h1:h2,...]
"""
import argparse, ctypes, hashlib, os, random, struct, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/recomp"))
from capstone import CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_ARM, Cs
from capstone import arm as A
from unicorn import UC_ARCH_ARM, UC_MODE_ARM, UC_PROT_ALL, UC_PROT_READ, UC_PROT_EXEC, Uc, UcError, UC_HOOK_CODE
from unicorn import arm_const as U

import lift
from image import Function, IMAGE_BASE

SCRATCH, SCRATCH_SIZE = 0x02000000, 0x10000
STACK, STACK_SIZE = 0x03000000, 0x10000
SP0 = STACK + 0x8000
SENTINEL = 0x04000000
HOST_SIZE = 0x04001000
CODE_LINK = 0x1000
WORK = ROOT / "build/lifter/fuzz"


class Cpu(ctypes.Structure):
    _fields_ = [("r", ctypes.c_uint32 * 16), ("n", ctypes.c_uint32), ("z", ctypes.c_uint32), ("c", ctypes.c_uint32),
                ("v", ctypes.c_uint32), ("fpscr", ctypes.c_uint32), ("fault", ctypes.c_uint32),
                ("budget", ctypes.c_uint32), ("memsize", ctypes.c_uint32), ("depth", ctypes.c_uint32),
                ("tls", ctypes.c_uint32), ("pad", ctypes.c_uint32), ("d", ctypes.c_uint64 * 32),
                ("trace", ctypes.c_uint32 * 64)]


class FakeImage:
    def __init__(self, size=0x100000):
        self.memory = bytearray(size)
        self.plt, self.imports, self.jump_tables = {}, {}, {}
        self.text_start, self.text_end = 0, size
        self.inner_entries = set()
        self.functions = []

    def u16(self, a):
        return self.memory[a] | self.memory[a + 1] << 8


def u16(b):
    return struct.unpack("<H", b)[0]


def rand_float_bits(rng):
    x = rng.choice((rng.uniform(-1000, 1000), rng.uniform(-1, 1), float(rng.randint(-100, 100)), 0.0, 1e30, -1e-30))
    return struct.unpack("<I", struct.pack("<f", x))[0]


def rand_double_bits(rng):
    x = rng.choice((rng.uniform(-1000, 1000), rng.uniform(-1, 1), float(rng.randint(-100, 100)), 0.0, 1e300))
    return struct.unpack("<Q", struct.pack("<d", x))[0]


def reg_value(rng):
    p = rng.random()
    if p < 0.35:
        return SCRATCH + 0x1000 + (rng.randrange(0, 0x4000) & ~3)
    if p < 0.40:
        return SCRATCH + 0x1000 + rng.randrange(0, 0x4000)
    if p < 0.55:
        return rng.randrange(0, 40)
    if p < 0.62:
        return rng.choice((0xFFFFFFFF, 0x80000000, 0x7FFFFFFF, 0xFFFF, 0x8000, 0xFF, 0x80, 0xFFFF8000, 0x10000))
    if p < 0.66:
        return (-rng.randrange(1, 40)) & 0xFFFFFFFF
    return rng.getrandbits(32)


def thumb_template(rng):
    """One random instruction (bytes) biased towards valid encodings."""
    r = rng.random()
    if r < 0.30:
        while True:
            hw = rng.getrandbits(16)
            if lift.thumb_size(hw) == 2:
                return struct.pack("<H", hw)
    fam = rng.choice([
        (0xE800, 0x1FF), (0xEA00, 0x1FF), (0xF000, 0x3FF), (0xF800, 0x1FF), (0xFA00, 0xFF), (0xFB00, 0xFF),
        (0xEC00, 0x3FF), (0xF000, 0x3FF), (0xF800, 0x1FF), (0xEA00, 0x1FF), (0xE800, 0x1FF), (0xEE00, 0x1FF),
        (0xED00, 0xFF), (0xEC40, 0x1F), (0xEE00, 0xFF),
    ])
    h1 = fam[0] | rng.getrandbits(16) & fam[1]
    h2 = rng.getrandbits(16)
    if fam[0] == 0xF000 and rng.random() < 0.6:
        h2 &= 0x7FFF
    if fam[0] in (0xEA00, 0xE800) and rng.random() < 0.5:
        h2 &= 0x7FFF
    if fam[0] in (0xEC00, 0xED00, 0xEE00, 0xEC40):
        h2 = (h2 & 0xF0FF) | (0xA00 | (rng.random() < 0.5) * 0x100)
        if fam[0] == 0xEE00 and rng.random() < 0.7:
            h2 &= ~0x10
    return struct.pack("<HH", h1, h2)


class Env:
    def __init__(self, thumb=True):
        self.thumb = thumb

    def build_uc(self, img):
        uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        uc.mem_map(IMAGE_BASE, 0x100000, UC_PROT_READ | UC_PROT_EXEC)
        uc.mem_write(IMAGE_BASE, bytes(img.memory))
        uc.mem_map(SCRATCH, SCRATCH_SIZE)
        uc.mem_map(STACK, STACK_SIZE)
        uc.mem_map(SENTINEL, 0x1000)
        try:
            uc.reg_write(U.UC_ARM_REG_C1_C0_2, uc.reg_read(U.UC_ARM_REG_C1_C0_2) | (0xF << 20))
        except Exception:
            pass
        uc.reg_write(U.UC_ARM_REG_FPEXC, 0x40000000)
        return uc


def compile_dll(srcs, entries, workdir):
    workdir.mkdir(parents=True, exist_ok=True)
    head = ('#include "recomp.h"\n#pragma clang diagnostic ignored "-Wunused-label"\n'
            '#pragma clang diagnostic ignored "-Wunused-variable"\n#pragma clang diagnostic ignored "-Wparentheses-equality"\n'
            '#pragma clang diagnostic ignored "-Wunused-but-set-variable"\n#define RC_EXPORT __declspec(dllexport)\n')
    protos = "".join(f"void f_{a:07x}(cpu_t *c, uint8_t *m);\n" for a in sorted(entries))
    body = [head, protos]
    body += [srcs[a] for a in sorted(srcs)]
    body += [f"void f_{a:07x}(cpu_t *c, uint8_t *m) {{ (void)m; c->fault = 7; }}" for a in sorted(entries) if a not in srcs]
    body.append("void rc_dispatch(cpu_t *c, uint8_t *m, uint32_t t) { (void)m; c->fault = RC_FAULT_DISPATCH; c->r[15] = t; }\n"
                "void rc_import(cpu_t *c, uint8_t *m, uint32_t slot) { (void)m; (void)slot; c->fault = 8; }\n"
                "void rc_hostcall(cpu_t *c, uint8_t *m, uint32_t slot) { (void)m; (void)slot; c->fault = 4; }\n"
                "void *rt_setjmp_prepare(cpu_t *c, uint32_t g) { static jmp_buf b; return &b; }\n"
                "RC_EXPORT void rc_run(uint32_t a, cpu_t *c, uint8_t *m) { switch (a) {\n" +
                "".join(f"case {IMAGE_BASE + a:#x}u: f_{a:07x}(c, m); break;\n" for a in sorted(srcs)) + "} }\n")
    text = "\n".join(body)
    h = hashlib.sha256((text + (ROOT / "tools/recomp/runtime/recomp.h").read_text()).encode()).hexdigest()[:12]
    cfile = workdir / f"fuzz_{h}.c"
    dll = workdir / f"fuzz_{h}.dll"
    if not dll.exists():
        cfile.write_text(text, encoding="utf-8")
        r = subprocess.run([sys.executable, "-m", "ziglang", "cc", "-shared", "-O1", "-target", "x86_64-windows-gnu",
                            "-DRECOMP_CHECKED", "-fwrapv", "-fno-strict-aliasing", "-ffp-contract=off",
                            f"-I{ROOT / 'tools/recomp/runtime'}", str(cfile), "-o", str(dll)], capture_output=True, text=True)
        if r.returncode:
            print(r.stderr[-6000:])
            raise SystemExit("compile failed " + str(cfile))
    return ctypes.CDLL(str(dll))


def run_unicorn(uc, addr, mem, regs, flags, fpscr, d, thumb):
    for a, data in mem.items():
        uc.mem_write(a, data)
    core = [getattr(U, f"UC_ARM_REG_R{k}") for k in range(13)] + [U.UC_ARM_REG_SP, U.UC_ARM_REG_LR]
    for k, r in enumerate(core):
        uc.reg_write(r, regs[k])
    uc.reg_write(U.UC_ARM_REG_CPSR, (0x10 | (flags << 28)) | (0x20 if thumb else 0))
    uc.reg_write(U.UC_ARM_REG_FPSCR, fpscr)
    for k in range(32):
        uc.reg_write(getattr(U, f"UC_ARM_REG_D{k}"), d[k])
    try:
        uc.emu_start(IMAGE_BASE + addr | (1 if thumb else 0), SENTINEL, count=200)
    except UcError as e:
        return None, str(e)
    pc = uc.reg_read(U.UC_ARM_REG_PC)
    if pc != SENTINEL:
        return None, f"stopped at {pc:#x}"
    return {"r": [uc.reg_read(r) for r in core], "f": uc.reg_read(U.UC_ARM_REG_CPSR) >> 28,
            "fp": uc.reg_read(U.UC_ARM_REG_FPSCR) >> 28,
            "d": [uc.reg_read(getattr(U, f"UC_ARM_REG_D{k}")) & 0xFFFFFFFFFFFFFFFF for k in range(32)],
            "mem": {SCRATCH: bytes(uc.mem_read(SCRATCH, SCRATCH_SIZE)), STACK: bytes(uc.mem_read(STACK, STACK_SIZE))}}, None


def is_nan32(u):
    return (u & 0x7F800000) == 0x7F800000 and (u & 0x007FFFFF) != 0


def is_nan64(u):
    return (u & 0x7FF0000000000000) == 0x7FF0000000000000 and (u & 0xFFFFFFFFFFFFF) != 0


def same_d(a, b):
    if a == b:
        return True
    ok = True
    for sh in (0, 32):
        x, y = (a >> sh) & 0xFFFFFFFF, (b >> sh) & 0xFFFFFFFF
        if x != y and not (is_nan32(x) and is_nan32(y)):
            ok = False
    return ok or (is_nan64(a) and is_nan64(b))


def compare(u, h):
    if h["fault"]:
        return f"host fault {h['fault']}"
    for k in range(15):
        x, y = u["r"][k], h["r"][k]
        if x != y and not (is_nan32(x) and is_nan32(y)):
            return f"r{k}: unicorn {x:#x} host {y:#x}"
    if (h["r"][15] & ~1) != SENTINEL:
        return f"pc {h['r'][15]:#x}"
    if u["f"] & 0xF != h["f"]:
        return f"nzcv: unicorn {u['f'] & 0xF:04b} host {h['f']:04b}"
    if u["fp"] != h["fp"]:
        return f"fpscr nzcv: unicorn {u['fp']:04b} host {h['fp']:04b}"
    for k in range(32):
        if not same_d(u["d"][k], h["d"][k]):
            return f"d{k}: unicorn {u['d'][k]:#018x} host {h['d'][k]:#018x}"
    for a, data in u["mem"].items():
        o = h["mem"][a]
        if data != o:
            for j in range(0, len(data), 4):
                if data[j:j + 4] != o[j:j + 4] and not (is_nan32(int.from_bytes(data[j:j + 4], "little")) and is_nan32(int.from_bytes(o[j:j + 4], "little"))):
                    return f"memory {a + j:#x}: unicorn {data[j:j + 4].hex()} host {o[j:j + 4].hex()}"
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--count", type=int, default=2000)
    ap.add_argument("--runs", type=int, default=6)
    ap.add_argument("--it", type=float, default=0.25, help="share of cases wrapped in an IT block")
    ap.add_argument("--only", help="comma list of mnemonics to keep (capstone names)")
    ap.add_argument("--words", help="hex encodings (h1:h2 or h1) to test instead of random ones")
    ap.add_argument("--mutate", help="OLD=>NEW: break every translation on purpose; the run must then report failures")
    args = ap.parse_args()
    rng = random.Random(args.seed)
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    md.detail = True
    only = set(args.only.split(",")) if args.only else None

    img = FakeImage()
    ents = {}
    cases = []  # (addr, bytes, description)
    addr = CODE_LINK
    tried = unsupported = 0
    unsup = {}
    t0 = time.time()
    fixed = []
    if args.words:
        for w in args.words.split(","):
            parts = [int(x, 16) for x in w.split(":")]
            fixed.append(b"".join(struct.pack("<H", p) for p in parts))
    while len(cases) < args.count:
        if args.words and not fixed:
            break
        use_it = rng.random() < args.it
        blob = b""
        if args.words:
            blob = fixed.pop(0)
        elif use_it:
            n = rng.randint(1, 4)
            fc = rng.randrange(0, 14)
            mask_bits = [rng.randrange(2) for _ in range(n - 1)]
            # mask: bit for instruction j (j>=2): 1 = else (opposite of firstcond's low bit pattern) - build the byte
            mask = 0
            for j, b in enumerate(mask_bits):
                mask |= b << (3 - j)
            mask |= 1 << (3 - (n - 1))
            # an instruction is "then" when its cond low bit equals firstcond bit 0
            it = 0xBF00 | fc << 4 | mask
            body = b""
            ok = True
            for _ in range(n):
                ins = thumb_template(rng)
                body += ins
            blob = struct.pack("<H", it) + body
        else:
            blob = thumb_template(rng)
        # validate: every instruction decodes with the same size, no branches, no pc
        off, ok, descr = 0, True, []
        while off < len(blob):
            sz = lift.thumb_size(u16(blob[off:off + 2]))
            if off + sz > len(blob):
                ok = False
                break
            got = next(md.disasm(blob[off:off + sz], 0x1000 + off, 1), None)
            if got is None or got.size != sz:
                ok = False
                break
            nm = got.insn_name()
            if nm in ("b", "bl", "blx", "bx", "cbz", "cbnz", "tbb", "tbh", "svc", "udf", "bkpt", "hint", "setend", "cps", "dmb", "dsb", "isb", "mrs", "msr", "ldrex", "strex", "ldrexb", "strexb", "ldrexh", "strexh", "ldrexd", "strexd", "clrex", "pld", "pldw", "pli"):
                ok = False
                break
            if nm.startswith("it") and (off > 0 or len(blob) == sz):
                ok = False
            if nm in ("ldrd", "strd") and got.writeback:
                ok = False
            for o in got.operands:
                if o.type == A.ARM_OP_REG and o.reg == A.ARM_REG_PC:
                    ok = False
                if o.type == A.ARM_OP_MEM and o.mem.base == A.ARM_REG_PC:
                    pass
            if only and nm not in only and not nm.startswith("it"):
                ok = False
            descr.append(f"{got.mnemonic} {got.op_str}")
            off += sz
        tried += 1
        if not ok:
            continue
        full = blob + b"\x70\x47"  # bx lr
        a = addr
        addr += (len(full) + 7) & ~7
        img.memory[a:a + len(full)] = full
        f = Function(a, len(full), f"t{a:x}", [], True)
        ents[a] = f
        cases.append((a, full, "; ".join(descr)))
    L = lift.Lifter(img, ents)
    srcs = {}
    kept = []
    for a, full, descr in cases:
        try:
            srcs[a] = L.lift(ents[a])
            kept.append((a, full, descr))
        except lift.Unsupported as e:
            key = str(e).split(" at ")[0]
            unsup.setdefault(key, []).append(descr)
    if args.mutate:
        old, new = args.mutate.split("=>", 1)
        srcs = {a: t.replace(old, new) for a, t in srcs.items()}
    print(f"cases {len(cases)} (tried {tried}), lifted {len(kept)}, {time.time() - t0:.1f}s")
    for k, v in sorted(unsup.items(), key=lambda kv: -len(kv[1])):
        print(f"  unsupported {len(v):4d} {k}   e.g. {v[0]}")
    dll = compile_dll(srcs, ents, WORK)
    print(f"compiled, {time.time() - t0:.1f}s")

    env = Env()
    uc = env.build_uc(img)
    host = ctypes.create_string_buffer(HOST_SIZE)
    base = ctypes.addressof(host)
    ctypes.memmove(base + IMAGE_BASE, bytes(img.memory), len(img.memory))
    fails = {}
    nrun = npass = 0
    tally = {}
    for a, full, descr in kept:
        done = 0
        bad = None
        for k in range(args.runs):
            r2 = random.Random(a * 7919 + k + args.seed * 1000003)
            scratch = bytearray()
            for _ in range(SCRATCH_SIZE // 4):
                p = r2.random()
                w = (SCRATCH + 0x1000 + (r2.randrange(0, 0x4000) & ~3)) if p < 0.3 else (rand_float_bits(r2) if p < 0.5 else r2.getrandbits(32) if p < 0.8 else r2.randrange(0, 64))
                scratch += struct.pack("<I", w)
            stack = bytes(r2.getrandbits(8) for _ in range(256)) * (STACK_SIZE // 256)
            mem = {SCRATCH: bytes(scratch), STACK: stack}
            regs = [reg_value(r2) for _ in range(13)] + [SP0, SENTINEL | 1]
            flags = r2.getrandbits(4)
            fpscr = r2.getrandbits(4) << 28
            d = []
            for _ in range(32):
                p = r2.random()
                d.append(rand_double_bits(r2) if p < 0.4 else (rand_float_bits(r2) | rand_float_bits(r2) << 32) if p < 0.9 else r2.getrandbits(64))
            u, why = run_unicorn(uc, a, mem, regs, flags, fpscr, d, True)
            if u is None:
                continue
            for aa, data in mem.items():
                ctypes.memmove(base + aa, data, len(data))
            c = Cpu()
            for j in range(15):
                c.r[j] = regs[j]
            c.n, c.z, c.c, c.v = (flags >> 3) & 1, (flags >> 2) & 1, (flags >> 1) & 1, flags & 1
            c.fpscr, c.budget, c.memsize, c.tls = fpscr, 100000, HOST_SIZE, 0
            for j in range(32):
                c.d[j] = d[j]
            dll.rc_run(ctypes.c_uint32(IMAGE_BASE + a), ctypes.byref(c), host)
            h = {"r": list(c.r), "f": (c.n << 3) | (c.z << 2) | (c.c << 1) | c.v, "fp": c.fpscr >> 28, "d": list(c.d),
                 "fault": c.fault,
                 "mem": {SCRATCH: ctypes.string_at(base + SCRATCH, SCRATCH_SIZE), STACK: ctypes.string_at(base + STACK, STACK_SIZE)}}
            diff = compare(u, h)
            done += 1
            if diff:
                bad = f"run {k}: {diff} | regs " + " ".join(f"{x:#x}" for x in regs) + f" flags {flags:04b}"
                break
        nrun += 1
        if bad:
            fails.setdefault((descr.split(";")[1] if descr.startswith("it") and ";" in descr else descr).split()[0].strip(), []).append((a, descr, bad, full.hex()))
        elif done:
            npass += 1
            for part in descr.split(";"):
                nm = part.split()[0].split(".")[0] if part.split() else ""
                tally[nm] = tally.get(nm, 0) + 1
    print("passed by mnemonic:", " ".join(f"{k}={v}" for k, v in sorted(tally.items())))
    print(f"ran {nrun}, passed {npass}, failed {sum(len(v) for v in fails.values())}, {time.time() - t0:.1f}s")
    for k, v in sorted(fails.items(), key=lambda kv: -len(kv[1])):
        print(f"FAIL {k}: {len(v)}")
        for a, descr, bad, hx in v[:3]:
            print(f"    {descr}   [{hx}]   {bad}")


if __name__ == "__main__":
    main()
