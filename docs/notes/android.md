# Android arm64 build (stream: Android packaging)

Files: `tools/recomp/build_apk.py`, `build_arm64.py`, `runtime/android_main.c` (all unchanged except one line in
`build_apk.py`: `-lGLESv1_CM` added to the link). No runtime file needed an edit: every runtime file compiled for
Android without errors.

## Build command (repository root, 2026-11-22)

```
py -I -u tools/recomp/build_apk.py --so original/apk/lib/armeabi-v7a/librainbowsix.so --out build/android
```

Toolchain found by the script: NDK 30.0.16248370 clang (`aarch64-linux-android26`), build-tools 37.0.0, platform android-36,
JDK 17.0.20. Translated code: 23,171 of 23,171 entries, 48 shards + dispatcher = 49 objects (`build/recomp-android/`,
`-O2 -fPIC -fwrapv -fno-strict-aliasing -ffp-contract=off`, objects cached by content hash, 8 parallel jobs). Runtime: 15 files
+ `image_meta.c` + `android_native_app_glue.c`.

Times: cold compile of the 49 objects about 3-4 minutes wall (not timed exactly; the first run's log was overwritten);
re-run with everything cached: 64 s (26 s image load, 59 s cumulative to objects, rest link, aapt2, zipalign, apksigner).
The lifted C comes from `build/cache/lifted-*.pkl` (a cold lift adds about 55 s).
Do not start two copies at once on the same `--out`: the second fails with `OSError: [Errno 22]` on `protos.h` (Windows file lock).

Sizes: `build/android/libr6sv.so` 24,721,040 bytes (stripped, `-s`); `build/android/r6sv-recomp.apk` 14,688,726 bytes
(contains `assets/image.bin` 6,750,208 bytes).

## Result and what was verified (commands run on this machine)

APK: `C:/Users/nacworkstation/Documents/Projects/R6SV_sc/build/android/r6sv-recomp.apk`

* Link with `-Wl,--no-undefined` succeeded: every symbol is resolved by the runtime, the translated code or the NDK
  stubs. Libraries: `-llog -landroid -laaudio -lEGL -lGLESv1_CM -lGLESv2 -ldl -lm` (DT_NEEDED shows all of them, plus libc).
* `llvm-readelf -h`: ELF64, Machine AArch64; the zip header check agrees (class 2, machine 0xb7). LOAD segments aligned 0x4000.
* APK contents: `AndroidManifest.xml`, `resources.arsc`, `assets/image.bin`, `lib/arm64-v8a/libr6sv.so`, `META-INF/*`.
  No `armeabi*` folder, no `classes.dex`.
* `aapt2 dump badging`: package com.r6sv.recomp, versionCode 1, minSdk 26, targetSdk 35, launchable-activity
  android.app.NativeActivity, native-code `arm64-v8a` only, GLES 2.0, debuggable.
* `apksigner verify`: verifies (v2 and v3; debug key `build/android/debug.keystore`, alias r6sv, password `android`).
  `zipalign -c -P 16 4`: aligned (16 KB).
* Undefined dynamic symbols of the .so (260): `AMotionEvent_*`/`AKeyEvent_*`/other NDK, libc/libm names (`@LIBC`),
  nothing else. No `gl*` is imported directly: the runtime resolves OpenGL through `dlsym`/`eglGetProcAddress` at run time
  (`getproc` in `android_main.c`). One weak undefined `memfd_create` (API 30+; weak, so it is null on older systems; source
  not found in `runtime/`, probably the NDK glue or libc headers).
* The runtime compiles with `-Wall -Wextra`; only warnings: unused functions in `gles1.c` (`is_atc`, `atc_decode`,
  `is_pvrtc`, `pvr_decode`, `gl_list_has`), which are the GL stream's.

## Not verified

* **The APK was never run: no phone is attached.** Nothing about EGL on a real driver, the game booting, touch, audio
  (AAudio), the data folder paths or the crash handler has been tested on arm64 hardware or an emulator.
* The `--checked` APK build (bounds checks, shadow call stack) was not built in this round.
* `build_arm64.py` (translated C only, musl target) was not run; `build_apk.py` supersedes it for the phone.
* Whether the game logic runs correctly is the host-runner streams' work; this build contains whatever `runtime/*.c` held at
  build time (other streams were editing it, so rebuild before testing).

## On the phone

```
adb install -r build/android/r6sv-recomp.apk
adb shell mkdir -p /sdcard/Android/data/com.r6sv.recomp/files
adb push original/data/gameloft/games/GloftR6HP/. /sdcard/Android/data/com.r6sv.recomp/files/
adb shell am start -n com.r6sv.recomp/android.app.NativeActivity
adb logcat -s nova3          # runtime log tag (runtime/core.c); a crash prints the game call chain
```

The data folder is 765 MB (4,165 files); `docs/DATA.md` says which files can be left out. If `adb push` into
`Android/data` is refused on Android 11+ with scoped storage, run the app once first (the folder is created), or push to
`/sdcard/Download` and move it with `adb shell` as root/`run-as`. `--install` on `build_apk.py` runs `adb install -r` itself.
Optional: `render_width.txt` in the data folder overrides the 854-pixel render width.

## Round 2 (2026-11-22): rebuilt from the current sources, android_main.c brought in line with host_main.c

Rebuilt with the same command (35 s, cached): `build/android/r6sv-recomp.apk` 14,688,726 bytes, `libr6sv.so` 24,721,632 bytes.
Changes in `runtime/android_main.c` only (nothing in the other runtime files needed a change for the NDK build):
- Default render buffer is exactly 854x480 like the runner (`render_width.txt` switches to width + screen aspect).
- The main loop runs on a 64 MB mmap'd stack (inline asm stack switch, aarch64 only): the runner links with 32 MB and the
  translated code recurses, while native_app_glue's thread has 1 MB. The looper stays with the same thread.
- All keys except volume/camera go to `rt_game_key` (the original Java did the same); `rt_game_exit_requested()` finishes the activity.
- Startup is the runner's: `rt_mem_init_from(assets image)`, GL via dlsym/eglGetProcAddress, `rt_frontend_paths`,
  `rt_game_boot(data, data, home, temp, w, h)`, then per frame input, `rt_game_step`, swap.
Verified: ELF64 AArch64; NEEDED liblog/libandroid/libaaudio/libEGL/libGLESv1_CM/libGLESv2/libdl/libm/libc; undefined symbols all
libc/NDK (plus weak `memfd_create`); aapt2 badging; apksigner v2+v3; zipalign 16K; the stack-switch sequence in the disassembly.
Not verified: any execution on arm64. See `docs/INSTALL.md`.
