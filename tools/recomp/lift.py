"""Translate ARM32 functions of the original library (Thumb-2 and ARM mode, VFP) into C.

`Lifter(image, entries).lift(f)` picks the translator by `f.thumb`: `ThumbLifter` (Thumb-1 and Thumb-2, the code of
almost the whole library) or the ARM-mode translator in `Lifter` itself (the _JNIEnv wrappers, ALicenseCheck and the
PLT-style veneers). Both emit the same C model, described here.

One C function per original function: `void f_<addr>(cpu_t *c, uint8_t *m)`. Core registers and
flags are locals (loaded on entry, stored on exit); VFP registers stay in `c->vfp`. Every instruction
gets a label `L_<addr>`; branches inside the function become `goto`. A write to PC ends the function
with the new PC in `c->r[15]`.

Calls store the register locals into `c`, call the target's C function (or `rc_import` for an import,
`rc_dispatch` for a computed target) and reload them. A computed jump goes through a switch over the
function's own labels; a target outside the function is a tail call through `rc_dispatch`. Branches
into the middle of another function make that address an entry of its own (see `entries`).
"""
from __future__ import annotations

import bisect
import re
import struct

from capstone import CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB, Cs
from capstone import arm as A

from image import IMAGE_BASE, Function, Image


class Unsupported(Exception):
    pass


CORE = {"r0": 0, "r1": 1, "r2": 2, "r3": 3, "r4": 4, "r5": 5, "r6": 6, "r7": 7, "r8": 8, "sb": 9, "sl": 10,
        "fp": 11, "ip": 12, "sp": 13, "lr": 14, "pc": 15}
COND = {
    A.ARM_CC_EQ: "z", A.ARM_CC_NE: "!z", A.ARM_CC_HS: "cf", A.ARM_CC_LO: "!cf", A.ARM_CC_MI: "n", A.ARM_CC_PL: "!n",
    A.ARM_CC_VS: "v", A.ARM_CC_VC: "!v", A.ARM_CC_HI: "(cf && !z)", A.ARM_CC_LS: "(!cf || z)",
    A.ARM_CC_GE: "(n == v)", A.ARM_CC_LT: "(n != v)", A.ARM_CC_GT: "(!z && n == v)", A.ARM_CC_LE: "(z || n != v)",
}
IMM_SHIFTS = {A.ARM_SFT_ASR: 1, A.ARM_SFT_LSL: 2, A.ARM_SFT_LSR: 3, A.ARM_SFT_ROR: 4, A.ARM_SFT_RRX: 5}
REG_SHIFTS = {A.ARM_SFT_ASR_REG: 1, A.ARM_SFT_LSL_REG: 2, A.ARM_SFT_LSR_REG: 3, A.ARM_SFT_ROR_REG: 4}
SHIFT_ALIAS = {"asr": 1, "lsl": 2, "lsr": 3, "ror": 4, "rrx": 5}
LOGICAL = {"and", "eor", "orr", "bic", "mov", "mvn", "tst", "teq"}
ARITH = {"add", "adc", "sub", "sbc", "rsb", "rsc", "cmp", "cmn"}
COMPARE = {"tst", "teq", "cmp", "cmn"}
LOADS = {"ldr": (32, False), "ldrb": (8, False), "ldrh": (16, False), "ldrsb": (8, True), "ldrsh": (16, True)}
STORES = {"str": 32, "strb": 8, "strh": 16}
MULTI = {"ldm": ("ia", True), "ldmib": ("ib", True), "ldmda": ("da", True), "ldmdb": ("db", True),
         "stm": ("ia", False), "stmib": ("ib", False), "stmda": ("da", False), "stmdb": ("db", False)}
VMULTI = {"vldmia": ("ia", True), "vldmdb": ("db", True), "vstmia": ("ia", False), "vstmdb": ("db", False)}


def hexu(v: int) -> str:
    return f"0x{v & 0xFFFFFFFF:08x}u"


# ---- hand-written replacements -----------------------------------------------------------------------
# Code the lifter does not translate gets a C body instead (address -> (name, size, C body)). The body receives the
# CPU state like a translated function and returns to LR. This library has none (no NEON, no hand-written assembly);
# the mechanism stays for the day an override is needed.
OVERRIDES: dict = {}


class Lifter:
    def __init__(self, image: Image, entries: dict | None = None):
        self.image = image
        self.md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
        self.md.detail = True
        self.entries = entries if entries is not None else {f.address: f for f in image.functions}
        self.slots = {name: k for k, name in enumerate(image.imports)}
        self.thumb = ThumbLifter(image, self.entries)

    # ---- per function -------------------------------------------------------------------------
    def lift(self, f: Function) -> str:
        if getattr(f, "thumb", False):
            return self.thumb.lift(f)
        if f.address in OVERRIDES:
            body = OVERRIDES[f.address][2]
            return (f"void f_{f.address:07x}(cpu_t *c, uint8_t *m) {{ /* {f.name}: hand-written replacement */\n"
                    f"  RC_ENTER({hexu(IMAGE_BASE + f.address)});\n  uint32_t r0 = c->r[0];\n{body}"
                    f"  c->r[15] = c->r[14]; c->depth--;\n}}")
        self.f = f
        self.end = f.address + f.size
        code = self.image.code(f)
        insns = []
        for off in range(0, f.size, 4):
            a = f.address + off
            if f.is_data(a):
                continue
            got = list(self.md.disasm(code[off:off + 4], a))
            if not got:
                raise Unsupported(f"undecodable word at {a:#x}")
            insns.append(got[0])
        if not insns:
            raise Unsupported("no code")
        self.code_addrs = {i.address for i in insns}
        out = [f"void f_{f.address:07x}(cpu_t *c, uint8_t *m) {{ /* {f.name} */"]
        out.append(f"  RC_ENTER({hexu(IMAGE_BASE + f.address)});")
        out.append("  uint32_t " + ", ".join(f"r{k} = c->r[{k}]" for k in range(15)) + ";")
        out.append("  uint32_t n = c->n, z = c->z, cf = c->c, v = c->v, pcv;")
        prev = None
        for i in insns:
            self.i = i
            self.word = int.from_bytes(i.bytes, "little")
            # S bit of data-processing and multiply encodings; capstone's update_flags is unreliable (adc)
            self.sbit = bool((self.word >> 20) & 1)
            self.pc = IMAGE_BASE + i.address + 8
            self.ret_addr = IMAGE_BASE + i.address + 4
            self.call_site = prev is not None and self.is_mov_lr_pc(prev) and prev.cc in (i.cc, A.ARM_CC_AL)
            self.returns = self.is_return(i)
            body = self.insn(i)
            line = f"L_{i.address:07x}: "
            if i.cc not in (A.ARM_CC_AL, A.ARM_CC_INVALID):
                line += f"if ({COND[i.cc]}) {{ {body} }}"
            else:
                line += f"{{ {body} }}"
            out.append(line + f"  /* {i.mnemonic} {i.op_str} */")
            prev = i
        # falling off the end continues in the next function
        nxt = self.end
        if nxt in self.entries:
            out.append(f"  RC_SAVE(); f_{nxt:07x}(c, m); RC_TAIL();  /* falls through */")
        else:
            out.append("  pcv = 0; c->fault = 3; goto out;  /* not reached */")
        out.append("out:")
        out.append("  " + " ".join(f"c->r[{k}] = r{k};" for k in range(15)))
        out.append("  c->r[15] = pcv; c->n = n; c->z = z; c->c = cf; c->v = v; c->depth--;")
        out.append("}")
        return "\n".join(out)

    def is_mov_lr_pc(self, i) -> bool:
        ops = i.operands
        return (i.insn_name() == "mov" and len(ops) == 2 and ops[0].type == A.ARM_OP_REG and ops[1].type == A.ARM_OP_REG
                and self.md.reg_name(ops[0].reg) == "lr" and self.md.reg_name(ops[1].reg) == "pc"
                and ops[1].shift.type == A.ARM_SFT_INVALID)

    def is_return(self, i) -> bool:
        """Instructions that return to the caller when they write PC: bx lr, mov pc, lr, and loads of PC from the stack."""
        name, ops = i.insn_name(), i.operands
        if name == "pop":
            return True
        if name == "ldm":
            return self.md.reg_name(ops[0].reg) == "sp"
        if name == "ldr":
            return ops[1].type == A.ARM_OP_MEM and self.md.reg_name(ops[1].mem.base) == "sp"
        if name == "bx":
            return self.md.reg_name(ops[0].reg) == "lr"
        if name == "mov":
            return (len(ops) == 2 and ops[1].type == A.ARM_OP_REG and self.md.reg_name(ops[1].reg) == "lr"
                    and ops[1].shift.type == A.ARM_SFT_INVALID)
        return False

    # ---- control transfer ---------------------------------------------------------------------
    def table_targets(self) -> list:
        """Local targets of a jump table at the current instruction (link addresses), or [] if it is none.
        `add pc, pc, rX, lsl #2` is followed by a row of `b` instructions (the first is the default case);
        `ldr pc, [pc, rX, lsl #2]` is followed by a table of absolute addresses in a literal pool."""
        i, ops = self.i, self.i.operands
        a = i.address
        name = i.insn_name()
        if name == "add" and len(ops) == 3 and ops[1].type == A.ARM_OP_REG and self.md.reg_name(ops[1].reg) == "pc":
            out, t = [], a + 4
            while t in self.code_addrs:
                w = int.from_bytes(self.image.memory[t:t + 4], "little")
                if (w >> 24) != 0xEA:  # unconditional B
                    break
                out.append(t)
                t += 4
            return out
        if name == "ldr" and ops[1].type == A.ARM_OP_MEM and self.md.reg_name(ops[1].mem.base) == "pc" and ops[1].mem.index:
            out, t = [], a + 8
            while self.f.is_data(t) and t < self.end:
                v = int.from_bytes(self.image.memory[t:t + 4], "little") - IMAGE_BASE
                if v in self.code_addrs:
                    out.append(v)
                t += 4
            return out
        return []

    def jump(self, expr: str) -> str:
        """A write of `expr` to PC."""
        if self.call_site:  # mov lr, pc; <write pc>: an indirect call returning to the next instruction
            return (f"uint32_t tgt = {expr}; r14 = {hexu(self.ret_addr)}; RC_SAVE(); RC_DISPATCH(c, m, tgt); "
                    f"RC_LOAD(); RC_AFTER_CALL({hexu(self.ret_addr)});")
        if self.returns:
            return f"pcv = {expr}; goto out;"
        cases = " ".join(f"case {hexu(IMAGE_BASE + t)}: goto L_{t:07x};" for t in sorted(set(self.table_targets())))
        sw = f"switch (pcv) {{ {cases} }} " if cases else ""
        # not a local target: a tail jump to another function
        return f"pcv = {expr}; {sw}c->r[15] = pcv; RC_SAVE(); RC_DISPATCH(c, m, pcv); RC_TAIL();"

    def call(self, t: int) -> str:
        ret = hexu(self.ret_addr)
        if t in self.image.plt:
            name, got = self.image.plt[t]
            return (f"r14 = {ret}; r12 = {hexu(IMAGE_BASE + got)}; RC_SAVE(); rc_import(c, m, {self.slots[name]}); "
                    f"RC_LOAD(); RC_AFTER_CALL({ret});  /* {name} */")
        if t in self.entries:
            return f"r14 = {ret}; RC_SAVE(); f_{t:07x}(c, m); RC_LOAD(); RC_AFTER_CALL({ret});"
        raise Unsupported(f"call to {t:#x}, not an entry")

    def tail(self, t: int) -> str:
        if t in self.image.plt:
            name, got = self.image.plt[t]
            return f"r12 = {hexu(IMAGE_BASE + got)}; RC_SAVE(); rc_import(c, m, {self.slots[name]}); RC_TAIL();  /* {name} */"
        if t in self.entries:
            return f"RC_SAVE(); f_{t:07x}(c, m); RC_TAIL();"
        raise Unsupported(f"branch to {t:#x}, not an entry")

    # ---- operands -----------------------------------------------------------------------------
    def reg(self, reg_id) -> int:
        name = self.md.reg_name(reg_id)
        if name not in CORE:
            raise Unsupported(f"register {name}")
        return CORE[name]

    def rd(self, reg_id) -> str:
        k = self.reg(reg_id)
        return hexu(self.pc) if k == 15 else f"r{k}"

    def wr(self, reg_id, expr: str) -> str:
        k = self.reg(reg_id)
        if k == 15:
            return self.jump(expr)
        return f"r{k} = {expr};"

    def op2(self, op) -> tuple[str, str]:
        """Value and shifter carry of a flexible second operand, as (setup code, value, carry)."""
        if op.type == A.ARM_OP_IMM:
            val = op.imm & 0xFFFFFFFF
            rot = (self.word >> 8) & 0xF
            carry = str(val >> 31) if (self.word >> 25) & 7 == 1 and rot else "cf"
            return "", hexu(val), carry
        if op.type != A.ARM_OP_REG:
            raise Unsupported("operand2 kind")
        x = self.rd(op.reg)
        st = op.shift.type
        if st == A.ARM_SFT_INVALID:
            return "", x, "cf"
        if st in IMM_SHIFTS:
            amt = str(op.shift.value)
            if st == A.ARM_SFT_RRX:
                amt = "1"
            return f"uint32_t sc = cf; uint32_t o2 = shc({IMM_SHIFTS[st]}, {x}, {amt}, cf, &sc);", "o2", "sc"
        if st in REG_SHIFTS:
            s = self.rd(op.shift.value)
            return f"uint32_t sc = cf; uint32_t o2 = shc({REG_SHIFTS[st]}, {x}, {s} & 0xff, cf, &sc);", "o2", "sc"
        raise Unsupported("shift kind")

    # ---- instructions -------------------------------------------------------------------------
    def insn(self, i) -> str:
        name = i.insn_name()
        ops = i.operands
        if name in LOGICAL or name in ARITH:
            return self.dataproc(name, ops)
        if name in SHIFT_ALIAS:
            return self.shift_alias(name, ops)
        if name in LOADS or name in STORES or name in ("ldrd", "strd"):
            return self.loadstore(name, ops)
        if name in MULTI or name in ("push", "pop"):
            return self.multi(name, ops)
        if name == "b":
            t = ops[0].imm
            if t in self.code_addrs and self.f.address <= t < self.end:
                back = "RC_BUDGET(c); " if t <= i.address else ""
                return f"{back}goto L_{t:07x};"
            return self.tail(t)
        if name == "bx":
            return self.jump(self.rd(ops[0].reg))
        if name == "bl":
            return self.call(ops[0].imm)
        if name == "blx":
            if ops[0].type != A.ARM_OP_REG:  # blx <label>: a call that switches to Thumb (LR keeps bit 0 clear)
                return self.call(ops[0].imm & ~1)
            ret = hexu(self.ret_addr)
            return (f"uint32_t tgt = {self.rd(ops[0].reg)}; r14 = {ret}; RC_SAVE(); RC_DISPATCH(c, m, tgt); "
                    f"RC_LOAD(); RC_AFTER_CALL({ret});")
        if name == "hint":
            return ""
        if name in ("mul", "mla", "mls"):
            d, a, b = (self.rd(o.reg) for o in ops[:3])
            if name == "mul":
                e = f"{a} * {b}"
            else:
                acc = self.rd(ops[3].reg)
                e = f"{acc} + {a} * {b}" if name == "mla" else f"{acc} - {a} * {b}"
            s = f"uint32_t res = {e}; " + self.wr(ops[0].reg, "res")
            if self.sbit:
                s += " n = res >> 31; z = res == 0;"
            return s
        if name in ("umull", "smull", "umlal", "smlal"):
            if self.sbit:
                raise Unsupported("long multiply with S")
            lo, hi, a, b = (self.rd(o.reg) for o in ops)
            if name[0] == "u":
                prod = f"(uint64_t){a} * {b}"
            else:
                prod = f"(uint64_t)((int64_t)(int32_t){a} * (int32_t){b})"
            if name.endswith("lal"):
                prod = f"(((uint64_t){hi} << 32) | {lo}) + {prod}"
            return (f"uint64_t u = {prod}; " + self.wr(ops[0].reg, "(uint32_t)u") + " "
                    + self.wr(ops[1].reg, "(uint32_t)(u >> 32)"))
        if name == "smulbb":
            a, b = (self.rd(o.reg) for o in ops[1:3])
            return self.wr(ops[0].reg, f"(uint32_t)((int32_t)(int16_t){a} * (int16_t){b})")
        if name == "clz":
            return self.wr(ops[0].reg, f"clz32({self.rd(ops[1].reg)})")
        if name in ("ubfx", "sbfx"):
            src, lsb, w = self.rd(ops[1].reg), ops[2].imm, ops[3].imm
            if name == "ubfx":
                e = f"(uint32_t)(((uint64_t){src} >> {lsb}) & {hexu((1 << w) - 1)})"
            else:
                e = f"(uint32_t)((int32_t)({src} << {32 - lsb - w}) >> {32 - w})"
            return self.wr(ops[0].reg, e)
        if name in ("bfi", "bfc"):
            dst = self.rd(ops[0].reg)
            if name == "bfi":
                src, lsb, w = self.rd(ops[1].reg), ops[2].imm, ops[3].imm
            else:
                src, lsb, w = "0", ops[1].imm, ops[2].imm
            mask = ((1 << w) - 1) << lsb
            return self.wr(ops[0].reg, f"({dst} & {hexu(~mask)}) | (({src} << {lsb}) & {hexu(mask)})")
        if name in ("uxtb", "uxth", "sxtb", "sxth", "uxtab", "uxtah", "sxtab", "sxtah"):
            rot = ((self.word >> 10) & 3) * 8
            src = ops[-1]
            if src.type != A.ARM_OP_REG:
                raise Unsupported("extend operand")
            x = f"ror32({self.rd(src.reg)}, {rot})"
            cast = {"b": "uint8_t", "h": "uint16_t"}[name[-1]]
            if name[0] == "s":
                cast = cast[1:]
            e = f"(uint32_t)({cast}){x}"
            if len(name) == 5:
                e = f"{self.rd(ops[1].reg)} + {e}"
            return self.wr(ops[0].reg, e)
        if name == "movt":
            d = self.rd(ops[0].reg)
            return self.wr(ops[0].reg, f"({d} & 0xffffu) | {hexu(ops[1].imm << 16)}")
        if name.startswith("v") or name in ("fconsts", "fconstd", "fmstat"):
            return self.vfp(name, ops)
        raise Unsupported(f"instruction {name}")

    def dataproc(self, name, ops) -> str:
        i = self.i
        if name in COMPARE:
            a, o = ops[0], ops[1]
            dst = None
        elif len(ops) == 2:
            dst = ops[0]
            a, o = (None, ops[1]) if name in ("mov", "mvn") else (ops[0], ops[1])
        else:
            dst, a, o = ops
        if dst is not None and self.reg(dst.reg) == 15 and self.sbit:
            raise Unsupported("flag-setting write to pc (exception return)")
        setup, b, carry = self.op2(o)
        x = self.rd(a.reg) if a is not None else "0"
        arith = {"add": (x, b, "0"), "adc": (x, b, "cf"), "sub": (x, f"~{b}", "1"), "sbc": (x, f"~{b}", "cf"),
                 "rsb": (b, f"~{x}", "1"), "rsc": (b, f"~{x}", "cf"), "cmp": (x, f"~{b}", "1"),
                 "cmn": (x, b, "0")}
        s = setup + " "
        if name in arith:
            p, q, cin = arith[name]
            s += f"uint32_t co, vo; uint32_t res = addc({p}, {q}, {cin}, &co, &vo);"
            flags = " n = res >> 31; z = res == 0; cf = co; v = vo;"
        else:
            e = {"and": f"{x} & {b}", "eor": f"{x} ^ {b}", "orr": f"{x} | {b}", "bic": f"{x} & ~{b}", "mov": b,
                 "mvn": f"~{b}", "tst": f"{x} & {b}", "teq": f"{x} ^ {b}"}[name]
            s += f"uint32_t res = {e};"
            flags = f" n = res >> 31; z = res == 0; cf = {carry};"
        if name in COMPARE:
            return s + flags
        if self.sbit:
            s += flags
        return s + " " + self.wr(dst.reg, "res")

    def shift_alias(self, name, ops) -> str:
        """lsl/lsr/asr/ror/rrx: MOV with a shifted register."""
        i = self.i
        x = self.rd(ops[1].reg)
        if name == "rrx":
            s = f"uint32_t sc = cf; uint32_t res = shc(5, {x}, 1, cf, &sc);"
        elif len(ops) == 3 and ops[2].type == A.ARM_OP_REG:
            s = f"uint32_t sc = cf; uint32_t res = shc({SHIFT_ALIAS[name]}, {x}, {self.rd(ops[2].reg)} & 0xff, cf, &sc);"
        elif len(ops) == 3 and ops[2].type == A.ARM_OP_IMM:
            s = f"uint32_t sc = cf; uint32_t res = shc({SHIFT_ALIAS[name]}, {x}, {ops[2].imm}, cf, &sc);"
        else:
            setup, b, carry = self.op2(ops[1])
            if not setup:
                raise Unsupported("shift alias without a shift")
            s = setup + f" uint32_t res = {b};"  # carry is the shifter's `sc`
        if self.sbit:
            s += " n = res >> 31; z = res == 0; cf = sc;"
        return s + " " + self.wr(ops[0].reg, "res")

    def offset(self, op) -> str:
        """Signed offset expression of a post-index operand or the index part of a memory operand."""
        up = (self.word >> 23) & 1
        if op.type == A.ARM_OP_IMM:
            mag = abs(op.imm)
            return hexu(mag if up else -mag)
        if op.type == A.ARM_OP_REG:
            x = self.rd(op.reg)
            if op.shift.type in IMM_SHIFTS:
                x = f"shc({IMM_SHIFTS[op.shift.type]}, {x}, {op.shift.value}, cf, &dummy)"
            elif op.shift.type != A.ARM_SFT_INVALID:
                raise Unsupported("register-shifted index")
            return x if up else f"(0u - {x})"
        raise Unsupported("offset kind")

    def address(self, ops, mem_index):
        """(setup, access address, writeback statement or '') for single and dual transfers."""
        mem = ops[mem_index].mem
        if mem.base == 0:
            raise Unsupported("memory operand without base")
        base_reg = mem.base
        base = self.rd(base_reg)
        p = (self.word >> 24) & 1
        w = (self.word >> 21) & 1
        setup = "uint32_t dummy; (void)dummy; "
        if p == 0:  # post-index: access at base, then base += offset
            if len(ops) <= mem_index + 1:
                raise Unsupported("post-index without offset")
            off = self.offset(ops[mem_index + 1])
            setup += f"uint32_t ea = {base}; uint32_t nb = ea + {off};"
            return setup, "ea", self.wr(base_reg, "nb")
        if mem.index != 0:
            idx = self.rd(mem.index)
            sh = ops[mem_index].shift
            if sh.type in IMM_SHIFTS:
                idx = f"shc({IMM_SHIFTS[sh.type]}, {idx}, {sh.value}, cf, &dummy)"
            elif sh.type != A.ARM_SFT_INVALID:
                raise Unsupported("register-shifted index")
            up = (self.word >> 23) & 1
            setup += f"uint32_t ea = {base} {'+' if up else '-'} {idx};"
        else:
            up = (self.word >> 23) & 1
            setup += f"uint32_t ea = {base} + {hexu(abs(mem.disp) if up else -abs(mem.disp))};"
        return setup, "ea", (self.wr(base_reg, "ea") if w else "")

    def loadstore(self, name, ops) -> str:
        if name in ("ldrd", "strd"):
            setup, ea, wb = self.address(ops, 2)
            t1, t2 = ops[0].reg, ops[1].reg
            if name == "ldrd":
                return (f"{setup} uint32_t lo = ld32(c, m, {ea}), hi = ld32(c, m, {ea} + 4); {wb} "
                        f"{self.wr(t1, 'lo')} {self.wr(t2, 'hi')}")
            return f"{setup} st32(c, m, {ea}, {self.rd(t1)}); st32(c, m, {ea} + 4, {self.rd(t2)}); {wb}"
        setup, ea, wb = self.address(ops, 1)
        t = ops[0].reg
        if name in LOADS:
            bits, signed = LOADS[name]
            val = f"ld{bits}(c, m, {ea})"
            if signed:
                val = f"(uint32_t)(int{bits}_t){val}"
            # writeback first, then the loaded register (a load into the base register keeps the loaded value)
            return f"{setup} uint32_t val = {val}; {wb} {self.wr(t, 'val')}"
        bits = STORES[name]
        cast = "" if bits == 32 else f"(uint{bits}_t)"
        return f"{setup} st{bits}(c, m, {ea}, {cast}{self.rd(t)}); {wb}"

    def multi(self, name, ops) -> str:
        if name in ("push", "pop"):
            base_reg, regs, wb = None, list(ops), True
            mode, load = ("db", False) if name == "push" else ("ia", True)
            base = "r13"
        else:
            mode, load = MULTI[name]
            base_reg, regs = ops[0].reg, list(ops[1:])
            wb = bool((self.word >> 21) & 1)
            base = self.rd(base_reg)
        ks = sorted(self.reg(o.reg) for o in regs)
        if 15 in ks and not load:
            raise Unsupported("pc in a store list")
        nbytes = 4 * len(ks)
        start = {"ia": "0", "ib": "4", "da": hexu(-nbytes + 4), "db": hexu(-nbytes)}[mode]
        newbase = f"b0 + {hexu(nbytes)}" if mode in ("ia", "ib") else f"b0 - {hexu(nbytes)}"
        s = f"uint32_t b0 = {base}; uint32_t ea = b0 + {start};"
        tail = ""
        if wb:
            s += " " + (f"r13 = {newbase};" if base_reg is None else self.wr(base_reg, newbase))
        base_k = 13 if base_reg is None else self.reg(base_reg)
        for j, k in enumerate(ks):
            a = f"ea + {4 * j}"
            if load:
                if k == 15:
                    tail = " " + self.jump(f"ld32(c, m, {a})")
                else:
                    s += f" r{k} = ld32(c, m, {a});"
            else:
                # a stored base register is its value before the writeback (`push {sp, lr}` in __aeabi_uldivmod)
                s += f" st32(c, m, {a}, {'b0' if k == base_k else f'r{k}'});"
        return s + tail

    # ---- VFP ----------------------------------------------------------------------------------
    def vreg(self, op):
        if op.type != A.ARM_OP_REG:
            raise Unsupported("vfp operand kind")
        name = self.md.reg_name(op.reg)
        m = re.fullmatch(r"([sdq])(\d+)", name)
        if not m:
            raise Unsupported(f"vfp register {name}")
        return m.group(1), int(m.group(2))

    def vval(self, op) -> tuple[str, str]:
        """(kind, C value as float/double) of a VFP register operand."""
        kind, k = self.vreg(op)
        if kind == "s":
            return "s", f"c->vfp.s[{k}]"
        if kind == "d":
            return "d", f"c->vfp.f64[{k}]"
        raise Unsupported("vector register")

    def vset(self, op, expr) -> str:
        kind, k = self.vreg(op)
        return f"c->vfp.s[{k}] = {expr};" if kind == "s" else f"c->vfp.f64[{k}] = {expr};"

    def vfp(self, name, ops) -> str:
        i = self.i
        if name in ("vldr", "vstr"):
            kind, k = self.vreg(ops[0])
            mem = ops[1].mem
            base = hexu((self.pc & ~3)) if self.md.reg_name(mem.base) == "pc" else self.rd(mem.base)
            up = (self.word >> 23) & 1
            ea = f"{base} + {hexu(abs(mem.disp) if up else -abs(mem.disp))}"
            if kind == "s":
                return (f"c->vfp.u32[{k}] = ld32(c, m, {ea});" if name == "vldr"
                        else f"st32(c, m, {ea}, c->vfp.u32[{k}]);")
            if kind == "d":
                return (f"c->vfp.d[{k}] = ld64(c, m, {ea});" if name == "vldr"
                        else f"st64(c, m, {ea}, c->vfp.d[{k}]);")
            raise Unsupported("vldr kind")
        if name == "vmov":
            kinds = []
            for o in ops:
                if o.type == A.ARM_OP_REG:
                    nm = self.md.reg_name(o.reg)
                    kinds.append("r" if nm in CORE else nm[0])
                else:
                    kinds.append("i" if o.type == A.ARM_OP_IMM else "f")
            kinds = "".join(kinds)
            if kinds == "rs":
                return self.wr(ops[0].reg, f"c->vfp.u32[{self.vreg(ops[1])[1]}]")
            if kinds == "sr":
                return f"c->vfp.u32[{self.vreg(ops[0])[1]}] = {self.rd(ops[1].reg)};"
            if kinds == "ss":
                return f"c->vfp.u32[{self.vreg(ops[0])[1]}] = c->vfp.u32[{self.vreg(ops[1])[1]}];"
            if kinds == "dd":
                return f"c->vfp.d[{self.vreg(ops[0])[1]}] = c->vfp.d[{self.vreg(ops[1])[1]}];"
            if kinds == "rrd":
                k = self.vreg(ops[2])[1]
                return (f"uint64_t u = c->vfp.d[{k}]; " + self.wr(ops[0].reg, "(uint32_t)u") + " "
                        + self.wr(ops[1].reg, "(uint32_t)(u >> 32)"))
            if kinds == "drr":
                k = self.vreg(ops[0])[1]
                return (f"c->vfp.d[{k}] = ((uint64_t){self.rd(ops[2].reg)} << 32) | {self.rd(ops[1].reg)};")
            if kinds in ("qi", "qf") and i.mnemonic in ("vmov.i32", "vmov.f32"):
                k = self.vreg(ops[0])[1]
                bits = ops[1].imm & 0xFFFFFFFF if kinds == "qi" else struct.unpack("<I", struct.pack("<f", ops[1].fp))[0]
                lanes = (bits << 32) | bits
                return f"c->vfp.d[{2 * k}] = 0x{lanes:016x}ull; c->vfp.d[{2 * k + 1}] = 0x{lanes:016x}ull;"
            raise Unsupported(f"vmov form {kinds} {i.mnemonic}")
        if name == "fconsts":
            k = self.vreg(ops[0])[1]
            return f"c->vfp.u32[{k}] = {hexu(struct.unpack('<I', struct.pack('<f', ops[1].fp))[0])};"
        if name == "fconstd":
            k = self.vreg(ops[0])[1]
            return f"c->vfp.d[{k}] = 0x{struct.unpack('<Q', struct.pack('<d', ops[1].fp))[0]:016x}ull;"
        if name == "fmstat":
            return "n = c->fpscr >> 31; z = (c->fpscr >> 30) & 1; cf = (c->fpscr >> 29) & 1; v = (c->fpscr >> 28) & 1;"
        if name in ("vcmp", "vcmpe"):
            kind, a = self.vval(ops[0])
            b = self.vval(ops[1])[1] if ops[1].type == A.ARM_OP_REG else "0.0"
            return f"c->fpscr = (c->fpscr & 0x0fffffffu) | fcmp({a}, {b});"
        if name in ("vadd", "vsub", "vmul", "vdiv", "vmla", "vmls", "vnmla", "vnmls", "vnmul"):
            kind, d = self.vval(ops[0])
            a, b = self.vval(ops[1])[1], self.vval(ops[2])[1]
            t = "float" if kind == "s" else "double"
            e = {"vadd": f"{a} + {b}", "vsub": f"{a} - {b}", "vmul": f"{a} * {b}", "vdiv": f"{a} / {b}",
                 "vmla": f"{d} + p", "vmls": f"{d} - p", "vnmla": f"-{d} - p", "vnmls": f"-{d} + p",
                 "vnmul": "-p"}[name]
            pre = f"{t} p = {a} * {b}; " if name in ("vmla", "vmls", "vnmla", "vnmls", "vnmul") else ""
            return f"{pre}{t} res = {e}; {self.vset(ops[0], 'res')}"
        if name in ("vneg", "vabs", "vsqrt"):
            kd, kk = self.vreg(ops[0])
            ks, sk = self.vreg(ops[1])
            if kd != ks or kd not in "sd":
                raise Unsupported("vector unary")
            if name == "vsqrt":
                fn = "sqrtf" if kd == "s" else "sqrt"
                return self.vset(ops[0], f"{fn}({self.vval(ops[1])[1]})")
            if kd == "s":
                e = f"c->vfp.u32[{sk}] ^ 0x80000000u" if name == "vneg" else f"c->vfp.u32[{sk}] & 0x7fffffffu"
                return f"c->vfp.u32[{kk}] = {e};"
            e = (f"c->vfp.d[{sk}] ^ 0x8000000000000000ull" if name == "vneg"
                 else f"c->vfp.d[{sk}] & 0x7fffffffffffffffull")
            return f"c->vfp.d[{kk}] = {e};"
        if name == "vcvt":
            m = re.fullmatch(r"vcvt(r?)(?:eq|ne|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)?\.(\w+)\.(\w+)",
                             i.mnemonic.split()[0])
            if not m or len(ops) != 2:
                raise Unsupported(f"vcvt form {i.mnemonic}")
            rnd, dt, st = m.groups()
            (dk, dn), (sk, sn) = self.vreg(ops[0]), self.vreg(ops[1])
            src = {"f32": f"c->vfp.s[{sn}]", "f64": f"c->vfp.f64[{sn}]",
                   "s32": f"(int32_t)c->vfp.u32[{sn}]", "u32": f"c->vfp.u32[{sn}]"}[st]
            if dt in ("f32", "f64"):
                return self.vset(ops[0], f"({'float' if dt == 'f32' else 'double'}){src}")
            if dt in ("s32", "u32"):
                x = f"(double){src}"
                if rnd:
                    x = f"nearbyint({x})"
                return f"c->vfp.u32[{dn}] = f2{dt[0]}32({x});"
            raise Unsupported(f"vcvt type {dt}")
        if name in ("vpush", "vpop"):
            ks = [self.vreg(o) for o in ops]
            if any(kind != "d" for kind, _ in ks):
                raise Unsupported("vpush of s registers")
            nb = 8 * len(ks)
            if name == "vpush":
                s = f"r13 -= {nb};"
                for j, (_, k) in enumerate(ks):
                    s += f" st64(c, m, r13 + {8 * j}, c->vfp.d[{k}]);"
                return s
            s = ""
            for j, (_, k) in enumerate(ks):
                s += f" c->vfp.d[{k}] = ld64(c, m, r13 + {8 * j});"
            return s + f" r13 += {nb};"
        if name in VMULTI:
            mode, load = VMULTI[name]
            base_reg = ops[0].reg
            ks = [self.vreg(o) for o in ops[1:]]
            size = 4 if ks[0][0] == "s" else 8
            nb = size * len(ks)
            b0 = self.rd(base_reg)
            s = f"uint32_t b0 = {b0}; uint32_t ea = b0{' - ' + str(nb) if mode == 'db' else ''};"
            for j, (kind, k) in enumerate(ks):
                a = f"ea + {size * j}"
                if kind == "s":
                    s += f" c->vfp.u32[{k}] = ld32(c, m, {a});" if load else f" st32(c, m, {a}, c->vfp.u32[{k}]);"
                else:
                    s += f" c->vfp.d[{k}] = ld64(c, m, {a});" if load else f" st64(c, m, {a}, c->vfp.d[{k}]);"
            if self.i.writeback:
                s += " " + self.wr(base_reg, f"b0 {'-' if mode == 'db' else '+'} {nb}")
            return s
        raise Unsupported(f"vfp instruction {name}")


def branch_targets(image: Image, f: Function):
    """(address, target, is_bl) of the B/BL(X) instructions of f that may leave it or enter another function, decoded from
    the words: ARM mode (cond != 1111), or Thumb (16/32-bit branches; calls are is_bl)."""
    if getattr(f, "thumb", False):
        yield from thumb_branches(image, f)
        return
    for a in range(f.address, f.address + f.size, 4):
        if f.is_data(a):
            continue
        w = int.from_bytes(image.memory[a:a + 4], "little")
        if (w >> 25) & 7 == 5 and w >> 28 != 0xF:
            off = w & 0xFFFFFF
            if off & 0x800000:
                off -= 0x1000000
            yield a, (a + 8 + 4 * off) & 0xFFFFFFFF, bool(w & 0x01000000)


def entries(image: Image) -> dict:
    """Every address that is entered from outside its function: the symbol table's (and the loader's) functions, plus
    the targets of calls and branches that land inside another function (shared entry points: the loader lists them in
    `image.inner_entries`; the scan below finds any the loader did not). Such an entry runs from its address to the end
    of the function that contains it and has the mode of that function. Works for ARM and Thumb code."""
    out = {f.address: f for f in image.functions}
    for a, (name, size, _) in OVERRIDES.items():
        out[a] = Function(a, size, name)
    ordered = sorted(out)
    work = list(image.functions)
    pending = [(t, None) for t in sorted(getattr(image, "inner_entries", ()))]
    while work or pending:
        if pending:
            t, _ = pending.pop()
            f, targets = None, [(t, t, True)]
        else:
            f = work.pop()
            targets = branch_targets(image, f)
        for a, t, is_bl in targets:
            inside = f is not None and f.address <= t < f.address + f.size
            if t in out or t in image.plt or (inside and not is_bl):
                continue
            k = bisect.bisect_right(ordered, t) - 1
            if k < 0:
                continue
            parent = out[ordered[k]]
            if not parent.address < t < parent.address + parent.size or parent.is_data(t):
                continue
            thumb = getattr(parent, "thumb", False)
            if f is not None and thumb != getattr(f, "thumb", False) and not is_bl:
                continue  # a plain branch never changes mode: this target is not code of that parent
            g = Function(t, parent.address + parent.size - t, f"{parent.name}+0x{t - parent.address:x}",
                         [(max(s, t), e) for s, e in parent.data_ranges if e > t], thumb)
            out[t] = g
            bisect.insort(ordered, t)
            work.append(g)
    return out


# ---- Thumb-1 / Thumb-2 ------------------------------------------------------------------------------------------
# The Thumb translator decodes the encodings itself (ARMv7-A Thumb-2 plus VFPv3-D32) and uses capstone only for the
# comment on each line and for error messages. Reading S, P/U/W, the modified-immediate carry and the IT block state
# from the bits avoids the places where capstone's detail is unreliable on this library (see docs/notes/lifter.md).

# C conditions for the ARM condition codes 0..13 (eq ne cs cc mi pl vs vc hi ls ge lt gt le)
TCC = ["z", "!z", "cf", "!cf", "n", "!n", "v", "!v", "(cf && !z)", "(!cf || z)", "(n == v)", "(n != v)",
       "(!z && n == v)", "(z || n != v)"]
SETJMP_IMPORTS = {"setjmp", "_setjmp", "sigsetjmp", "__sigsetjmp"}
SHIFT_NAMES = ("lsl", "lsr", "asr", "ror")


def sx(v: int, bits: int) -> int:
    v &= (1 << bits) - 1
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def thumb_size(h1: int) -> int:
    return 4 if (h1 >> 11) >= 0x1D else 2


def thumb_branch(a: int, h1: int, h2: int = 0, size: int = 2):
    """(kind, target) of a branch encoding at `a`, or None. kinds: bcc (conditional, cond in the third item), b, cb
    (cbz/cbnz), bl, blx (call to ARM code); `target` is a link address with the Thumb bit cleared."""
    if size == 2:
        if (h1 >> 12) == 0xD and ((h1 >> 8) & 15) < 14:
            return "bcc", a + 4 + sx((h1 & 0xFF) << 1, 9), (h1 >> 8) & 15
        if (h1 >> 11) == 0x1C:
            return "b", a + 4 + sx((h1 & 0x7FF) << 1, 12), None
        if (h1 & 0xF500) == 0xB100:
            return "cb", a + 4 + (((h1 >> 9) & 1) << 6 | ((h1 >> 3) & 31) << 1), None
        return None
    if (h1 >> 11) != 0x1E or not (h2 & 0x8000):
        return None
    s, j1, j2 = (h1 >> 10) & 1, (h2 >> 13) & 1, (h2 >> 11) & 1
    i1, i2 = 1 ^ (j1 ^ s), 1 ^ (j2 ^ s)
    b14, b12 = (h2 >> 14) & 1, (h2 >> 12) & 1
    if b12:  # B.W (T4) or BL
        off = sx(s << 24 | i1 << 23 | i2 << 22 | (h1 & 0x3FF) << 12 | (h2 & 0x7FF) << 1, 25)
        return ("bl" if b14 else "b"), a + 4 + off, None
    if b14:  # BLX <label>: to ARM code, word-aligned base
        off = sx(s << 24 | i1 << 23 | i2 << 22 | (h1 & 0x3FF) << 12 | (h2 & 0x7FE) << 1, 25)
        return "blx", ((a + 4) & ~3) + off, None
    if ((h1 >> 7) & 7) != 7:  # B<cond>.W (T3)
        off = sx(s << 20 | j2 << 19 | j1 << 18 | (h1 & 0x3F) << 12 | (h2 & 0x7FF) << 1, 21)
        return "bcc", a + 4 + off, (h1 >> 6) & 15
    return None


def thumb_branches(image: Image, f: Function):
    """(address, target, is_call) of the direct branches and calls in a Thumb function, for `entries`."""
    a, end = f.address, f.address + f.size
    ranges = sorted(f.data_ranges)
    ri = 0
    while a < end:
        while ri < len(ranges) and ranges[ri][1] <= a:
            ri += 1
        if ri < len(ranges) and ranges[ri][0] <= a:
            a = ranges[ri][1]
            continue
        h1 = image.u16(a)
        size = thumb_size(h1)
        h2 = image.u16(a + 2) if size == 4 else 0
        br = thumb_branch(a, h1, h2, size)
        if br:
            yield a, br[1], br[0] in ("bl", "blx")
        a += size


def shift_expr(kind: str, x: str, amt: int):
    """(value, carry-out) C expressions for a constant shift of the expression `x`. The carry is None when the shifter
    leaves C alone (LSL #0). kind: lsl lsr asr ror rrx; amt 0..32 (lsr/asr 32 come from an encoded #0)."""
    if kind == "lsl":
        if amt == 0:
            return x, None
        return f"({x} << {amt})", f"(({x} >> {32 - amt}) & 1)"
    if kind == "lsr":
        if amt == 32:
            return "0u", f"({x} >> 31)"
        return f"({x} >> {amt})", f"(({x} >> {amt - 1}) & 1)"
    if kind == "asr":
        if amt >= 32:
            return f"(uint32_t)((int32_t){x} >> 31)", f"({x} >> 31)"
        return f"(uint32_t)((int32_t){x} >> {amt})", f"(({x} >> {amt - 1}) & 1)"
    if kind == "ror":
        return f"ror32({x}, {amt})", f"(ror32({x}, {amt}) >> 31)"
    return f"((cf << 31) | ({x} >> 1))", f"({x} & 1)"  # rrx


def imm_shift(typ: int, imm5: int):
    """DecodeImmShift: (kind, amount)."""
    if typ == 0:
        return "lsl", imm5
    if typ == 1:
        return "lsr", imm5 or 32
    if typ == 2:
        return "asr", imm5 or 32
    return ("ror", imm5) if imm5 else ("rrx", 1)


def modified_immediate(imm12: int):
    """ThumbExpandImm_C: (value, carry-out constant or None for 'unchanged')."""
    if (imm12 >> 10) == 0:
        b = imm12 & 0xFF
        v = (b, b << 16 | b, b << 24 | b << 8, b * 0x01010101)[(imm12 >> 8) & 3]
        return v, None
    unrot = 0x80 | (imm12 & 0x7F)
    rot = (imm12 >> 7) & 31
    v = ((unrot >> rot) | (unrot << (32 - rot))) & 0xFFFFFFFF
    return v, v >> 31


def vfp_expand_imm(imm8: int, double: bool) -> int:
    sign, b = (imm8 >> 7) & 1, (imm8 >> 6) & 1
    if double:
        return sign << 63 | (b ^ 1) << 62 | (0xFF if b else 0) << 54 | (imm8 & 0x3F) << 48
    return sign << 31 | (b ^ 1) << 30 | (0x1F if b else 0) << 25 | (imm8 & 0x3F) << 19


class ThumbLifter:
    """Thumb-1 and Thumb-2 on the ARM lifter's model: one C function per entry, register locals r0..r14 and the flags
    n z cf v, VFP state in c->vfp. Thumb specifics: PC reads as address + 4 (word-aligned for literal loads and ADR);
    return addresses carry the Thumb bit; IT blocks are resolved statically (every instruction inside gets its own
    condition and 16-bit flag-setting data processing does not set flags there); targets given to the dispatcher have
    bit 0 cleared (RC_DISPATCH)."""

    def __init__(self, image: Image, entries: dict | None = None):
        self.image = image
        self.entries = entries if entries is not None else {f.address: f for f in image.functions}
        self.md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
        self.slots = {name: k for k, name in enumerate(image.imports)}
        self.text_lo, self.text_hi = getattr(image, "text_start", 0), getattr(image, "text_end", 0)

    # ---- per function ----------------------------------------------------------------------------------------
    def lift(self, f: Function) -> str:
        self.f = f
        self.end = f.address + f.size
        recs = self.decode(f)
        if not recs:
            raise Unsupported("no code")
        self.code_addrs = {r[0] for r in recs}
        self.uses_ex = False
        self.uses_setjmp = False
        out = []
        for a, size, h1, h2, cond, in_it in recs:
            self.a, self.size, self.h1, self.h2 = a, size, h1, h2
            self.in_it = in_it
            self.cond = cond
            self.pc = IMAGE_BASE + a + 4
            self.ret_addr = (IMAGE_BASE + a + size) | 1
            body = self.insn()
            text = self.comment()
            if isinstance(cond, int):
                line = f"L_{a:07x}: if ({TCC[cond]}) {{ {body} }}"
            else:
                line = f"L_{a:07x}: {{ {body} }}"
            out.append(line + f"  /* {text} */")
        head = [f"void f_{f.address:07x}(cpu_t *c, uint8_t *m) {{ /* {f.name} (Thumb) */",
                f"  RC_ENTER({hexu(IMAGE_BASE + f.address)});",
                "  uint32_t " + ", ".join(f"r{k} = c->r[{k}]" for k in range(15)) + ";",
                "  uint32_t n = c->n, z = c->z, cf = c->c, v = c->v, pcv;"]
        if self.uses_ex:
            head.append("  uint32_t exa = 0, exv = 0; uint64_t exv64 = 0;  /* exclusive monitor */")
        tail = []
        nxt = self.end
        if nxt in self.entries:
            tail.append(f"  RC_SAVE(); f_{nxt:07x}(c, m); RC_TAIL();  /* falls through */")
        else:
            tail.append("  pcv = 0; c->fault = 3; goto out;  /* not reached */")
        tail.append("out:")
        tail.append("  " + " ".join(f"c->r[{k}] = r{k};" for k in range(15)))
        tail.append("  c->r[15] = pcv; c->n = n; c->z = z; c->c = cf; c->v = v; c->depth--;")
        tail.append("}")
        return "\n".join(head + out + tail)

    def decode(self, f: Function) -> list:
        """[(address, size, hw1, hw2, cond, in_it)] of the instructions of f in order. cond is None (always), an int
        (ARM condition code of an instruction inside an IT block) or "it" for the IT instruction itself; in_it tells
        whether the instruction is inside an IT block (16-bit data processing then does not set flags)."""
        mem = self.image.memory
        a, end = f.address, f.address + f.size
        ranges = sorted(f.data_ranges)
        ri, recs, conds = 0, [], []
        while a < end:
            while ri < len(ranges) and ranges[ri][1] <= a:
                ri += 1
            if ri < len(ranges) and ranges[ri][0] <= a:
                a = ranges[ri][1]
                conds = []
                continue
            if a & 1:
                raise Unsupported(f"odd instruction address at {a:#x}")
            h1 = mem[a] | mem[a + 1] << 8
            size = thumb_size(h1)
            if a + size > end:
                raise Unsupported(f"instruction runs past the function end at {a:#x}")
            h2 = (mem[a + 2] | mem[a + 3] << 8) if size == 4 else 0
            if size == 2 and (h1 & 0xFF00) == 0xBF00 and (h1 & 0xF):
                fc, mask = (h1 >> 4) & 15, h1 & 15
                st = fc << 4 | mask
                conds = []
                while True:
                    conds.append(st >> 4)
                    st = (st & 0xE0) | ((st << 1) & 0x1F)
                    if (st & 0xF) == 0:
                        break
                recs.append((a, size, h1, h2, "it", False))
            else:
                in_it = bool(conds)
                cc = conds.pop(0) if conds else None
                if cc == 14:
                    cc = None
                recs.append((a, size, h1, h2, cc, in_it))
            a += size
        return recs

    def comment(self) -> str:
        try:
            raw = bytes(self.image.memory[self.a:self.a + self.size])
            i = next(self.md.disasm(raw, self.a, 1), None)
            if i is not None:
                return f"{i.mnemonic} {i.op_str}".replace("*/", "* /").strip()
        except Exception:  # noqa: BLE001 - only a comment
            pass
        return f"{self.h1:04x} {self.h2:04x}" if self.size == 4 else f"{self.h1:04x}"

    def unsupported(self, what: str):
        name = what
        try:
            raw = bytes(self.image.memory[self.a:self.a + self.size])
            i = next(self.md.disasm(raw, self.a, 1), None)
            if i is not None:
                name = f"{what} ({i.mnemonic.split('.')[0]})"
        except Exception:  # noqa: BLE001
            pass
        raise Unsupported(f"thumb {name} at {self.a:#x}")

    # ---- registers ---------------------------------------------------------------------------------------------
    def R(self, k: int) -> str:
        """Value of register k as an instruction operand: PC reads as address + 4."""
        return hexu(self.pc) if k == 15 else f"r{k}"

    def write(self, k: int, expr: str, ret: bool = False) -> str:
        if k == 15:
            return self.jump(expr, ret)
        return f"r{k} = {expr};"

    def jump(self, expr: str, ret: bool = False) -> str:
        """A write of `expr` to PC. A return (bx lr, pop {pc}, ldr pc,[sp],#4, mov pc,lr) leaves with the target in
        c->r[15]; anything else is a tail jump through the dispatcher."""
        if ret:
            return f"pcv = {expr}; goto out;"
        return f"pcv = {expr}; c->r[15] = pcv; RC_SAVE(); RC_DISPATCH(c, m, pcv); RC_TAIL();"

    def goto(self, t: int) -> str:
        if not (self.f.address <= t < self.end):
            return self.tail(t)
        if t not in self.code_addrs:
            self.unsupported("branch into the middle of an instruction")
        back = "RC_BUDGET(c); " if t <= self.a else ""
        return f"{back}goto L_{t:07x};"

    def call(self, t: int) -> str:
        ret = hexu(self.ret_addr)
        if t in self.image.plt:
            name, got = self.image.plt[t]
            if name in SETJMP_IMPORTS:
                self.uses_setjmp = True
                return (f"r14 = {ret}; r12 = {hexu(IMAGE_BASE + got)}; RC_SAVE(); RC_SETJMP();  /* {name} */")
            return (f"r14 = {ret}; r12 = {hexu(IMAGE_BASE + got)}; RC_SAVE(); rc_import(c, m, {self.slots[name]}); "
                    f"RC_LOAD(); RC_AFTER_CALL({ret});  /* {name} */")
        if t in self.entries:
            return f"r14 = {ret}; RC_SAVE(); f_{t:07x}(c, m); RC_LOAD(); RC_AFTER_CALL({ret});"
        self.unsupported(f"call to a non-entry")

    def tail(self, t: int) -> str:
        if t in self.image.plt:
            name, got = self.image.plt[t]
            return (f"r12 = {hexu(IMAGE_BASE + got)}; RC_SAVE(); rc_import(c, m, {self.slots[name]}); "
                    f"RC_TAIL();  /* {name} */")
        if t in self.entries:
            return f"RC_SAVE(); f_{t:07x}(c, m); RC_TAIL();"
        self.unsupported("branch to a non-entry")

    def call_reg(self, expr: str) -> str:
        ret = hexu(self.ret_addr)
        return (f"uint32_t tgt = {expr}; r14 = {ret}; RC_SAVE(); RC_DISPATCH(c, m, tgt); RC_LOAD(); "
                f"RC_AFTER_CALL({ret});")

    # ---- shared pieces -----------------------------------------------------------------------------------------
    def dp(self, op: str, rd: int, x: str, b: str, carry, S: bool, setup: str = "") -> str:
        """Data processing. x = Rn expression, b = operand 2, carry = shifter carry-out expression for logical S
        instructions (None: C unchanged)."""
        arith = {"add": (x, b, "0"), "adc": (x, b, "cf"), "sub": (x, f"~{b}", "1"), "sbc": (x, f"~{b}", "cf"),
                 "rsb": (b, f"~{x}", "1"), "cmp": (x, f"~{b}", "1"), "cmn": (x, b, "0")}
        test = op in ("cmp", "cmn", "tst", "teq")
        if op in arith and (S or test):
            p, q, cin = arith[op]
            s = setup + (f"uint32_t co, vo; uint32_t res = addc({p}, {q}, {cin}, &co, &vo); "
                         "n = res >> 31; z = res == 0; cf = co; v = vo;")
        elif op in arith:
            e = {"add": f"{x} + {b}", "adc": f"{x} + {b} + cf", "sub": f"{x} - {b}", "sbc": f"{x} + ~{b} + cf",
                 "rsb": f"{b} - {x}"}[op]
            s = setup + f"uint32_t res = {e};"
        else:
            e = {"and": f"{x} & {b}", "eor": f"{x} ^ {b}", "orr": f"{x} | {b}", "bic": f"{x} & ~{b}", "orn": f"{x} | ~{b}",
                 "mov": b, "mvn": f"~{b}", "tst": f"{x} & {b}", "teq": f"{x} ^ {b}"}[op]
            s = setup + f"uint32_t res = {e};"
            if S or test:
                s += " n = res >> 31; z = res == 0;" + (f" cf = {carry};" if carry is not None else "")
        if test:
            return s
        return s + " " + self.write(rd, "res")

    def shift_reg(self, kind: str, rd: int, x: str, amt: str, S: bool) -> str:
        """lsl/lsr/asr/ror by a register (amount = low byte)."""
        code = {"asr": 1, "lsl": 2, "lsr": 3, "ror": 4}[kind]
        s = f"uint32_t sc = cf; uint32_t res = shc({code}, {x}, {amt} & 0xff, cf, &sc);"
        if S:
            s += " n = res >> 31; z = res == 0; cf = sc;"
        return s + " " + self.write(rd, "res")

    def ea_imm(self, rn: int, off: int, p: int, w: int, lit_ok: bool = True):
        """(setup, address expression, writeback statement) for [rn, #off] forms; off is signed."""
        base = hexu(self.pc & ~3) if rn == 15 else f"r{rn}"
        offs = hexu(off)
        if p:
            return f"uint32_t ea = {base} + {offs};", "ea", (f"r{rn} = ea;" if w else "")
        return f"uint32_t ea = {base};", "ea", f"r{rn} = ea + {offs};"

    def load(self, rt: int, size: int, signed: bool, setup: str, ea: str, wb: str, ret: bool = False) -> str:
        val = f"ld{size}(c, m, {ea})"
        if signed:
            val = f"(uint32_t)(int{size}_t){val}"
        else:
            val = f"(uint32_t){val}" if size < 32 else val
        return f"{setup} uint32_t val = {val}; {wb} {self.write(rt, 'val', ret)}"

    def literal_const(self, lit: int, size: int, signed: bool):
        """The constant a pc-relative load reads, when it lies in the (read-only) code of the image."""
        if not (self.text_lo <= lit and lit + size // 8 <= self.text_hi):
            return None
        v = int.from_bytes(self.image.memory[lit:lit + size // 8], "little")
        if signed:
            v = sx(v, size) & 0xFFFFFFFF
        return v

    def store(self, rt: int, size: int, setup: str, ea: str, wb: str) -> str:
        cast = "" if size == 32 else f"(uint{size}_t)"
        return f"{setup} st{size}(c, m, {ea}, {cast}{self.R(rt)}); {wb}"

    def multi(self, load: bool, mode: str, rn: int, regs: list, wb: bool) -> str:
        """ldm/stm/push/pop. mode 'ia' or 'db'."""
        n = 4 * len(regs)
        if 15 in regs and not load:
            self.unsupported("pc in a store list")
        start = "0u" if mode == "ia" else hexu(-n)
        newb = f"b0 + {n}u" if mode == "ia" else f"b0 - {n}u"
        s = f"uint32_t b0 = r{rn}; uint32_t ea = b0 + {start};"
        if wb and not (load and rn in regs):
            s += f" r{rn} = {newb};"
        tail = ""
        for j, k in enumerate(regs):
            a = f"ea + {4 * j}u"
            if load:
                if k == 15:
                    tail = " " + self.jump(f"ld32(c, m, {a})", ret=(rn == 13))
                else:
                    s += f" r{k} = ld32(c, m, {a});"
            else:
                s += f" st32(c, m, {a}, {'b0' if k == rn else f'r{k}'});"
        return s + tail

    # ---- dispatch ----------------------------------------------------------------------------------------------
    def insn(self) -> str:
        h1, h2 = self.h1, self.h2
        if self.cond == "it":
            return ""
        if self.size == 2:
            return self.insn16(h1)
        return self.insn32(h1, h2)

    # ---- 16-bit encodings ----------------------------------------------------------------------------------------
    def table_switch(self, jt) -> str:
        """The `switch` of a tbb/tbh table: cases by entry index grouped by target; an index past the table faults."""
        groups = {}
        for k, t in enumerate(jt.targets):
            groups.setdefault(t, []).append(k)
        idx = f"r{self.h2 & 15}"
        cases = []
        for t, ks in groups.items():
            if t not in self.code_addrs:
                self.unsupported("jump table target is not an instruction")
            cases.append(" ".join(f"case {k}u:" for k in ks) + f" {self.goto(t)}")
        return (f"switch ({idx}) {{ " + " ".join(cases) +
                " default: c->fault = RC_FAULT_DISPATCH; pcv = 0; goto out; }")

    def word_table_switch(self, jt, expr: str) -> str:
        """`bx rB` at the end of an `adr; ldr.w; add; bx` table: a switch over the table's targets (the register holds
        table + word, bit 0 set)."""
        cases = []
        for t in sorted(set(jt.targets)):
            if t not in self.code_addrs:
                self.unsupported("jump table target is not an instruction")
            cases.append(f"case {hexu(IMAGE_BASE + t)}: {self.goto(t)}")
        return (f"pcv = {expr} & ~1u; switch (pcv) {{ " + " ".join(cases) +
                f" default: c->r[15] = pcv; RC_SAVE(); RC_DISPATCH(c, m, pcv); RC_TAIL(); }}")

    def insn16(self, hw: int) -> str:
        S = not self.in_it
        top = hw >> 11
        a = self.a
        if top < 3:  # lsl/lsr/asr rd, rm, #imm5 (lsl #0 is movs rd, rm)
            kind, amt = imm_shift(top, (hw >> 6) & 31)
            val, carry = shift_expr(kind, f"r{(hw >> 3) & 7}", amt)
            return self.dp("mov", hw & 7, "0u", val, carry, S)
        if top == 3:  # add/sub register or imm3
            rn, rd, field = (hw >> 3) & 7, hw & 7, (hw >> 6) & 7
            b = f"{field}u" if (hw >> 10) & 1 else f"r{field}"
            return self.dp("sub" if (hw >> 9) & 1 else "add", rd, f"r{rn}", b, None, S)
        if top < 8:  # mov/cmp/add/sub rd, #imm8
            rd, imm = (hw >> 8) & 7, hexu(hw & 0xFF)
            op = ("mov", "cmp", "add", "sub")[top - 4]
            return self.dp(op, rd, f"r{rd}", imm, None, S)
        if top == 8:
            if not (hw >> 10) & 1:
                opc, rm, rdn = (hw >> 6) & 15, (hw >> 3) & 7, hw & 7
                X, M = f"r{rdn}", f"r{rm}"
                if opc in (2, 3, 4, 7):
                    return self.shift_reg({2: "lsl", 3: "lsr", 4: "asr", 7: "ror"}[opc], rdn, X, M, S)
                if opc == 13:
                    return f"uint32_t res = {M} * {X}; " + ("n = res >> 31; z = res == 0; " if S else "") + f"r{rdn} = res;"
                op = {0: "and", 1: "eor", 5: "adc", 6: "sbc", 8: "tst", 10: "cmp", 11: "cmn", 12: "orr", 14: "bic"}.get(opc)
                if op:
                    return self.dp(op, rdn, X, M, None, S)
                if opc == 9:
                    return self.dp("rsb", rdn, M, "0u", None, S)
                return self.dp("mvn", rdn, "0u", M, None, S)  # 15
            op, rm = (hw >> 8) & 3, (hw >> 3) & 15
            rdn = ((hw >> 4) & 8) | (hw & 7)
            if op == 0:
                return self.dp("add", rdn, self.R(rdn), self.R(rm), None, False)
            if op == 1:
                return self.dp("cmp", rdn, self.R(rdn), self.R(rm), None, True)
            if op == 2:
                if rdn == 15:
                    return self.write(15, self.R(rm), ret=(rm == 14))
                return self.dp("mov", rdn, "0u", self.R(rm), None, False)
            if (hw >> 7) & 1:  # blx rm
                return self.call_reg(self.R(rm))
            jt = self.image.jump_tables.get(a)
            if jt is not None and jt.kind == "thumb_word":
                return self.word_table_switch(jt, self.R(rm))
            return self.jump(self.R(rm), ret=(rm == 14))
        if top == 9:  # ldr rt, [pc, #imm8*4]
            rt, lit = (hw >> 8) & 7, ((self.pc & ~3) + (hw & 0xFF) * 4)
            return self.ldr_literal(rt, 32, False, lit - IMAGE_BASE)
        if top in (10, 11):  # load/store register offset
            opc, rm, rn, rt = (hw >> 9) & 7, (hw >> 6) & 7, (hw >> 3) & 7, hw & 7
            setup = f"uint32_t ea = r{rn} + r{rm};"
            if opc < 3:
                return self.store(rt, (32, 16, 8)[opc], setup, "ea", "")
            size, signed = ((8, True), (32, False), (16, False), (8, False), (16, True))[opc - 3]
            return self.load(rt, size, signed, setup, "ea", "")
        if 12 <= top <= 17:  # load/store immediate
            rn, rt, imm5 = (hw >> 3) & 7, hw & 7, (hw >> 6) & 31
            ld = top & 1
            size = {6: 32, 7: 8, 8: 16}[top >> 1]
            off = imm5 * (size // 8)
            setup = f"uint32_t ea = r{rn} + {hexu(off)};"
            return self.load(rt, size, False, setup, "ea", "") if ld else self.store(rt, size, setup, "ea", "")
        if top in (18, 19):  # ldr/str rt, [sp, #imm8*4]
            rt = (hw >> 8) & 7
            setup = f"uint32_t ea = r13 + {hexu((hw & 0xFF) * 4)};"
            return self.load(rt, 32, False, setup, "ea", "") if top & 1 else self.store(rt, 32, setup, "ea", "")
        if top == 20:  # adr
            return f"r{(hw >> 8) & 7} = {hexu((self.pc & ~3) + (hw & 0xFF) * 4)};"
        if top == 21:  # add rd, sp, #imm8*4
            return f"r{(hw >> 8) & 7} = r13 + {hexu((hw & 0xFF) * 4)};"
        if top in (22, 23):
            return self.misc16(hw)
        if top == 24 or top == 25:  # stm/ldm rn!, {list}
            rn = (hw >> 8) & 7
            regs = [k for k in range(8) if (hw >> k) & 1]
            if not regs:
                self.unsupported("empty register list")
            return self.multi(bool(top & 1), "ia", rn, regs, True)
        if top in (26, 27):
            cond = (hw >> 8) & 15
            if cond >= 14:
                self.unsupported("udf/svc")
            t = a + 4 + sx((hw & 0xFF) << 1, 9)
            return f"if ({TCC[cond]}) {{ {self.goto(t)} }}"
        # top == 28: b <label> (T2)
        return self.goto(a + 4 + sx((hw & 0x7FF) << 1, 12))

    def misc16(self, hw: int) -> str:
        if (hw & 0xFF00) == 0xB000:  # add/sub sp, #imm7*4
            imm = hexu((hw & 0x7F) * 4)
            return f"r13 = r13 {'-' if hw & 0x80 else '+'} {imm};"
        if (hw & 0xF500) == 0xB100:  # cbz/cbnz
            kind, t, _ = thumb_branch(self.a, hw)
            rn = hw & 7
            return f"if (r{rn} {'!=' if hw & 0x800 else '=='} 0) {{ {self.goto(t)} }}"
        if (hw & 0xFF00) == 0xB200:  # sxth sxtb uxth uxtb
            opc = (hw >> 6) & 3
            x = f"r{(hw >> 3) & 7}"
            cast = ("int16_t", "int8_t", "uint16_t", "uint8_t")[opc]
            return f"r{hw & 7} = (uint32_t)({cast}){x};"
        if (hw & 0xFE00) == 0xB400:  # push
            regs = [k for k in range(8) if (hw >> k) & 1] + ([14] if (hw >> 8) & 1 else [])
            return self.multi(False, "db", 13, regs, True)
        if (hw & 0xFE00) == 0xBC00:  # pop
            regs = [k for k in range(8) if (hw >> k) & 1] + ([15] if (hw >> 8) & 1 else [])
            return self.multi(True, "ia", 13, regs, True)
        if (hw & 0xFF00) == 0xBA00:  # rev rev16 revsh
            op = (hw >> 6) & 3
            fn = {0: "rev32", 1: "rev16", 3: "revsh"}.get(op)
            if fn is None:
                self.unsupported("undefined rev")
            return f"r{hw & 7} = {fn}(r{(hw >> 3) & 7});"
        if (hw & 0xFF00) == 0xBF00:  # hints (IT was handled by decode)
            return ""
        self.unsupported("misc 16-bit encoding")

    def ldr_literal(self, rt: int, size: int, signed: bool, lit: int) -> str:
        """Load from a pc-relative address (link address `lit`): the constant is folded when it is in the code."""
        v = self.literal_const(lit, size, signed)
        if v is not None:
            return self.write(rt, hexu(v))
        return self.load(rt, size, signed, f"uint32_t ea = {hexu(IMAGE_BASE + lit)};", "ea", "")

    # ---- 32-bit encodings ----------------------------------------------------------------------------------------
    def insn32(self, h1: int, h2: int) -> str:
        op1, op2 = (h1 >> 11) & 3, (h1 >> 4) & 0x7F
        if op1 == 1:  # 1110 1xxx
            if (op2 & 0x64) == 0:
                return self.ldm_stm32(h1, h2)
            if (op2 & 0x64) == 0x04:
                return self.dual_excl(h1, h2)
            if (op2 & 0x60) == 0x20:
                return self.dp_shifted(h1, h2)
            if (h1 & 0xFC00) == 0xEC00:
                return self.vfp(h1, h2)
            self.unsupported("32-bit encoding")
        if op1 == 2:  # 1111 0xxx
            if not (h2 & 0x8000):
                return self.dp_modimm(h1, h2) if not (h1 & 0x0200) else self.dp_plain(h1, h2)
            return self.branch_misc(h1, h2)
        # 1111 1xxx
        if (h1 & 0xFE00) == 0xF800:
            return self.ldst32(h1, h2)
        if (h1 & 0xFF00) == 0xFA00:
            return self.dpreg32(h1, h2)
        if (h1 & 0xFF80) == 0xFB00:
            return self.mul32(h1, h2)
        if (h1 & 0xFF80) == 0xFB80:
            return self.longmul32(h1, h2)
        if (h1 & 0xFC00) == 0xFC00:
            self.unsupported("NEON or coprocessor encoding")
        self.unsupported("32-bit encoding")

    def ldm_stm32(self, h1: int, h2: int) -> str:
        op, W, L, rn = (h1 >> 7) & 3, (h1 >> 5) & 1, (h1 >> 4) & 1, h1 & 15
        if op not in (1, 2):
            self.unsupported("srs/rfe")
        regs = [k for k in range(16) if (h2 >> k) & 1]
        if not regs:
            self.unsupported("empty register list")
        return self.multi(bool(L), "ia" if op == 1 else "db", rn, regs, bool(W))

    def dual_excl(self, h1: int, h2: int) -> str:
        P, U, W, L, rn = (h1 >> 8) & 1, (h1 >> 7) & 1, (h1 >> 5) & 1, (h1 >> 4) & 1, h1 & 15
        rt = (h2 >> 12) & 15
        if P or W:  # ldrd/strd (immediate)
            rt2 = (h2 >> 8) & 15
            imm = (h2 & 0xFF) * 4
            setup, ea, wb = self.ea_imm(rn, imm if U else -imm, P, W)
            if L:
                return (f"{setup} uint32_t lo = ld32(c, m, {ea}), hi = ld32(c, m, {ea} + 4u); {wb} "
                        f"{self.write(rt, 'lo')} {self.write(rt2, 'hi')}")
            return f"{setup} st32(c, m, {ea}, {self.R(rt)}); st32(c, m, {ea} + 4u, {self.R(rt2)}); {wb}"
        op3 = (h2 >> 4) & 15
        if not U:  # ldrex / strex
            self.uses_ex = True
            imm = hexu((h2 & 0xFF) * 4)
            ea = f"uint32_t ea = r{rn} + {imm};"
            if L:
                return f"{ea} exa = ea; exv = ld32(c, m, ea); {self.write(rt, 'exv')}"
            rd = (h2 >> 8) & 15
            return f"{ea} uint32_t st = rc_strex32(c, m, ea, exa, exv, {self.R(rt)}); exa = 0; r{rd} = st;"
        if L and op3 in (0, 1):  # tbb / tbh
            jt = self.image.jump_tables.get(self.a)
            if jt is None or rn != 15:
                self.unsupported("tbb/tbh without a decoded jump table")
            return self.table_switch(jt)
        if op3 in (4, 5, 7):
            self.uses_ex = True
            bits = {4: 8, 5: 16, 7: 64}[op3]
            ea = f"uint32_t ea = r{rn};"
            if L:
                if bits == 64:
                    return (f"{ea} exa = ea; exv64 = ld64(c, m, ea); {self.write(rt, '(uint32_t)exv64')} "
                            f"{self.write((h2 >> 8) & 15, '(uint32_t)(exv64 >> 32)')}")
                return f"{ea} exa = ea; exv = ld{bits}(c, m, ea); {self.write(rt, 'exv')}"
            rd = h2 & 15
            if bits == 64:
                val = f"((uint64_t){self.R((h2 >> 8) & 15)} << 32) | {self.R(rt)}"
                return f"{ea} uint32_t st = rc_strex64(c, m, ea, exa, exv64, {val}); exa = 0; r{rd} = st;"
            return f"{ea} uint32_t st = rc_strex{bits}(c, m, ea, exa, exv, {self.R(rt)}); exa = 0; r{rd} = st;"
        self.unsupported("load/store exclusive or table branch")

    def dp_shifted(self, h1: int, h2: int) -> str:
        op4, S, rn = (h1 >> 5) & 15, (h1 >> 4) & 1, h1 & 15
        rd, rm, typ = (h2 >> 8) & 15, h2 & 15, (h2 >> 4) & 3
        imm5 = ((h2 >> 12) & 7) << 2 | ((h2 >> 6) & 3)
        kind, amt = imm_shift(typ, imm5)
        val, carry = shift_expr(kind, self.R(rm), amt)
        x = self.R(rn)
        if op4 == 0:
            return self.dp("tst" if rd == 15 and S else "and", rd, x, val, carry, bool(S))
        if op4 == 1:
            return self.dp("bic", rd, x, val, carry, bool(S))
        if op4 == 2:
            if rn == 15:
                return self.dp("mov", rd, "0u", val, carry, bool(S))
            return self.dp("orr", rd, x, val, carry, bool(S))
        if op4 == 3:
            if rn == 15:
                return self.dp("mvn", rd, "0u", val, carry, bool(S))
            return self.dp("orn", rd, x, val, carry, bool(S))
        if op4 == 4:
            return self.dp("teq" if rd == 15 and S else "eor", rd, x, val, carry, bool(S))
        if op4 == 6:
            return self.pkh(h1, h2)
        arith = {8: "add", 10: "adc", 11: "sbc", 13: "sub", 14: "rsb"}
        if op4 == 8 and rd == 15 and S:
            return self.dp("cmn", rd, x, val, None, True)
        if op4 == 13 and rd == 15 and S:
            return self.dp("cmp", rd, x, val, None, True)
        if op4 in arith:
            return self.dp(arith[op4], rd, x, val, None, bool(S))
        self.unsupported("data-processing shifted register")

    def pkh(self, h1: int, h2: int) -> str:
        rn, rd, rm = h1 & 15, (h2 >> 8) & 15, h2 & 15
        tb = (h2 >> 5) & 1
        imm = ((h2 >> 12) & 7) << 2 | ((h2 >> 6) & 3)
        if tb:
            sh = f"(uint32_t)((int32_t){self.R(rm)} >> {imm if imm else 31})"  # pkhtb: asr #0 means #32
            return f"r{rd} = ({self.R(rn)} & 0xffff0000u) | ({sh} & 0xffffu);"
        return f"r{rd} = ({self.R(rn)} & 0xffffu) | (({self.R(rm)} << {imm}) & 0xffff0000u);"

    def dp_modimm(self, h1: int, h2: int) -> str:
        op4, S, rn = (h1 >> 5) & 15, (h1 >> 4) & 1, h1 & 15
        rd = (h2 >> 8) & 15
        imm12 = ((h1 >> 10) & 1) << 11 | ((h2 >> 12) & 7) << 8 | (h2 & 0xFF)
        val, cflag = modified_immediate(imm12)
        b, carry = hexu(val), (None if cflag is None else str(cflag))
        x = self.R(rn)
        if op4 == 0:
            return self.dp("tst" if rd == 15 and S else "and", rd, x, b, carry, bool(S))
        if op4 == 1:
            return self.dp("bic", rd, x, b, carry, bool(S))
        if op4 == 2:
            return self.dp("mov" if rn == 15 else "orr", rd, "0u" if rn == 15 else x, b, carry, bool(S))
        if op4 == 3:
            return self.dp("mvn" if rn == 15 else "orn", rd, "0u" if rn == 15 else x, b, carry, bool(S))
        if op4 == 4:
            return self.dp("teq" if rd == 15 and S else "eor", rd, x, b, carry, bool(S))
        if op4 == 8:
            return self.dp("cmn" if rd == 15 and S else "add", rd, x, b, None, bool(S))
        if op4 in (10, 11, 14):
            return self.dp({10: "adc", 11: "sbc", 14: "rsb"}[op4], rd, x, b, None, bool(S))
        if op4 == 13:
            return self.dp("cmp" if rd == 15 and S else "sub", rd, x, b, None, bool(S))
        self.unsupported("data-processing modified immediate")

    def dp_plain(self, h1: int, h2: int) -> str:
        op5, rn = (h1 >> 4) & 31, h1 & 15
        rd = (h2 >> 8) & 15
        i = (h1 >> 10) & 1
        imm3, imm8 = (h2 >> 12) & 7, h2 & 0xFF
        imm12 = i << 11 | imm3 << 8 | imm8
        lsb = imm3 << 2 | ((h2 >> 6) & 3)
        if op5 in (0x00, 0x0A):  # addw / subw / adr
            if rn == 15:
                base = self.pc & ~3
                return self.write(rd, hexu(base + imm12 if op5 == 0 else base - imm12))
            return self.write(rd, f"r{rn} {'+' if op5 == 0 else '-'} {hexu(imm12)}")
        if op5 == 0x04:  # movw
            return self.write(rd, hexu((h1 & 15) << 12 | imm12))
        if op5 == 0x0C:  # movt
            return self.write(rd, f"(r{rd} & 0xffffu) | {hexu(((h1 & 15) << 12 | imm12) << 16)}")
        if op5 in (0x10, 0x12, 0x18, 0x1A):  # ssat / usat
            sh, sat = (h1 >> 5) & 1, h2 & 31
            imm5 = imm3 << 2 | ((h2 >> 6) & 3)
            if sh and imm5 == 0:
                self.unsupported("ssat16/usat16")
            x = self.R(rn)
            x = f"(int32_t)({x} << {imm5})" if not sh else f"((int32_t){x} >> {imm5 if imm5 else 31})"
            fn = "rc_ssat" if op5 < 0x18 else "rc_usat"
            return self.write(rd, f"{fn}({x}, {sat + 1 if op5 < 0x18 else sat})")
        if op5 in (0x14, 0x1C):  # sbfx / ubfx
            w = (h2 & 31) + 1
            if lsb + w > 32:
                self.unsupported("bitfield out of range")
            x = self.R(rn)
            if op5 == 0x1C:
                return self.write(rd, f"(uint32_t)((({x}) >> {lsb}) & {hexu((1 << w) - 1)})" if w < 32 else x)
            return self.write(rd, f"(uint32_t)((int32_t)({x} << {32 - lsb - w}) >> {32 - w})")
        if op5 == 0x16:  # bfi / bfc
            msb = h2 & 31
            if msb < lsb:
                self.unsupported("bitfield out of range")
            w = msb - lsb + 1
            mask = ((1 << w) - 1) << lsb
            src = "0u" if rn == 15 else f"r{rn}"
            return self.write(rd, f"(r{rd} & {hexu(~mask)}) | (({src} << {lsb}) & {hexu(mask)})")
        self.unsupported("data-processing plain immediate")

    def branch_misc(self, h1: int, h2: int) -> str:
        br = thumb_branch(self.a, h1, h2, 4)
        if br is not None:
            kind, t, cond = br
            if kind == "b":
                return self.goto(t)
            if kind == "bcc":
                return f"if ({TCC[cond]}) {{ {self.goto(t)} }}"
            if kind == "bl":
                return self.call(t)
            if kind == "blx":
                return self.call(t)
        if h1 == 0xF3AF and (h2 & 0xFF00) == 0x8000:  # nop.w, yield, wfe, wfi, sev
            return ""
        if h1 == 0xF3BF and (h2 & 0xFF00) == 0x8F00:
            op = (h2 >> 4) & 15
            if op == 2:  # clrex
                self.uses_ex = True
                return "exa = 0;"
            if op in (4, 5, 6):  # dsb dmb isb
                return "__atomic_thread_fence(__ATOMIC_SEQ_CST);"
        self.unsupported("branch/miscellaneous control")

    def ldst32(self, h1: int, h2: int) -> str:
        S, size_code, L, rn = (h1 >> 8) & 1, (h1 >> 5) & 3, (h1 >> 4) & 1, h1 & 15
        rt = (h2 >> 12) & 15
        if size_code == 3 or (S and (not L or size_code == 2)):
            self.unsupported("load/store single (undefined)")
        size = (8, 16, 32)[size_code]
        if L and rt == 15 and size < 32:
            return ""  # pld/pli/preload hints
        if L and rn == 15:  # literal
            imm = h2 & 0xFFF
            base = self.pc & ~3
            lit = (base + imm if (h1 >> 7) & 1 else base - imm) - IMAGE_BASE
            return self.ldr_literal(rt, size, bool(S), lit)
        if (h1 >> 7) & 1:  # imm12
            setup, ea, wb = self.ea_imm(rn, h2 & 0xFFF, 1, 0)
        elif ((h2 >> 6) & 0x3F) == 0:  # register offset
            sh = (h2 >> 4) & 3
            idx = f"r{h2 & 15}" if not sh else f"(r{h2 & 15} << {sh})"
            if (h2 & 15) == 15:
                self.unsupported("pc as index register")
            setup, ea, wb = f"uint32_t ea = r{rn} + {idx};", "ea", ""
        elif (h2 >> 11) & 1:  # imm8 with P/U/W
            P, U, W = (h2 >> 10) & 1, (h2 >> 9) & 1, (h2 >> 8) & 1
            if not P and not W:
                self.unsupported("load/store single (undefined)")
            imm = h2 & 0xFF
            setup, ea, wb = self.ea_imm(rn, imm if U else -imm, P, W)
        else:
            self.unsupported("load/store single")
        if L:
            ret = (rt == 15 and rn == 13 and size == 32 and wb != "")
            return self.load(rt, size, bool(S), setup, ea, wb, ret)
        return self.store(rt, size, setup, ea, wb)

    def dpreg32(self, h1: int, h2: int) -> str:
        op1, op2, rn = (h1 >> 4) & 15, (h2 >> 4) & 15, h1 & 15
        rd, rm = (h2 >> 8) & 15, h2 & 15
        if op1 < 8:
            if op2 == 0:  # lsl/lsr/asr/ror rd, rn, rm
                return self.shift_reg(SHIFT_NAMES[(op1 >> 1) & 3], rd, self.R(rn), self.R(rm), bool(op1 & 1))
            if op2 & 8:  # sign/zero extend (and add)
                grp, rot = op1 >> 1, (op2 & 3) * 8
                if grp not in (0, 2):
                    self.unsupported("sxtb16/uxtb16")
                x = self.R(rm) if not rot else f"ror32({self.R(rm)}, {rot})"
                signed = not (op1 & 1)
                cast = ("int16_t" if signed else "uint16_t") if grp == 0 else ("int8_t" if signed else "uint8_t")
                e = f"(uint32_t)({cast}){x}"
                if rn != 15:
                    e = f"{self.R(rn)} + {e}"
                return self.write(rd, e)
            self.unsupported("data-processing register")
        if op2 < 8:
            self.unsupported("parallel add/subtract")
        key = (op1, op2)
        x, y = self.R(rn), self.R(rm)
        e = {(9, 8): f"rev32({y})", (9, 9): f"rev16({y})", (9, 10): f"rbit32({y})", (9, 11): f"revsh({y})",
             (11, 8): f"clz32({y})", (8, 8): f"rc_qadd({y}, {x})", (8, 10): f"rc_qsub({y}, {x})",
             (8, 9): f"rc_qadd({y}, rc_qadd({x}, {x}))", (8, 11): f"rc_qsub({y}, rc_qadd({x}, {x}))"}.get(key)
        if e is None:
            self.unsupported("sel or miscellaneous register operation")
        return self.write(rd, e)

    def mul32(self, h1: int, h2: int) -> str:
        op1, rn = (h1 >> 4) & 7, h1 & 15
        ra, rd, op2, rm = (h2 >> 12) & 15, (h2 >> 8) & 15, (h2 >> 4) & 15, h2 & 15
        x, y, acc = self.R(rn), self.R(rm), self.R(ra)
        if op1 == 0:
            if op2 == 0:
                return self.write(rd, f"{x} * {y}" if ra == 15 else f"{acc} + {x} * {y}")
            if op2 == 1:
                return self.write(rd, f"{acc} - {x} * {y}")
        elif op1 == 1:  # smulxy / smlaxy
            hx = f"((int32_t){x} >> 16)" if op2 & 2 else f"(int32_t)(int16_t){x}"
            hy = f"((int32_t){y} >> 16)" if op2 & 1 else f"(int32_t)(int16_t){y}"
            e = f"(uint32_t)({hx} * {hy})" if ra == 15 else f"(uint32_t)({hx} * {hy} + (int32_t){acc})"
            return self.write(rd, e)
        elif op1 == 3 and op2 < 2:  # smulwy / smlawy
            hy = f"((int32_t){y} >> 16)" if op2 & 1 else f"(int32_t)(int16_t){y}"
            prod = f"((int64_t)(int32_t){x} * {hy}) >> 16"
            e = f"(uint32_t)({prod})" if ra == 15 else f"(uint32_t)(({prod}) + (int32_t){acc})"
            return self.write(rd, e)
        elif op1 in (2, 4) and op2 < 2:  # smuad/smlad, smusd/smlsd
            yy = f"ror32({y}, 16)" if op2 & 1 else y
            lo = f"(int32_t)(int16_t){x} * (int32_t)(int16_t){yy}"
            hi = f"((int32_t){x} >> 16) * ((int32_t){yy} >> 16)"
            e = f"{lo} + {hi}" if op1 == 2 else f"{lo} - {hi}"
            if ra != 15:
                e += f" + (int32_t){acc}"
            return self.write(rd, f"(uint32_t)({e})")
        elif op1 in (5, 6) and op2 < 2:  # smmul/smmla/smmls (+ rounding)
            rnd = " + 0x80000000ll" if op2 & 1 else ""
            prod = f"(int64_t)(int32_t){x} * (int32_t){y}"
            if op1 == 5 and ra == 15:
                e = f"({prod}{rnd}) >> 32"
            elif op1 == 5:
                e = f"(((int64_t)(int32_t){acc} << 32) + {prod}{rnd}) >> 32"
            else:
                e = f"(((int64_t)(int32_t){acc} << 32) - {prod}{rnd}) >> 32"
            return self.write(rd, f"(uint32_t)({e})")
        self.unsupported("multiply (usad8 or dual multiply)")

    def longmul32(self, h1: int, h2: int) -> str:
        op1, rn = (h1 >> 4) & 7, h1 & 15
        rlo, rhi, op2, rm = (h2 >> 12) & 15, (h2 >> 8) & 15, (h2 >> 4) & 15, h2 & 15
        x, y = self.R(rn), self.R(rm)
        if op1 == 1 and op2 == 15:
            return self.write(rhi, f"sdiv32({x}, {y})")
        if op1 == 3 and op2 == 15:
            return self.write(rhi, f"udiv32({x}, {y})")
        acc = f"(((uint64_t){self.R(rhi)} << 32) | {self.R(rlo)})"
        if op1 == 0 and op2 == 0:
            prod = f"(uint64_t)((int64_t)(int32_t){x} * (int32_t){y})"
        elif op1 == 2 and op2 == 0:
            prod = f"(uint64_t){x} * {y}"
        elif op1 == 4 and op2 == 0:
            prod = f"{acc} + (uint64_t)((int64_t)(int32_t){x} * (int32_t){y})"
        elif op1 == 6 and op2 == 0:
            prod = f"{acc} + (uint64_t){x} * {y}"
        elif op1 == 6 and op2 == 6:  # umaal
            prod = f"(uint64_t){x} * {y} + {self.R(rlo)} + {self.R(rhi)}"
        elif op1 == 4 and (op2 & 0xC) == 8:  # smlalxy
            hx = f"((int32_t){x} >> 16)" if op2 & 2 else f"(int32_t)(int16_t){x}"
            hy = f"((int32_t){y} >> 16)" if op2 & 1 else f"(int32_t)(int16_t){y}"
            prod = f"{acc} + (uint64_t)((int64_t){hx} * {hy})"
        else:
            self.unsupported("long multiply variant")
        return f"uint64_t u = {prod}; " + self.write(rlo, "(uint32_t)u") + " " + self.write(rhi, "(uint32_t)(u >> 32)")

    # ---- VFP (VFPv3-D32, single and double) -----------------------------------------------------------------------
    def vfp(self, h1: int, h2: int) -> str:
        if ((h2 >> 9) & 7) != 5:
            self.unsupported("coprocessor access")
        dbl = (h2 >> 8) & 1
        kind = (h1 >> 8) & 15
        if kind == 0xE:
            return self.vfp_dp(h1, h2, dbl) if not (h2 & 0x10) else self.vfp_xfer(h1, h2, dbl)
        if kind not in (0xC, 0xD):
            self.unsupported("coprocessor encoding")
        if (h1 & 0xFFE0) == 0xEC40:  # vmov between two core registers and two single / one double register
            to_core, rt2, rt = (h1 >> 4) & 1, h1 & 15, (h2 >> 12) & 15
            M, Vm = (h2 >> 5) & 1, h2 & 15
            if dbl:
                k = M << 4 | Vm
                if to_core:
                    return (f"uint64_t u = c->vfp.d[{k}]; {self.write(rt, '(uint32_t)u')} "
                            f"{self.write(rt2, '(uint32_t)(u >> 32)')}")
                return f"c->vfp.d[{k}] = ((uint64_t){self.R(rt2)} << 32) | {self.R(rt)};"
            k = Vm << 1 | M
            if k > 30:
                self.unsupported("vmov to s31 pair")
            if to_core:
                return (f"uint32_t lo = c->vfp.u32[{k}], hi = c->vfp.u32[{k + 1}]; {self.write(rt, 'lo')} "
                        f"{self.write(rt2, 'hi')}")
            return f"c->vfp.u32[{k}] = {self.R(rt)}; c->vfp.u32[{k + 1}] = {self.R(rt2)};"
        P, U, D, W, L, rn = (h1 >> 8) & 1, (h1 >> 7) & 1, (h1 >> 6) & 1, (h1 >> 5) & 1, (h1 >> 4) & 1, h1 & 15
        Vd, imm8 = (h2 >> 12) & 15, h2 & 0xFF
        first = (D << 4 | Vd) if dbl else (Vd << 1 | D)
        if P and not W:  # vldr / vstr
            base = self.pc & ~3 if rn == 15 else None
            off = imm8 * 4 if U else -imm8 * 4
            if base is not None and L:
                v = self.literal_const(base + off - IMAGE_BASE, 64 if dbl else 32, False)
                if v is not None:
                    return (f"c->vfp.d[{first}] = 0x{v:016x}ull;" if dbl else f"c->vfp.u32[{first}] = {hexu(v)};")
            ea = f"{hexu(base) if base is not None else f'r{rn}'} + {hexu(off)}"
            if dbl:
                return (f"c->vfp.d[{first}] = ld64(c, m, {ea});" if L else f"st64(c, m, {ea}, c->vfp.d[{first}]);")
            return (f"c->vfp.u32[{first}] = ld32(c, m, {ea});" if L else f"st32(c, m, {ea}, c->vfp.u32[{first}]);")
        if (not P and U) or (P and not U and W):
            mode = "ia" if not P else "db"
            if P == U:
                self.unsupported("vldm/vstm form")
            count = imm8 >> 1 if dbl else imm8
            if dbl and imm8 & 1:
                self.unsupported("fldmx/fstmx")
            if count == 0 or first + count > (32 if dbl else 32):
                self.unsupported("vldm/vstm register range")
            return self.vmulti(bool(L), mode, rn, first, count, bool(dbl), bool(W))
        self.unsupported("vfp load/store form")

    def vmulti(self, load: bool, mode: str, rn: int, first: int, count: int, dbl: bool, wb: bool) -> str:
        size = 8 if dbl else 4
        nb = size * count
        s = f"uint32_t b0 = r{rn}; uint32_t ea = b0{f' - {nb}u' if mode == 'db' else ''};"
        for j in range(count):
            k, a = first + j, f"ea + {size * j}u"
            if dbl:
                s += f" c->vfp.d[{k}] = ld64(c, m, {a});" if load else f" st64(c, m, {a}, c->vfp.d[{k}]);"
            else:
                s += f" c->vfp.u32[{k}] = ld32(c, m, {a});" if load else f" st32(c, m, {a}, c->vfp.u32[{k}]);"
        if wb:
            s += f" r{rn} = b0 {'-' if mode == 'db' else '+'} {nb}u;"
        return s

    def vfp_xfer(self, h1: int, h2: int, dbl: int) -> str:
        """Register transfers between core and VFP registers (single-register vmov, vmrs, vmsr)."""
        A, L, Vn, rt = (h1 >> 5) & 7, (h1 >> 4) & 1, h1 & 15, (h2 >> 12) & 15
        N = (h2 >> 7) & 1
        if A == 0 and not dbl and (h2 & 0x6F) == 0:
            k = Vn << 1 | N
            if L:
                return self.write(rt, f"c->vfp.u32[{k}]")
            return f"c->vfp.u32[{k}] = {self.R(rt)};"
        if A == 7 and not dbl and Vn == 1 and (h2 & 0xEF) == 0:
            if L:
                if rt == 15:
                    return "n = c->fpscr >> 31; z = (c->fpscr >> 30) & 1; cf = (c->fpscr >> 29) & 1; v = (c->fpscr >> 28) & 1;"
                return self.write(rt, "c->fpscr")
            return f"c->fpscr = {self.R(rt)};"
        self.unsupported("vfp register transfer (scalar or system register)")

    def vfp_dp(self, h1: int, h2: int, dbl: int) -> str:
        p, D, q, r = (h1 >> 7) & 1, (h1 >> 6) & 1, (h1 >> 5) & 1, (h1 >> 4) & 1
        Vn, Vd, N, op, M, Vm = h1 & 15, (h2 >> 12) & 15, (h2 >> 7) & 1, (h2 >> 6) & 1, (h2 >> 5) & 1, h2 & 15

        def reg(V, X, is_double):
            return (X << 4 | V) if is_double else (V << 1 | X)

        d, n, mm = reg(Vd, D, dbl), reg(Vn, N, dbl), reg(Vm, M, dbl)
        t = "double" if dbl else "float"
        val = (lambda k: f"c->vfp.f64[{k}]") if dbl else (lambda k: f"c->vfp.s[{k}]")
        raw = (lambda k: f"c->vfp.d[{k}]") if dbl else (lambda k: f"c->vfp.u32[{k}]")
        if (p, q, r) != (1, 1, 1):
            sel = (p, q, r)
            names = {(0, 0, 0): ("vmla", "vmls"), (0, 0, 1): ("vnmls", "vnmla"), (0, 1, 0): ("vmul", "vnmul"),
                     (0, 1, 1): ("vadd", "vsub"), (1, 0, 0): ("vdiv", None)}
            if sel not in names or names[sel][op] is None:
                self.unsupported("fused multiply-add")
            name = names[sel][op]
            a, b, dd = val(n), val(mm), val(d)
            pre = f"{t} pr = {a} * {b}; " if name in ("vmla", "vmls", "vnmla", "vnmls", "vnmul") else ""
            e = {"vadd": f"{a} + {b}", "vsub": f"{a} - {b}", "vmul": f"{a} * {b}", "vdiv": f"{a} / {b}",
                 "vmla": f"{dd} + pr", "vmls": f"{dd} - pr", "vnmla": f"-{dd} - pr", "vnmls": f"-{dd} + pr",
                 "vnmul": "-pr"}[name]
            return f"{pre}{t} res = {e}; {val(d)} = res;"
        opc2, opb = Vn, (h2 >> 6) & 3
        if (h2 & 0xF0) == 0:  # vmov immediate: imm4H = Vn, imm4L = Vm
            bits = vfp_expand_imm(Vn << 4 | Vm, bool(dbl))
            return f"c->vfp.d[{d}] = 0x{bits:016x}ull;" if dbl else f"c->vfp.u32[{d}] = {hexu(bits)};"
        if opc2 == 0 and opb == 1:  # vmov rd, rm
            return f"{raw(d)} = {raw(mm)};"
        if opc2 == 0 and opb == 3:  # vabs
            return (f"{raw(d)} = {raw(mm)} & 0x7fffffffffffffffull;" if dbl else
                    f"{raw(d)} = {raw(mm)} & 0x7fffffffu;")
        if opc2 == 1 and opb == 1:  # vneg
            return (f"{raw(d)} = {raw(mm)} ^ 0x8000000000000000ull;" if dbl else
                    f"{raw(d)} = {raw(mm)} ^ 0x80000000u;")
        if opc2 == 1 and opb == 3:  # vsqrt
            return f"{val(d)} = {'sqrt' if dbl else 'sqrtf'}({val(mm)});"
        if opc2 in (4, 5) and op:  # vcmp / vcmpe
            b = "0.0" if opc2 == 5 else val(mm)
            return f"c->fpscr = (c->fpscr & 0x0fffffffu) | fcmp((double){val(d)}, (double){b});"
        if opc2 == 7 and opb == 3:  # vcvt between double and single
            if dbl:  # f32.f64: Sd = (float)Dm
                return f"c->vfp.s[{reg(Vd, D, 0)}] = (float)c->vfp.f64[{reg(Vm, M, 1)}];"
            return f"c->vfp.f64[{reg(Vd, D, 1)}] = (double)c->vfp.s[{reg(Vm, M, 0)}];"
        if opc2 == 8 and op:  # vcvt from integer: Sm holds the integer
            src = f"(int32_t)c->vfp.u32[{reg(Vm, M, 0)}]" if N else f"c->vfp.u32[{reg(Vm, M, 0)}]"
            return f"{val(d)} = ({t}){src};"
        if opc2 in (12, 13) and op:  # vcvt to integer: Sd receives it
            x = f"(double){val(mm)}"
            if not N:  # vcvtr: round per FPSCR (to nearest-even here)
                x = f"nearbyint({x})"
            fn = "f2s32" if opc2 == 13 else "f2u32"
            return f"c->vfp.u32[{reg(Vd, D, 0)}] = {fn}({x});"
        self.unsupported("vfp data processing")
