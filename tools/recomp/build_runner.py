#!/usr/bin/env python3
"""Build the Windows host runner: the translated library (runtime variant), runtime/, and the packed image.

  py -3 tools/recomp/build_runner.py --so original/apk/lib/armeabi-v7a/librainbowsix.so --out build/runner [--fast]
  py -3 tools/recomp/build_runner.py --so ... --out build/runner-dry --no-translate     # runtime only, no link

Writes <out>/r6sv_runner.exe and <out>/image.bin (and image_meta.c, obj/). By default the translated code is the
checked build (bounds checks, loop budget, shadow call stack for the crash handler), which shares its object cache
with the differential test; --fast builds an optimized, unchecked copy instead. --no-translate skips the lifter and
the translated objects: it packs the image, compiles every runtime file with warnings on and links against a stub
`rc_dispatch`, listing the symbols that are still undefined. It is the check to run while the lifter is unfinished.

Run it:  <out>/r6sv_runner.exe --image <out>/image.bin --angle build/runner/angle --data <GloftR6HP folder>
(host_main.c documents the options: --offscreen, --frames, --shot, --tap, --drag, --wav ...)
"""
from __future__ import annotations

import argparse
import concurrent.futures
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from build_common import ROOT, RUNTIME, runtime_sources, translate  # noqa: E402

X86_FAST = ("x86_64-windows-gnu", ["-O2"], "unused")
ZIG = [sys.executable, "-m", "ziglang", "cc"]
STUB = """#include "recomp.h"
/* --no-translate: stands in for the translated library's dispatcher */
void rc_dispatch(cpu_t *c, uint8_t *m, uint32_t t) { (void)m; c->fault = RC_FAULT_DISPATCH; c->r[15] = t; }
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--so", type=Path, required=True, help="the original library (armeabi-v7a)")
    ap.add_argument("--out", type=Path, default=Path("build/runner"))
    ap.add_argument("--fast", action="store_true", help="optimized, unchecked translated code")
    ap.add_argument("--no-translate", action="store_true", help="skip the lifter; compile the runtime and link a stub")
    ap.add_argument("--no-cache", action="store_true", help="lift again even if the lifted C is cached")
    args = ap.parse_args()
    t0 = time.time()
    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    subprocess.run([sys.executable, "-I", str(ROOT / "tools/recomp/pack_image.py"), "--so", str(args.so), "--out", str(out)], check=True)

    checked = not args.fast
    objs = []
    if not args.no_translate:
        from difftest import X86_TEST, compile_objects
        image, ents, sources, _ = translate(args.so, use_cache=not args.no_cache)
        target = X86_FAST if args.fast else X86_TEST
        work = ROOT / ("build/recomp-fast" if args.fast else "build/recomp")
        objs = compile_objects(image, ents, sources, work, target=target, variant="runtime")
        print(f"translated code: {len(objs)} objects, {time.time() - t0:.0f}s")

    flags = ["-O1", "-target", "x86_64-windows-gnu", "-Wall", "-Wextra", "-Wno-unused-parameter", f"-I{RUNTIME}",
             f"-I{ROOT / 'tools/recomp/runtime'}"] + (["-DRECOMP_CHECKED"] if checked else [])
    rt_dir = out / "obj"
    rt_dir.mkdir(parents=True, exist_ok=True)
    srcs = runtime_sources("host") + [out / "image_meta.c"]
    if args.no_translate:
        (out / "stub_dispatch.c").write_text(STUB, encoding="utf-8")
        srcs.append(out / "stub_dispatch.c")
    print("runtime files:", " ".join(s.stem for s in srcs))

    def cc(src):
        obj = rt_dir / (src.stem + ".o")
        r = subprocess.run([*ZIG, "-c", *flags, str(src), "-o", str(obj)], capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError(f"{src.name}:\n{r.stderr[-3000:]}")
        if r.stderr.strip():
            print(f"warnings in {src.name}:\n{r.stderr.strip()[-1500:]}")
        return obj

    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        rt_objs = list(pool.map(cc, srcs))
    print(f"runtime compiled: {len(rt_objs)} files")
    exe = out / "r6sv_runner.exe"
    link = [*ZIG, "-target", "x86_64-windows-gnu", *map(str, objs), *map(str, rt_objs), "-luser32", "-lgdi32", "-lwinmm",
            "-Wl,--stack,33554432", "-o", str(exe)]
    r = subprocess.run(link, capture_output=True, text=True)
    if r.returncode:
        undefined = sorted({ln.split("undefined symbol:")[1].strip() for ln in r.stderr.splitlines() if "undefined symbol:" in ln})
        if args.no_translate:
            print(f"link (expected to fail without translated code): {len(undefined)} undefined symbols")
            for u in undefined:
                print("  ", u)
            return
        raise RuntimeError(f"link failed:\n{r.stderr[-4000:]}")
    print(f"{exe}: {exe.stat().st_size} bytes, {time.time() - t0:.0f}s")


if __name__ == "__main__":
    main()
