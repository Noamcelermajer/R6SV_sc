"""Whole-library differential test of the lifter (independent of difftest.py): every entry compiled into one checked DLL, sampled functions run in
Unicorn (Thumb or ARM, picked by Function.thumb) and in the DLL from the same random state.

  py -I tools/recomp/lift_check_lib.py build   # lift + compile all shards into build/lifter/full (about 7 min, cached)
  py -I tools/recomp/lift_check_lib.py run --sample 400 --seed 1 --kind leaf|calls|all [--arm]
  py -I tools/recomp/lift_check_lib.py pickle  # cache the loaded Image in build/lifter/image.pkl
"""
import sys, os, pickle, time, ctypes, random, struct, hashlib, subprocess, concurrent.futures, json, argparse
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/recomp"))
from unicorn import UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_PROT_EXEC, UC_PROT_READ, Uc, UcError
from unicorn import arm_const as U
import lift, models
from image import IMAGE_BASE, IMPORT_STUB_BASE

WORK = ROOT / "build/lifter/full"
SHARDS = 64
SCRATCH, SCRATCH_SIZE = 0x02000000, 0x100000
STACK, STACK_SIZE = 0x03000000, 0x100000
SP = STACK + 0xF0000
SENTINEL = 0x04000000
KUSER = 0xFFFF0000
HOST_SIZE = models.HEAP + models.HEAP_SIZE
RUNTIME = ROOT / "tools/recomp/runtime"
BX_LR = (0xE12FFF1E).to_bytes(4, "little")

sys.path.insert(0, str(ROOT / "tools/recomp"))
from lift_check_insn import Cpu, rand_float_bits, rand_double_bits, same_d, is_nan32, is_nan64


def load():
    """The library as an Image; a pickle of it in build/lifter/image.pkl (made by `lift_check_lib.py pickle`) saves 40 s."""
    pk = ROOT / "build/lifter/image.pkl"
    if pk.exists():
        img = pickle.load(open(pk, "rb"))
    else:
        from image import Image
        img = Image(ROOT / "original/apk/lib/armeabi-v7a/librainbowsix.so")
    ents = lift.entries(img)
    return img, ents


def build_all(img, ents):
    t0 = time.time()
    L = lift.Lifter(img, ents)
    srcs = {}
    for a, f in ents.items():
        try:
            srcs[a] = L.lift(f)
        except lift.Unsupported as e:
            print("unsupported", hex(a), e)
    print(f"lifted {len(srcs)}/{len(ents)} {time.time()-t0:.0f}s", flush=True)
    WORK.mkdir(parents=True, exist_ok=True)
    protos = "".join(f"void f_{a:07x}(cpu_t *c, uint8_t *m);\n" for a in sorted(ents))
    (WORK / "protos.h").write_text(protos)
    head = ('#include "recomp.h"\n#include "protos.h"\n#pragma clang diagnostic ignored "-Wunused-label"\n'
            '#pragma clang diagnostic ignored "-Wunused-variable"\n#pragma clang diagnostic ignored "-Wparentheses-equality"\n'
            '#pragma clang diagnostic ignored "-Wunused-but-set-variable"\n')
    addrs = sorted(ents)
    files = []
    for k in range(SHARDS):
        body = [head]
        for a in addrs[k::SHARDS]:
            body.append(srcs.get(a) or f"void f_{a:07x}(cpu_t *c, uint8_t *m) {{ (void)m; c->fault = RC_FAULT_UNLIFTED; }}")
        files.append((WORK / f"shard{k:02d}.c", "\n".join(body) + "\n"))
    disp = [head, "#define RC_EXPORT __declspec(dllexport)",
            "static void (*handler)(cpu_t *, uint8_t *, uint32_t);",
            "RC_EXPORT void rc_set_import_handler(void *h) { handler = (void (*)(cpu_t *, uint8_t *, uint32_t))h; }",
            "void rc_import(cpu_t *c, uint8_t *m, uint32_t slot) { handler(c, m, slot); c->r[15] = c->r[14]; }",
            "void rc_hostcall(cpu_t *c, uint8_t *m, uint32_t slot) { (void)m; (void)slot; c->fault = RC_FAULT_DISPATCH; }",
            "void *rt_setjmp_prepare(cpu_t *c, uint32_t g) { static jmp_buf b; return &b; }",
            "RC_EXPORT void rc_run(uint32_t addr, cpu_t *c, uint8_t *m) { rc_dispatch(c, m, addr); }",
            "void rc_dispatch(cpu_t *c, uint8_t *m, uint32_t t) {",
            f"  if (t >= {IMPORT_STUB_BASE:#x}u && t < {IMPORT_STUB_BASE + 4 * len(img.imports):#x}u) {{ rc_import(c, m, (t - {IMPORT_STUB_BASE:#x}u) / 4); return; }}",
            "  if (rc_kuser(c, m, t)) return;", "  switch (t) {"]
    for a in addrs:
        disp.append(f"  case {IMAGE_BASE + a:#x}u: f_{a:07x}(c, m); return;")
    slots = {n: k for k, n in enumerate(img.imports)}
    for a, (name, _) in sorted(img.plt.items()):
        disp.append(f"  case {IMAGE_BASE + a:#x}u: rc_import(c, m, {slots[name]}); return;")
    disp += ["  }", "  c->fault = RC_FAULT_DISPATCH; c->r[15] = t;", "}"]
    files.append((WORK / "dispatch.c", "\n".join(disp) + "\n"))
    flags = ["-O1", "-target", "x86_64-windows-gnu", "-DRECOMP_CHECKED", "-fwrapv", "-fno-strict-aliasing", "-ffp-contract=off",
             f"-I{RUNTIME}", f"-I{WORK}"]
    key = (RUNTIME / "recomp.h").read_text() + protos + " ".join(flags)

    def one(item):
        path, text = item
        dg = hashlib.sha256((text + key).encode()).hexdigest()[:16]
        obj = path.with_suffix(f".{dg}.o")
        if not obj.exists():
            path.write_text(text, encoding="utf-8")
            r = subprocess.run([sys.executable, "-m", "ziglang", "cc", "-c", *flags, str(path), "-o", str(obj)], capture_output=True, text=True)
            if r.returncode:
                return path.name, r.stderr[-3000:]
        return obj, None
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        res = list(pool.map(one, files))
    errs = [(n, e) for n, e in res if isinstance(n, str) and e]
    for n, e in errs:
        print("COMPILE ERROR", n, e)
    if errs:
        raise SystemExit(1)
    objs = [str(o) for o, _ in res]
    lib = WORK / "full.dll"
    r = subprocess.run([sys.executable, "-m", "ziglang", "cc", "-shared", "-target", "x86_64-windows-gnu", *objs, "-o", str(lib)], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("link failed " + r.stderr[-3000:])
    print(f"built {lib} {time.time()-t0:.0f}s", flush=True)
    pickle.dump(set(srcs), open(WORK / "lifted.pkl", "wb"))
    pickle.dump({a: ("rc_import" in s or "RC_DISPATCH" in s or "; f_" in s) for a, s in srcs.items()}, open(WORK / "calls.pkl", "wb"))


def scratch_image(rng, lo, hi):
    from array import array
    words = array("I")
    for _ in range(SCRATCH_SIZE // 4):
        p = rng.random()
        if p < 0.40: w = SCRATCH + (rng.randrange(0, 0x40000) & ~3)
        elif p < 0.60: w = rng.randrange(0, 65)
        elif p < 0.75: w = 0
        elif p < 0.85: w = rand_float_bits(rng)
        elif p < 0.90: w = lo + (rng.randrange(0, hi - lo) & ~3)
        else: w = rng.getrandbits(32)
        words.append(w)
    return words.tobytes()


def reg_value(rng, lo, hi):
    p = rng.random()
    if p < 0.45: return SCRATCH + (rng.randrange(0, 0x40000) & ~3)
    if p < 0.65: return rng.randrange(0, 65)
    if p < 0.75: return 0
    if p < 0.90: return rng.getrandbits(32)
    return lo + (rng.randrange(0, hi - lo) & ~3)


class UcM(models.Machine):
    def __init__(self, uc, writable):
        self.uc = uc
        self.regs = [getattr(U, f"UC_ARM_REG_R{k}") for k in range(13)] + [U.UC_ARM_REG_SP, U.UC_ARM_REG_LR]
        super().__init__(writable)
    def reg(self, k): return self.uc.reg_read(self.regs[k])
    def set_reg(self, k, v): self.uc.reg_write(self.regs[k], v)
    def read(self, a, n): return bytes(self.uc.mem_read(a, n))
    def write(self, a, data): self.uc.mem_write(a, data)


class HostM(models.Machine):
    def __init__(self, host, writable):
        self.base = ctypes.addressof(host); self.cpu = None
        super().__init__(writable)
    def reg(self, k): return self.cpu.r[k]
    def set_reg(self, k, v): self.cpu.r[k] = v
    def check(self, a, n):
        if a < 0x00100000 or a + n > HOST_SIZE: raise models.Unmodelled(f"host access {a:#x}")
    def read(self, a, n): self.check(a, n); return ctypes.string_at(self.base + a, n)
    def write(self, a, data): self.check(a, len(data)); ctypes.memmove(self.base + a, data, len(data))


IMPORT_HANDLER = ctypes.CFUNCTYPE(None, ctypes.POINTER(Cpu), ctypes.c_void_p, ctypes.c_uint32)


class Harness:
    def __init__(self, img, dll, seed):
        self.img, self.dll = img, dll
        self.names = list(img.imports)
        rw = IMAGE_BASE + (img.writable[0][0] & ~0xFFF)
        self.writable = [(rw, IMAGE_BASE + img.size)]
        self.lo, self.hi = IMAGE_BASE + img.writable[0][0], IMAGE_BASE + img.writable[-1][1]
        self.scratch = [scratch_image(random.Random(seed * 100 + k), self.lo, self.hi) for k in range(8)]
        self.regions = self.writable + [(SCRATCH, SCRATCH + SCRATCH_SIZE), (STACK, STACK + STACK_SIZE), (models.HEAP, models.HEAP + models.HEAP_SIZE)]
        self.base_image = bytes(img.memory)
        uc = self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        uc.mem_map(IMAGE_BASE, rw - IMAGE_BASE, UC_PROT_READ | UC_PROT_EXEC)
        uc.mem_map(rw, IMAGE_BASE + img.size - rw)
        uc.mem_map(SCRATCH, SCRATCH_SIZE); uc.mem_map(STACK, STACK_SIZE); uc.mem_map(models.HEAP, models.HEAP_SIZE)
        uc.mem_write(IMAGE_BASE, self.base_image)
        uc.mem_map(IMPORT_STUB_BASE, 0x1000, UC_PROT_READ | UC_PROT_EXEC); uc.mem_write(IMPORT_STUB_BASE, BX_LR * 0x400)
        uc.mem_map(KUSER, 0x1000, UC_PROT_READ | UC_PROT_EXEC); uc.mem_write(KUSER, BX_LR * 0x400)
        uc.hook_add(UC_HOOK_CODE, self._imp, begin=IMPORT_STUB_BASE, end=IMPORT_STUB_BASE + 0xFFF)
        uc.hook_add(UC_HOOK_CODE, self._kuser, begin=KUSER, end=KUSER + 0xFFF)
        try:
            uc.reg_write(U.UC_ARM_REG_C1_C0_2, uc.reg_read(U.UC_ARM_REG_C1_C0_2) | (0xF << 20))
        except Exception:
            pass
        uc.reg_write(U.UC_ARM_REG_FPEXC, 0x40000000)
        self.ucm = UcM(uc, self.regions)
        self.cpsr0 = uc.reg_read(U.UC_ARM_REG_CPSR) & 0x0FFFFFFF & ~0x220
        self.host = ctypes.create_string_buffer(HOST_SIZE)
        ctypes.memmove(ctypes.addressof(self.host) + IMAGE_BASE, self.base_image, len(self.base_image))
        ctypes.memmove(ctypes.addressof(self.host) + IMPORT_STUB_BASE, BX_LR * 0x400, 0x1000)
        self.hm = HostM(self.host, self.regions)
        self._h = IMPORT_HANDLER(self._host_import)
        dll.rc_set_import_handler(self._h)
        self.core = [getattr(U, f"UC_ARM_REG_R{k}") for k in range(13)] + [U.UC_ARM_REG_SP, U.UC_ARM_REG_LR]
        self.dregs = [getattr(U, f"UC_ARM_REG_D{k}") for k in range(32)]

    def _imp(self, uc, address, size, _):
        name = self.names[(address - IMPORT_STUB_BASE) // 4]
        try:
            models.run_import(self.ucm, name)
        except (models.Unmodelled, UcError) as e:
            self.abort = f"import {name}: {e}"
            uc.emu_stop()

    def _kuser(self, uc, address, size, _):
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
            models.run_import(self.hm, self.names[slot])
        except Exception:
            cpu.contents.fault = 8

    def snapshot(self, v):
        mem = {a: self.base_image[a - IMAGE_BASE:b - IMAGE_BASE] for a, b in self.writable}
        mem[SCRATCH] = self.scratch[v]
        mem[STACK] = self.scratch[(v + 3) % 8]
        mem[models.HEAP] = bytes(models.HEAP_SIZE)
        return mem

    def state(self, rng, thumb):
        regs = [reg_value(rng, self.lo, self.hi) for _ in range(13)] + [SP, SENTINEL | (1 if thumb else 0)]
        d = []
        for _ in range(32):
            p = rng.random()
            d.append(rand_double_bits(rng) if p < 0.45 else (rand_float_bits(rng) | rand_float_bits(rng) << 32) if p < 0.9 else rng.getrandbits(64))
        return regs, rng.getrandbits(4), rng.getrandbits(4) << 28, d

    def run_uc(self, addr, thumb, mem, regs, flags, fpscr, d):
        uc = self.uc
        for a, data in mem.items():
            uc.mem_write(a, data)
        for k, r in enumerate(self.core):
            uc.reg_write(r, regs[k])
        uc.reg_write(U.UC_ARM_REG_CPSR, self.cpsr0 | (flags << 28))
        uc.reg_write(U.UC_ARM_REG_FPSCR, fpscr)
        for k, r in enumerate(self.dregs):
            uc.reg_write(r, d[k])
        self.ucm.reset()
        self.abort = None
        try:
            uc.emu_start((IMAGE_BASE + addr) | (1 if thumb else 0), SENTINEL, count=2_000_000)
        except UcError as e:
            return None, f"unicorn: {e}"
        if self.abort:
            return None, self.abort
        pc = uc.reg_read(U.UC_ARM_REG_PC)
        if pc != SENTINEL:
            return None, "unicorn stopped"
        return {"r": [uc.reg_read(r) for r in self.core] + [pc], "f": uc.reg_read(U.UC_ARM_REG_CPSR) >> 28,
                "fp": uc.reg_read(U.UC_ARM_REG_FPSCR) >> 28, "d": [uc.reg_read(r) & 0xFFFFFFFFFFFFFFFF for r in self.dregs],
                "mem": {a: bytes(uc.mem_read(a, b - a)) for a, b in self.regions}}, None

    def run_host(self, addr, mem, regs, flags, fpscr, d):
        base = ctypes.addressof(self.host)
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
        self.dll.rc_run(ctypes.c_uint32(IMAGE_BASE + addr), ctypes.byref(c), self.host)
        return {"r": list(c.r), "f": (c.n << 3) | (c.z << 2) | (c.c << 1) | c.v, "fp": c.fpscr >> 28, "d": list(c.d),
                "mem": {a: ctypes.string_at(base + a, b - a) for a, b in self.regions}, "fault": c.fault}

    def compare(self, u, h, thumb):
        if h["fault"]:
            return f"host fault {h['fault']}"
        for k in range(15):
            x, y = u["r"][k], h["r"][k]
            if x != y and not (is_nan32(x) and is_nan32(y)):
                return f"r{k}: unicorn {x:#x} host {y:#x}"
        if (h["r"][15] & ~1) != SENTINEL:
            return f"pc host {h['r'][15]:#x}"
        if u["f"] != h["f"]:
            return f"nzcv: unicorn {u['f']:04b} host {h['f']:04b}"
        if u["fp"] != h["fp"]:
            return f"fpscr nzcv: unicorn {u['fp']:04b} host {h['fp']:04b}"
        for k in range(32):
            if not same_d(u["d"][k], h["d"][k]):
                return f"d{k}: unicorn {u['d'][k]:#018x} host {h['d'][k]:#018x}"
        for a, data in u["mem"].items():
            o = h["mem"][a]
            if data == o:
                continue
            for j in range(0, len(data), 4):
                if data[j:j + 4] == o[j:j + 4]:
                    continue
                x, y = int.from_bytes(data[j:j + 4], "little"), int.from_bytes(o[j:j + 4], "little")
                if is_nan32(x) and is_nan32(y):
                    continue
                k = j & ~7
                if is_nan64(int.from_bytes(data[k:k + 8], "little")) and is_nan64(int.from_bytes(o[k:k + 8], "little")):
                    continue
                return f"memory {a + j:#x}: unicorn {data[j:j + 4].hex()} host {o[j:j + 4].hex()}"
        return None

    def test(self, addr, thumb, runs):
        done, notes = 0, {}
        for k in range(runs):
            rng = random.Random(addr * 1000 + k)
            mem = self.snapshot(k % 8)
            regs, flags, fpscr, d = self.state(rng, thumb)
            u, why = self.run_uc(addr, thumb, mem, regs, flags, fpscr, d)
            if u is None:
                key = why.split(" at ")[0].split(" (")[0][:60]
                notes[key] = notes.get(key, 0) + 1
                continue
            h = self.run_host(addr, mem, regs, flags, fpscr, d)
            if h["fault"] == 7:
                return "untranslated-callee", done, ""
            diff = self.compare(u, h, thumb)
            if diff:
                return "fail", done, f"run {k}: {diff}"
            done += 1
        if done < 4:
            return "no-coverage", done, json.dumps(notes)
        return "pass", done, ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=("build", "run", "pickle"))
    ap.add_argument("--sample", type=int, default=400)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--runs", type=int, default=12)
    ap.add_argument("--kind", default="all")
    ap.add_argument("--only", type=lambda s: int(s, 16), nargs="*")
    ap.add_argument("--arm", action="store_true", help="sample ARM-mode functions only")
    a = ap.parse_args()
    if a.cmd == "pickle":
        from image import Image
        img = Image(ROOT / "original/apk/lib/armeabi-v7a/librainbowsix.so")
        img.elf = None
        img._d = None
        (ROOT / "build/lifter").mkdir(parents=True, exist_ok=True)
        pickle.dump(img, open(ROOT / "build/lifter/image.pkl", "wb"), protocol=4)
        return
    img, ents = load()
    if a.cmd == "build":
        build_all(img, ents)
        return
    lifted = pickle.load(open(WORK / "lifted.pkl", "rb"))
    calls = pickle.load(open(WORK / "calls.pkl", "rb"))
    dll = ctypes.CDLL(str(WORK / "full.dll"))
    H = Harness(img, dll, a.seed)
    pool = sorted(x for x in lifted if (not ents[x].thumb) == a.arm)
    if a.kind == "leaf":
        pool = [x for x in pool if not calls[x]]
    elif a.kind == "calls":
        pool = [x for x in pool if calls[x]]
    picks = sorted(a.only) if a.only else sorted(random.Random(a.seed).sample(pool, min(a.sample, len(pool))))
    counts = {}
    t0 = time.time()
    for x in picks:
        f = ents[x]
        st, done, detail = H.test(x, f.thumb, a.runs)
        kind = "calls" if calls.get(x) else "leaf"
        counts.setdefault(kind, {})
        counts[kind][st] = counts[kind].get(st, 0) + 1
        if st == "fail":
            print(f"fail {x:#x} {f.name[:60]} ({'T' if f.thumb else 'A'}): {detail}", flush=True)
    print(json.dumps(counts), f"{time.time() - t0:.0f}s")


main()
