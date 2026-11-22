"""Shared steps of build_runner.py, build_apk.py and build_arm64.py: translate the library, find the runtime sources.

The interface to the lifter is `Lifter(image, entries).lift(f)` (raises Unsupported for a function it cannot lift) and
`difftest.compile_objects(image, ents, sources, workdir, target=..., variant="runtime")`; nothing else of them is used
here, so the lifter can change underneath.
"""
from __future__ import annotations

import hashlib
import pickle
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / "runtime"

# platform-specific runtime files; every other runtime/*.c is common (so new files of the other streams are picked up)
HOST_ONLY = {"host_main", "audio_win"}
ANDROID_ONLY = {"android_main", "audio_android"}


def runtime_sources(platform: str) -> list[Path]:
    """The runtime/*.c files of one platform ("host" or "android")."""
    skip = ANDROID_ONLY if platform == "host" else HOST_ONLY
    return sorted(p for p in RUNTIME.glob("*.c") if p.stem not in skip)


def translate(so: Path, use_cache: bool = True):
    """Load the library and lift every entry. Returns (image, entries, sources, unlifted_count).

    Lifted C text is cached in build/cache keyed by the library, lift.py and image.py contents, so the three build
    scripts (and repeated runs) lift once."""
    from image import Image
    from lift import Lifter, Unsupported, entries

    t0 = time.time()
    image = Image(so)
    ents = entries(image)
    print(f"{len(ents)} entries found, {time.time() - t0:.0f}s")
    h = hashlib.sha256()
    for p in (Path(so), Path(__file__).parent / "lift.py", Path(__file__).parent / "image.py"):
        h.update(p.read_bytes())
    cache = ROOT / "build" / "cache" / f"lifted-{h.hexdigest()[:16]}.pkl"
    if use_cache and cache.exists():
        sources = pickle.loads(cache.read_bytes())
        print(f"lifted C from cache {cache.name}: {len(sources)} of {len(ents)} entries")
        return image, ents, sources, len(ents) - len(sources)
    lifter = Lifter(image, ents)
    sources, bad = {}, 0
    for a, f in ents.items():
        try:
            sources[a] = lifter.lift(f)
        except Unsupported:
            bad += 1
    print(f"lifted {len(sources)} of {len(ents)} entries ({bad} unsupported), {time.time() - t0:.0f}s")
    cache.parent.mkdir(parents=True, exist_ok=True)
    cache.write_bytes(pickle.dumps(sources))
    return image, ents, sources, bad
