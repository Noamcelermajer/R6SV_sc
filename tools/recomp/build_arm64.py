#!/usr/bin/env python3
"""Translate the whole library and build it as an arm64 Android shared object (optimized, unchecked): the check that
the translated C compiles for the phone, without the runtime. The APK is built by build_apk.py.

  py -3 tools/recomp/build_arm64.py --so original/apk/lib/armeabi-v7a/librainbowsix.so --work build/arm64
"""
from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from build_common import ROOT, translate  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--so", type=Path, required=True, help="the original library (armeabi-v7a)")
    ap.add_argument("--work", type=Path, default=Path("build/arm64"))
    ap.add_argument("--no-cache", action="store_true", help="lift again even if the lifted C is cached")
    args = ap.parse_args()
    from difftest import ARM64_ANDROID, build
    t0 = time.time()
    image, ents, sources, _ = translate(args.so, use_cache=not args.no_cache)
    lib = build(image, ents, sources, ROOT / args.work if not args.work.is_absolute() else args.work, target=ARM64_ANDROID)
    print(f"{lib}: {lib.stat().st_size} bytes, {len(sources)} of {len(ents)} entries translated, {time.time() - t0:.0f}s")


if __name__ == "__main__":
    main()
