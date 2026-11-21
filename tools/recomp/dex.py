#!/usr/bin/env python3
"""Minimal classes.dex reader: classes, methods (with access flags) and, per method, the calls and strings it uses.

Enough to see which native methods the Java side declares and in which order it calls them.

  py -3 tools/recomp/dex.py classes.dex --natives
  py -3 tools/recomp/dex.py classes.dex --class GL2JNILib        # calls and strings of each method
"""
from __future__ import annotations

import argparse
import struct
from pathlib import Path

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
         self.proto_ids_off, _, _, self.method_ids_size, self.method_ids_off, self.class_defs_size,
         self.class_defs_off) = h[9:21]
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

    def classes(self):
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
        """(kind, text) for each invoke and const-string in a method body, in order."""
        if not code_off:
            return []
        n = struct.unpack_from("<I", self.b, code_off + 12)[0]
        ins = struct.unpack_from(f"<{n}H", self.b, code_off + 16)
        out, pc = [], 0
        widths = _widths()
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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dex", type=Path)
    ap.add_argument("--natives", action="store_true")
    ap.add_argument("--class", dest="cls")
    args = ap.parse_args()
    d = Dex(args.dex.read_bytes())
    for cls, methods in d.classes():
        for idx, acc, code in methods:
            _, name, proto = d.method(idx)
            if args.natives and acc & ACC_NATIVE:
                print(f"{cls} {name}{proto}")
            if args.cls and args.cls in cls:
                print(f"{cls}->{name}{proto} {'native' if acc & ACC_NATIVE else ''}")
                for kind, text in d.code_refs(code):
                    print(f"    {kind}: {text}")


if __name__ == "__main__":
    main()
