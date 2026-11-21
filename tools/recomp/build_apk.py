#!/usr/bin/env python3
"""Build the 64-bit Android APK: the translated library and runtime/ for arm64-v8a, in a NativeActivity app.

  py -3 tools/recomp/build_apk.py --so libNOVA3_neon.so [--checked] [--out build/apk]

Needs the Android SDK (build-tools, a platform, the NDK) under ANDROID_HOME, C:\\Android\\Sdk or
%LOCALAPPDATA%\\Android\\Sdk, and a
JDK (JAVA_HOME, or Microsoft's JDK 17 under Program Files) for keytool/apksigner. --checked builds the translated
code with bounds checks and the shadow call stack (slower, better crash reports).

The APK does not contain game data. Copy the extracted OBB contents (main.1050 then patch.1070 over it) to the
device: adb push <data folder>/. /sdcard/Android/data/com.nova3.recomp/files/
"""
from __future__ import annotations

import argparse
import concurrent.futures
import os
import shutil
import subprocess
import sys
import time
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from difftest import compile_objects  # noqa: E402
from image import Image  # noqa: E402
from lift import Lifter, Unsupported, entries  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
PACKAGE = "com.nova3.recomp"
API = 26
RUNTIME_SRC = ["core", "mem", "libc", "pthread", "jni", "gl", "sles", "printf", "game", "audio_android", "android_main"]

MANIFEST = f"""<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="{PACKAGE}"
    android:versionCode="1" android:versionName="0.1">
  <uses-sdk android:minSdkVersion="{API}" android:targetSdkVersion="34" />
  <uses-feature android:glEsVersion="0x00020000" android:required="true" />
  <application android:label="N.O.V.A. 3 (recompiled)" android:hasCode="false" android:extractNativeLibs="true"
      android:theme="@android:style/Theme.NoTitleBar.Fullscreen">
    <activity android:name="android.app.NativeActivity" android:exported="true" android:launchMode="singleTask"
        android:screenOrientation="sensorLandscape"
        android:configChanges="orientation|keyboardHidden|keyboard|screenSize|screenLayout|navigation">
      <meta-data android:name="android.app.lib_name" android:value="nova3" />
      <intent-filter>
        <action android:name="android.intent.action.MAIN" />
        <category android:name="android.intent.category.LAUNCHER" />
      </intent-filter>
    </activity>
  </application>
</manifest>
"""


def sdk_root() -> Path:
    for v in (os.environ.get("ANDROID_HOME"), os.environ.get("ANDROID_SDK_ROOT"), r"C:\Android\Sdk",
              os.path.join(os.environ.get("LOCALAPPDATA", ""), "Android", "Sdk"),
              str(Path.home() / "AppData" / "Local" / "Android" / "Sdk")):
        if v and Path(v, "build-tools").exists():
            return Path(v)
    sys.exit("Android SDK not found (set ANDROID_HOME)")


def newest(folder: Path) -> Path:
    return sorted(folder.iterdir(), key=lambda p: [int(x) if x.isdigit() else 0 for x in p.name.split(".")])[-1]


def java_env() -> dict:
    env = dict(os.environ)
    if not env.get("JAVA_HOME"):
        jdks = sorted(Path(r"C:\Program Files\Microsoft").glob("jdk-*")) if os.name == "nt" else []
        if jdks:
            env["JAVA_HOME"] = str(jdks[-1])
    # the JDK's AF_UNIX loopback socket fails under long temp paths on Windows
    Path("C:/jtmp").mkdir(exist_ok=True) if os.name == "nt" else None
    env["JAVA_TOOL_OPTIONS"] = "-Djdk.net.unixdomain.tmpdir=C:\\jtmp" if os.name == "nt" else ""
    return env


def run(cmd, **kw):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)
    if r.returncode:
        raise RuntimeError(f"{Path(str(cmd[0])).name} failed:\n{(r.stdout + r.stderr)[-4000:]}")
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--so", type=Path, required=True)
    ap.add_argument("--out", type=Path, default=Path("build/apk"))
    ap.add_argument("--checked", action="store_true")
    args = ap.parse_args()
    t0 = time.time()
    sdk = sdk_root()
    ndk = newest(sdk / "ndk")
    bt = newest(sdk / "build-tools")
    android_jar = newest(sdk / "platforms") / "android.jar"
    tc = ndk / "toolchains/llvm/prebuilt" / ("windows-x86_64" if os.name == "nt" else "linux-x86_64") / "bin"
    clang = tc / ("clang.exe" if os.name == "nt" else "clang")
    triple = f"aarch64-linux-android{API}"
    print(f"NDK {ndk.name}, build-tools {bt.name}, {android_jar.parent.name}")

    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    run([sys.executable, ROOT / "tools/recomp/pack_image.py", "--so", args.so, "--out", out])

    image = Image(args.so)
    ents = entries(image)
    lifter = Lifter(image, ents)
    sources = {}
    for a, f in ents.items():
        try:
            sources[a] = lifter.lift(f)
        except Unsupported:
            pass
    opt = ["-O1", "-fPIC", "-DRECOMP_CHECKED"] if args.checked else ["-O2", "-fPIC"]
    work = Path("build/recomp-android" + ("-checked" if args.checked else ""))
    objs = compile_objects(image, ents, sources, work, target=(triple, opt, "unused", [clang]), variant="runtime")
    print(f"translated code: {len(objs)} objects, {time.time() - t0:.0f}s")

    glue = ndk / "sources/android/native_app_glue"
    flags = ["-target", triple, "-O2", "-fPIC", "-Wall", "-Wno-unused-parameter", f"-I{ROOT / 'runtime'}",
             f"-I{ROOT / 'tools/recomp/runtime'}", f"-I{glue}"] + (["-DRECOMP_CHECKED"] if args.checked else [])
    rt_dir = out / "obj"
    rt_dir.mkdir(exist_ok=True)
    srcs = [ROOT / "runtime" / f"{n}.c" for n in RUNTIME_SRC] + [out / "image_meta.c", glue / "android_native_app_glue.c"]

    def cc(src):
        obj = rt_dir / (src.stem + ".o")
        run([clang, "-c", *flags, src, "-o", obj])
        return obj

    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        rt_objs = list(pool.map(cc, srcs))
    lib = out / "libnova3.so"
    run([clang, "-target", triple, "-shared", "-o", lib, *objs, *rt_objs, "-llog", "-landroid", "-laaudio", "-lEGL",
         "-lGLESv2", "-ldl", "-lm", "-Wl,-u,ANativeActivity_onCreate", "-Wl,--no-undefined", "-s"])
    print(f"{lib}: {lib.stat().st_size} bytes")

    # package: manifest + assets/image.bin + lib/arm64-v8a/libnova3.so, aligned and signed
    (out / "AndroidManifest.xml").write_text(MANIFEST, encoding="utf-8")
    assets = out / "assets"
    assets.mkdir(exist_ok=True)
    shutil.copyfile(out / "image.bin", assets / "image.bin")
    exe = ".exe" if os.name == "nt" else ""
    bat = ".bat" if os.name == "nt" else ""
    unsigned = out / "unsigned.apk"
    run([bt / f"aapt2{exe}", "link", "-o", unsigned, "-I", android_jar, "--manifest", out / "AndroidManifest.xml",
         "-A", assets])
    with zipfile.ZipFile(unsigned, "a", zipfile.ZIP_DEFLATED) as z:
        z.write(lib, "lib/arm64-v8a/libnova3.so")
    aligned = out / "aligned.apk"
    run([bt / f"zipalign{exe}", "-f", "-P", "16", "4", unsigned, aligned])
    env = java_env()
    keystore = out / "debug.keystore"
    if not keystore.exists():
        keytool = Path(env["JAVA_HOME"]) / "bin" / f"keytool{exe}"
        run([keytool, "-genkeypair", "-keystore", keystore, "-storepass", "android", "-keypass", "android", "-alias",
             "nova3", "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000", "-dname", "CN=nova3-recomp debug"],
            env=env)
    apk = out / "nova3-recomp.apk"
    run([bt / f"apksigner{bat}", "sign", "--ks", keystore, "--ks-pass", "pass:android", "--key-pass", "pass:android",
         "--ks-key-alias", "nova3", "--out", apk, aligned], env=env)
    run([bt / f"apksigner{bat}", "verify", apk], env=env)
    print(f"{apk}: {apk.stat().st_size} bytes, signed and verified, {time.time() - t0:.0f}s")


if __name__ == "__main__":
    main()
