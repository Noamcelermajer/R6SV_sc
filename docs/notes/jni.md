# JNI layer and boot sequence (runtime/jni.c, runtime/game.c)

Owner: the JNI stream. Files: `runtime/jni.c`, `runtime/game.c`, this note, `runtime/test/jni_test.c`; one marked block in
`runtime/rt.h` ("JNI / game layer"). Specification: `docs/JAVA-SIDE.md` (sections 4, 5 and 9). Sound goes to `runtime/media.h`
(implemented in `runtime/media.c` by the audio stream).

## Design

- `JavaVM` (8 slots) and `JNIEnv` (233 slots) are tables in game memory whose entries are host functions (`rt_hostfn_add`).
  One env serves every thread: `AttachCurrentThread`, `AttachCurrentThreadAsDaemon` and `GetEnv` (any version) store it and return 0.
- Handles are `0x00d00000 + index` (below the host-function range, never dereferenced by the game): classes, strings (UTF-8
  inside, UTF-16 views on request), objects, arrays (storage in game memory, so `GetPrimitiveArrayCritical` and `Get<T>ArrayElements`
  return the array's own guest pointer and the `Release` calls copy nothing), method IDs and field IDs. 262,144 handles; nothing
  is recycled (the live paths create none per frame; the log warns at 94 %).
  `NewGlobalRef`/`NewLocalRef` return their argument, `Delete*Ref` do nothing, `PushLocalFrame` returns 0, `PopLocalFrame` returns its argument.
- `GetStringUTFChars` returns a lazily created, never freed guest copy stored in the string object (the game keeps the pointer from
  `GetManufacturer`/`GetModel`); `ReleaseStringUTFChars` is a no-op. `GetStringChars`/`GetStringCritical` allocate and are freed by their `Release`.
- Lookups (`GetMethodID`, `GetStaticMethodID`, `GetFieldID`, `GetStaticFieldID`) resolve against a table in `jni.c`
  (`methods[]`, `fields[]`). An unknown `(class, name, signature)`, or a static looked up as an instance method and the reverse,
  returns NULL and is logged once ("JNI: no such method ..."): that log is what to look at when the game takes an unexpected path.
  `FindClass` returns a handle for any class (bare handle, logged once, when the class is not in `known_classes[]`) except
  `android/view/ViewRoot`, which fails with a pending exception (`ExceptionCheck` returns 1 until `ExceptionClear`).
- Calls: all of Call{Object,Boolean,Byte,Char,Short,Int,Long,Float,Double,Void}Method, CallNonvirtual..., CallStatic... and NewObject,
  each in the variadic, `V` (va_list = pointer to the argument words after `mid`, doubles 8-aligned, floats promoted) and `A`
  (jvalue[]) forms, decode their arguments from the method's signature and run the emulation handler. Return: the value in r0,
  64-bit values in r0:r1, a float as its bits in r0 (softfp).
- `NewObject*` allocates the object and runs the `<init>` handler (that is how `AudioTrack` gets its media track).
- Functions not implemented (`DefineClass`, reflection, `SetStatic*Field`, ...): logged once with their name, return 0. Set/Get field calls: only
  `Context.TELEPHONY_SERVICE` ("phone") exists. A name table (`jni_name`) gives readable traces: `-v -v -v` (`rt_verbose > 2`) logs every JNIEnv call.

## Emulated Java

Class `com/gameloft/android/ANMP/GloftR6HP/` is abbreviated `P/`.

| class.method | behaviour |
|---|---|
| `P/Game.db()[B` | `igli.bin` verbatim (262,144 bytes), see "igli.bin" below |
| `P/Game.dc()[B` | `"0"`, `P/Game.da()Landroid/app/Activity;` an Activity object handle, `P/Game.d()V` nothing |
| `P/Game.isMoveMC()I`, `getMoveCamera_X/Y/Flexible_X/Flexible_Y()I` | -1 |
| `P/Game.getGPUInfo()I` | 2 (ATC) |
| `P/Game.GetPhoneLanguage()I` | from the locale (below) |
| `P/Game.GetManufacturer()`, `GetModel()` | `"google"`, `"pixel"` unless `rt_jni_set_device` says otherwise (one handle each, interned UTF) |
| `P/Game.IsWifiEnabled()I` | 1 |
| `P/Game.SetDeviceMute(I)V` | `rt_media_set_mute(arg == 1)` |
| `P/Game.openBrowser(String)V` | `rt_host_open_url(url)` (weak default: log) |
| `P/Game.sendAppToBackground()V` | `rt_host_background()` (weak default: log) |
| `P/Game.Exit()V` | sets `rt_game_exit_requested()`, calls `rt_host_exit()` (weak default: `exit(0)`, as the Java `System.exit(0)`) |
| `P/Game.launchGetGames/launchGLLive/launchIGP`, `notifyTrophy(I)`, `SetReorientation(I)`, `ShowSpinner(String)`, `DismissSpinner()` | no-ops (`notifyTrophy` only logs with `-v`; `androidTrophy.dat` is not written, the file is write-only for the native side) |
| `P/GLResLoader` (5 methods), `P/GameRenderer` (`getKeyboardText`, `setKeyboard`, `isKeyboardRequired`, `swapEGLBuffers`) | lookup only, return 0/NULL (never called) |
| `P/GLMediaPlayer` (34 methods) | lookup only; `setMasterVolume(F)`/`getMasterVolume()F` keep a value; `pauseAll`/`resumeAll`/`stopAll` log if ever called; the rest return 0. The Java sound engine is dead: Vox plays through AudioTrack |
| `P/GLUtils/Device` (8 methods) | `a()` `{0}`, `b()` empty, `c()` "00", `d()` "1.1.6", `IsWifiEnable()` true, `getHostName()` "pixel_pixel", `e(I)` nothing, `getUserAgent()` "GL_EMU_001" (never called) |
| `android/media/AudioTrack` | `<init>(IIIIII)V` -> `rt_media_track_create(stream, rate, channels, encoding, bytes, mode)` (the id is kept in the object); static `getMinBufferSize(III)I` -> `rt_media_min_buffer`; `play/pause/stop/release()V` -> `rt_media_track_*`; `write([BII)I` -> `rt_media_track_write(id, G(array + offset), size)` and returns what it returns (bad array/range: -1, logged) |
| `android/content/Context`, `android/app/Activity`: `getSystemService(String)Object`; `android/telephony/TelephonyManager.getDeviceId()` | ALicenseCheck's dead path: a TelephonyManager object for "phone", device id "000000000000000" |

Media defaults: `jni.c` carries weak definitions of every `media.h` function (discard audio, log once), so the link works without `media.c`;
the strong ones in `media.c` win.

### igli.bin

`Game.db()` is called once at boot (`ALicenseCheck::LoadConfig` reads it into a 0x40000 byte buffer). Order of sources: (1) `rt_jni_set_igli(data, size)`
(a front end that embeds it, for instance as an APK asset), (2) `$R6SV_IGLI`, (3) `<data>/igli.bin`, (4) `<home>/igli.bin`, (5) `<temp>/igli.bin`,
(6) `original/apk/res/raw/igli.bin` relative to the cwd (development). If none exists, 262,144 zero bytes (logged). The file is not part of the game data
folder (`original/data/...`), it comes from `original/apk/res/raw/igli.bin`: **the APK/runner build has to ship it** (copy it next to the data, or embed it and call
`rt_jni_set_igli`). With zero bytes the game still boots in principle (`ConfigFile::Decode` only logs SERVER/EKEY/EPID); not verified because the library does not run yet.

### Language

Two numberings (JAVA-SIDE.md 4.1). `rt_jni_set_locale("de")` (also "fr_FR", "pt-BR", "zh-TW", ...) sets both: `Game.GetPhoneLanguage` (en 0, fr 1, de 2, it 3, es 4, ja 5, pt 8, ru 9,
zh-CN 10, zh-TW 11) and the index game.c passes to `nativeStart` (`rt_jni_game_language`: en 0, de 1, fr 2, it 3, es 4, ja 5, pt 6; ru and zh give 0). Default: `$R6SV_LANG`, else "en".
The front ends do not call it yet.

## Boot sequence (game.c, `rt_game_boot`)

1. `rt_data_imports_init`, `rt_imports_init`, `rt_jni_init`, class and object handles, `/proc/cpuinfo` alias (VFP, no NEON, as NOVA3: vox then uses its C++ mixers).
2. Constructors (`rt_init_array`), run here: no other file runs them.
3. `JNI_OnLoad(vm, 0)` (warns if not 0x10004).
4. UI-thread part: `GLUtils.Device.nativeInit`, `GLResLoader.nativeInit`.
5. `surface_created`: `GameRenderer.nativeInit` (instance, sets `g_env`), `GLResLoader.nativeInit`, `GLMediaPlayer.nativeGetTotalSounds` (635),
   `...OfSameInstance` (2), `GLMediaPlayer.nativeInit`, `Game.nativeInit` (21 + 4 lookups, `db()`), `GameRenderer.nativeStart(1, w, h, lang)` (w >= h; runs `appInit`).
6. `GameRenderer.nativeResize` (no-op in the game), `GameGLSurfaceView.nativeResume`.
The Gameloft logo video that Java plays first is skipped. `w`, `h` are what the front end passes (854 x height by `frontend.h`); the game gets exactly that to
`appInit` and no extra touch scaling is done here (the front ends scale touches back to the render buffer, StormGLOFT's 854/W, 480/H factors are theirs).

Per frame: `rt_game_step` = queued input, then `GameRenderer.nativeRender` (the front end swaps buffers). While paused: no frame, queued input dropped (Java clears to black).
Input entry points (any thread; delivered on the render thread inside the next `rt_game_step`, 512-event queue, a full queue drops moves first):
- `rt_game_touch(action 0 down / 1 up / 2 move, x, y, pointer)` -> `nativeIsRightTouchPad(-1)`, `nativeIsLeftTouchPad(-1)` (Java's state reset), then
  `Game.nativeTouchPressed/Released/Moved(x, y, id)`.
- `rt_game_key(androidKeyCode, down)` -> `Game.nativeSetOnKeyDown/Up`; 24, 25 and 27 are dropped as in `Game.onKeyDown`. (Java also turns BACK with scancode 305 into 225; the front ends have no scancode.)
- `rt_game_accel(x, y, z)` -> `Game.nativeAccelerometer` (stored by the game, no effect).
- `rt_game_pause()` / `rt_game_resume()` -> `nativePause` / `nativeResume`; ignored before `nativeStart` returned (nativePause would dereference a null application).
- `rt_game_restart_gl(w, h)`: the GL context was recreated: repeats step 5 (Java's `onSurfaceCreated`); the game reloads all textures. `rt_game_resize` only forwards `nativeResize`.

## Tests

`runtime/test/jni_test.c` fakes the translated library (guest functions implemented in C that call the env/vm tables through `rc_hostcall` exactly as the game's
wrappers do): `py -m ziglang cc -O1 -target x86_64-windows-gnu -Iruntime -Itools/recomp/runtime runtime/test/jni_test.c runtime/jni.c -o build/jni/jni_test.exe && build/jni/jni_test.exe`.
Result when written: 94 checks, 0 failures. It covers the whole boot order, the 25 Game lookups (and negative lookups), `db()` byte for byte, V/plain call forms, a float in a va_list,
interned UTF pointers surviving `DeleteLocalRef`, the AudioTrack protocol of vox from a second host thread (GetEnv/Attach, NewObjectV with 6 ints, nonvirtual play/write/stop/release,
`NewByteArray` + `GetPrimitiveArrayCritical`), the `ViewRoot` exception, the input queue (order, key filter, accel bits, pause drops input), `restart_gl`.
`jni.c` and `game.c` also compile warning-free with the NDK clang for aarch64 (`aarch64-linux-android30-clang -c -Wall -Wextra`).

## Assumed / not verified (the library does not run yet)

- Whether the game needs more JNIEnv behaviour than the spec lists: anything else shows up in the log as "JNI: ..." lines.
- `getGPUInfo() = 2`, manufacturer/model defaults, language mapping beyond what the dex shows.
- The AudioTrack `write` return and blocking behaviour belong to `media.c`; the pthread that vox starts must be able to call the env (it does: one env for all threads) and `rt_call` on its own CPU state.
- `GetStringUTFRegion` assumes ASCII (offsets are bytes); `GetStringRegion` is exact.
- Not implemented on purpose: the Java installer, the logo video, IME input, trophies file, GLLive/IGP, the touchpad natives (`nativeTouchPad*`, `nativeOnTouchGame`, slide/touchpad state), `processTouchpadAsPointer`, `nativeCanInterrupt`, `nativeDestroy`, `onKeyboardFinish` (no caller on a touchscreen; export them from the game if one ever needs them).
