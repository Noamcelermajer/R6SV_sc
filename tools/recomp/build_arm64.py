#!/usr/bin/env python3
"""Translate the whole library and build it as an arm64 Android shared object (optimized, unchecked).

  py -3 tools/recomp/build_arm64.py --so libNOVA3_neon.so --work build/arm64
"""
from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from difftest import ARM64_ANDROID, build  # noqa: E402
from image import Image  # noqa: E402
from lift import Lifter, Unsupported, entries  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--so", type=Path, required=True)
    ap.add_argument("--work", type=Path, default=Path("build/arm64"))
    args = ap.parse_args()
    t0 = time.time()
    image = Image(args.so)
    ents = entries(image)
    lifter = Lifter(image, ents)
    sources = {}
    for a, f in ents.items():
        try:
            sources[a] = lifter.lift(f)
        except Unsupported:
            pass
    lib = build(image, ents, sources, args.work, target=ARM64_ANDROID)
    print(f"{lib}: {lib.stat().st_size} bytes, {len(sources)} of {len(ents)} entries translated, "
          f"{time.time() - t0:.0f}s")


if __name__ == "__main__":
    main()
