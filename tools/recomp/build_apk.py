#!/usr/bin/env python3
"""Build the 64-bit Android APK: the translated library and runtime/ for arm64-v8a, in a NativeActivity app.

  py -3 tools/recomp/build_apk.py --so original/apk/lib/armeabi-v7a/librainbowsix.so [--checked] [--out build/apk]
  py -3 tools/recomp/build_apk.py --so ... --out build/apk-dry --no-translate     # packaging check, the APK cannot run
  py -3 tools/recomp/build_apk.py --so ... --install                              # adb install -r afterwards

Finds the Android SDK (ANDROID_HOME, ANDROID_SDK_ROOT, %LOCALAPPDATA%\\Android\\Sdk, C:\\Android\\Sdk: build-tools, the
newest platform, the newest NDK) and a JDK (JAVA_HOME, Program Files\\Microsoft|Eclipse Adoptium|Java, preferring 17)
for keytool/apksigner. Produces <out>/r6sv-recomp.apk, a debug-signed APK with no Java code (hasCode=false,
NativeActivity), arm64-v8a only, minSdk 26, targetSdk 35. --checked builds the translated code with bounds checks and
the shadow call stack (slower, better crash reports).

The APK does not contain the game data. Copy the content of GloftR6HP (the folder with data/, SaveGame.bin ...) to the
device: adb push <GloftR6HP folder>/. /sdcard/Android/data/com.r6sv.recomp/files/
(docs/DATA.md says which files can be left out; the app logs where it looks).
"""
from __future__ import annotations

import argparse
import concurrent.futures
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from build_common import ROOT, RUNTIME, runtime_sources, translate  # noqa: E402

PACKAGE = "com.r6sv.recomp"
LIB_NAME = "r6sv"                       # libr6sv.so, android.app.lib_name
APP_LABEL = "Rainbow Six: Shadow Vanguard (recompiled)"
APK_NAME = "r6sv-recomp.apk"
KEY_ALIAS = "r6sv"
MIN_SDK = 26
TARGET_SDK = 35
VERSION_CODE, VERSION_NAME = 1, "0.1"
ANDROID_API = MIN_SDK                    # the API level of the NDK sysroot the code is compiled against

MANIFEST = f"""<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="{PACKAGE}"
    android:versionCode="{VERSION_CODE}" android:versionName="{VERSION_NAME}">
  <uses-sdk android:minSdkVersion="{MIN_SDK}" android:targetSdkVersion="{TARGET_SDK}" />
  <uses-feature android:glEsVersion="0x00020000" android:required="true" />
  <uses-feature android:name="android.hardware.touchscreen" android:required="true" />
  <uses-feature android:name="android.hardware.sensor.accelerometer" android:required="false" />
  <uses-feature android:name="android.hardware.wifi" android:required="false" />
  <uses-permission android:name="android.permission.INTERNET" />
  <uses-permission android:name="android.permission.ACCESS_NETWORK_STATE" />
  <uses-permission android:name="android.permission.ACCESS_WIFI_STATE" />
  <uses-permission android:name="android.permission.VIBRATE" />
  <application android:label="{APP_LABEL}"@ICON@ android:hasCode="false" android:extractNativeLibs="true"
      android:debuggable="true" android:allowBackup="false" android:isGame="true" android:appCategory="game"
      android:theme="@style/GameTheme">
    <activity android:name="android.app.NativeActivity" android:exported="true" android:launchMode="singleTask"
        android:screenOrientation="sensorLandscape" android:resizeableActivity="false"
        android:configChanges="orientation|keyboardHidden|keyboard|screenSize|smallestScreenSize|screenLayout|navigation|uiMode|density|fontScale|layoutDirection|locale|colorMode">
      <meta-data android:name="android.app.lib_name" android:value="{LIB_NAME}" />
      <intent-filter>
        <action android:name="android.intent.action.MAIN" />
        <category android:name="android.intent.category.LAUNCHER" />
      </intent-filter>
    </activity>
  </application>
</manifest>
"""

# full screen on a phone with a notch or a camera hole too (the attribute is ignored below API 28)
STYLES = """<?xml version="1.0" encoding="utf-8"?>
<resources>
  <style name="GameTheme" parent="@android:style/Theme.NoTitleBar.Fullscreen">
    <item name="android:windowBackground">@android:color/black</item>
    <item name="android:windowLayoutInDisplayCutoutMode">shortEdges</item>
  </style>
</resources>
"""

STUB = """#include "recomp.h"
/* --no-translate: stands in for the translated library's dispatcher */
void rc_dispatch(cpu_t *c, uint8_t *m, uint32_t t) { (void)m; c->fault = RC_FAULT_DISPATCH; c->r[15] = t; }
"""


def version_key(name: str):
    return [int(x) for x in re.findall(r"\d+", name)] or [0]


def newest(folder: Path, pattern: str = "*") -> Path:
    items = [p for p in folder.glob(pattern) if p.is_dir()]
    if not items:
        sys.exit(f"nothing in {folder}")
    return max(items, key=lambda p: version_key(p.name))


def sdk_root() -> Path:
    cands = [os.environ.get("ANDROID_HOME"), os.environ.get("ANDROID_SDK_ROOT"),
             os.path.join(os.environ.get("LOCALAPPDATA", ""), "Android", "Sdk"), r"C:\Android\Sdk",
             str(Path.home() / "AppData" / "Local" / "Android" / "Sdk"), str(Path.home() / "Android" / "Sdk")]
    for v in cands:
        if v and Path(v, "build-tools").exists() and Path(v, "ndk").exists():
            return Path(v)
    sys.exit("Android SDK with build-tools and ndk not found (set ANDROID_HOME)")


def find_jdk() -> Path:
    if os.environ.get("JAVA_HOME") and Path(os.environ["JAVA_HOME"], "bin").exists():
        return Path(os.environ["JAVA_HOME"])
    pf = [os.environ.get("ProgramFiles", r"C:\Program Files")]
    found = []
    for base in pf:
        for vendor in ("Microsoft", "Eclipse Adoptium", "Java", "Zulu"):
            found += [p for p in Path(base, vendor).glob("jdk*") if (p / "bin").exists()]
    if not found:
        sys.exit("no JDK found (set JAVA_HOME to a JDK 17)")
    found.sort(key=lambda p: (version_key(p.name)[0] != 17, version_key(p.name)))
    return found[0]


def run(cmd, **kw):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)
    if r.returncode:
        raise RuntimeError(f"{Path(str(cmd[0])).name} failed:\n{(r.stdout + r.stderr)[-4000:]}")
    return r


def pack_data(folder, igli, pak):
    """assets/gamedata.pak: "R6PK1", then per file u16 name length, name, u64 size, content; a zero length ends it.
    data/3d.zip is left out (the runtime never reads it); igli.bin (the APK's res/raw) is added."""
    files = sorted(p for p in folder.rglob("*") if p.is_file() and p.relative_to(folder).as_posix() != "data/3d.zip")
    entries = [(p.relative_to(folder).as_posix(), p) for p in files]
    if igli.is_file():
        entries.append(("igli.bin", igli))
    total = sum(p.stat().st_size for _, p in entries)
    with open(pak, "wb") as f:
        f.write(b"R6PK1\0\0\0")
        for name, p in entries:
            n = name.encode("utf-8")
            f.write(struct.pack("<H", len(n)) + n + struct.pack("<Q", p.stat().st_size))
            with open(p, "rb") as src:
                shutil.copyfileobj(src, f, 1 << 20)
        f.write(struct.pack("<H", 0))
    print(f"{pak}: {len(entries)} files, {total} bytes of game data")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--so", type=Path, required=True, help="the original library (armeabi-v7a)")
    ap.add_argument("--out", type=Path, default=Path("build/apk"))
    ap.add_argument("--checked", action="store_true", help="bounds checks and shadow call stack in the translated code")
    ap.add_argument("--no-translate", action="store_true", help="skip the lifter: packaging check, the APK cannot run")
    ap.add_argument("--no-cache", action="store_true", help="lift again even if the lifted C is cached")
    ap.add_argument("--data", type=Path, help="the game's data folder (GloftR6HP): packed into the APK as "
                    "assets/gamedata.pak, which the app unpacks on its first start")
    ap.add_argument("--install", action="store_true", help="adb install -r the APK afterwards")
    args = ap.parse_args()
    t0 = time.time()
    win = os.name == "nt"
    exe = ".exe" if win else ""
    bat = ".bat" if win else ""
    sdk = sdk_root()
    ndk = newest(sdk / "ndk")
    bt = newest(sdk / "build-tools")
    plat = newest(sdk / "platforms", "android-*")
    android_jar = plat / "android.jar"
    jdk = find_jdk()
    tc = ndk / "toolchains/llvm/prebuilt" / ("windows-x86_64" if win else "linux-x86_64") / "bin"
    clang = tc / ("clang.exe" if win else "clang")
    triple = f"aarch64-linux-android{ANDROID_API}"
    print(f"SDK {sdk}\n  NDK {ndk.name}, build-tools {bt.name}, {plat.name}, JDK {jdk.name}")
    for p in (clang, android_jar, bt / f"aapt2{exe}", bt / f"zipalign{exe}", bt / f"apksigner{bat}", jdk / "bin" / f"keytool{exe}"):
        if not p.exists():
            sys.exit(f"missing {p}")

    out = args.out if args.out.is_absolute() else ROOT / args.out
    out.mkdir(parents=True, exist_ok=True)
    run([sys.executable, "-I", ROOT / "tools/recomp/pack_image.py", "--so", args.so, "--out", out])

    objs = []
    if not args.no_translate:
        from difftest import compile_objects
        image, ents, sources, _ = translate(args.so, use_cache=not args.no_cache)
        opt = ["-O1", "-fPIC", "-DRECOMP_CHECKED"] if args.checked else ["-O2", "-fPIC"]
        work = ROOT / ("build/recomp-android" + ("-checked" if args.checked else ""))
        objs = compile_objects(image, ents, sources, work, target=(triple, opt, "unused", [clang]), variant="runtime")
        print(f"translated code: {len(objs)} objects, {time.time() - t0:.0f}s")

    glue = ndk / "sources/android/native_app_glue"
    flags = ["-target", triple, "-O2", "-fPIC", "-Wall", "-Wextra", "-Wno-unused-parameter", f"-I{RUNTIME}",
             f"-I{ROOT / 'tools/recomp/runtime'}", f"-I{glue}"] + (["-DRECOMP_CHECKED"] if args.checked else [])
    rt_dir = out / "obj"
    rt_dir.mkdir(exist_ok=True)
    srcs = runtime_sources("android") + [out / "image_meta.c", glue / "android_native_app_glue.c"]
    if args.no_translate:
        (out / "stub_dispatch.c").write_text(STUB, encoding="utf-8")
        srcs.append(out / "stub_dispatch.c")
    print("runtime files:", " ".join(s.stem for s in srcs))

    def cc(src):
        obj = rt_dir / (src.stem + ".o")
        r = run([clang, "-c", *flags, src, "-o", obj])
        if r.stderr.strip():
            print(f"warnings in {src.name}:\n{r.stderr.strip()[-1500:]}")
        return obj

    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        rt_objs = list(pool.map(cc, srcs))
    lib = out / f"lib{LIB_NAME}.so"
    link = [clang, "-target", triple, "-shared", "-o", lib, *objs, *rt_objs, "-llog", "-landroid", "-laaudio", "-lEGL",
            "-lGLESv1_CM", "-lGLESv2", "-ldl", "-lm", "-Wl,-u,ANativeActivity_onCreate", "-Wl,-z,max-page-size=16384", "-s"]
    r = subprocess.run([str(c) for c in link + ["-Wl,--no-undefined"]], capture_output=True, text=True)
    runnable = r.returncode == 0 and not args.no_translate  # without translated code nothing can run
    if r.returncode:
        undefined = sorted({ln.split("undefined symbol:")[1].strip() for ln in r.stderr.splitlines() if "undefined symbol:" in ln})
        if not args.no_translate:
            raise RuntimeError(f"link failed:\n{r.stderr[-4000:]}")
        print(f"link without translated code: {len(undefined)} undefined symbols (the APK below cannot run):")
        for u in undefined:
            print("  ", u)
        run(link + ["-Wl,--unresolved-symbols=ignore-all"])
    print(f"{lib}: {lib.stat().st_size} bytes")

    # package: manifest + resources + assets/image.bin + lib/arm64-v8a/libr6sv.so, aligned and signed
    res = out / "res" / "values"
    res.mkdir(parents=True, exist_ok=True)
    # the launcher icon is the original APK's own (res/drawable/icon.png next to the lib/ folder the .so came from)
    icon = Path(args.so).resolve().parents[2] / "res" / "drawable" / "icon.png"
    drawable = out / "res" / "drawable"
    if icon.is_file():
        drawable.mkdir(exist_ok=True)
        shutil.copyfile(icon, drawable / "icon.png")
    elif drawable.exists():
        shutil.rmtree(drawable)
    (out / "AndroidManifest.xml").write_text(
        MANIFEST.replace("@ICON@", ' android:icon="@drawable/icon"' if icon.is_file() else ""), encoding="utf-8")
    (res / "styles.xml").write_text(STYLES, encoding="utf-8")
    assets = out / "assets"
    assets.mkdir(exist_ok=True)
    shutil.copyfile(out / "image.bin", assets / "image.bin")
    pak = assets / "gamedata.pak"
    if args.data:
        pack_data(args.data, Path(args.so).resolve().parents[2] / "res" / "raw" / "igli.bin", pak)
    elif pak.exists():
        pak.unlink()
    compiled = out / "res.zip"
    run([bt / f"aapt2{exe}", "compile", "--dir", out / "res", "-o", compiled])
    unsigned = out / "unsigned.apk"
    run([bt / f"aapt2{exe}", "link", "-o", unsigned, "-I", android_jar, "--manifest", out / "AndroidManifest.xml",
         "--min-sdk-version", MIN_SDK, "--target-sdk-version", TARGET_SDK, "--version-code", VERSION_CODE,
         "--version-name", VERSION_NAME, "-0", "pak", "-A", assets, str(compiled)])
    with zipfile.ZipFile(unsigned, "a", zipfile.ZIP_DEFLATED) as z:
        z.write(lib, f"lib/arm64-v8a/lib{LIB_NAME}.so")
    aligned = out / "aligned.apk"
    run([bt / f"zipalign{exe}", "-f", "-P", "16", "4", unsigned, aligned])
    env = dict(os.environ, JAVA_HOME=str(jdk))
    if win:  # the JDK's AF_UNIX loopback socket fails under long temp paths on Windows
        Path("C:/jtmp").mkdir(exist_ok=True)
        env["JAVA_TOOL_OPTIONS"] = "-Djdk.net.unixdomain.tmpdir=C:\\jtmp"
    keystore = out / "debug.keystore"
    if not keystore.exists():
        run([jdk / "bin" / f"keytool{exe}", "-genkeypair", "-keystore", keystore, "-storepass", "android", "-keypass", "android",
             "-alias", KEY_ALIAS, "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000", "-dname", "CN=r6sv-recomp debug"], env=env)
    apk = out / (APK_NAME if runnable else APK_NAME.replace(".apk", "-norun.apk"))
    if args.data:
        apk = apk.with_name(apk.stem + "-full.apk")
    run([bt / f"apksigner{bat}", "sign", "--ks", keystore, "--ks-pass", "pass:android", "--key-pass", "pass:android",
         "--ks-key-alias", KEY_ALIAS, "--out", apk, aligned], env=env)
    run([bt / f"apksigner{bat}", "verify", apk], env=env)
    print(f"{apk}: {apk.stat().st_size} bytes, signed and verified, {time.time() - t0:.0f}s")
    if args.install and runnable:
        run([sdk / "platform-tools" / f"adb{exe}", "install", "-r", apk])
        print("installed")


if __name__ == "__main__":
    main()
