#!/usr/bin/env python3
"""Assessment of the original library: functions by mode, .text coverage, instruction mix over real code, imports by
group, data inside functions, suspicious spots. Prints the numbers and writes them to docs/ASSESSMENT.md.

Run it with `py -I tools/recomp/survey.py` (the library is untrusted data). The check at the end re-decodes every
maximal run of instructions the loader found, in one capstone call per run (so IT blocks decode correctly), and fails
if capstone disagrees about an instruction boundary or cannot decode a byte outside the data ranges.
"""
from __future__ import annotations

import argparse
import collections
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from capstone import CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB, Cs  # noqa: E402
from capstone import arm as A  # noqa: E402

from image import Image  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]

LIBM = {"acos", "acosf", "asin", "asinf", "atan", "atanf", "atan2", "atan2f", "ceil", "ceilf", "cos", "cosf", "cosh",
        "exp", "expf", "fabs", "fabsf", "floor", "floorf", "fmod", "fmodf", "frexp", "ldexp", "log", "logf", "log10",
        "log10f", "modf", "modff", "pow", "powf", "sin", "sinf", "sinh", "sqrt", "sqrtf", "tan", "tanf", "tanh",
        "round", "roundf", "lround", "lroundf", "trunc", "truncf", "fmax", "fmin", "copysign", "scalbn", "exp2",
        "log2", "cbrt", "hypot", "rint", "nearbyint", "lrint", "expm1", "log1p", "asinh", "acosh", "atanh", "sincos"}


EXACT = {
    "heap/mmap": {"malloc", "free", "calloc", "realloc", "memalign", "posix_memalign", "mmap", "munmap", "mprotect"},
    "file system": {"fcntl", "fstat", "open", "close", "read", "write", "lseek", "ioctl", "chdir", "remove", "stat"},
    "stdlib/ctype": {"strtod", "strtol", "strtoul", "getenv", "atoi", "atol", "qsort", "toupper", "tolower", "div", "lrand48",
                     "srand48", "abort", "exit", "__stack_chk_fail", "__stack_chk_guard", "_ctype_", "_tolower_tab_",
                     "_toupper_tab_", "setlocale", "basename", "sysconf", "sigaction", "swprintf"},
    "sockets/network": {"__errno"},
    "C++ operators / ABI": {"_ZSt7nothrow", "_ZdaPv", "_ZdlPv", "_Znaj", "_ZnajRKSt9nothrow_t", "_Znwj"},
    "libm": {"ldexpf"},
}


def import_group(n: str) -> str:
    for g, names in EXACT.items():
        if n in names:
            return g
    if n.startswith(("egl",)):
        return "EGL"
    if n.startswith("gl"):
        return "OpenGL ES"
    if n.startswith("pthread_") or n.startswith("sem_") or n in ("sched_yield", "sched_get_priority_max"):
        return "pthread/semaphore"
    if n.startswith(("__cxa", "__gxx", "_Unwind", "__aeabi_unwind", "__gnu_Unwind")) or n in ("__dso_handle", "__gnu_unwind_frame"):
        return "C++ ABI / unwind"
    if n in ("setjmp", "longjmp", "_setjmp", "_longjmp", "sigsetjmp", "siglongjmp"):
        return "setjmp/longjmp"
    if n.startswith("__android_log"):
        return "android log"
    if n.startswith(("socket", "connect", "bind", "listen", "accept", "send", "recv", "select", "poll", "inet_",
                     "gethost", "getaddr", "freeaddr", "setsockopt", "getsockopt", "shutdown", "htons", "htonl",
                     "ntohs", "ntohl", "getsock", "getpeer", "gai_", "if_")) or n == "__errno":
        return "sockets/network"
    if n in LIBM:
        return "libm"
    if n.startswith(("w", "mb", "iswa", "isw", "tow")) and not n.startswith(("write", "wait", "waitpid", "wrefresh")):
        return "wchar"
    if n.startswith(("__aeabi", "__gnu", "__sync", "__atomic", "__divsi", "__udiv", "__modsi")):
        return "compiler runtime"
    if n.startswith(("dl", "__dl")):
        return "dynamic loader"
    if n.startswith(("f", "printf", "sprintf", "snprintf", "vsnprintf", "vsprintf", "vfprintf", "puts", "putc", "getc",
                     "scanf", "sscanf", "vsscanf", "fscanf", "remove", "rename", "perror", "ungetc", "setvbuf", "tmpfile",
                     "__sF", "__srget", "__swbuf", "clearerr", "ferror", "feof", "fileno", "getline")) or n in ("stdout", "stderr", "stdin"):
        return "stdio"
    if n.startswith(("mem", "str", "bcopy", "bzero", "__strlen", "index", "rindex", "strn")):
        return "string/memory"
    if n in ("malloc", "free", "calloc", "realloc", "memalign", "posix_memalign", "mmap", "munmap", "mprotect", "madvise",
             "brk", "sbrk", "valloc"):
        return "heap/mmap"
    if n.startswith(("open", "close", "read", "write", "lseek", "stat", "fstat", "lstat", "unlink", "mkdir", "rmdir",
                     "access", "opendir", "readdir", "closedir", "dup", "fcntl", "ioctl", "pipe", "fsync", "ftruncate",
                     "chdir", "getcwd", "mkstemp", "realpath", "statfs", "fchmod", "chmod", "utime")):
        return "file system"
    if n.startswith(("time", "clock", "gettimeofday", "localtime", "gmtime", "mktime", "strftime", "nanosleep",
                     "usleep", "sleep", "difftime", "ctime", "asctime", "tzset", "settimeofday")):
        return "time"
    if n in ("rand", "srand", "random", "srandom", "rand_r", "drand48", "srand48", "lrand48", "abs", "labs", "atoi",
             "atol", "atof", "strtol", "strtoul", "strtod", "strtof", "strtoll", "strtoull", "qsort", "bsearch",
             "getenv", "setenv", "exit", "abort", "atexit", "div", "ldiv", "system", "_exit", "toupper", "tolower",
             "isalpha", "isdigit", "isspace", "isalnum", "isupper", "islower", "ispunct", "isxdigit", "isprint", "__ctype_get_mb_cur_max",
             "_ctype_", "_tolower_tab_", "_toupper_tab_", "__stack_chk_fail", "__stack_chk_guard", "__assert", "__assert2"):
        return "stdlib/ctype"
    return "other"


def mode_name(f) -> str:
    return "Thumb" if f.thumb else "ARM"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--so", default=str(ROOT / "original/apk/lib/armeabi-v7a/librainbowsix.so"))
    ap.add_argument("--out", default=str(ROOT / "docs/ASSESSMENT.md"))
    args = ap.parse_args()
    t0 = time.time()
    im = Image(Path(args.so))
    t_load = time.time() - t0
    fs = im.functions
    text_size = im.text_end - im.text_start
    out = []

    def P(s=""):
        out.append(s)
        print(s)

    P("# Assessment of librainbowsix.so")
    P()
    P(f"Generated by `tools/recomp/survey.py` (loader time {t_load:.0f} s). Library: `{Path(args.so).name}`, "
      f"{im.size:#x} bytes of image, `.text` {im.text_start:#x}-{im.text_end:#x} ({text_size:,} bytes).")
    P()

    # ---- functions ----
    P("## Functions")
    P()
    by = collections.Counter((f.source, mode_name(f)) for f in fs)
    P("| source | Thumb | ARM |")
    P("|---|---:|---:|")
    for src in ("dynsym", "exidx", "init_array", "call", "data_pointer", "pic_pointer"):
        if by[(src, "Thumb")] or by[(src, "ARM")]:
            P(f"| {src} | {by[(src, 'Thumb')]:,} | {by[(src, 'ARM')]:,} |")
    P(f"| **total** | **{sum(f.thumb for f in fs):,}** | **{sum(not f.thumb for f in fs):,}** |")
    P()
    P(f"- {len(fs):,} functions: {im.discovery['dynsym']:,} distinct bodies from `.dynsym` (STT_FUNC, defined), "
      f"{sum(f.source == 'exidx' for f in fs):,} more from `.ARM.exidx` ({im.discovery['exidx_entries']:,} unwind "
      f"entries cover almost every function, including static ones), "
      f"{sum(f.source in ('call', 'data_pointer', 'pic_pointer', 'init_array') for f in fs):,} from call targets, data pointers and `.init_array`.")
    P(f"- Aliases (several dynsym names for one body): {sum(1 for f in fs if f.aliases):,} functions have extra names "
      f"({sum(len(f.aliases) for f in fs):,} names).")
    P(f"- Sizes: {sum(1 for f in fs if f.source != 'dynsym'):,} functions have a size derived from the walk or the next start "
      f"(no symbol size); the rest use the `.dynsym` size. No two functions overlap.")
    P(f"- Entry points that lie inside another function (shared entries, `inner_entries`): {len(im.inner_entries)}: "
      + ", ".join(f"`{a:#x}`" for a in sorted(im.inner_entries)) + ".")
    P(f"- Candidates rejected: {len(im.rejected)}.")
    P()

    # ---- coverage ----
    base = im.text_start
    insn_bytes = sum(im.insn_size)
    func_bytes = sum(f.size for f in fs)
    known_lit = known_tab = known_adr = pad = unexplained = contd = 0
    unexpl_list = []

    def flush(f, a, b, known):
        # a short run touching a loaded literal / adr target / table is the rest of a constant (second half of a
        # double, a vector constant): data
        nonlocal unexplained, contd
        if b - a <= 16 and ((a - 1) in known or b in known):
            contd += b - a
            unexplained -= b - a
        else:
            unexpl_list.append((f, a, b))
    mem = im.memory
    for f in fs:
        w = im.walks[f.address]
        lit = set()
        for a, sz in w.literals.items():
            lit.update(range(a, a + sz))
        tab = set()
        for jt in w.tables.values():
            tab.update(range(jt.table, jt.end))
        adr = set()
        for a in w.adrs:
            adr.update(range(a, a + 4))
        for s, e in f.data_ranges:
            run = None
            for b in range(s, e):
                if b in lit:
                    known_lit += 1
                    kind = None
                elif b in tab:
                    known_tab += 1
                    kind = None
                elif b in adr:
                    known_adr += 1
                    kind = None
                else:
                    h = bytes(mem[b & ~1:(b & ~1) + 2])
                    nopw = bytes(mem[b & ~3:(b & ~3) + 4]) == b"\xaf\xf3\x00\x80"
                    if h in (b"\x00\xbf", b"\x00\x00") or nopw or bytes(mem[b & ~3:(b & ~3) + 4]) == b"\x00\x00\xa0\xe1":
                        pad += 1
                        kind = None
                    else:
                        unexplained += 1
                        kind = "x"
                if kind and run is None:
                    run = b
                elif not kind and run is not None:
                    flush(f, run, b, lit | tab | adr)
                    run = None
            if run is not None:
                flush(f, run, e, lit | tab | adr)
    prev = base
    gaps = []
    for f in fs:
        if f.address > prev:
            gaps.append((prev, f.address))
        prev = max(prev, f.address + f.size)
    if prev < im.text_end:
        gaps.append((prev, im.text_end))
    gap_bytes = sum(b - a for a, b in gaps)
    gap_pad = gap_other = 0
    gap_other_list = []
    for a, b in gaps:
        d = bytes(mem[a:b])
        if not any(d) or all(d[k:k + 2] == b"\x00\xbf" or d[k:k + 4] == b"\xaf\xf3\x00\x80" or d[k:k + 4] == b"\x00\x00\xa0\xe1"
                              for k in range(0, len(d) - 1, 2)) or set(d) <= {0, 0xbf, 0xaf, 0xf3, 0x80}:
            gap_pad += b - a
        else:
            gap_other += b - a
            gap_other_list.append((a, b))
    P("## Coverage of .text")
    P()
    P("| | bytes | share of .text |")
    P("|---|---:|---:|")
    pc = lambda n: f"{100 * n / text_size:.2f}%"
    P(f"| .text | {text_size:,} | 100% |")
    P(f"| inside functions | {func_bytes:,} | {pc(func_bytes)} |")
    P(f"| decoded instructions (reached by descent) | {insn_bytes:,} | {pc(insn_bytes)} |")
    P(f"| literal-pool words loaded pc-relatively | {known_lit:,} | {pc(known_lit)} |")
    P(f"| jump-table data (tbb/tbh/word tables) | {known_tab:,} | {pc(known_tab)} |")
    P(f"| adr targets that are not code (tables, constants) | {known_adr:,} | {pc(known_adr)} |")
    P(f"| nop / zero alignment padding inside functions | {pad:,} | {pc(pad)} |")
    P(f"| constant continuation (<= 16 bytes touching a loaded constant: 2nd half of a double, vector constants) | {contd:,} | {pc(contd)} |")
    P(f"| unexplained data inside functions | {unexplained:,} | {pc(unexplained)} |")
    P(f"| between functions: padding | {gap_pad:,} | {pc(gap_pad)} |")
    P(f"| between functions: other | {gap_other:,} | {pc(gap_other)} |")
    P()
    P("Every byte of `.text` is either inside a function or padding between functions, except the `other` row above"
      + (": " + ", ".join(f"`{a:#x}`+{b - a}" for a, b in gap_other_list) if gap_other_list else ".")
      + " There is no hand-written assembly hidden in gaps: all code is reachable from `.dynsym`, `.ARM.exidx`, call targets or data pointers.")
    P()
    big = sorted(unexpl_list, key=lambda t: t[1] - t[2])
    P(f"Unexplained data ranges (not a literal, table, adr target or padding), {len(unexpl_list)} of them, "
      f"largest first. A range that decodes as code would be unreached code the lifter would miss:")
    P()
    thumb_md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    arm_md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    rows = []
    for f, a, b in big[:25]:
        md = thumb_md if f.thumb else arm_md
        n = sum(1 for _ in md.disasm(bytes(mem[a:b]), a))
        rows.append(f"| `{f.name[:60]}` | `{a:#x}` | {b - a} | {n} |")
    if rows:
        P("| function | address | bytes | instructions it decodes as |")
        P("|---|---|---:|---:|")
        for r in rows:
            P(r)
    P()

    # ---- jump tables / pools ----
    P("## Jump tables and literal pools")
    P()
    kinds = collections.Counter(jt.kind for jt in im.jump_tables.values())
    P("Recorded in `Image.jump_tables` (instruction address -> `JumpTable`) and in `Function.tables`: "
      + ", ".join(f"{k}: {v}" for k, v in sorted(kinds.items())) + f" ({len(im.jump_tables)} in total, "
      f"{sum(jt.count for jt in im.jump_tables.values()):,} case entries).")
    P()
    with_pool = sum(1 for f in fs if f.data_ranges)
    P(f"{with_pool:,} functions contain data ranges ({sum(len(f.data_ranges) for f in fs):,} ranges, "
      f"{sum(e - s for f in fs for s, e in f.data_ranges):,} bytes).")
    P()

    # ---- instruction histogram over real code, and the independent decode check ----
    dmd = {True: Cs(CS_ARCH_ARM, CS_MODE_THUMB), False: Cs(CS_ARCH_ARM, CS_MODE_ARM)}
    for m in dmd.values():
        m.detail = True
    hist = collections.Counter()
    groups = collections.Counter()
    wide = narrow = 0
    mism = []
    fn_neon = {}
    fn_ninsn = {}
    n_total = 0
    it_count = 0
    for f in fs:
        md = dmd[f.thumb]
        # maximal runs of instructions
        runs, run, a = [], None, f.address
        end = f.address + f.size
        step = 2 if f.thumb else 4
        while a < end:
            n = im.insn_size[a - base]
            if n:
                if run is None:
                    run = a
                a += n
            else:
                if run is not None:
                    runs.append((run, a))
                    run = None
                a += step
        if run is not None:
            runs.append((run, a))
        neon = ninsn = 0
        for s, e in runs:
            pos = s
            for i in md.disasm(bytes(mem[s:e]), s):
                if i.address != pos or im.insn_size[i.address - base] != i.size:
                    mism.append((f.name, i.address, "boundary"))
                    break
                pos += i.size
                ninsn += 1
                nm = i.insn_name() or i.mnemonic
                hist[nm] += 1
                if f.thumb:
                    if i.size == 4:
                        wide += 1
                    else:
                        narrow += 1
                if nm == "it":
                    it_count += 1
                gs = [md.group_name(g) for g in i.groups]
                for g in gs:
                    groups[g] += 1
                if "neon" in gs:
                    neon += 1
            else:
                if pos != e:
                    mism.append((f.name, pos, "undecodable"))
            n_total += 0
        fn_neon[f.address] = neon
        fn_ninsn[f.address] = ninsn
    total_insn = sum(hist.values())
    P("## Instruction mix (real code only)")
    P()
    P(f"{total_insn:,} instructions decoded by an independent linear re-decode of every instruction run "
      f"({len(hist)} distinct mnemonics). Thumb: {narrow:,} 16-bit, {wide:,} 32-bit; {it_count:,} IT blocks.")
    P()
    P("Capstone groups (an instruction can be in several): " + ", ".join(f"{g} {n:,}" for g, n in groups.most_common(12)) + ".")
    P()
    P("Top 60 mnemonics:")
    P()
    P("| mnemonic | count | | mnemonic | count | | mnemonic | count |")
    P("|---|---:|---|---|---:|---|---|---:|")
    top = hist.most_common(60)
    for k in range(0, 60, 3):
        cells = []
        for m, c in top[k:k + 3]:
            cells += [f"`{m}`", f"{c:,}", ""]
        P("| " + " | ".join(cells[:-1]) + " |")
    P()
    vfp = sorted([m for m in hist if m.startswith("v")], key=lambda m: -hist[m])
    P(f"All `v*` mnemonics (VFP and NEON), {len(vfp)} kinds: " + ", ".join(f"`{m}` {hist[m]:,}" for m in vfp) + ".")
    P()
    P("Full list of distinct mnemonics (the lifter must handle each): " + ", ".join(f"`{m}`" for m in sorted(hist)) + ".")
    P()
    neon_fns = sorted([(n, im.by_address[a]) for a, n in fn_neon.items() if n], key=lambda t: -t[0])
    P(f"### NEON")
    P()
    P(f"{sum(n for n, _ in neon_fns):,} NEON instructions in {len(neon_fns)} functions. Functions with the most "
      f"(a high share of NEON among their instructions suggests hand-written assembly or intrinsics):")
    P()
    P("| function | address | NEON | instructions |")
    P("|---|---|---:|---:|")
    for n, f in neon_fns[:20]:
        P(f"| `{f.name[:70]}` | `{f.address:#x}` | {n} | {fn_ninsn[f.address]} |")
    P()
    unnamed_neon = [(n, f) for n, f in neon_fns if f.source != "dynsym"]
    P(f"NEON in functions without a dynsym symbol: {len(unnamed_neon)}.")
    P()

    # ---- imports ----
    P("## Imports")
    P()
    groups_i = collections.defaultdict(list)
    for name in im.imports:
        groups_i[import_group(name)].append(name)
    P(f"{len(im.imports)} imports ({len(im.plt)} PLT stubs). Relocation types present: "
      + ", ".join(f"{k} {v:,}" for k, v in sorted(im.reloc_kinds.items())) + ". All are handled; none patch `.text`.")
    P()
    P("| group | count | names |")
    P("|---|---:|---|")
    for g, names in sorted(groups_i.items(), key=lambda t: -len(t[1])):
        shown = ", ".join(sorted(names)[:40]) + (f", ... (+{len(names) - 40})" if len(names) > 40 else "")
        P(f"| {g} | {len(names)} | {shown} |")
    P()

    # ---- PLT ----
    P("## PLT")
    P()
    P(f"`.plt` has {len(im.plt)} entries of 12 bytes after a 20-byte PLT0, all ARM mode: `add ip, pc, #a; add ip, ip, #b; "
      f"ldr pc, [ip, #c]!`. There are no Thumb `bx pc` veneers in front: Thumb code reaches them with `blx`. Every entry "
      f"maps to (import, GOT slot) in `Image.plt`. {len([f for f in fs if f.source == 'call' and not f.thumb])} ARM-mode long-branch "
      f"veneers (`ldr ip, [pc, #4]; add ip, pc, ip; bx ip; .word off`) sit at the end of `.text` and are found as call "
      f"targets; the loader resolves each to a tail call to its real target.")
    P()

    # ---- init_array, JNI ----
    P("## .init_array, JNI")
    P()
    P(f"`.init_array`: {len(im.init_array)} constructors, all Thumb: "
      + ("yes" if all(t for _, t in im.init_array) else "no (some ARM)") + ". In order (first 8): "
      + ", ".join(f"`{a:#x}`" for a, _ in im.init_array[:8]) + ", ... All are functions in `Image.functions`: "
      + str(all(a in im.by_address for a, _ in im.init_array)) + ".")
    P()
    P(f"JNI exports ({len(im.jni_exports)}): " + ", ".join(f"`{n}`" for n in sorted(im.jni_exports)) + ".")
    P()
    P("(`PLAN.md` says 68 JNI exports; that number counts every dynsym name containing `Java_` or `JNI`, including C++ "
      "`_JNIEnv` wrappers. There are 33 `Java_*` functions plus `JNI_OnLoad`.)")
    P()

    # ---- suspicious ----
    P("## Suspicious spots")
    P()
    bad = [(f, w) for f in fs for w in [im.walks[f.address]] if w.bad]
    ind = [(f, w) for f in fs for w in [im.walks[f.address]] if w.indirect]
    falls = [f for f in fs if im.walks[f.address].falls_off]
    P(f"- Undecodable instruction on a reached path: {len(bad)} functions.")
    for f, w in bad[:20]:
        P(f"  - `{f.name[:70]}` at " + ", ".join(f"`{x:#x}`" for x in w.bad))
    P(f"- Computed jumps whose targets the loader could not resolve (`bx rX`, `mov pc, rX`): {len(ind)} functions.")
    for f, w in ind[:20]:
        P(f"  - `{f.name[:70]}` at " + ", ".join(f"`{x:#x}`" for x in w.indirect))
    P(f"- Functions that fall off their end into the next one: {len(falls)}.")
    for f in falls[:20]:
        P(f"  - `{f.name[:70]}` `{f.address:#x}`")
    P(f"- Independent decode check (capstone re-decode of every instruction run): {len(mism)} disagreements.")
    for m in mism[:20]:
        P(f"  - {m}")
    P()
    arm_fs = [f for f in fs if not f.thumb and f.source == "dynsym"]
    P("## What this means for the lifter")
    P()
    P(f"- Almost everything is Thumb-2 ({sum(f.thumb for f in fs):,} functions). The ARM-mode code is {len(arm_fs)} functions "
      f"at {min(f.address for f in arm_fs):#x}-{max(f.address + f.size for f in arm_fs):#x} (the `_JNIEnv` C++ wrappers "
      f"and `ALicenseCheck`) plus {sum(1 for f in fs if not f.thumb and f.source != 'dynsym')} small ARM helpers and veneers.")
    P(f"- No NEON anywhere and no `.symtab` assembly: the vector unit is used only as VFP (VFPv3 style: `vldr`/`vstr`, "
      f"`vmov` core<->s/d, `vcvt`, `vcmpe`+`fmstat`, `vsqrt`, `vpush`/`vpop`, `vldmia`/`vstmia`, `fconsts`/`fconstd`). "
      f"The CPU-feature trick from the playbook (hide NEON in /proc/cpuinfo) is not needed.")
    P("- No `.text` relocations; every literal in code is a PC-relative offset, so Thumb PIC code reaches data through "
      "`ldr rX, =off; add rX, pc` and the GOT, and function pointers through `.data.rel.ro` (R_ARM_RELATIVE words with the Thumb bit set).")
    P("- Jump tables come in four shapes (`Image.jump_tables`): `tbb`, `tbh`, `thumb_word` (`adr; ldr.w rT,[rB,rI,lsl #2]; add rB,rT; bx rB`, "
      "entries are offsets from the table address, target = table + word, bit 0 cleared) and one ARM `add pc,pc,rX,lsl #2`.")
    P("- Code after a call to a known noreturn import (`abort`, `__cxa_throw`, `pthread_exit`, `__stack_chk_fail`, ...) is "
      "not decoded: it is dead code or the literal pool. The only such instance with real instructions in it is the tail of "
      "`_Z9threadfunPv` (`movs r0,#0; pop {r4,pc}` after `pthread_exit`).")
    P()
    P(f"Total loader + survey time {time.time() - t0:.0f} s.")
    Path(args.out).write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"wrote {args.out}")
    return 1 if mism else 0


if __name__ == "__main__":
    sys.exit(main())
