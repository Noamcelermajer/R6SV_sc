#!/usr/bin/env python3
"""Minimal classes.dex reader: classes, methods (with access flags) and, per method, the calls, field accesses, class
references and strings it uses, in bytecode order.

Enough to see which native methods the Java side declares, which exported Java_* function implements each one, and in
which order the Java code calls them. For a full listing use the SDK's `dexdump -d classes.dex`.

  py -I tools/recomp/dex.py classes.dex --natives                      # native methods (static/instance, JNI name)
  py -I tools/recomp/dex.py classes.dex --natives --so lib.so          # ... and whether lib.so exports each one
  py -I tools/recomp/dex.py classes.dex --class GameRenderer           # calls, fields, strings of each method
  py -I tools/recomp/dex.py classes.dex --class Game --method onCreate # only methods whose name contains this
  py -I tools/recomp/dex.py classes.dex --xref nativeInit              # methods whose body references this text
  py -I tools/recomp/dex.py classes.dex --classes                      # list every class with its method count
"""
from __future__ import annotations

import argparse
import struct
from pathlib import Path

ACC_STATIC = 0x8
ACC_NATIVE = 0x100


def uleb(b: bytes, i: int):
    v = shift = 0
    while True:
        x = b[i]
        i += 1
        v |= (x & 0x7F) << shift
        shift += 7
        if x < 0x80:
            return v, i


class Dex:
    def __init__(self, data: bytes):
        self.b = b = data
        h = struct.unpack_from("<8sI20sIIIIIIIIIIIIIIIIIIII", b, 0)
        (self.string_ids_size, self.string_ids_off, self.type_ids_size, self.type_ids_off, self.proto_ids_size,
         self.proto_ids_off, self.field_ids_size, self.field_ids_off, self.method_ids_size, self.method_ids_off,
         self.class_defs_size, self.class_defs_off) = h[9:21]
        self.strings = [self._string(struct.unpack_from("<I", b, self.string_ids_off + 4 * k)[0])
                        for k in range(self.string_ids_size)]
        self.types = [self.strings[struct.unpack_from("<I", b, self.type_ids_off + 4 * k)[0]]
                      for k in range(self.type_ids_size)]

    def _string(self, off):
        _, i = uleb(self.b, off)
        j = self.b.index(b"\0", i)
        return self.b[i:j].decode("utf-8", "replace")

    def proto(self, k):
        _, ret, params = struct.unpack_from("<III", self.b, self.proto_ids_off + 12 * k)
        args = []
        if params:
            n = struct.unpack_from("<I", self.b, params)[0]
            args = [self.types[struct.unpack_from("<H", self.b, params + 4 + 2 * j)[0]] for j in range(n)]
        return f"({''.join(args)}){self.types[ret]}"

    def method(self, k):
        cls, proto, name = struct.unpack_from("<HHI", self.b, self.method_ids_off + 8 * k)
        return self.types[cls], self.strings[name], self.proto(proto)

    def field(self, k):
        """(class, name, type) of field_ids[k]."""
        cls, typ, name = struct.unpack_from("<HHI", self.b, self.field_ids_off + 8 * k)
        return self.types[cls], self.strings[name], self.types[typ]

    def classes(self):
        """(descriptor, methods) with methods = [(method_idx, access_flags, code_off)]."""
        for k in range(self.class_defs_size):
            cls, acc, sup, ifaces, src, ann, data_off, static_off = struct.unpack_from("<8I", self.b,
                                                                                         self.class_defs_off + 32 * k)
            methods = []
            if data_off:
                i = data_off
                sf, i = uleb(self.b, i)
                inf, i = uleb(self.b, i)
                dm, i = uleb(self.b, i)
                vm, i = uleb(self.b, i)
                for _ in range(sf + inf):
                    _, i = uleb(self.b, i)
                    _, i = uleb(self.b, i)
                for count in (dm, vm):
                    idx = 0
                    for _ in range(count):
                        d, i = uleb(self.b, i)
                        idx += d
                        a, i = uleb(self.b, i)
                        code, i = uleb(self.b, i)
                        methods.append((idx, a, code))
            yield self.types[cls], methods

    def code_refs(self, code_off):
        """(kind, text) for each reference in a method body, in order. kind is call, string, field (text is
        'iget|iput|sget|sput Lcls;->name:type') or class (const-class, check-cast, instance-of, new-instance,
        new-array)."""
        if not code_off:
            return []
        n = struct.unpack_from("<I", self.b, code_off + 12)[0]
        ins = struct.unpack_from(f"<{n}H", self.b, code_off + 16)
        out, pc = [], 0
        widths = _widths()
        class_ops = {0x1C: "const-class", 0x1F: "check-cast", 0x20: "instance-of", 0x22: "new-instance",
                     0x23: "new-array"}
        while pc < n:
            op = ins[pc] & 0xFF
            if op == 0x00 and ins[pc] >> 8 in (1, 2, 3):  # payload pseudo-instructions
                ident = ins[pc]
                if ident == 0x100:
                    size = ins[pc + 1]
                    pc += 4 + 2 * size
                elif ident == 0x200:
                    size = ins[pc + 1]
                    pc += 2 + 4 * size
                else:
                    width, size = ins[pc + 1], ins[pc + 2] | (ins[pc + 3] << 16)
                    pc += 4 + (size * width + 1) // 2
                continue
            if 0x6E <= op <= 0x72 or 0x74 <= op <= 0x78:
                cls, name, proto = self.method(ins[pc + 1])
                out.append(("call", f"{cls}->{name}{proto}"))
            elif op == 0x1A:
                out.append(("string", self.strings[ins[pc + 1]]))
            elif op == 0x1B:
                out.append(("string", self.strings[ins[pc + 1] | (ins[pc + 2] << 16)]))
            elif 0x52 <= op <= 0x6D:  # iget* 0x52-0x58, iput* 0x59-0x5f, sget* 0x60-0x66, sput* 0x67-0x6d
                cls, name, typ = self.field(ins[pc + 1])
                kind = ("iget", "iput", "sget", "sput")[(op - 0x52) // 7]
                out.append(("field", f"{kind} {cls}->{name}:{typ}"))
            elif op in class_ops:
                out.append(("class", f"{class_ops[op]} {self.types[ins[pc + 1]]}"))
            pc += widths[op]
        return out


def _widths():
    """Instruction width in 16-bit units, by opcode (Dalvik formats)."""
    w = [1] * 256
    for op in list(range(0x02, 0x03)) + [0x05, 0x08, 0x13, 0x15, 0x16, 0x19, 0x1A, 0x1C, 0x1F, 0x20, 0x22, 0x23,
                                          0x29, 0x2D, 0x2E, 0x2F, 0x30, 0x31] + list(range(0x32, 0x38)) + \
            list(range(0x38, 0x3E)) + list(range(0x44, 0x6E)) + list(range(0x90, 0xB0)) + list(range(0xD0, 0xE3)):
        w[op] = 2
    for op in [0x03, 0x06, 0x09, 0x14, 0x17, 0x1B, 0x24, 0x25, 0x26, 0x2A, 0x2B, 0x2C] + list(range(0x6E, 0x73)) + \
            list(range(0x74, 0x79)):
        w[op] = 3
    w[0x18] = 5
    return w


def jni_name(cls: str, name: str) -> str:
    """Exported symbol the JVM looks for: Java_<mangled class>_<mangled method> (no overload suffix)."""
    def mangle(s):
        out = []
        for ch in s:
            if ch == "/":
                out.append("_")
            elif ch == "_":
                out.append("_1")
            elif ch == ";":
                out.append("_2")
            elif ch == "[":
                out.append("_3")
            elif ch.isalnum() and ord(ch) < 128:
                out.append(ch)
            else:
                out.append("_0%04x" % ord(ch))
        return "".join(out)
    return "Java_" + mangle(cls[1:-1]) + "_" + mangle(name)


def so_exports(path: Path) -> set[str]:
    from elftools.elf.elffile import ELFFile
    with open(path, "rb") as f:
        sec = ELFFile(f).get_section_by_name(".dynsym")
        return {s.name for s in sec.iter_symbols() if s["st_shndx"] != "SHN_UNDEF"}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dex", type=Path)
    ap.add_argument("--natives", action="store_true")
    ap.add_argument("--so", type=Path, help="with --natives: shared library whose exports are checked")
    ap.add_argument("--class", dest="cls", help="substring of the class descriptor")
    ap.add_argument("--method", help="with --class: only methods whose name contains this")
    ap.add_argument("--xref", help="methods whose body calls, reads or mentions text containing this")
    ap.add_argument("--classes", action="store_true")
    args = ap.parse_args()
    d = Dex(args.dex.read_bytes())
    exports = so_exports(args.so) if args.so else None
    for cls, methods in d.classes():
        if args.classes:
            print(f"{cls} {len(methods)} methods")
        for idx, acc, code in methods:
            _, name, proto = d.method(idx)
            if args.natives and acc & ACC_NATIVE:
                sym = jni_name(cls, name)
                tail = ""
                if exports is not None:
                    tail = "  exported" if sym in exports else "  NOT EXPORTED"
                print(f"{cls} {name}{proto} {'static' if acc & ACC_STATIC else 'instance'} -> {sym}{tail}")
            if args.cls and args.cls in cls and (not args.method or args.method in name):
                flags = ("static " if acc & ACC_STATIC else "") + ("native" if acc & ACC_NATIVE else "")
                print(f"{cls}->{name}{proto} {flags}")
                for kind, text in d.code_refs(code):
                    print(f"    {kind}: {text}")
            if args.xref:
                hits = [(k, t) for k, t in d.code_refs(code) if args.xref in t]
                if hits:
                    print(f"{cls}->{name}{proto}")
                    for kind, text in hits:
                        print(f"    {kind}: {text}")


if __name__ == "__main__":
    main()
