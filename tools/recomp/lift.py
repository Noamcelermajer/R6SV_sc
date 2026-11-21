"""Translate ARM32 (ARM mode, VFP) functions of the original library into C.

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

from capstone import CS_ARCH_ARM, CS_MODE_ARM, Cs
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
# Code the lifter does not translate (hand-written NEON assembly) gets a C body instead. Each override is a whole
# entry: it receives the CPU state like a translated function and returns to LR. Its address, size and name come
# from the symbol table's labels, which mark these routines as untyped symbols between two functions.

# DecodeAdpcmNeonMonoAsm(state *p): decodes two IMA ADPCM streams side by side (one per NEON lane in the original).
# p: [0],[1] step index A/B; [2],[3] predictor A/B; [4] src A; [5] dst A; [6] end of src A; [7] table; [8] src B;
# [9] dst B. Table words 0-15 hold 39 - (index adjustment of the nibble), words 39.. the step sizes, so the index
# update is uqsub8 then qadd8 with 39 on the index biased by 39, exactly as the original computes it. Each source
# word is 8 samples, low nibble first; samples saturate to int16. Writes back [0]-[3]; the pointers stay as they were.
ADPCM_NEON_MONO = r"""
    uint32_t p = r0, tab = ld32(c, m, p + 28), src[2] = {ld32(c, m, p + 16), ld32(c, m, p + 32)};
    uint32_t dst[2] = {ld32(c, m, p + 20), ld32(c, m, p + 36)}, end = ld32(c, m, p + 24);
    uint32_t idx[2] = {ld32(c, m, p) + 39, ld32(c, m, p + 4) + 39};
    uint32_t pred[2] = {(ld32(c, m, p + 8) + 0x8000u) & 0xffffu, (ld32(c, m, p + 12) + 0x8000u) & 0xffffu};
    while (src[0] < end) {
        uint32_t word[2] = {ld32(c, m, src[0]), ld32(c, m, src[1])};
        src[0] += 4; src[1] += 4;
        for (int k = 0; k < 8; k++)
            for (int ch = 0; ch < 2; ch++) {
                uint32_t nib = (word[ch] >> (4 * k)) & 15, step = ld32(c, m, tab + 4 * idx[ch]) & 0xffffu;
                uint32_t adj = ld32(c, m, tab + 4 * nib), r = 0;
                for (int b = 0; b < 32; b += 8) { /* uqsub8 idx, idx, adj; qadd8 idx, idx, 39 */
                    int32_t x = (int32_t)((idx[ch] >> b) & 255) - (int32_t)((adj >> b) & 255);
                    x = x < 0 ? 0 : x;
                    int32_t y = (int32_t)(int8_t)x + (b == 0 ? 39 : 0);
                    y = y > 127 ? 127 : y < -128 ? -128 : y;
                    r |= ((uint32_t)y & 255) << b;
                }
                idx[ch] = r;
                uint32_t diff = (step >> 3) + ((nib & 4) ? step : 0) + ((nib & 2) ? step >> 1 : 0) + ((nib & 1) ? step >> 2 : 0);
                diff &= 0xffffu;
                if (nib & 8) pred[ch] = pred[ch] > diff ? pred[ch] - diff : 0;
                else pred[ch] = pred[ch] + diff > 0xffffu ? 0xffffu : pred[ch] + diff;
                st16(c, m, dst[ch], (uint16_t)(pred[ch] - 0x8000u));
                dst[ch] += 2;
            }
    }
    st32(c, m, p, idx[0] - 39);
    st32(c, m, p + 4, idx[1] - 39);
    st32(c, m, p + 8, (uint32_t)(int32_t)(int16_t)(pred[0] - 0x8000u));
    st32(c, m, p + 12, (uint32_t)(int32_t)(int16_t)(pred[1] - 0x8000u));
"""

OVERRIDES = {0x9EAF8C: ("DecodeAdpcmNeonMonoAsm", 0x9EB638 - 0x9EAF8C, ADPCM_NEON_MONO)}


class Lifter:
    def __init__(self, image: Image, entries: dict | None = None):
        self.image = image
        self.md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
        self.md.detail = True
        self.entries = entries if entries is not None else {f.address: f for f in image.functions}
        self.slots = {name: k for k, name in enumerate(image.imports)}

    # ---- per function -------------------------------------------------------------------------
    def lift(self, f: Function) -> str:
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
            return (f"uint32_t tgt = {expr}; r14 = {hexu(self.ret_addr)}; RC_SAVE(); rc_dispatch(c, m, tgt); "
                    f"RC_LOAD(); RC_AFTER_CALL({hexu(self.ret_addr)});")
        if self.returns:
            return f"pcv = {expr}; goto out;"
        cases = " ".join(f"case {hexu(IMAGE_BASE + t)}: goto L_{t:07x};" for t in sorted(set(self.table_targets())))
        sw = f"switch (pcv) {{ {cases} }} " if cases else ""
        # not a local target: a tail jump to another function
        return f"pcv = {expr}; {sw}c->r[15] = pcv; RC_SAVE(); rc_dispatch(c, m, pcv); RC_TAIL();"

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
            if ops[0].type != A.ARM_OP_REG:
                raise Unsupported("blx to Thumb code")
            ret = hexu(self.ret_addr)
            return (f"uint32_t tgt = {self.rd(ops[0].reg)}; r14 = {ret}; RC_SAVE(); rc_dispatch(c, m, tgt); "
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
    """(address, target, is_bl) of the B/BL instructions in f, decoded from the words (ARM mode, cond != 1111)."""
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
    """Every address that is entered from outside its function: the symbol table's functions, plus the targets of
    calls and branches that land inside another function (libgcc's shared entry points, for example). Such an
    entry runs from its address to the end of the function that contains it."""
    out = {f.address: f for f in image.functions}
    for a, (name, size, _) in OVERRIDES.items():
        out[a] = Function(a, size, name)
    ordered = sorted(out)
    work = list(image.functions)
    while work:
        f = work.pop()
        for a, t, is_bl in branch_targets(image, f):
            inside = f.address <= t < f.address + f.size
            if t in out or t in image.plt or (inside and not is_bl):
                continue
            k = bisect.bisect_right(ordered, t) - 1
            if k < 0:
                continue
            parent = out[ordered[k]]
            if not parent.address < t < parent.address + parent.size or parent.is_data(t):
                continue
            g = Function(t, parent.address + parent.size - t, f"{parent.name}+0x{t - parent.address:x}",
                         [(max(s, t), e) for s, e in parent.data_ranges if e > t])
            out[t] = g
            bisect.insort(ordered, t)
            work.append(g)
    return out
