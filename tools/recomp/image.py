"""The original library as game memory: segments loaded at IMAGE_BASE, relocated, with its functions and code ranges.

Game memory layout used by the recompiler and the tests (32-bit addresses):
  0x00000000-0x00ffffff  unmapped, so a null pointer faults
  IMAGE_BASE ...         the ELF image at its link addresses + IMAGE_BASE
"""
from __future__ import annotations

import bisect
from dataclasses import dataclass, field
from pathlib import Path

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection
from elftools.elf.descriptions import describe_reloc_type

IMAGE_BASE = 0x01000000
IMPORT_STUB_BASE = 0x00f00000  # imports resolve here: one 4-byte slot per import, never mapped as code


@dataclass
class Function:
    address: int  # link address (without IMAGE_BASE)
    size: int
    name: str
    data_ranges: list = field(default_factory=list)  # (start, end) link addresses of literal pools inside the function

    def is_data(self, addr: int) -> bool:
        return any(s <= addr < e for s, e in self.data_ranges)


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
        self.imports = {}
        self.got_imports = {}  # GOT slot link address -> import name
        self._relocate()
        self.plt = self._plt()  # PLT entry link address -> (import name, GOT slot link address)
        self.functions = self._functions()
        self.by_address = {f.address: f for f in self.functions}

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
            out[a] = (self.got_imports[got], got)
            a += 12
        return out

    @property
    def writable(self):
        """(start, end) link ranges of the writable segments."""
        return [(a, b) for a, b, fl in self.segments if fl & 2]

    def _relocate(self):
        dynsym = self.elf.get_section_by_name(".dynsym")
        slot = 0
        for sec in self.elf.iter_sections():
            if not isinstance(sec, RelocationSection):
                continue
            for r in sec.iter_relocations():
                kind = describe_reloc_type(r["r_info_type"], self.elf)
                at = r["r_offset"]
                word = int.from_bytes(self.memory[at:at + 4], "little")
                if kind == "R_ARM_RELATIVE":
                    word = (word + IMAGE_BASE) & 0xFFFFFFFF
                elif kind in ("R_ARM_GLOB_DAT", "R_ARM_JUMP_SLOT", "R_ARM_ABS32"):
                    sym = dynsym.get_symbol(r["r_info_sym"])
                    if sym["st_shndx"] != "SHN_UNDEF":
                        target = sym["st_value"] + IMAGE_BASE
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

    def _functions(self):
        symtab = self.elf.get_section_by_name(".symtab")
        funcs, marks = {}, []
        for s in symtab.iter_symbols():
            if s.name in ("$a", "$d", "$t"):
                marks.append((s["st_value"], s.name))
            elif s["st_info"]["type"] == "STT_FUNC" and s["st_shndx"] != "SHN_UNDEF":
                a = s["st_value"] & ~1
                old = funcs.get(a)
                # several names for one body: keep the one with a size, then the shortest name
                if old is None or (old.size == 0 and s["st_size"]) or (
                        bool(s["st_size"]) == bool(old.size) and len(s.name) < len(old.name)):
                    funcs[a] = Function(a, s["st_size"], s.name)
        # libgcc's assembly routines (__aeabi_uldivmod, __aeabi_idiv, ...) have size 0: they run to the next function
        starts = sorted(funcs)
        text = self.elf.get_section_by_name(".text")
        text_end = text["sh_addr"] + text["sh_size"]
        for k, a in enumerate(starts):
            if funcs[a].size == 0:
                funcs[a].size = (starts[k + 1] if k + 1 < len(starts) else text_end) - a
        marks.sort()
        keys = [m[0] for m in marks]
        for f in funcs.values():
            end = f.address + f.size
            i = bisect.bisect_right(keys, f.address) - 1
            state = marks[i][1] if i >= 0 else "$a"
            start = f.address
            j = i + 1
            while j < len(marks) and marks[j][0] < end:
                addr, kind = marks[j]
                if kind != state:
                    if state == "$d":
                        f.data_ranges.append((start, addr))
                    state, start = kind, addr
                j += 1
            if state == "$d":
                f.data_ranges.append((start, end))
        return sorted(funcs.values(), key=lambda f: f.address)

    def code(self, f: Function) -> bytes:
        return bytes(self.memory[f.address:f.address + f.size])
