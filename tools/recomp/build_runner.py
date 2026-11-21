#!/usr/bin/env python3
"""Build the Windows host runner: the translated library (runtime variant), runtime/, and the packed image.

  py -3 tools/recomp/build_runner.py --so libNOVA3_neon.so --out build/runner [--fast]

Writes build/runner/nova3_runner.exe and build/runner/image.bin. By default the translated code is the checked
build (bounds checks, loop budget), which shares its object cache with the differential test; --fast builds an
optimized, unchecked copy instead.
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
from difftest import X86_TEST, compile_objects  # noqa: E402
from image import Image  # noqa: E402
from lift import Lifter, Unsupported, entries  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
RUNTIME_SRC = ["core", "mem", "libc", "pthread", "jni", "gl", "sles", "printf", "game", "audio_win", "host_main"]
X86_FAST = ("x86_64-windows-gnu", ["-O2"], "unused")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--so", type=Path, required=True)
    ap.add_argument("--out", type=Path, default=Path("build/runner"))
    ap.add_argument("--fast", action="store_true", help="optimized, unchecked translated code")
    args = ap.parse_args()
    t0 = time.time()
    subprocess.run([sys.executable, str(ROOT / "tools/recomp/pack_image.py"), "--so", str(args.so), "--out",
                    str(args.out)], check=True)
    image = Image(args.so)
    ents = entries(image)
    lifter = Lifter(image, ents)
    sources = {}
    for a, f in ents.items():
        try:
            sources[a] = lifter.lift(f)
        except Unsupported:
            pass
    target = X86_FAST if args.fast else X86_TEST
    work = Path("build/recomp-fast") if args.fast else Path("build/recomp")
    objs = compile_objects(image, ents, sources, work, target=target, variant="runtime")
    print(f"translated code: {len(objs)} objects, {time.time() - t0:.0f}s")

    flags = ["-O1", "-target", "x86_64-windows-gnu", "-Wall", "-Wno-unused-parameter", f"-I{ROOT / 'runtime'}",
             f"-I{ROOT / 'tools/recomp/runtime'}"] + ([] if args.fast else ["-DRECOMP_CHECKED"])
    rt_dir = args.out / "obj"
    rt_dir.mkdir(parents=True, exist_ok=True)
    srcs = [ROOT / "runtime" / f"{n}.c" for n in RUNTIME_SRC] + [args.out / "image_meta.c"]

    def cc(src):
        obj = rt_dir / (src.stem + ".o")
        r = subprocess.run([sys.executable, "-m", "ziglang", "cc", "-c", *flags, str(src), "-o", str(obj)],
                           capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError(f"{src.name}:\n{r.stderr[-3000:]}")
        return obj

    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        rt_objs = list(pool.map(cc, srcs))
    exe = args.out / "nova3_runner.exe"
    r = subprocess.run([sys.executable, "-m", "ziglang", "cc", "-target", "x86_64-windows-gnu", *map(str, objs),
                        *map(str, rt_objs), "-luser32", "-lgdi32", "-lwinmm", "-o", str(exe)], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f"link failed:\n{r.stderr[-4000:]}")
    print(f"{exe}: {exe.stat().st_size} bytes, {time.time() - t0:.0f}s")


if __name__ == "__main__":
    main()
