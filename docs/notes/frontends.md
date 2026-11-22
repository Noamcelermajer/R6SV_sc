# Front ends: host runner, Android activity, build scripts

Owner: the front-end stream. Files: `runtime/host_main.c`, `runtime/android_main.c`, `runtime/frontend.h`,
`tools/recomp/build_runner.py`, `build_apk.py`, `build_arm64.py`, `build_common.py`, `pack_image.py`, `docs/DATA.md`.
One marked edit in `runtime/core.c`: `rt_call` clears bit 0 of the function address (Thumb pointers).

## Commands (from the repository root; none of these links yet, see "State")

```
# runtime-only check (runs now, no lifter): packs the image, compiles all runtime files, links a stub dispatcher
py -I tools/recomp/build_runner.py --so original/apk/lib/armeabi-v7a/librainbowsix.so --out build/runner-dry --no-translate
py -I tools/recomp/build_apk.py    --so original/apk/lib/armeabi-v7a/librainbowsix.so --out build/apk-dry --no-translate

# real builds, once the lifter translates the library
py -I tools/recomp/build_runner.py --so original/apk/lib/armeabi-v7a/librainbowsix.so --out build/runner [--fast]
py -I tools/recomp/build_apk.py    --so original/apk/lib/armeabi-v7a/librainbowsix.so --out build/apk [--checked] [--install]
py -I tools/recomp/build_arm64.py  --so original/apk/lib/armeabi-v7a/librainbowsix.so          # translated C only, for arm64

# run on Windows (ANGLE DLLs are in build/runner/angle; the exe finds <exe folder>\angle by itself)
build\runner\r6sv_runner.exe --image build\runner\image.bin --data original\data\gameloft\games\GloftR6HP ^
    --offscreen --frames 600 --shot 300:build\runner\f300.png --tap 200:427:240 --wav build\runner\a.wav -v

# phone (arm64): install, push the data, start
adb install -r build/apk/r6sv-recomp.apk
adb push original/data/gameloft/games/GloftR6HP/. /sdcard/Android/data/com.r6sv.recomp/files/
adb shell am start -n com.r6sv.recomp/android.app.NativeActivity
adb logcat -s nova3          # rt_log uses the tag "nova3" (runtime/core.c); crashes print the guest call chain
```

`py -I` is deliberate (PLAN.md: the scripts read files from `original/`). `build_apk.py` finds the SDK, NDK 30, platform
android-36, build-tools 37 and JDK 17 on this machine by itself (ANDROID_HOME, ANDROID_SDK_ROOT, `%LOCALAPPDATA%\Android\Sdk`,
`C:\Android\Sdk`; JAVA_HOME or `Program Files\Microsoft\jdk-17*`). The lifted C is cached in `build/cache/` (key: library,
`lift.py`, `image.py` contents), so the three scripts lift once.

## State (verified by running the commands above on 2026-11-21)

- Done and run: `pack_image.py` on `librainbowsix.so` (412 constructors, 348 imports, 7 data import slots, 77 entry exports,
  ~52,900 function names); `build_runner.py --no-translate` compiles all 16 runtime files with `-Wall -Wextra`, links
  `r6sv_runner.exe`, which starts, loads ANGLE, creates an ES 2 pbuffer, loads the image and stops at the first constructor
  because the dispatcher is a stub (fault 4) and the crash handler runs; `build_apk.py --no-translate` produces a signed,
  verified, 16 KB-aligned `r6sv-recomp-norun.apk` (package com.r6sv.recomp, minSdk 26, targetSdk 35, arm64-v8a only,
  hasCode=false, `aapt2 dump badging` checked). PNG output of the runner was checked by decoding it (CRC, zlib).
- Not done: anything that needs translated code: no real link, no run of the game, no touch/frame/screenshot run, no device.
  Not tested on a phone: the accelerometer, the immersive-mode JNI call, EGL on a real driver.

## Decisions

- **Package and names**: `com.r6sv.recomp`, library `libr6sv.so` (`android.app.lib_name` = `r6sv`), asset `image.bin`, label
  "Rainbow Six: Shadow Vanguard (recompiled)", APK `r6sv-recomp.apk`, runner `r6sv_runner.exe`, keystore alias `r6sv`.
- **Data**: the data folder is the content of `GloftR6HP` (not its parent). Android: `/sdcard/Android/data/com.r6sv.recomp/files`
  (`externalDataPath`, no permission). The front ends call `rt_frontend_paths(data, home)` (frontend.h) before `rt_game_boot`:
  `rt_libc_set_data_dir(data)`, `rt_libc_add_data_prefix("/sdcard/gameloft/games/GloftR6HP")`, the same without the leading
  slash (vox opens `sdcard/gameloft/games/GloftR6HP/data/audio.zip` relative), and `rt_path_alias("/data/data/com.gameloft.android.ANMP.GloftR6HP", home)`.
  Details and the evidence are in `docs/DATA.md` section 4.
- **Screen size**: the game picks its menu layout from the viewport width (`MenuManager::LoadSWF`: more than 853 selects the
  854 layouts; `CTextureManager::loadAllTexture`: exactly 854 selects `loading854`). Rendering is a buffer 854 pixels wide with the
  screen's aspect ratio (`RT_RENDER_WIDTH`, override with `render_width.txt` in the data folder on Android, `--width/--height`
  on Windows); touches are scaled back. Not seen running: whether an aspect other than 16:9 distorts the 3D view (the viewport
  comes from `appInit(width, height)`, the game may assume 854x480).
- **GL context version**: `RT_EGL_CONTEXT_VERSION` in `runtime/frontend.h` (2). Change it there or with `-DRT_EGL_CONTEXT_VERSION=3`
  (the EGL config's renderable bit follows). The library imports ES 1.1 and ES 2 entry points; ANGLE has no ES 1, so the GL stream
  has to provide ES 1.1 on top of ES 2 (`gles1.c`).
- **Orientation**: `sensorLandscape`, `configChanges` for everything (no activity restart), `resizeableActivity=false`,
  theme with `windowLayoutInDisplayCutoutMode=shortEdges`, `AWINDOW_FLAG_FULLSCREEN`, and an immersive-mode JNI call on focus
  (`setSystemUiVisibility`, failures are logged and ignored).
- **Manifest**: INTERNET, ACCESS_NETWORK_STATE, ACCESS_WIFI_STATE (the game asks `IsWifiEnable`), VIBRATE; GLES 2.0 required;
  accelerometer and wifi features optional; `android:debuggable=true` (a debug APK); no storage permission.
- **Thumb**: every address in `image_meta.c` (constructors, exports, function table) and the argument of `rt_call` has bit 0 cleared;
  `Image.init_array` carries the mode separately. The dispatcher generated by `difftest.compile_objects` switches on exact
  addresses without the Thumb bit, so a computed target (`BLX reg`, a function pointer from game memory) must be masked by the
  lifter before `rc_dispatch`; if it is not, `rc_dispatch` itself should do `t &= ~1u` (hand-off to the lifter stream).

## Interfaces the front ends use (rt.h and frontend.h)

`rt_mem_init/rt_mem_init_from`, `rt_gl_getproc`, `rt_game_boot(obb, data, home, temp, w, h)` (obb is passed as the data folder: the
game has no OBB), `rt_game_resize`, `rt_game_touch(action 0 down / 1 up / 2 move, x, y, pointer id)` (Android codes; the game
layer converts), `rt_game_step`, `rt_game_pause/resume`, `rt_game_cpu`, `rt_print_trace`, `rt_log`, `rt_fatal`.
New, declared in `frontend.h`, with weak do-nothing definitions in both front ends until `game.c` defines them:
`rt_game_accel(x, y, z)` (raw m/s^2 of the Android sensor; maps to `Game.nativeAccelerometer(FFF)`) and `rt_game_key(keycode, down)`
(Android BACK, keycode 4; the Java side passes the scan code to `nativeSetOnKeyDown/Up`). The game layer in the library itself
is `appInit(w, h, x)`, `appRender`, `appOnTouch`, `notifyTouchPress/Moved/Released`, `notifyKeyPressed/Released`, `appPause`,
`appReorientation`, `appRecoverOGLContext`, `appDeinit` (all exported, all in `rt_exports`).

## Open problems and hand-offs

1. `game.c` still drives the N.O.V.A. 3 sequence (`GL2JNILib_*`, `lockPointer*`, `GloftN3HM`). Rainbow Six's Java side is
   `GameRenderer.nativeInit/nativeStart(IIII)/nativeResize/nativeRender`, `Game.nativeInit`, `Game.nativeTouchPressed(III)` etc.;
   until it is rewritten the runner starts and stops at the constructors.
2. `libc.c` data imports: the generated table has seven slots, `__sF`, `_ctype_`, `_tolower_tab_`, `__stack_chk_guard` (handled) and
   **`_toupper_tab_`, `__dso_handle`, `_ZSt7nothrow`** (not handled: `rt_data_imports_init` calls `rt_fatal("no data for import ...")`).
   `_toupper_tab_` is a pointer to 257 shorts like `_tolower_tab_`; `__dso_handle` and `std::nothrow` need any 4 readable bytes.
3. Textures are Adreno ATC (docs/DATA.md section 2); `gles1.c` has the decoders (`is_atc`, `atc_decode`) but they are not yet called
   (compiler warning "unused function").
4. The runner's `--wav` sets the environment variable `NOVA_AUDIO_WAV`, which `audio_win.c` reads; the name is left from N.O.V.A. 3.
5. Logging of unimplemented JNI calls is the JNI stream's; unimplemented imports are logged once by `rc_import` (core.c).
6. Deep recursion: the Android main thread is created by `native_app_glue` with bionic's default stack (1 MB); checked builds allow
   2,000 nested translated calls. The Windows runner links with a 32 MB stack. If a device run overflows, move the game loop to a
   thread with a big stack (the looper must then be that thread's).
7. Case: Android file names are case-sensitive; the libc stream's `rt_map_path` is expected to fall back to a case-insensitive match.
