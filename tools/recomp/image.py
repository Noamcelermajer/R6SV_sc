"""The original library as game memory: segments loaded at IMAGE_BASE, relocated, with its functions and code ranges.

Written for N.O.V.A. 3 (ARM, unstripped) and extended for Rainbow Six: Shadow Vanguard (Thumb-2 with VFP, only
.dynsym). Function discovery and literal-pool detection are described in docs/notes/loader.md.

Game memory layout used by the recompiler and the tests (32-bit addresses):
  0x00000000-0x00ffffff  unmapped, so a null pointer faults
  IMAGE_BASE ...         the ELF image at its link addresses + IMAGE_BASE
"""
from __future__ import annotations

import bisect
import struct
from dataclasses import dataclass, field
from pathlib import Path

from capstone import (CS_ARCH_ARM, CS_AC_WRITE, CS_MODE_ARM, CS_MODE_THUMB, Cs)
from capstone import arm as A
from elftools.elf.descriptions import describe_reloc_type
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

IMAGE_BASE = 0x01000000
IMPORT_STUB_BASE = 0x00f00000  # imports resolve here: one 4-byte slot per import, never mapped as code

# imports that never return: the code after a call to them is data (a literal pool) or the next function
NORETURN_IMPORTS = {
    "abort", "exit", "_exit", "__cxa_throw", "__cxa_rethrow", "__cxa_bad_cast", "__cxa_bad_typeid",
    "__cxa_pure_virtual", "__cxa_call_unexpected", "__assert", "__assert2", "__stack_chk_fail", "longjmp",
    "siglongjmp", "_longjmp", "pthread_exit", "_Unwind_Resume", "__android_log_assert", "__aeabi_unwind_cpp_pr0",
    "__gnu_Unwind_Resume", "__fortify_fatal", "__libc_fatal", "execv", "quick_exit",
}


@dataclass
class Function:
    address: int  # link address (without IMAGE_BASE), Thumb bit cleared
    size: int
    name: str
    data_ranges: list = field(default_factory=list)  # (start, end) link addresses of literal pools/tables inside it
    thumb: bool = False
    aliases: list = field(default_factory=list)  # the other dynsym names of the same body
    source: str = "dynsym"  # dynsym | exidx | init_array | call | data_pointer | pic_pointer
    tables: dict = field(default_factory=dict, compare=False)  # jump-table instruction address -> JumpTable

    def is_data(self, addr: int) -> bool:
        return any(s <= addr < e for s, e in self.data_ranges)


@dataclass
class JumpTable:
    kind: str  # "tbb", "tbh", "arm_add" (add pc,pc,rX,lsl #2 + row of B), "arm_ldr" (ldr pc,[pc,rX,lsl #2] + words)
    insn: int  # address of the branching instruction
    table: int  # address of the first table byte/word/branch (Thumb: insn + 4)
    count: int  # number of entries
    targets: list  # case target addresses, in table order, Thumb bit cleared (duplicates kept)
    end: int  # first address after the table's data

    @property
    def esize(self) -> int:
        return {"tbb": 1, "tbh": 2}.get(self.kind, 4)


class _Walk:
    __slots__ = ("reached", "calls", "literals", "tables", "ptrs", "bad", "indirect", "falls_off", "adrs", "branches_out")

    def __init__(self):
        self.reached = {}  # pc -> size
        self.calls = set()  # (target, thumb): direct calls and tail branches out of the range
        self.literals = {}  # address -> size of words loaded pc-relatively
        self.tables = {}  # insn -> JumpTable
        self.ptrs = set()  # (address, thumb) code pointers built pc-relatively (callbacks)
        self.bad = []  # addresses where decoding failed
        self.indirect = []  # computed jumps with unknown targets
        self.falls_off = False
        self.adrs = set()  # addresses taken with adr/add rX,pc,#n (may be code or data)


def _uncond(i) -> bool:
    return i.cc in (A.ARM_CC_AL, A.ARM_CC_INVALID)


def _writes_pc(i) -> bool:
    for o in i.operands:
        if o.type == A.ARM_OP_REG and o.reg == A.ARM_REG_PC and o.access & CS_AC_WRITE:
            return True
    return False


class _Descent:
    """Recursive descent over one function: follows every path from the entry and records what it touches."""

    def __init__(self, image: "Image"):
        self.img = image
        self.md = {False: Cs(CS_ARCH_ARM, CS_MODE_ARM), True: Cs(CS_ARCH_ARM, CS_MODE_THUMB)}
        for m in self.md.values():
            m.detail = True
        self.noret = {a for a, (n, _) in image.plt.items() if n in NORETURN_IMPORTS}

    def walk(self, start: int, thumb: bool, lo: int, hi: int, barrier=frozenset()) -> _Walk:
        img, mem = self.img, self.img.memory
        md = self.md[thumb]
        w = _Walk()
        reached, calls = w.reached, w.calls
        pcoff = 4 if thumb else 8
        step = 2 if thumb else 4
        cmps = {}  # register -> last immediate compared against (bounds tbb tables)
        regvals = {}  # register -> constant held (literal / movw / movt), for pc-relative pointer building
        adrreg, tblldr, addres, pcvals = {}, {}, {}, {}  # adr results; table loads; table entry + base; pc-relative sums
        todo = [start]
        while todo:
            pc = todo.pop()
            stop = False
            while not stop:
                if pc in reached or pc in barrier:
                    break
                if pc == hi:
                    calls.add((hi, thumb))
                    w.falls_off = True
                    break
                if pc < lo or pc > hi or pc & (step - 1):
                    break
                n = 0
                prev_mov_lr_pc = False
                for i in md.disasm(bytes(mem[pc:min(pc + 256, hi)]), pc):
                    n += 1
                    pc = i.address
                    if pc in reached or pc in barrier or (i.size == 4 and (pc + 2) in barrier):
                        stop = True
                        break
                    reached[pc] = i.size
                    nxt = pc + i.size
                    iid, ops, unc = i.id, i.operands, _uncond(i)
                    # constants and addresses held in registers (pc-relative literals, movw/movt, adr)
                    sets = []  # (dict, register, value) applied after the registers this instruction writes are dropped
                    if iid in (A.ARM_INS_LDR, A.ARM_INS_LDRB, A.ARM_INS_LDRH, A.ARM_INS_LDRSB, A.ARM_INS_LDRSH,
                               A.ARM_INS_LDRD, A.ARM_INS_VLDR) and len(ops) > 1:
                        m = next((o for o in ops if o.type == A.ARM_OP_MEM), None)
                        if m is not None and m.mem.base == A.ARM_REG_PC and not m.mem.index:
                            base = ((pc + pcoff) & ~3) if thumb else pc + 8
                            lit = base + m.mem.disp
                            sz = {A.ARM_INS_LDRB: 1, A.ARM_INS_LDRSB: 1, A.ARM_INS_LDRH: 2, A.ARM_INS_LDRSH: 2,
                                  A.ARM_INS_LDRD: 8}.get(iid, 4)
                            if iid == A.ARM_INS_VLDR and i.op_str.startswith("d"):
                                sz = 8
                            w.literals[lit] = max(w.literals.get(lit, 0), sz)
                            if iid == A.ARM_INS_LDR and ops[0].type == A.ARM_OP_REG and ops[0].reg != A.ARM_REG_PC:
                                sets.append((regvals, ops[0].reg, img.u32(lit)))
                        elif m is not None and iid == A.ARM_INS_LDR and m.mem.base in adrreg and m.mem.index \
                                and m.shift.value == 2 and ops[0].type == A.ARM_OP_REG:
                            sets.append((tblldr, ops[0].reg, (adrreg[m.mem.base], m.mem.index)))
                    elif iid == A.ARM_INS_ADR and len(ops) == 2 and ops[1].type == A.ARM_OP_IMM:
                        t = ((pc + pcoff) & ~3) + ops[1].imm if thumb else pc + 8 + ops[1].imm
                        w.adrs.add(t)
                        sets.append((adrreg, ops[0].reg, t))
                    elif iid in (A.ARM_INS_ADDW, A.ARM_INS_SUBW) and len(ops) == 3 and ops[1].type == A.ARM_OP_REG \
                            and ops[1].reg == A.ARM_REG_PC:
                        t = ((pc + pcoff) & ~3) + (ops[2].imm if iid == A.ARM_INS_ADDW else -ops[2].imm)
                        w.adrs.add(t)
                        sets.append((adrreg, ops[0].reg, t))
                    elif iid == A.ARM_INS_ADD and len(ops) == 3 and ops[1].type == A.ARM_OP_REG \
                            and ops[1].reg == A.ARM_REG_PC and ops[2].type == A.ARM_OP_IMM:
                        t = ((pc + pcoff) & ~3) + ops[2].imm if thumb else pc + 8 + ops[2].imm
                        w.adrs.add(t)
                        sets.append((adrreg, ops[0].reg, t))
                    elif iid == A.ARM_INS_MOVW and ops[1].type == A.ARM_OP_IMM:
                        sets.append((regvals, ops[0].reg, ops[1].imm & 0xFFFF))
                    elif iid == A.ARM_INS_MOVT and ops[1].type == A.ARM_OP_IMM:
                        if ops[0].reg in regvals:
                            sets.append((regvals, ops[0].reg, (regvals[ops[0].reg] & 0xFFFF) | ((ops[1].imm & 0xFFFF) << 16)))
                    elif iid == A.ARM_INS_ADD and ops and ops[-1].type == A.ARM_OP_REG:
                        regs = [o.reg for o in ops[1:] if o.type == A.ARM_OP_REG] if len(ops) == 3 else [ops[0].reg, ops[1].reg]
                        if A.ARM_REG_PC in regs:
                            other = [r for r in regs if r != A.ARM_REG_PC]
                            if len(other) == 1 and other[0] in regvals:
                                v = (regvals[other[0]] + pc + pcoff) & 0xFFFFFFFF
                                if img.in_text(v & ~1) and (v & 1 if thumb else not v & 3):
                                    w.ptrs.add((v & ~1, bool(v & 1)))
                                sets.append((pcvals, ops[0].reg, v))
                        else:  # add rN, rBase, rOffset: a table entry added to the table address
                            tb = [r for r in regs if r in tblldr]
                            ot = [r for r in regs if r in adrreg]
                            if len(tb) == 1 and ot and adrreg[ot[0]] == tblldr[tb[0]][0]:
                                sets.append((addres, ops[0].reg, tblldr[tb[0]]))
                    elif iid == A.ARM_INS_CMP and len(ops) == 2 and ops[0].type == A.ARM_OP_REG \
                            and ops[1].type == A.ARM_OP_IMM:
                        cmps[ops[0].reg] = ops[1].imm
                    if regvals or adrreg or tblldr or addres or pcvals:
                        for o in ops:
                            if o.type == A.ARM_OP_REG and o.access & CS_AC_WRITE:
                                regvals.pop(o.reg, None)
                                adrreg.pop(o.reg, None)
                                tblldr.pop(o.reg, None)
                                addres.pop(o.reg, None)
                                pcvals.pop(o.reg, None)
                    for dct, r, v in sets:
                        dct[r] = v

                    # control flow
                    if iid in (A.ARM_INS_BL, A.ARM_INS_BLX) and ops and ops[0].type == A.ARM_OP_IMM:
                        t = ops[0].imm & 0xFFFFFFFF
                        if t in img.plt_range_set:
                            if t in self.noret and unc:
                                stop = True
                                break
                        elif img.in_text(t):
                            calls.add((t & ~1, (not thumb) if iid == A.ARM_INS_BLX else thumb))
                        prev_mov_lr_pc = False
                        pc = nxt
                        continue
                    if iid == A.ARM_INS_BLX and ops and ops[0].type == A.ARM_OP_REG:
                        pc = nxt
                        continue
                    if iid == A.ARM_INS_B or iid in (A.ARM_INS_CBZ, A.ARM_INS_CBNZ):
                        t = ops[-1].imm & 0xFFFFFFFF
                        if lo <= t < hi:
                            todo.append(t)
                        elif t in img.plt_range_set:
                            pass  # tail call into a PLT stub
                        elif img.in_text(t):
                            calls.add((t, thumb))
                        if iid == A.ARM_INS_B and unc:
                            stop = True
                            break
                        pc = nxt
                        continue
                    if iid in (A.ARM_INS_TBB, A.ARM_INS_TBH):
                        jt = self._thumb_table(i, lo, hi, cmps)
                        w.tables[pc] = jt
                        todo.extend(jt.targets)
                        for t in jt.targets:
                            if not lo <= t < hi:
                                calls.add((t, True))
                        stop = True
                        break
                    if iid == A.ARM_INS_BX and ops and ops[0].type == A.ARM_OP_REG:
                        r = ops[0].reg
                        if r in addres and thumb:
                            jt = self._word_table(i, lo, hi, cmps, *addres[r])
                            w.tables[pc] = jt
                            todo.extend(jt.targets)
                            for t in jt.targets:
                                if not lo <= t < hi:
                                    calls.add((t, True))
                            stop = True
                            break
                        if r in pcvals and unc:  # veneer: ldr ip,=off; add ip, pc, ip; bx ip
                            t = pcvals[r]
                            if img.in_text(t & ~1) and t & ~1 not in img.plt_range_set:
                                calls.add((t & ~1, bool(t & 1)))
                            stop = True
                            break
                        if r != A.ARM_REG_LR and not prev_mov_lr_pc:
                            w.indirect.append(pc)
                        if unc and not prev_mov_lr_pc:
                            stop = True
                            break
                        prev_mov_lr_pc = False
                        pc = nxt
                        continue
                    if _writes_pc(i) and iid not in (A.ARM_INS_BL, A.ARM_INS_BLX):
                        if not thumb:
                            jt = self._arm_table(i, lo, hi, w)
                            if jt:
                                w.tables[pc] = jt
                                todo.extend(jt.targets)
                                for t in jt.targets:
                                    if not lo <= t < hi:
                                        calls.add((t, False))
                                if unc:
                                    stop = True
                                    break
                        is_call = prev_mov_lr_pc
                        if iid in (A.ARM_INS_MOV, A.ARM_INS_ADD, A.ARM_INS_LDR) and not _is_return(i) \
                                and not (iid == A.ARM_INS_LDR and not any(o.type == A.ARM_OP_MEM and o.mem.base == A.ARM_REG_PC for o in ops)) \
                                and not is_call and pc not in w.tables:
                            # computed jump we do not model (mov pc, rX; ldr pc, [rX, ...]); returns are not recorded
                            w.indirect.append(pc)
                        if unc and not is_call:
                            stop = True
                            break
                        prev_mov_lr_pc = False
                        pc = nxt
                        continue
                    prev_mov_lr_pc = (not thumb and iid == A.ARM_INS_MOV and len(ops) == 2 and ops[0].reg == A.ARM_REG_LR
                                      and ops[1].type == A.ARM_OP_REG and ops[1].reg == A.ARM_REG_PC)
                    pc = nxt
                if n == 0 and not stop:
                    w.bad.append(pc)
                    break
                # chunk exhausted: continue decoding from pc (stop flag ends the path)
        return w

    def _word_table(self, i, lo, hi, cmps, table, idx) -> JumpTable:
        """Thumb-2 switch: adr rB, table; ldr rT, [rB, rI, lsl #2]; add rB, rT; bx rB. Entry = target - table."""
        img = self.img
        count = cmps[idx] + 1 if idx in cmps and 0 <= cmps[idx] < 0x10000 else None
        targets, k, minfwd = [], 0, hi
        while True:
            pos = table + 4 * k
            if pos + 4 > hi or pos >= minfwd or (count is not None and k >= count):
                break
            t = (table + img.u32(pos)) & 0xFFFFFFFE
            if count is None and not lo <= t < hi:
                break
            targets.append(t)
            if t > pos:
                minfwd = min(minfwd, t)
            k += 1
        return JumpTable("thumb_word", i.address, table, k, targets, table + 4 * k)

    def _thumb_table(self, i, lo, hi, cmps) -> JumpTable:
        img = self.img
        kind = "tbb" if i.id == A.ARM_INS_TBB else "tbh"
        es = 1 if kind == "tbb" else 2
        table = i.address + 4
        idx = i.operands[0].mem.index
        count = None
        if idx in cmps and 0 <= cmps[idx] < 0x10000:
            count = cmps[idx] + 1
        # heuristic bound: the table ends where the first forward case target starts
        limit, k, targets = hi, 0, []
        minfwd = hi
        while True:
            pos = table + k * es
            if pos + es > hi or pos >= minfwd or (count is not None and k >= count):
                break
            v = img.memory[pos] if es == 1 else img.u16(pos)
            t = table + 2 * v
            targets.append(t)
            if t > pos:
                minfwd = min(minfwd, t)
            k += 1
        end = table + k * es
        end = (end + 1) & ~1  # code after a tbb table is halfword aligned
        return JumpTable(kind, i.address, table, k, targets, end)

    def _arm_table(self, i, lo, hi, w):
        img, ops, iid = self.img, i.operands, i.id
        if iid == A.ARM_INS_ADD and len(ops) == 3 and ops[1].type == A.ARM_OP_REG and ops[1].reg == A.ARM_REG_PC:
            t = i.address + 8
            out = [i.address + 4]
            while lo <= t < hi and (img.u32(t) >> 24) == 0xEA:
                out.append(t)
                t += 4
            return JumpTable("arm_add", i.address, i.address + 8, len(out) - 1, out, i.address + 8)  # rows are code: no table data
        if iid == A.ARM_INS_LDR and len(ops) > 1 and ops[1].type == A.ARM_OP_MEM and ops[1].mem.base == A.ARM_REG_PC \
                and ops[1].mem.index:
            t = i.address + 8
            out = [i.address + 4] if not _uncond(i) else []
            n = 0
            while t < hi:
                v = img.u32(t)
                if not lo <= v < hi or v & 3:
                    break
                out.append(v)
                n += 1
                t += 4
            return JumpTable("arm_ldr", i.address, i.address + 8, n, out, t)
        return None


def _is_return(i) -> bool:
    ops = i.operands
    if i.id in (A.ARM_INS_POP, A.ARM_INS_LDM, A.ARM_INS_LDMDA, A.ARM_INS_LDMDB, A.ARM_INS_LDMIB):
        return True
    if i.id == A.ARM_INS_MOV and len(ops) == 2 and ops[1].type == A.ARM_OP_REG and ops[1].reg == A.ARM_REG_LR:
        return True
    if i.id == A.ARM_INS_LDR and len(ops) > 1 and ops[1].type == A.ARM_OP_MEM and ops[1].mem.base == A.ARM_REG_SP:
        return True
    return False


class Image:
    def __init__(self, path: Path):
        self.path = Path(path)
        raw = self.path.read_bytes()
        self.elf = ELFFile(self.path.open("rb"))
        loads = [s for s in self.elf.iter_segments() if s["p_type"] == "PT_LOAD"]
        self.size = max(s["p_vaddr"] + s["p_memsz"] for s in loads)
        self.size = (self.size + 0xFFFF) & ~0xFFFF
        self.memory = bytearray(self.size)
        self.segments = []
        for s in loads:
            off, va, fs = s["p_offset"], s["p_vaddr"], s["p_filesz"]
            self.memory[va:va + fs] = raw[off:off + fs]
            self.segments.append((va, va + s["p_memsz"], s["p_flags"]))
        text = self.elf.get_section_by_name(".text")
        self.text_start = text["sh_addr"]
        self.text_end = text["sh_addr"] + text["sh_size"]
        self.imports = {}
        self.got_imports = {}  # GOT slot link address -> import name
        self.reloc_pointers = {}  # address of a relocated word -> link address it points to (RELATIVE / defined symbols)
        self.text_relocs = []  # relocations that patch the code itself (should be empty)
        self._relocate()
        self.plt = self._plt()  # PLT entry link address -> (import name, GOT slot link address)
        self.plt_range_set = set(self.plt)
        self.functions = []
        self.by_address = {}
        self.jump_tables = {}  # tbb/tbh/ARM jump-table instruction address -> JumpTable
        self.inner_entries = set()  # call/branch targets inside another function (shared entry points)
        self.init_array = []  # (address, thumb) of the static constructors, in order
        self.jni_exports = {}  # name -> address
        self.insn_size = bytearray(self.text_end - self.text_start)  # per text byte: size of the instruction starting there
        self.discovery = {}
        self.rejected = []  # (address, reason) of candidate entries that were not made functions
        self._discover()
        self.functions.sort(key=lambda f: f.address)
        self.by_address = {f.address: f for f in self.functions}

    # ---- memory access -------------------------------------------------------------------------------------------

    def u32(self, a: int) -> int:
        return int.from_bytes(self.memory[a:a + 4], "little")

    def u16(self, a: int) -> int:
        return int.from_bytes(self.memory[a:a + 2], "little")

    def in_text(self, a: int) -> bool:
        return self.text_start <= a < self.text_end

    def is_insn(self, a: int) -> bool:
        return self.in_text(a) and self.insn_size[a - self.text_start] != 0

    def instructions(self, f: Function):
        """(address, size) of every instruction found in f, in address order."""
        base = self.text_start
        a = f.address
        end = f.address + f.size
        while a < end:
            n = self.insn_size[a - base]
            if n:
                yield a, n
                a += n
            else:
                a += 2 if f.thumb else 4

    @property
    def writable(self):
        """(start, end) link ranges of the writable segments."""
        return [(a, b) for a, b, fl in self.segments if fl & 2]

    def code(self, f: Function) -> bytes:
        return bytes(self.memory[f.address:f.address + f.size])

    # ---- PLT and relocations -------------------------------------------------------------------------------------

    def _plt(self):
        """Decode the PLT entries: `add ip, pc, #a; add ip, ip, #b; ldr pc, [ip, #c]!` -> GOT slot entry+8+a+b+c."""
        sec = self.elf.get_section_by_name(".plt")
        start, end = sec["sh_addr"], sec["sh_addr"] + sec["sh_size"]
        out = {}
        a = start + 20  # PLT0 is 5 words
        while a + 12 <= end:
            w = [int.from_bytes(self.memory[a + 4 * k:a + 4 * k + 4], "little") for k in range(3)]
            if w[0] & 0x0FFFF000 != 0x028FC000 or w[1] & 0x0FFFF000 != 0x028CC000 or w[2] & 0x0FFFF000 != 0x05BCF000:
                raise ValueError(f"unexpected PLT entry at {a:#x}")
            imm = lambda x: ((x & 0xFF) >> (((x >> 8) & 0xF) * 2) | (x & 0xFF) << (32 - ((x >> 8) & 0xF) * 2)) & 0xFFFFFFFF \
                if (x >> 8) & 0xF else x & 0xFF
            got = (a + 8 + imm(w[0]) + imm(w[1]) + (w[2] & 0xFFF)) & 0xFFFFFFFF
            if got not in self.got_imports:
                raise ValueError(f"PLT entry {a:#x} uses GOT slot {got:#x} with no import relocation")
            out[a] = (self.got_imports[got], got)
            a += 12
        return out

    def _relocate(self):
        dynsym = self.elf.get_section_by_name(".dynsym")
        slot = 0
        self.reloc_kinds = {}
        for sec in self.elf.iter_sections():
            if not isinstance(sec, RelocationSection):
                continue
            for r in sec.iter_relocations():
                kind = describe_reloc_type(r["r_info_type"], self.elf)
                self.reloc_kinds[kind] = self.reloc_kinds.get(kind, 0) + 1
                at = r["r_offset"]
                word = int.from_bytes(self.memory[at:at + 4], "little")
                if self.text_start <= at < self.text_end:
                    self.text_relocs.append(at)
                if kind == "R_ARM_RELATIVE":
                    self.reloc_pointers[at] = word
                    word = (word + IMAGE_BASE) & 0xFFFFFFFF
                elif kind in ("R_ARM_GLOB_DAT", "R_ARM_JUMP_SLOT", "R_ARM_ABS32"):
                    sym = dynsym.get_symbol(r["r_info_sym"])
                    if sym["st_shndx"] != "SHN_UNDEF":
                        target = sym["st_value"] + IMAGE_BASE
                        addend = word if kind == "R_ARM_ABS32" else 0
                        self.reloc_pointers[at] = (sym["st_value"] + addend) & 0xFFFFFFFF
                    else:
                        if sym.name not in self.imports:
                            self.imports[sym.name] = IMPORT_STUB_BASE + 4 * slot
                            slot += 1
                        target = self.imports[sym.name]
                        self.got_imports[at] = sym.name
                    word = ((word if kind == "R_ARM_ABS32" else 0) + target) & 0xFFFFFFFF
                else:
                    raise ValueError(f"unhandled relocation {kind} at {at:#x}")
                self.memory[at:at + 4] = word.to_bytes(4, "little")

    # ---- function discovery --------------------------------------------------------------------------------------

    def _exidx_starts(self):
        sec = self.elf.get_section_by_name(".ARM.exidx")
        if sec is None:
            return []
        out = []
        base = sec["sh_addr"]
        for k in range(sec["sh_size"] // 8):
            w = self.u32(base + 8 * k)
            if w & 0x80000000:
                continue
            v = w & 0x7FFFFFFF
            if v & 0x40000000:
                v -= 0x80000000
            out.append((base + 8 * k + v) & 0xFFFFFFFF)
        return out

    def _discover(self):
        d = _Descent(self)
        dynsym = self.elf.get_section_by_name(".dynsym")
        seeds = {}  # address -> dict(thumb, name, size, aliases, source)
        for s in dynsym.iter_symbols():
            n = s.name
            if s["st_shndx"] != "SHN_UNDEF" and (n.startswith("Java_") or n == "JNI_OnLoad") and s["st_info"]["type"] == "STT_FUNC":
                self.jni_exports[n] = s["st_value"] & ~1
            if s["st_info"]["type"] != "STT_FUNC" or s["st_shndx"] == "SHN_UNDEF":
                continue
            a = s["st_value"] & ~1
            old = seeds.get(a)
            if old is None:
                seeds[a] = dict(thumb=bool(s["st_value"] & 1), name=n, size=s["st_size"], aliases=[], source="dynsym")
                continue
            # several names for one body: keep the one with a size, then the shortest name
            if (old["size"] == 0 and s["st_size"]) or (bool(s["st_size"]) == bool(old["size"]) and len(n) < len(old["name"])):
                old["aliases"].append(old["name"])
                old["name"], old["size"] = n, s["st_size"] or old["size"]
            else:
                old["aliases"].append(n)
        n_dynsym = len(seeds)

        def covering(a):  # sized dynsym function containing a (not at its start)
            k = bisect.bisect_right(sized_starts, a) - 1
            if k >= 0:
                s0 = sized_starts[k]
                return s0 if s0 < a < s0 + seeds[s0]["size"] else None
            return None

        sized_starts = sorted(a for a, v in seeds.items() if v["size"])
        self.exidx_total = 0
        for a in self._exidx_starts():
            self.exidx_total += 1
            if not self.in_text(a) or a in seeds:
                continue
            if covering(a) is not None:
                self.inner_entries.add(a)
                continue
            seeds[a] = dict(thumb=None, name=f"sub_{a:x}", size=0, aliases=[], source="exidx")
        sec = self.elf.get_section_by_name(".init_array")
        if sec is not None:
            for k in range(sec["sh_size"] // 4):
                at = sec["sh_addr"] + 4 * k
                v = self.reloc_pointers.get(at)
                if v is None:
                    continue
                self.init_array.append((v & ~1, bool(v & 1)))
                a = v & ~1
                if a not in seeds and covering(a) is None:
                    seeds[a] = dict(thumb=bool(v & 1), name=f"init_{a:x}", size=0, aliases=[], source="init_array")
        starts = sorted(seeds)
        self._d = d
        info = {}  # address -> (Function, walk)

        def next_start(a):
            k = bisect.bisect_right(starts, a)
            return starts[k] if k < len(starts) else self.text_end

        def run(a, seed):
            thumb = seed["thumb"]
            hi = a + seed["size"] if seed["size"] else next_start(a)
            if thumb is None:
                thumb = self._guess_mode(d, a, hi)
                seed["thumb"] = thumb
            w, data = self._analyze(d, a, thumb, hi)
            return thumb, hi, w

        pending = []  # (address, thumb or None, source) candidates outside every known function
        order = sorted(seeds)
        for a in order:
            seed = seeds[a]
            thumb, hi, w = run(a, seed)
            f = self._make_function(a, thumb, hi, seed, w, trim=not seed["size"])
            info[a] = (f, w)
            self._collect(w, a, pending, f, trusted=True)
        # code reached from calls and data pointers that is not covered by any function
        for at, v in sorted(self.reloc_pointers.items()):
            if self.in_text(v & ~1) and not (self.text_start <= at < self.text_end):
                pending.append((v & ~1, bool(v & 1) if v & 1 else (None if not v & 3 else True), "data_pointer"))
        while pending:
            a, thumb, source = pending.pop()
            if not self.in_text(a) or a in info:
                continue
            k = bisect.bisect_right(starts, a) - 1
            if k >= 0 and a < starts[k] + info[starts[k]][0].size:
                if a != starts[k]:
                    self.inner_entries.add(a)
                continue
            if source == "data_pointer" and thumb is None:
                if a & 3:
                    continue
                thumb = False  # even word-aligned pointer: an ARM function, checked below
            hi = next_start(a)
            seed = dict(thumb=thumb, name=f"sub_{a:x}", size=0, aliases=[], source=source)
            if source == "data_pointer" and not self._plausible_entry(a, thumb):
                self.rejected.append((a, "data pointer is not a plausible function start"))
                continue
            thumb2, hi, w = run(a, seed)
            if w.bad and source == "data_pointer" or not w.reached:
                self.rejected.append((a, "does not decode"))
                continue
            f = self._make_function(a, thumb2, hi, seed, w, trim=True)
            bisect.insort(starts, a)
            info[a] = (f, w)
            self._collect(w, a, pending, f, trusted=source != "data_pointer")
        self._finish(info)
        self.discovery.update(dynsym=n_dynsym, exidx_entries=self.exidx_total)

    def _plausible_entry(self, a, thumb) -> bool:
        """A pointer from data is a function when its first instruction is a typical prologue or the code decodes."""
        if thumb:
            hw = self.u16(a)
            if a & 1:
                return False
            return (hw & 0xFE00) == 0xB400 or hw == 0xE92D or hw >> 11 >= 0x1D  # push {..}, push.w, or any 32-bit encoding
        w = self.u32(a)
        return a & 3 == 0 and (w >> 28) == 0xE

    def _guess_mode(self, d, a, hi) -> bool:
        """Mode of a start known only from exidx (no dynsym symbol, so no Thumb bit). Odd-halfword starts are Thumb;
        otherwise both walks are scored: a walk that hits undecodable bytes loses, and ARM wins a tie only when it has
        an ARM prologue and covers more bytes."""
        if a & 3:
            return True
        wt = d.walk(a, True, a, hi)
        wa = d.walk(a, False, a, hi)
        clean_t, clean_a = not wt.bad, not wa.bad
        if clean_t != clean_a:
            return clean_t
        w = self.u32(a)
        arm_prologue = (w & 0xFFFF0000) in (0xE92D0000, 0xE52D0000) or w == 0xE1A0C00D or (w & 0xFFFFF000) == 0xE24DD000
        bt = sum(wt.reached.values())
        ba = sum(wa.reached.values())
        return not (arm_prologue and ba > bt)

    def _analyze(self, d, a, thumb, hi):
        """Walk with barriers until no walked instruction overlaps a loaded literal or a table."""
        barrier = set()
        for _ in range(8):
            w = d.walk(a, thumb, a, hi, barrier)
            data = set()
            for lit, sz in w.literals.items():
                data.update(range(lit, lit + sz))
            for jt in w.tables.values():
                data.update(range(jt.table, jt.end))
            hit = {p for p, n in w.reached.items() if any(b in data for b in range(p, p + n))}
            if not hit:
                return w, data
            barrier |= data
        return w, data

    def _make_function(self, a, thumb, hi, seed, w, trim):
        end = hi
        if trim:
            ext = max([p + n for p, n in w.reached.items()] + [lit + sz for lit, sz in w.literals.items() if lit < hi]
                      + [jt.end for jt in w.tables.values()] + [a])
            step = 2 if thumb else 4
            end = (ext + step - 1) & ~(step - 1)
            if hi - end < 4:
                end = hi
        f = Function(a, end - a, seed["name"], [], thumb, list(seed["aliases"]), seed["source"])
        f.tables = dict(w.tables)
        self.functions.append(f)
        self.jump_tables.update(w.tables)
        return f

    def _collect(self, w, a, pending, f, trusted):
        for t, th in w.calls:
            if t != a:
                pending.append((t, th, "call"))
        for t, th in w.ptrs:
            pending.append((t, th, "pic_pointer"))
        for lit, sz in w.literals.items():
            if sz == 4:
                v = self.u32(lit)
                if self.in_text(v & ~1) and v & 1:  # an absolute Thumb pointer in a literal (non-PIC callback)
                    pending.append((v & ~1, True, "data_pointer"))

    def _finish(self, info):
        base = self.text_start
        for a, (f, w) in info.items():
            for pc, n in w.reached.items():
                if f.address <= pc < f.address + f.size:
                    self.insn_size[pc - base] = n
            # data ranges: every byte of the function that is not part of a reached instruction
            step = 2 if f.thumb else 4
            end = f.address + f.size
            runs, run, pc = [], None, f.address
            while pc < end:
                n = self.insn_size[pc - base]
                if n:
                    if run is not None:
                        runs.append((run, pc))
                        run = None
                    pc += n
                else:
                    if run is None:
                        run = pc
                    pc += step
            if run is not None:
                runs.append((run, min(pc, end)))
            f.data_ranges = runs
        self.walks = {a: w for a, (f, w) in info.items()}
