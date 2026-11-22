# Java side and JNI surface of Rainbow Six: Shadow Vanguard (Android, 32-bit)

Everything the original Java layer does for `librainbowsix.so`, what the native library asks of it, and the
specification of the JNI layer that `runtime/jni.c` has to emulate. Source material: `original/apk/classes.dex`
(125 classes, 87 of them with one or two letter obfuscated names), `AndroidManifest.xml`, `res/raw`, `assets/data.save`,
`lib/armeabi-v7a/librainbowsix.so` (the game) and `libStormGLOFT.so` (repacker hook library).

Legend: **[V]** verified in the bytecode or in the ARM disassembly (address given), **[A]** assumed or inferred.
All addresses are virtual addresses in `librainbowsix.so`; symbol values in `.dynsym` carry the Thumb bit (+1),
the addresses below do not. Functions above 0x53f2e0 that sit in ARM mode are marked (ARM).

Reproduce with: `py -I tools/recomp/dex.py original/apk/classes.dex --natives --so original/apk/lib/armeabi-v7a/librainbowsix.so`
(also `--class`, `--method`, `--xref`), the SDK's `dexdump -d` for full bytecode and `aapt dump xmltree` for the manifest.

## 0. Where to start

Section 11 is the 30-line summary for the implementers of the JNI layer, audio and boot sequence; section 9 is the JNI specification; the sections in between are the evidence.

## 1. Package and manifest [V]

- Package `com.gameloft.android.ANMP.GloftR6HP`, versionCode 116 (0x74), versionName `1.1.6`, minSdk 9, installLocation auto.
- Launcher activity `.Game` (NoTitleBar.Fullscreen theme `0x01030007`), `screenOrientation=0` (landscape),
  `launchMode=3` (singleInstance), `configChanges=0x400001ff` (it handles every configuration change itself, so
  rotation/keyboard changes never recreate the activity), `clearTaskOnLaunch`, `finishOnTaskLaunch`.
- Other activities: `.installer.GameInstaller` (landscape; checks / downloads the data files), `.MyVideoView`
  (landscape; plays the Gameloft logo), `.GLiveMain` (portrait; WebView portal "Gameloft Live"), `.IGPActivity`
  (landscape; in-game ads).
- Features: `touchscreen.multitouch`, `touchscreen`. Permissions: RAISED_THREAD_PRIORITY, WRITE_EXTERNAL_STORAGE,
  INTERNET, ACCESS_WIFI_STATE, READ_PHONE_STATE, VIBRATE, MODIFY_AUDIO_SETTINGS, ACCESS_NETWORK_STATE,
  RESTART_PACKAGES, CHANGE_WIFI_STATE, WAKE_LOCK, CHANGE_NETWORK_STATE.
- Meta-data `xperiaplay_content`, `game_display_name`, `game_icon`: the game is Xperia Play flavoured (see 3.5,
  `Game.isXperiaPlayPhone()` returns `true` on every device) and was tuned for 854x480 (section 8.4).
- `Game.<clinit>` loads the libraries in this order: `System.loadLibrary("rainbowsix")` then
  `System.loadLibrary("StormGLOFT")` [V].
- Java needs `libStormGLOFT.so` only as a game patch; the native code never calls into it by name (section 8).

## 2. Native methods declared by the dex [V]

36 `native` methods, 33 of them implemented by exported `Java_*` symbols (the 3 others are never called), plus `JNI_OnLoad`. `docs/PLAN.md` mentions 68 Java exports; the dynamic symbol table has 33. No `RegisterNatives` is used.
"Thread": U = Android UI thread, G = GLSurfaceView's GL thread (the thread that owns the EGL context and renders).

| Java method (class `com.gameloft.android.ANMP.GloftR6HP.`) | kind | exported symbol (suffix) @address | Java call site, thread | what the native code does |
|---|---|---|---|---|
| `GameRenderer.nativeInit()V` | instance | `GameRenderer_nativeInit` @27241c | `onSurfaceCreated`, G | stores `env` in the global `g_env` (0x628958). **Every later native-to-Java call uses this global**, so it must be valid on the thread that calls them |
| `GameRenderer.nativeStart(IIII)V` | static | `GameRenderer_nativeStart` @272430 | `onSurfaceCreated`, G | args `(1, width, height, languageIndex)`. First call: `appInit(width, height, lang)` (creates the glitch device and the whole game), then caches the `GameRenderer` class (global ref) and 4 method IDs. Later calls (GL context recreated): only records `restart=1` at 0x665ce8 so the next frame reloads all textures |
| `GameRenderer.nativeResize(II)V` | static | `GameRenderer_nativeResize` @272530 | `onSurfaceChanged`, G | `bx lr` (no-op) |
| `GameRenderer.nativeRender()V` | static | `GameRenderer_nativeRender` @272534 | `onDrawFrame`, G (every frame) | `appRender(0,0,0)` @274c18: one `glitch::IDevice::run` + `Application::Update` tick |
| `GameRenderer.nativeDestroy()V` | instance | `GameRenderer_nativeDestroy` @272544 | **no caller** | `appDeinit` (no-op) and `DeleteGlobalRef` of the cached refs |
| `GameRenderer.onKeyboardFinish()V` | static | `GameRenderer_onKeyboardFinish` @272704 | `Game.onKeyDown` (U) when the soft keyboard is closed | `AndroidOS_onKeyboardFinish` = `bx lr` |
| `GameGLSurfaceView.nativePause()V` | instance | `GameGLSurfaceView_nativePause` @2723c0 | `onWindowFocusChanged(false)` (U) and `GameRenderer.onDrawFrame` (G) | sets `paused=1` (0x665ce0), then `appPause(1)` -> `Application::Pause(app)`. **Dereferences the application object: do not call before `nativeStart` returned** |
| `GameGLSurfaceView.nativeResume()V` | instance | `GameGLSurfaceView_nativeResume` @2723dc | `onWindowFocusChanged(true)` (U) | `paused=0`, flag 0x628848=1, `appPause(0)` (sets the resume counter 0x62a564=1): only flags, safe before init; the following `appRender` frames reload the textures and call `Application::Resume` (3.3) |
| `Game.nativeInit()V` | static | `Game_nativeInit` @272c14 | `onSurfaceCreated`, G | caches the `Game` class and 21 static method IDs (section 4.1), then `ALicenseCheck_SetRMS_PATH("/data/data/com.gameloft.android.ANMP.GloftR6HP")` and `ALicenseCheck_InitLicense(env, GameClass)`. Uses `g_env`, not its `env` argument |
| `Game.nativeTouchPressed(III)V` | instance | @272f4c | `onTouchEvent`, U | `(x, y, pointerId)` -> `OS_GetTime` + `notifyTouchPress` -> `glitch::IDevice::postMouseEventFromUser` |
| `Game.nativeTouchMoved(III)V` | instance | @272f3c | U | `notifyTouchMoved(x, y, id)` |
| `Game.nativeTouchReleased(III)V` | instance | @272f88 | U | `notifyTouchReleased(x, y, id)` |
| `Game.nativeSetOnKeyDown(I)V` / `nativeSetOnKeyUp(I)V` | static | @272f30 / @272f24 | `onKeyDown` / `onKeyUp`, U | `notifyKeyPressed` / `notifyKeyReleased(androidKeyCode)` |
| `Game.nativeIsRightTouchPad(I)I`, `nativeIsLeftTouchPad(I)I`, `nativeIsTouchPad(I)I`, `nativeIsSlideEnable(I)I`, `nativeIsSlideChange(I)I` | static | @27308c, @2730a0, @2730b4, @273078, @273064 | `onTouchEvent` / `onConfigurationChanged` / `onCreate`, U | each stores its argument in a global (Xperia Play touchpad / slider state). Return value never used by Java |
| `Game.nativeCanInterrupt()I` | instance | @2731ec | `Game.onPause`, U | returns the constant 1 (global 0x622a68, initial value 1, never written). `onPause` spins on it until non-zero |
| `Game.processTouchpadAsPointer(ILandroid/view/ViewParent;Z)Z` | static | @273134 | `onUserInteraction`, U (once) | Xperia Play only: `FindClass("android/view/ViewRoot")`, `setProcessPositionEvents(Z)V`. Safe to return 0 |
| `Game.nativeAccelerometer(FFF)V`, `nativeOrientation(FFF)V` | instance | @272f98, @272fc8 | **no caller** | store three floats (softfp: floats arrive in r2, r3, stack) |
| `Game.nativeOnTouchGame(IIII)V`, `nativeTouchPadPressed/Moved/Released(III)V` | static/instance | @273124, @2730d8, @2730c8, @273114 | **no caller** | Xperia touchpad / `appOnTouch(action,x,y,id)` |
| `Game.nativeEnd()V`, `Game.nativeGyro(FFF)V`, `GLiveMain.nativeInit()V` | | **not exported** | **no caller** | the JVM would throw `UnsatisfiedLinkError` if called; nobody calls them |
| `GLResLoader.nativeInit()V` | static | `GLResLoader_nativeInit` @272744 | `GLResLoader.init`, called from U (`Game.onCreate`) and from G (`onSurfaceCreated`) | `NewGlobalRef(class)` + 5 static method IDs (section 4.3). Uses its `env` argument |
| `GLUtils.Device.nativeInit()V` | static | `GLUtils_Device_nativeInit` @2742e8 | `Device.init`, U (`Game.onCreate`) | `ADevice::Init(env, cls)` @2742f0: `NewGlobalRef(cls)` + 8 static method IDs (section 4.5) |
| `GLMediaPlayer.nativeInit()V` | static | `GLMediaPlayer_nativeInit` @273660 | `GLMediaPlayer.init(II)`, G | `NewGlobalRef(class)` + 34 static method IDs (section 4.4) |
| `GLMediaPlayer.nativeGetTotalSounds()I` / `nativeGetTotalSoundsOfSameInstance()I` | static | @273b40 / @273b48 | `GLMediaPlayer.init()`, G | return the constants 635 (0x27b) and 2 |
| `GLMediaPlayer.nativeSetStopOnMusic(I)V` | static | @273b4c | no caller | `bx lr` |

Calling convention reminder: `armeabi-v7a` JNI is softfp, so `float` arguments of these natives travel in core
registers (`nativeAccelerometer`: r2, r3, [sp]). `JNI_OnLoad(vm, reserved)` @272404 calls `VoxSetJavaVM(vm)`, then
`NVThreadInit(vm)` (saves the vm at 0x628b74) and returns `0x00010004` (JNI_VERSION_1_4) [V].

## 3. Startup, frame loop, pause/resume and input

### 3.1 Process and Activity start [V]

1. `Game.<clinit>` (U): `loadLibrary("rainbowsix")` runs `JNI_OnLoad` (above), then `loadLibrary("StormGLOFT")` runs
   its `JNI_OnLoad`, which patches the game library (section 8).
2. `Game.onCreate(Bundle)` (U):
   1. `com.savegame.SavesRestoringPortable.DoSmth(ctx)`: repacker class, restores `assets/data.save` (section 7.3).
   2. If `GameInstaller.sbStarted` is false: start `.installer.GameInstaller` and `finish()`. The installer verifies
      the data folder against `pack.info`, downloads missing files, sets `sbStarted` and starts `Game` again. The
      port has no installer: the data folder is a precondition.
   3. Window flags `0x480` (FULLSCREEN | KEEP_SCREEN_ON), `setVolumeControlStream(3)`, `requestWindowFeature(NO_TITLE)`.
   4. `new GameGLSurfaceView(this)`: its constructor creates `GameRenderer(ctx, view)`, which sets the UI thread
      priority to -15, creates the Java keyboard helper (`x`) and reads `Display.getWidth()/getHeight()` into
      `GameRenderer.b/c`. The surface format is `TRANSLUCENT (-3)`; EGL context factory with
      `EGL_CONTEXT_CLIENT_VERSION = 2`; `ConfigChooser(8,8,8,8, depth 24, stencil 0)` with `EGL_RENDERABLE_TYPE =
      EGL_OPENGL_ES2_BIT`, accepting the first config whose R,G,B,A are exactly 8 and depth >= 24; render mode is the
      default (continuous).
   5. `setContentView`, hide keyboard, `WifiManager` + `enableWifi(1)`, `Game.d = getLanguage(ISO3 language, ISO3
      country)` (section 4.1), `Game.p/q = Build.MANUFACTURER/MODEL` lower-cased.
   6. Xperia-Play branch (taken everywhere because `isXperiaPlayPhone()` is constant true): surface callback,
      `requestFocus`, `MotionEvent.obtain(...).getSource()` probe (sets `Game.v = true`), `nativeIsSlideEnable(0|1)`.
   7. `Game.b = this`, `SUtils.setContext`, `new Device()` and `Device.init()` -> **`Device.nativeInit` (U)**,
      `new GLResLoader()` and `GLResLoader.init()` -> **`GLResLoader.nativeInit` (U)**, telephony listener.
3. `onStart`, `onResume` (U): `GameGLSurfaceView.onResume()` starts the GL thread, `installer.utils.d.a(ctx)` parses
   `res/raw/gi_settings.xml` (device texture lists, used only by `getGPUInfo`), sensor manager and orientation listener
   are created (they forward nothing, see 3.4).

### 3.2 GL thread boot (the exact native call order) [V]

The GL thread chooses the EGL config, creates the ES 2.0 context, then calls `GameRenderer.onSurfaceCreated`:

1. `MyVideoView.isVideoCompleted()`: false on a cold start. Then `Game` starts `.MyVideoView` with extra
   `video_name = /sdcard/gameloft/games/GloftR6HP/data/video/LOGO_Gameloft_320x480.mp4` and calls `finish()`; the
   video activity plays the Gameloft logo with `VideoView`, and when it ends sets `isVideoCompleted = true`, starts
   `Game` again and finishes. Native code is never reached before the logo has played. Port: skip the logo (treat
   `isVideoCompleted` as true) or play it with `AMediaPlayer`/MediaCodec before step 2 (it is a separate H.264 file).
2. `GameRenderer.nativeInit()` (G): sets `g_env`.
3. `GLResLoader.init()` (G): `nativeInit` again (re-caches with the GL thread's env), then stores Resources/AssetManager.
4. `GLMediaPlayer.init()` (G): `nativeGetTotalSounds()`=635, `nativeGetTotalSoundsOfSameInstance()`=2, `init(635,2)` which calls
   `GLMediaPlayer.nativeInit()` once (flag), then builds the Java `SoundInfo`/`bf`/`GLMusicSingle` objects (not used by native).
5. `Game.nativeInit()` (G): caches `Game` statics, `ALicenseCheck_InitLicense` (which calls `Game.db()` once, section 4.6).
6. `GameRenderer.nativeStart(1, GameRenderer.b, GameRenderer.c, Game.d)` (G): width, height = display size, larger side
   first (`onSurfaceChanged` swaps them when width < height), language = `Game.d`. This runs `appInit`, which takes
   seconds: `GFX_InitializeAndroid` (GL state), `Application::CreateInstance`, `glitch::createDevice(driver 1, size, bits argument 16,
   ...)` (ES 2.0, falls back to ES 1.1), `Application::InitWin32`.
   `appInit` also fixes the global path strings (section 7.1).
7. `onSurfaceChanged(gl, w, h)`: prints, swaps w/h if w < h, `glViewport(0,0,w,h)`, `nativeResize(w,h)` (no-op).
8. Per frame (G): `onDrawFrame`: if `GameGLSurfaceView.a` (paused) clear to black and, if `b`, call `nativePause()`
   once; else `nativeRender()`. GLSurfaceView swaps buffers after `onDrawFrame` returns; the game never calls
   `eglSwapBuffers` itself (`AndroidOS_callSwapBuffers` is dead code and its enable flag 0x665ce4 stays 0).

`onWindowFocusChanged(true)` (U) usually arrives before the first frame: `nativeResume()` runs before `nativeStart` and is safe.

### 3.3 Pause, resume, context loss [V]

- `Game.onPause` (U): `while (nativeCanInterrupt() == 0) sleep(20)` (returns 1 at once), then `GameGLSurfaceView.onPause()`
  (GLSurfaceView stops the GL thread and, by default, destroys the EGL context and surface).
- `Game.onResume` (U): `GameGLSurfaceView.onResume()` (new GL thread run: `onSurfaceCreated` runs again, so steps 2-6 of
  3.2 run again, `nativeStart` this time with the game already initialised), sensors and orientation listener.
- `GameGLSurfaceView.onWindowFocusChanged(false)` (U): `a=b=true`, `nativePause()`, `GLMediaPlayer.suspend()`.
  `(true)`: `a=false`, `nativeResume()`.
- Context recovery in native: `nativePause` sets `paused`; while paused or before init, `appRender` just sets
  `restart=1` (0x665ce8). After a resume, `appRender` re-runs `GFX_InitializeAndroid` (GL state), calls
  `ShowSpinner`/`CTextureManager::unloadAllTexture`/`DismissSpinner` once and then `CTextureManager::loadAllTexture(&progress)` over the
  following frames (progress counter 0x62a568, with `Application::DrawBackground` as loading screen), and finally `Application::Resume`.
  So every pause/resume cycle reloads every texture from disk, which is also what makes a lost GL context harmless [V reading
  of @274c18; the exact frame count is not traced].
  A NativeActivity port that keeps its EGL context alive can ignore the second `nativeStart`; one that recreates the
  context must call `GameRenderer.nativeStart(1, w, h, lang)` again on the new context's thread.
- `Game.onStop` unregisters the sensor listener; `Game.onDestroy` removes the telephony listener. The phone-state
  listener (`ak`) moves the task to back/front around calls.

### 3.4 Input [V]

Touch (`Game.onTouchEvent`, U). On a touchscreen (`getSource() != SOURCE_TOUCHPAD 0x100008`; the Xperia touchpad branch
sets the `Game.M..R` camera fields and is dead elsewhere):

| MotionEvent | native call(s) | arguments |
|---|---|---|
| `ACTION_DOWN` (0) | `nativeTouchPressed(x, y, 0)` | `getX(0), getY(0)` as `int`, **pointer id literally 0** |
| `ACTION_POINTER_DOWN` (5) | `nativeTouchPressed(x, y, id)` | pointer at `(action & 0xff00) >> 8`, `id = getPointerId(index)` |
| `ACTION_MOVE` (2) | `nativeTouchMoved(x, y, id)` for every pointer | `getX(i), getY(i), getPointerId(i)` |
| `ACTION_UP` (1) | `nativeTouchReleased(x, y, id)` for every pointer | same |
| `ACTION_POINTER_UP` (6) | `nativeTouchReleased(x, y, id)` | the lifted pointer |
| `ACTION_CANCEL` (3) and others | none | |

Coordinates are raw view pixels, origin top-left, y down, truncated to `int`; `Game.tx/ty` rescalers are used only on the
touchpad branch. Before every event Java calls `nativeIsRightTouchPad(-1)`, `nativeIsLeftTouchPad(-1)` (state resets;
return values ignored). Native side: `notifyTouch*` post an `SEvent` to `glitch::IDevice::postMouseEventFromUser`
(they read `Gameplay::GetHud()` state and, for the weapon wheel, add 200 to x); they are called from the UI thread
while the GL thread runs the game (assumed benign in the original; serialise onto the GL thread in the port).
`libStormGLOFT` scales these coordinates by `854/surfaceWidth, 480/surfaceHeight` (section 8.4).

Keys (`onKeyDown/Up`, U): `nativeSetOnKeyDown/Up(androidKeyCode)`. Filtered out (returned false, not forwarded): 24, 25
(volume), 27 (camera). `KEYCODE_BACK` (4) with scancode 305 (Xperia Play circle button) is sent as the synthetic 225 (0xe1).
Native `notifyKeyPressed` @275320 acts on: 19 20 21 22 (D-pad), 4 (back), 82 (menu), 80, 102 and 103 (shoulder buttons), 225.
When the Java soft keyboard is active (`x.b`) key events edit a Java string instead (section 6.2).

Sensors: `Game.onSensorChanged` and `aj.onOrientationChanged` are empty; `nativeAccelerometer/Orientation/Gyro` are never
called. There is no tilt input. Multitouch (`nativeTouch*` with distinct pointer ids) and the keys are the only gameplay input.

## 4. What the native code looks up and calls in Java

The game keeps `g_env` (0x628958) and global refs; the lookups run once at the `nativeInit/nativeStart` calls above.
The "live" column says whether any code path in the game can reach the call, established by call-site search
(`bl` cross references over all 21,000 Thumb and 253 ARM functions) [V].

### 4.1 `Game` statics (cached by `Game.nativeInit`; class global at 0x628ba4) [V]

All exist in the dex as static methods with exactly these signatures (checked by script). Native wrapper names in the second column.

| Java static | native wrapper @addr / caller (live) | Java implementation | return / effect expected by native |
|---|---|---|---|
| `sendAppToBackground()V` | `nativeSendAppBackground` @273200 / `Application::Update`, `nativeExit` | `moveTaskToBack(true)` | none |
| `openBrowser(String)V` | `nativeOpenBrowser` @273230 / `Application::GoToTwitter` ("http://ingameads.gameloft.com/redir/?from=r6hp&op=tbfv&game=r6hp&t=twitter") | `ACTION_VIEW` intent for non-empty URL | none |
| `Exit()V` | `nativeExit` @273278 / `MenuBase::OnFSCommand` (menu "exit") | `GLResLoader.destroy`, `GLMediaPlayer.destroy`, destroys the view, `System.exit(0)` | does not return |
| `IsWifiEnabled()I` | `nativeIsWifiEnabled` @2732ac / no caller | `WifiManager.isWifiEnabled() ? 1 : 0` | int |
| `GetPhoneLanguage()I` | `nativeGetPhoneLanguage` @27341c / `Application::GetDeviceLanguage` | by `Locale.getISO3Language()`: eng 0, fra 1, deu 2, ita 3, spa 4, jpn 5, bra or por 8, rus 9; `Locale.toString()` zh_CN 10, zh_TW 11; anything else 0 | int language id. Note: differs from `Game.d` below |
| `GetManufacturer()String` / `GetModel()String` | `nativeGetManufacturer` @27344c / `nativeGetModel` @273498 / `Application` constructor | return `Game.p` / `Game.q`: lower-cased `Build.MANUFACTURER` / `Build.MODEL`, set in `onCreate`, `""` before | native gets `const char*` via `GetStringUTFChars` and **never releases it**, then deletes only the local ref: the pointer must stay valid forever. `Application::InitDeviceFeaturesProfile` (via `GetDeviceModel`) compares the model with `r800`, `lg-su660`, `lg-p990`, `nexus s` and the manufacturer with `htc` |
| `launchGetGames()V` | `nativeLaunchGetGames` @273514 / no caller | `ACTION_VIEW vzw://s/COMBATHD` | none |
| `launchGLLive(I)V` | `nativeOpenGLLive` @273544 / `Application::GoToGLLive` | starts `GLiveMain` (WebView portal, extras `language`, `gginame="25489"`) | none |
| `notifyTrophy(I)V` | `nativeNotifyTrophy` @2735a4 / `AchievementItem::OnAchievementComplete` | reads/creates `/sdcard/gameloft/games/GloftR6HP/androidTrophy.dat` (one int per line, initial 127, `GLiveMain.bb.length` entries) and sets entry `[i]=1` | none; the file is write-only for the native side [A] |
| `launchIGP(I)V` | `nativelaunchIGP` @272ba8 / `Application::GoToIGP` (sets byte 0x628ba0=1 first) | starts `IGPActivity` (ads/portal web page) | none |
| `SetReorientation(I)V` | `nativeSetOrientation` @2735d4 / no caller | enables/disables the OrientationEventListener | none |
| `ShowSpinner(String)V` | `ShowSpinner` @272ff8 / `appRender` (texture reload) | posts a `Dialog` with a `ProgressBar` to the UI thread | none (called on G, Java posts to U) |
| `DismissSpinner()V` | `DismissSpinner` @273034 / `appRender` | dismisses it | none |
| `isMoveMC()I` | `nativeIsMoveMC` @2732dc / `appOnTouch`, `notifyTouch*` | `1` if `M != -1 && N != -1 && M < 380` else `-1`; on a touchscreen `M = N = -1` | **-1**; the native compares the result with 1 |
| `getMoveCamera_X()I`, `getMoveCamera_Y()I`, `getMoveCamera_Flexible_X()I`, `getMoveCamera_Flexible_Y()I` | `nativeGetCameraMove_X/Y/Flexible_X/Y` @27330c, @27333c, @27336c, @27339c / `Hud::UpdateMovementControls` | return statics `O, P, Q, R`, all initialised to -1 and only written by the Xperia touchpad branch | **-1** each |
| `getGPUInfo()I` | `getGPUType` @2734e4 / `appInit`, result goes to `customizeSoure(int)` @274844 | 3 if the device is in `gi_settings.xml`'s `dxt_textures` list, else 2 if in `atc_textures`, else 1 if in `pvrt_textures` (the `default` manufacturer entry is PVRT), else 0 (mapping verified through `installer.utils.d.a/b/c` -> `h.a/b/c`). `customizeSoure(2)` sets byte 0x62a8a0=0 and 0x62a89f=1 | int. Return **2** (ATC): matches the ATC data pack and the Adreno 205 identity that StormGLOFT fakes [A] |
| `SetDeviceMute(I)V` | `nativeSetDeviceMute` @273574 / `SoundManagerVox::Update`, `DeactivateContext`, `SetSoundMute` | `AudioManager.setStreamMute(STREAM_MUSIC, arg == 1)` | none |
| `d()V`, `db()[B`, `dc()[B`, `da()Landroid/app/Activity;` | `ALicenseCheck::Init` (ARM, 0x53f344) looks them up on the `Game` class (4.6) | `d`: `Tracking.init(TelephonyManager)` + `onLaunchGame`; `db`: `GLResLoader.getResourceFull(0x7f040004)` = raw resource `igli.bin` (262,144 bytes), falls back to the device id bytes; `dc`: bytes of `GloftDRM.a()` (a key string); `da`: the Activity | see 4.6 |

`Game.d` (not a static looked up by native, passed to `nativeStart`) comes from `getLanguage(iso3Lang, iso3Country)`
over the table eng 0, deu/ger 1, fra/fre 2, ita 3, esl/spa 4, jpn 5, por 6 (the `por/BRA` and `por/PRT` rows both
return 6, the first match); no match 0 [V]. Whether the game uses the `nativeStart` language, the
`GetPhoneLanguage` language or both could not be read from the Java side; provide both honestly.

### 4.2 `GameRenderer` statics (cached by `nativeStart`) [V live lookup, dead calls]

`getKeyboardText()[B`, `setKeyboard(ILjava/lang/String;I)V`, `isKeyboardRequired()I`, `swapEGLBuffers()V`; wrappers
`AndroidOS_getKeyboardText/showKeyboard/isKeyboardRequired/callSwapBuffers` @2725f8-@27270c. **No code in the game calls
those wrappers** (no `bl`, no data pointer): the lookups must succeed, the methods are never invoked.

### 4.3 `GLResLoader` statics (cached by `GLResLoader.nativeInit`) [V live lookup, dead calls]

`getResourceFull(String)[B`, `getResourceBytes(String,int,int)[B`, `getResourceLength(String)I`, `getSoundRaw(I)[B`,
`getResourceLengthSoundRaw(I)I`. Wrappers `GetNativeResourceFull/Bytes/Length` @272818, @2728a4, @27292c have no callers.
The Java versions map `./x/name.ext` to a drawable named `name_res_...` or an APK asset; the APK assets folder contains
only `data.save` and an empty `testasset.txt`.

### 4.4 `GLMediaPlayer` statics (cached by `GLMediaPlayer.nativeInit`) [V live lookup, dead calls]

34 methods: `isLoaded(I)I unload(I)V load(I)V registerSoundFile(ILjava/lang/String;I)V play(IFI)I play(IFIF)I pause(I)V
pauseAll()V resume(IF)V resumeAll(F)V stop(I)V setVolume(FI)V getVolume(I)F setMasterVolume(F)V getMasterVolume()F stopAll()V
onRecoverAudio()V recoverAudio()V reinit()V isRecoveringAudio()I isPlaying(I)Z isEmitterPlaying(II)Z setEmitterVolume(IIF)V
setEmitterPitch(IIF)V setEmitterParams(IIFF)V stopEmitter(II)V pauseEmitter(II)V resumeEmitter(II)V getEmitter(I)I update()V
loadSoundGroup(IZ)V swapPool(I)V swapAllPools()V setSwapTimer(Z)V`, called by the 36 `AndroidOS_Audio*` exports @273b50-@2742c4
(`AndroidOS_AudioPlay`, `...Load`, `...SetVolume`, ...). **None of those exports has a caller**: the game's sound goes
through `SoundManagerVox`/`vox`, section 5. These Java methods implement a second, unused engine
(`SoundPool` for effects, `MediaPlayer` wrappers `af`/`GLMusicSingle` for music). All 34 lookups must succeed; nothing else is needed.

### 4.5 `GLUtils.Device` statics (cached by `ADevice::Init`, class global at 0x628de4) [V]

| Java static | ADevice wrapper | Java implementation |
|---|---|---|
| `a()[B` | `getIMEI(char*)` @2744d0 | `Build`-derived device id string bytes (`Device.b`), a 1-byte `{0}` if null |
| `b()[B` | `getOperatorName` @2744f8 | network operator name bytes (`Device.e`) |
| `c()[B` | `getLineNumber` @274520 | `getLine1Number()` bytes, default `"00"` |
| `d()[B` | `getGameVersion(char*)` @274548 | `"1.1.6"` |
| `IsWifiEnable()Z` | `isWifiEnable` @2745b0 | wifi state == enabled |
| `getHostName()[B` | `getHostName` @27457c | `Build.MODEL + "_" + Build.PRODUCT` |
| `e(I)V` | `setUniqueCode(int)` @2745d4 | stores the unique code |
| `getUserAgent()String` | `getUserAgent(char*)` @274478, `ALicenseCheck_GetUserAgent` | WebView user-agent string, `"GL_EMU_001"` when no WebView |

The only external caller is `ALicenseCheck_GetUserAgent` @274778 (which itself has no caller); `ADevice::createUniqueCode` returns
`currentTimeMillis*0x22b9+0x457`. Treat as lookup-only; implement the getters anyway with the values above.

### 4.6 Licensing: `ALicenseCheck` [V]

- `Game.nativeInit` -> `ALicenseCheck_SetRMS_PATH("/data/data/com.gameloft.android.ANMP.GloftR6HP")` and
  `ALicenseCheck_InitLicense(env, GameClass)` -> `ALicenseCheck::Init` (ARM @53f344): `NewGlobalRef(Game)`,
  `GetStaticMethodID` on it for `d()V`, `db()[B`, `dc()[B`, `da()Landroid/app/Activity;`, `FindClass("android/content/Context")`
  (+ `NewGlobalRef`; on failure it only logs "Can't get a reference from the class..."), clears three buffers, then
  `LoadConfig` @543118: **one live call**, `CallJNIFuncChar(GameClass, db, buf, 0x40000)`: memset 256 KiB, call `db()`,
  `GetArrayLength`, `GetByteArrayRegion` (guarded by a length check that logs "Warning char overflow"), then
  `ConfigFile::Decode` splits the blob into three strings logged as `SERVER`, `EKEY`, `EPID`. The blob is
  `res/raw/igli.bin`, exactly 262,144 bytes of high-entropy data; return it verbatim.
- `ALicenseCheck_ValidateLicense(bool)` @53f304 (ARM) is `mov r0,#0; return`, i.e. patched out (the original calls
  `ValidateNative`/`ValidateServer`). Its only caller, `PreloadingState::FocusGain` @337fbc, ignores the result. So:
  **a "successful check" leaves nothing behind and nothing is required to succeed.** `ValidateServer` (HTTP to the
  Gameloft key server, `SaveRMS` writing `<RMS_PATH>/iglu.bin`, `getIMEI` via `TelephonyManager.getDeviceId`) is unreachable.
- `Game.dc()` / `GloftDRM` (key from `SharedPreferences gl_a..gl_e`, product id "1189", URL `confirmation.gameloft.com/partners/android/validate_key.php`) are therefore dead for the port.

### 4.7 `android/media/AudioTrack` (the vox audio driver) [V, live]

Section 5.

### 4.8 `android/view/ViewRoot` [V, Xperia only]

`processTouchpadAsPointer` looks up `android/view/ViewRoot.setProcessPositionEvents(Z)V`; Java calls it once from
`onUserInteraction` inside try/catch. Return 0 (or any non-1) and never create `ViewRoot`; no effect on gameplay.

## 5. Audio end to end [V]

1. **Who decides.** All gameplay audio goes through the C++ class `SoundManagerVox` into the statically linked **Vox**
   engine (`libs/Vox/src/*.cpp`, `vox_driver_android.cpp`). It loads its sound pack from the relative path
   `sdcard/gameloft/games/GloftR6HP/data/audio.zip` (note: no leading slash; 0x59ccb8), 1,654 `.wav` and 14 `.vxn` files
   stored uncompressed in a 290 MB zip, plus `data/audio/sfx/sounds.xml`. Formats: MS WAV tag 17 (IMA ADPCM, 4-bit,
   mono and stereo at 22,050/32,000/44,100 Hz) for nearly everything, PCM16 for a dozen files, and Vox's own `VoxN` interactive
   music container for the 14 `.vxn`. Vox has its own decoders (`vox_decoder_mswav` with IMA ADPCM, `vox_decoder_native`,
   `stb_vorbis`, `mpc8`, raw). **Vox decodes, mixes and resamples in C++ and produces the PCM itself.**
2. **How PCM leaves the library.** `vox::DriverAndroid::Init` @500c74 chooses by the global `g_androidApiLevel` (0x627390,
   initial value 3; `VoxSetAndroidAPILevel` @500ad0 has no caller): level > 8 would run `_InitOSL` (OpenSL ES), otherwise
   `_InitAT` @500ce8 (JNI `AudioTrack`). The game library does not link `libOpenSLES` (no `slCreateEngine` import), and
   `_InitOSL` expects engine objects that nobody creates, so **the shipping path is AudioTrack through JNI**. No other
   Java audio API is on the live path. The mixer output is **44,100 Hz, 16-bit signed little-endian, stereo, interleaved**.
3. **The AudioTrack protocol** (all calls go through JNI; `vm` is saved by `VoxSetJavaVM` from `JNI_OnLoad`):
   - `_InitAT` (called on the game thread): `vm->GetEnv(&env, 0x10002)`; `FindClass("android/media/AudioTrack")`, `NewGlobalRef`
     (global 0x6645fc); `GetMethodID(<init>, "(IIIIII)V")`, `GetStaticMethodID(getMinBufferSize, "(III)I")`,
     `GetMethodID` for `play()V`, `pause()V`, `stop()V`, `release()V`, `write([BII)I`; then
     `getMinBufferSize(44100, CHANNEL_OUT_STEREO=12, ENCODING_PCM_16BIT=2)` via `CallStaticIntMethodV`. Derived values:
     `bufferFrames = minBytes / 4` (global at driver+0x58), `updateFrames = min(bufferFrames, 1024)` (driver+0x5c), update
     period `updateFrames / 44100` seconds. Then it starts a **pthread** (`UpdateThreadedAT` @5019c8).
   - `UpdateThreadedAT` (vox thread): `vm->AttachCurrentThread(&env, 0)`; `PushLocalFrame(2)`;
     `NewObject(AudioTrack, <init>, 3 /*STREAM_MUSIC*/, 44100, 12, 2, bufferFrames*4 /*bytes*/, 1 /*MODE_STREAM*/)` (the `NewObjectV`
     table entry); `CallNonvirtualVoidMethod(track, AudioTrackClass, play)`; `NewByteArray(bufferFrames*4)`. Then loop until
     `running` is cleared: `DoCallbackAT`: `GetPrimitiveArrayCritical(array)` -> `DriverCallbackInterface::_FillBuffer(short*, updateFrames)`
     (the mixer) -> `ReleasePrimitiveArrayCritical(array, ptr, 0)` -> `CallNonvirtualIntMethod(track, class, write, array, 0, updateFrames*4)`,
     then it sleeps with `usleep` to keep to the update period (it does not rely on `write` blocking).
   - `Suspend`: `CallNonvirtualVoidMethod(... pause)` on the track; `Resume`: `... play`; `Shutdown`: clears `running`, joins the
     thread, which calls `stop`, `release`, `PopLocalFrame`, `DetachCurrentThread`.
4. **What the JNI layer must do.** Provide an `android/media/AudioTrack` emulation with exactly these members (specification
   in 9.4): a PCM ring buffer fed by `write` and drained by the host audio callback (AAudio/OpenSL ES on Android,
   miniaudio/WASAPI or an SDL-style callback on the Windows runner). `write` should accept up to `updateFrames*4` (<= 4096) bytes
   per call, copy them into the ring and return the byte count; blocking when the ring is full is allowed but not
   required. `getMinBufferSize` can return any multiple of 4 between 4,096 and 16,384 bytes (it only influences the update
   period: `min(bytes/4, 1024)` frames per chunk, i.e. 1,024-frame chunks of about 23 ms for any value >= 4,096 bytes).
5. `SetDeviceMute(1)` (on deactivation) is the only other audio-related Java call; implement as "output muted".
6. The Java `GLMediaPlayer` engine (`SoundPool`, `MediaPlayer`, `res/raw/sound_0000.ogg`, `raw_000.ogg`, `SoundBinFile`) is unused; `nativeGetTotalSounds()` = 635
   only sizes its Java tables.

## 6. Resource loading, keyboard, video, online

### 6.1 Resource loading (`GLResLoader`)

Dead on the native side (4.3). All game data is read by the native code with plain libc `fopen`/`fread` (33 callers of
`fopen`, 1 of `open`, `mmap` only inside STLport) from the data folder (section 7). The archives `data/texts.zip`,
`data/2d.zip`, `data/3d_tga.zip` are mounted by the glitch file system (strings at 0x585d00-0x585d2c); `3d.zip` and `audio.zip`
(vox) are opened separately. There is no APK asset or resource read at run time except `Game.db()` (4.6).

### 6.2 Keyboard and text input

Java: `GameRenderer.setKeyboard(show, initialText, maxLen)` shows the IME on the GL view; while it is up
`Game.onKeyDown` edits `x.a` (allowed characters `A-Za-z 0-9 . # $ & * _ - + / \ |`, max length `x.c` default 250, BACK/ENTER
ends input and calls `GameRenderer.onKeyboardFinish()`), `getKeyboardText()` returns the trimmed bytes. The native
wrappers for these four methods are never called (4.2), so the game has **no live text input path** in this build [V]; the
port needs no IME. Do export `Java_..._GameRenderer_onKeyboardFinish` (a no-op) for completeness.

### 6.3 Video / cutscenes

Only the Gameloft logo (`.MyVideoView`, 3.2 step 1; files `data/video/LOGO_Gameloft_{320x480,480x800,640x960,768x1024}.mp4`
and `_iPhone.m4v`; Java always asks for the 320x480 one) is played by Java. The native library imports no media API and
contains no call to start a video; cinematics are in-engine (`cin_*.wav` sound files, glitch animation). No video JNI is needed.

### 6.4 Networking and online (GLLive, lobby)

- Java `GLiveMain` is a WebView portal ("Gameloft Live", `launchGLLive`); the native hooks `m_GLLiveNew/m_GLLiveDraw`
  (@274781, @274785) are empty and `nativeOpenGLLive` only starts that activity. Offline or on a modern device: make
  `launchGLLive`, `launchIGP`, `openBrowser` no-ops (or open the URL in a browser).
- Multiplayer and server config are native: `XPlayerManager`, `GLXPlayerMPLobby`, `MpManager`, `CIPhoneSocket`/`LCAndroidSocket`
  use BSD sockets (`socket connect select recv send getaddrinfo gethostbyname`) to `alpha01.gameloft.com:7500`, HTTP to
  `http://gllive.gameloft.com/ope/ServerConfig.php`, and the license/config module (4.6). All of those hosts are defunct.
  Offline behaviour: connections fail, the game falls back to the cached `serverConfig.sav` and `oconf.bar` in the data root.
  No JNI is involved; `IsInternetAvaliable`/`IsWifiEnabled` are never called by native code.

## 7. Paths, files and `assets/data.save`

### 7.1 Paths hard-coded by the native side [V]

`appInit` @27486c builds these globals from literals:

| global | value | uses |
|---|---|---|
| 0x628f40 (copied to 0x629740 and 0x629b40) | `"/sdcard/gameloft/games/GloftR6HP" + "/"` | data root; `GetSaveFilePath(out, name)` @274750 and `GetConfigFilePath` @27472c = `sprintf(out, "%s%s", 0x629b40, name)` |
| 0x629f40 | `"/sdcard/gameloft/games/GloftR6HP/"` | second copy |
| 0x629340 | `"/data/data/com.gameloft.android.ANMP.GloftR6HP/"` | app-private folder |
| `ALicenseCheck` RMS path | `/data/data/com.gameloft.android.ANMP.GloftR6HP` | `iglu.bin` (dead) |

Files the native code reads/writes under the data root: `user.dat`, `SaveGame.bin`, `SavedState.bin` (409,726 bytes in the
shipped copy), `XPData.bin`, `XPCheckpoint.bin`, `GLLiveSync.bin`, `oconf.bar`, `serverConfig.sav`, `log_send.txt`/`log_recv.txt`
(debug). Read-only game data, written as relative paths that depend on the current directory being the data root:
`data/Res.array`, `data/android/%s`, `data/2d/swf/%s`, `data/texts.zip`, `data/2d.zip`, `data/3d_tga.zip`, `data0/%08X%08X.obl|.obf`
and `splash_mm.tga`, shaders `*.shd`, `glsl.config`. One path has a different form: vox opens
`sdcard/gameloft/games/GloftR6HP/data/audio.zip` **relative to the process cwd, expected to be `/`**. In the port:
remap `/sdcard/gameloft/games/GloftR6HP[/...]` to the real data directory in the libc shim, resolve relative `data/...` against that
directory, and a relative path starting with `sdcard/gameloft/` against the filesystem root, mapped the same way [A].

Java-side files: `/sdcard/gameloft/games/GloftR6HP/androidTrophy.dat` (trophy flags), `/sdcard/gameloft/games/GloftR6HP/.nomedia`
(0 bytes in the shipped data; `Device.d1` stores a random device UUID in it when it has none), `InsTime`
(installer timestamp), `/data/data/<pkg>/pack.info`, `prefs/gl_res_ver`, `shared_prefs/GameTInfo.xml` (installer state).

### 7.2 Shipped data layout (`original/data/gameloft/games/GloftR6HP`, 765 MB)

`data/` holds `2d.zip` (383 entries), `3d.zip` (4,575), `3d_tga.zip` (723 textures), `audio.zip`, `texts.zip` (1,221 `.glyph`, 11 `.lang`),
the `.array` tables, `Constants.bin`, `pack3D.info`, `textureProfileDependencies.array`, `android/` (calibri.ttf, `splash_mm.tga`,
`menu/{HUD960,hud_854,mainmenu960,mainmenu_854}.swf`, `sprite/Loading*`), `video/`, `audio/sfx/sounds.xml`, `automat`, `menus`, `structs`.

### 7.3 `assets/data.save` [V]

11,072 bytes. It is **not** game data: it is read by the repacker class `com.savegame.SavesRestoringPortable` (called first in
`Game.onCreate`), which decrypts it with AES-128-CBC/PKCS5 (key `00fb82dabf3c62d0c0d974cfc1c95eb1`, IV `2ca46456134d94e6967734223de8e646`,
hard-coded in the dex) into a zip and unpacks it into `/data/data/<pkg>/` on the first run (SharedPreferences flag
`savegame/notfirst`). The decrypted zip holds `pack.info` (29,734 bytes: the installer's manifest of data files with sizes and CRCs),
`prefs/gl_res_ver` (`205`) and `shared_prefs/GameTInfo.xml` (three tracking booleans). Its purpose is to make the installer believe
the 205-version data pack is already installed. It plays no role in a build without the installer.

## 8. `libStormGLOFT.so` [V]

A repacker's hook library ("storm"): 899 exports, own copy of the Mesa GLSL optimizer, an ETC/PVRTC/S3TC/ATC decoder set, ELF
parser and an inline/GOT patcher (`hook_import_function`, `hook_export_function`, `hook_direct`). It depends on `libEGL`,
`libGLESv1_CM`, `libGLESv2`, `libandroid`, `libdl`. Not part of the game; do not translate it, reproduce what it changes.

### 8.1 What its `JNI_OnLoad` does

Runs at the second `System.loadLibrary`. It calls `hook_import_function("/data/data/<pkg>/lib/librainbowsix.so", name, replacement)` to
replace six GOT entries of the game and hooks (ELF symbol lookup plus code patching, `hook_export_function`/`hook_direct`) five of the game's own
functions `appInit`, `appOnTouch`, `notifyTouchPress`, `notifyTouchMoved`, `notifyTouchReleased`:

| patched | replacement behaviour |
|---|---|
| import `glGetString` | `GL_VENDOR` -> `"Qualcomm"`, `GL_RENDERER` -> `"Adreno (TM) 205"`, `GL_VERSION` -> `"OpenGL ES 2.0 1566933"`, `GL_SHADING_LANGUAGE_VERSION` -> `"OpenGL ES GLSL ES 1.00"`, `GL_EXTENSIONS` -> `"GL_OES_compressed_ETC1_RGB8_texture GL_AMD_compressed_ATC_texture GL_AMD_program_binary"`; everything else passes through |
| import `glCompressedTexImage2D` | decompresses on the CPU and uploads with `glTexImage2D`: PVRTC (0x8C00-0x8C03), S3TC DXT1/3/5 (0x83F0-0x83F3), ATC RGB / RGBA explicit / RGBA interpolated (0x8C92, 0x8C93, 0x87EE), ETC1 (0x8D64); RGB -> `GL_RGB`, others -> `GL_RGBA`, `GL_UNSIGNED_BYTE`; returns without calling the real function |
| import `glTexImage2D` | when `is16BitRendering` is set, converts RGBA8888 to RGBA4444 (`GL_UNSIGNED_SHORT_4_4_4_4`); otherwise pass-through |
| import `glShaderSource` | runs the shader text through `glslopt` (Mesa GLSL optimizer, target ES 2.0), uploads the optimized text; on failure the original (assumed) |
| import `glViewport` | ignores its arguments, uses `glViewport(0, 0, my_width, my_height)` = the real surface size |
| import `glScissor` | pass-through |
| hook on the game's `appInit` | records the real `(width, height)` in `my_width/my_height`, then calls the original `appInit(854, 480, lang)` (doubles `orig_width = 854.0`, `orig_height = 480.0` at 0xc6010/0xc6018) |
| hooks on `appOnTouch`, `notifyTouchPress/Moved/Released` | multiply the incoming x by `orig_width / my_width` and y by `orig_height / my_height` before calling the original |

### 8.2 Does the game need it on a modern GPU?

- **Texture decompression: yes.** The game believes it is on an Adreno because `genericDriverInit` queries the extension string
  (movw 0x8c92/0x8c93/0x87ee/0x8c01-3/0x83f0-1 at 0x4904b2.., 0x498688..) and glitch refuses ATC on the CPU ("decompressing ATC formats not
  supported", "decompression of DXT formats not implemented" at 0x589828). With the fake extension string it uploads ATC textures through
  `glCompressedTexImage2D`. The runtime must (a) report `GL_AMD_compressed_ATC_texture` in `glGetString(GL_EXTENSIONS)`
  and (b) decode ATC to RGBA8 before uploading. See 8.3 for the formats that occur.
- **GL strings**: the identity matters only for `Application::InitDeviceFeaturesProfile` and shader/driver selection; reporting the same
  strings as StormGLOFT is the safe choice.
- **GLSL optimizer**: not needed (a modern driver compiles the original GLSL ES 1.00 shaders, 76 `.glsl` files in `3d.zip`).
- **16-bit rendering**: skip (`is16BitRendering` looks like a low-memory switch [A]).
- **854x480**: see 8.4.

### 8.3 Texture formats in the shipped data

Scan of every texture container (zip members parsed in memory) [V]:

| container | count | format |
|---|---|---|
| `3d_tga.zip` | 584 | **DDS** (`DDS ` magic, despite the `.tga` name), FourCC **`ATC `** = ATITC RGB, 4 bpp, 8 bytes per 4x4 block, mipmaps |
| `3d_tga.zip` | 119 | DDS FourCC **`ATCA`** = ATC with alpha, 8 bpp, 16 bytes per block (explicit alpha 0x8C93 by the usual Qualcomm FourCC convention [A]; the interpolated variant would be `ATCI`, absent) |
| `3d_tga.zip` | 20 | **PVR v2** containers named `.tga`, pixel type 0x12 = RGBA8888 uncompressed, 1 level, 256/512/1024 square (skyboxes, `add_glow*`, `dmm28_env_map`, ...) |
| `2d.zip` | 308 + 4 | DDS `ATCA` + DDS `ATC ` (HUD, menu, font_3D) |
| `2d.zip` | 6 | `mipmap_{32..1024}.pvr`: PVR v2 PVRTC 2 bpp with mip chain (probably a test set) [A] |
| `3d.zip` | 10 | DDS (5 `ATCA`, 5 `ATC `): effects `rain`, `MUZZLE_ALL`, `particles`... |
| `data/android` | 1 | `splash_mm.tga`: DDS ATCA 2048x2048 |

No DXT, PVRTC(4 bpp), ETC1, real TGA, PNG or JPEG textures occur; the `.pvr` loader and the PVRTC/DXT paths in the engine are never fed.
`data.txt` in `res/raw` lists the three possible download packs (PVRT, ATC, DXT); this is the ATC one.
Decoder needed: ATC RGB (and RGBA explicit) block decode: the ATC colour block is the two 16-bit endpoints (RGB 5-6-5 with the "mode" bit 15
of endpoint 0 selecting the 5-5-5 + interpolation variant) and 2 bits per texel; the explicit alpha block is 4 bits per texel (16 bytes: alpha block
first, colour block second [A, verify against one decoded texture]).

### 8.4 Resolution

The shipped menus include both `mainmenu_854.swf`/`hud_854.swf` and 960-wide variants, and Java compares touch x against 380, 400, 530, 770
(Xperia Play touchpad geometry). StormGLOFT renders the game at a fixed **854x480** internal resolution and stretches it to the real
viewport, scaling touch input. Whether the engine is correct at other sizes is untested; the safe way to reproduce the original is: call
`GameRenderer.nativeStart(1, 854, 480, lang)`, set the GL viewport to the full surface (this stretches), scale touch coordinates by
`854/surfaceW` and `480/surfaceH`. Letterboxing instead of stretching is a presentation choice.

## 9. Specification of the C JNI layer (`runtime/jni.c`)

### 9.1 Objects and tables

- `JavaVM`: guest-memory struct with a function table of 8 slots [V: only slots 4, 5, 6 are used]: `AttachCurrentThread(vm, JNIEnv** out, void* args)`
  returns 0 and stores the single global `JNIEnv*`; `DetachCurrentThread` returns 0; `GetEnv(vm, void** out, int version)` returns 0 (JNI_OK)
  with the same env for every thread and any version asked (the game asks 0x10002); `DestroyJavaVM` and `AttachCurrentThreadAsDaemon`: trivial.
  `JNI_OnLoad` has to be called with this vm before anything else and must return 0x00010004.
- `JNIEnv`: pointer to pointer to a table of **at least 232 entries** in guest memory, 4 bytes each (reserved slots 0-3, `GetVersion` is 4, ... `ExceptionCheck` is 228).
  The game indexes up to 0x390/4 = 228 [V]. Fill every entry with a stub that aborts with the JNI function name (index to name table in section 9.2) so that a missing function
  is found immediately.
- Handles are guest pointers to small descriptor records the runtime allocates: `jclass`, `jobject`, `jstring`, `jbyteArray`, `jmethodID`, `jfieldID`. `NewGlobalRef(x)`, `NewLocalRef(x)` return `x`;
  `DeleteGlobalRef`, `DeleteLocalRef`, `PushLocalFrame` (returns 0), `PopLocalFrame(x)` (returns `x`) are no-ops.
- **Variadic calls arrive as the `V` entries.** The game's own wrappers (`_JNIEnv::CallStaticIntMethod` @272b69 and its siblings) are
  thunks that build an AAPCS `va_list` (a pointer to the argument words that continue after `mid`) and call the `...MethodV` entry:
  `CallVoidMethodV`(62) @0xf8, `CallStaticObjectMethodV`(115), `CallStaticBooleanMethodV`(118), `CallStaticIntMethodV`(130), `CallStaticFloatMethodV`(136),
  `CallStaticVoidMethodV`(142), `NewObjectV`(29), `CallNonvirtualIntMethodV`(80), `CallNonvirtualVoidMethodV`(92), `CallObjectMethodV`(35) [V].
  Argument words: `int/boolean/char` 1 word, `float` is promoted to `double` (2 words, 8-aligned), `long` 2 words, references 1 word. Also implement the
  non-V forms (114 `CallStaticObjectMethod`, 129 `CallStaticIntMethod`, 141 `CallStaticVoidMethod`, 61 `CallVoidMethod`): the dead `GetNativeResource*`/`AndroidOS_*` code calls those directly, and the
  recompiled arguments follow the normal guest calling convention.
- `GetStringUTFChars` must return memory that **outlives `DeleteLocalRef`** (and is never freed): `nativeGetManufacturer/Model` keep the pointer. Intern the strings.
- `jbyteArray` objects are backed by guest memory so that `GetPrimitiveArrayCritical` can return a writable guest pointer (the AudioTrack path writes the PCM through it).

### 9.2 JNIEnv functions that must work (index: name) [V = reached by live code]

| idx | function | users |
|---|---|---|
| 6 | `FindClass` | vox (`android/media/AudioTrack`), ALicenseCheck (`android/content/Context`), `processTouchpadAsPointer` (`android/view/ViewRoot`, may return NULL) |
| 17 | `ExceptionClear`, 15 `ExceptionOccurred`, 228 `ExceptionCheck` | `processTouchpadAsPointer` only; return 0 |
| 19 / 20 | `PushLocalFrame` / `PopLocalFrame` | vox update thread |
| 21 / 22 / 23 / 25 | `NewGlobalRef` / `DeleteGlobalRef` / `DeleteLocalRef` / `NewLocalRef` | all |
| 29 | `NewObjectV` | vox: constructs `AudioTrack` |
| 31 | `GetObjectClass` | ALicenseCheck (dead path) |
| 33 | `GetMethodID` | vox (AudioTrack instance methods) |
| 35 | `CallObjectMethodV` | ALicenseCheck dead path (`getSystemService`) |
| 61-62 | `CallVoidMethod` / `V` | `processTouchpadAsPointer` only |
| 80 | `CallNonvirtualIntMethodV` | vox: `AudioTrack.write` |
| 92 | `CallNonvirtualVoidMethodV` | vox: `play`, `pause`, `stop`, `release` |
| 113 | `GetStaticMethodID` | every `nativeInit` |
| 114, 115, 117/118, 129/130, 135/136, 141/142 | `CallStaticObject/Boolean/Int/Float/Void` (plain and `V`) | `Game` wrappers; `CallStaticFloat*` has no live user |
| 144, 145 | `GetStaticFieldID`, `GetStaticObjectField` | ALicenseCheck dead path |
| 167 | `NewStringUTF` | `ShowSpinner`, `openBrowser`, `ADevice` |
| 169 | `GetStringUTFChars` | `GetManufacturer`, `GetModel`, `getUserAgent` |
| 171 | `GetArrayLength` | `db()` result, `ADevice::CallJNIFuncChar` |
| 176 | `NewByteArray` | vox AudioTrack buffer |
| 200 | `GetByteArrayRegion` | `db()` result (256 KiB), `ADevice` |
| 222 / 223 | `GetPrimitiveArrayCritical` / `ReleasePrimitiveArrayCritical` | vox `DoCallbackAT` |

### 9.3 Classes, methods and fields to emulate

`mid` stands for `GetStaticMethodID`/`GetMethodID` which must return a non-NULL, distinct id for every row marked "lookup"; unknown `(class, name, sig)` triples should log and return NULL.

**`com/gameloft/android/ANMP/GloftR6HP/Game`** (global class ref given as the `jclass` argument of `Game.nativeInit` [V])

| method | calls reached | return value | status |
|---|---|---|---|
| `sendAppToBackground()V` | yes | host: move to background, otherwise nothing | V |
| `openBrowser(Ljava/lang/String;)V` | yes (Twitter button) | open URL (`ACTION_VIEW`), or log | V |
| `Exit()V` | yes | request activity finish, then end the process/loop | V |
| `IsWifiEnabled()I` | no | 1 | V (lookup) |
| `GetPhoneLanguage()I` | yes | 0 en, 1 fr, 2 de, 3 it, 4 es, 5 ja, 8 pt, 9 ru, 10 zh-CN, 11 zh-TW; default 0 | V |
| `GetManufacturer()Ljava/lang/String;` | yes | lower-case manufacturer string, e.g. `"google"` (stable pointer) | V |
| `GetModel()Ljava/lang/String;` | yes | lower-case model; avoid `r800`, `lg-su660`, `lg-p990`, `nexus s` unless the profile for that model is wanted | V |
| `launchGetGames()V`, `launchGLLive(I)V`, `launchIGP(I)V`, `SetReorientation(I)V` | `launchGLLive`/`launchIGP` yes | no-op (log) | V |
| `notifyTrophy(I)V` | yes | no-op, or write `androidTrophy.dat` | V |
| `ShowSpinner(Ljava/lang/String;)V`, `DismissSpinner()V` | yes (texture reload) | no-op (optional loading indicator) | V |
| `isMoveMC()I` | yes, every touch | **-1** | V |
| `getMoveCamera_X()I`, `getMoveCamera_Y()I`, `getMoveCamera_Flexible_X()I`, `getMoveCamera_Flexible_Y()I` | yes | **-1** each | V |
| `getGPUInfo()I` | yes (once, `appInit`) | 2 | A (value), V (call) |
| `SetDeviceMute(I)V` | yes | store mute flag (1 = mute) and gate audio output | V |
| `d()V` | no | no-op | V (lookup) |
| `db()[B` | **yes, once at boot** | `res/raw/igli.bin`, 262,144 bytes, verbatim | V |
| `dc()[B` | no | a short byte array (e.g. `"0"`) | V (lookup) |
| `da()Landroid/app/Activity;` | no | any non-null handle | V (lookup) |

**`.../GLResLoader`** (lookup only): `getResourceFull(Ljava/lang/String;)[B`, `getResourceBytes(Ljava/lang/String;II)[B`, `getResourceLength(Ljava/lang/String;)I`, `getSoundRaw(I)[B`, `getResourceLengthSoundRaw(I)I`. Return NULL / 0 if ever called. V.

**`.../GameRenderer`** (lookup only): `getKeyboardText()[B`, `setKeyboard(ILjava/lang/String;I)V`, `isKeyboardRequired()I`, `swapEGLBuffers()V`. `isKeyboardRequired` -> 0. V.

**`.../GLMediaPlayer`** (lookup only, 34 methods, section 4.4). Never called; return 0/NULL. V.

**`.../GLUtils/Device`** (lookup via `ADevice::Init`, calls only from dead wrappers): `a()[B`, `b()[B`, `c()[B`, `d()[B`, `IsWifiEnable()Z`, `getHostName()[B`, `e(I)V`, `getUserAgent()Ljava/lang/String;`.
Values if called: `a` `{0}`, `b` `""`, `c` `"00"`, `d` `"1.1.6"`, `IsWifiEnable` true, `getHostName` `"<model>_<product>"`, `getUserAgent` `"GL_EMU_001"`. V (lookups).

**`android/content/Context`** (`FindClass` only): return a non-null class handle. V.
**`android/view/ViewRoot`**: `FindClass` may fail (return NULL); `processTouchpadAsPointer` then returns 0 (it checks `ExceptionCheck`). V.

**Fields**: only `ALicenseCheck`'s dead path uses `GetStaticFieldID/GetStaticObjectField` (for `Context.TELEPHONY_SERVICE`); no live field access. V.

### 9.4 `android/media/AudioTrack` (live, the only Java API that must really work)

| member | signature | behaviour |
|---|---|---|
| class | `android/media/AudioTrack` | `FindClass` returns a class handle; `NewGlobalRef` |
| `<init>` | `(IIIIII)V` | `(streamType=3, sampleRate=44100, channelConfig=12, audioFormat=2, bufferSizeBytes, mode=1)`; creates the output stream: 44,100 Hz, 2 ch, int16 (honour `sampleRate`/`channelConfig` if they ever differ; do not assume) |
| `getMinBufferSize` (static) | `(III)I` | `(44100, 12, 2)` -> return e.g. 8192 (a multiple of 4; 7-16 KiB are all fine) |
| `play` | `()V` | start/resume the stream (the thread calls it right after construction) |
| `pause` | `()V` | pause (called on `Suspend`) |
| `stop` | `()V` | stop (called on shutdown) |
| `release` | `()V` | destroy the stream |
| `write` | `([BII)I` | `(array, offsetInBytes=0, sizeInBytes<=4096)`: copy `sizeInBytes` of interleaved int16 LE PCM into the ring buffer, return the number of bytes accepted (on overflow, accept fewer or block briefly) |
| `byte[]` | `NewByteArray(bufferFrames*4)` | backing store reachable as a guest pointer; `Get/ReleasePrimitiveArrayCritical` hand it to the mixer |

Threading: the game thread runs `_InitAT` (`GetEnv`, class/method lookup, `getMinBufferSize`) and the vox thread (created with `pthread_create`)
runs `AttachCurrentThread`, `NewObject`, `play`, the `write` loop and, at shutdown, `stop`/`release`/`DetachCurrentThread`. `pause`/`play` come from the game
thread via `Suspend`/`Resume`. The emulation must be thread-safe for those two threads.

### 9.5 Entry points the host has to call (the "Java" side of the port)

In order, all on the GL/render thread unless noted (`env` = the global env, `thiz`/`cls` = dummy handles; `GameRenderer.nativeStart`
caches its `cls` argument, so pass the same handle that `GetStaticMethodID` will later be asked about):

1. `JNI_OnLoad(vm, 0)` (library load; expects 0x10004).
2. `Device.nativeInit(env, cls)`; `GLResLoader.nativeInit(env, cls)` (they use their `env` argument).
3. `GameRenderer.nativeInit(env, thiz)` (sets `g_env`; **must run before `Game.nativeInit`**).
4. `GLResLoader.nativeInit`, `GLMediaPlayer.nativeGetTotalSounds()` (635), `nativeGetTotalSoundsOfSameInstance()` (2), `GLMediaPlayer.nativeInit` (all lookups; no Java object model needed).
5. `Game.nativeInit(env, cls)`.
6. `GameRenderer.nativeStart(env, cls, 1, w, h, lang)` (see 8.4 for w, h; `lang` per 4.1).
7. every frame: `GameRenderer.nativeRender(env, cls)`, then swap buffers.
8. input from any thread, ideally marshalled to the render thread: `Game.nativeTouchPressed/Moved/Released(env, thiz, x, y, id)`, `Game.nativeSetOnKeyDown/Up(env, cls, keyCode)`.
9. lifecycle: `GameGLSurfaceView.nativePause(env, thiz)` on focus loss (never before 6 returned), `nativeResume` on focus gain; if the GL context is recreated, repeat 3-6 (`nativeStart` with 1).
10. optional: `Game.nativeCanInterrupt` returns 1; `Game.nativeIsSlideEnable(...)` etc. may be skipped.

## 10. Open points and risks

- The exact mapping `Game.d` vs `GetPhoneLanguage` use in the game could only be traced to the Java values; both are specified.
- `getGPUInfo()` return value: effects of 0/1/3 on `customizeSoure` and the engine were not traced beyond type 2 (flags 0x62a8a0/0x62a89f).
- ATC `ATCA` block layout and the explicit-vs-interpolated alpha question should be checked with one decoded texture (8.3).
- Whether the engine renders correctly at sizes other than 854x480 is unknown (8.4).
- `notifyTouch*` is entered from the UI thread in the original; the engine's event queue is probably tolerant, not proven.
- `data0/%08X%08X.obl|.obf` (0x576a94) is referenced but absent from the shipped data; assumed optional streaming content.
- The data folder is a precondition (no installer): `.nomedia`, the zips and the tables must exist before `nativeStart`; no file is created by the Java layer except `androidTrophy.dat`.

## 11. Summary for the people implementing the JNI layer, audio and boot sequence

1. All JNI exports are `Game_*`, `GameRenderer_*`, `GameGLSurfaceView_*`, `GLResLoader_nativeInit`, `GLMediaPlayer_*`, `Device_nativeInit`, `JNI_OnLoad`; no `RegisterNatives`. `JNI_OnLoad` returns 0x10004.
2. Boot order on the GL thread: `GameRenderer.nativeInit` (stores the env globally), `GLResLoader/GLMediaPlayer` lookups, `Game.nativeInit`, `GameRenderer.nativeStart(1, w, h, lang)`, then `nativeRender` every frame. `Device.nativeInit` and `GLResLoader.nativeInit` also run once earlier from the UI thread.
3. The game stores one global `JNIEnv*` (`GameRenderer.nativeInit`) and uses it for every later call from any thread; `GetEnv`/`AttachCurrentThread` can return that same env.
4. The game calls JNI through the `V` entries (va_list = pointer to the argument words after `mid`); implement `CallStatic{Object,Int,Void,Boolean,Float}MethodV`, `NewObjectV`, `CallNonvirtual{Int,Void}MethodV` first.
5. Only three Java behaviours are really needed: `Game.db()` returns the 262,144-byte `igli.bin`; `android/media/AudioTrack` works; `Game.isMoveMC/getMoveCamera_*` return -1.
6. Everything else in `Game` is fire-and-forget (`ShowSpinner`, `Exit`, `openBrowser`, `launch*`, `notifyTrophy`, `SetDeviceMute`) or never called (`IsWifiEnabled`, `launchGetGames`, `SetReorientation`).
7. The Java audio engine (`GLMediaPlayer`, `SoundPool`, `AndroidOS_Audio*`), the keyboard, `GLResLoader`, `ADevice` getters and `ALicenseCheck` server code are looked up but never called: make the lookups succeed and ignore the rest.
8. Audio: the native Vox engine decodes `data/audio.zip` (IMA ADPCM WAV + VoxN) and mixes 44.1 kHz stereo int16 itself; it pushes PCM only through `AudioTrack.write([BII)I` from its own thread, in chunks of at most 1024 frames (4096 bytes) every ~23 ms.
9. `AudioTrack` members used: `<init>(IIIIII)V` with `(3, 44100, 12, 2, bytes, 1)`, static `getMinBufferSize(III)I`, `play`, `pause`, `stop`, `release`, `write`; plus `NewByteArray` + `GetPrimitiveArrayCritical` for the buffer.
10. No OpenSL ES is used (API-level global is 3); Vox has no other output.
11. Licensing is neutralised in the binary: `ALicenseCheck_ValidateLicense` returns 0 and its result is ignored; nothing needs to succeed and nothing is left behind (no `iglu.bin`).
12. Touch: only `Game.nativeTouchPressed/Moved/Released(x, y, id)`; raw pixels, origin top-left, ACTION_DOWN uses id 0, pointer-down uses `getPointerId`, UP/MOVE loop over all pointers; CANCEL is ignored.
13. Keys: `nativeSetOnKeyDown/Up(androidKeyCode)`; the game acts on 19-22, 4, 82, 80, 102, 103 and 225 (BACK with scancode 305); volume (24, 25) and 27 are not forwarded.
14. No accelerometer, gyro, IME or video input/output from native code. The only video is the Java logo (`LOGO_Gameloft_320x480.mp4`) played before `nativeInit`; skip it.
15. `nativePause` dereferences the application object: never call it before `nativeStart` returned. `nativeResume` before init is safe.
16. After pause/resume the next frames reload every texture (progressive, with a loading background); if the GL context is recreated, call `nativeStart(1, ...)` again; if it is preserved, nothing else is needed.
17. EGL: ES 2.0 context, RGBA8888, depth 24, stencil 0; surface format translucent; continuous rendering; the game does not call `eglSwapBuffers`.
18. Paths: data root `/sdcard/gameloft/games/GloftR6HP/` (plus `data/...` relative paths against it) and `/data/data/com.gameloft.android.ANMP.GloftR6HP/`; vox opens `sdcard/gameloft/games/GloftR6HP/data/audio.zip` relative to cwd `/`. Remap all in the libc shim.
19. Save files in the data root: `user.dat`, `SaveGame.bin`, `SavedState.bin`, `XPData.bin`, `XPCheckpoint.bin`, `oconf.bar`, `serverConfig.sav`, `GLLiveSync.bin`.
20. `assets/data.save` is an AES-encrypted zip with installer state for `GameInstaller` (`pack.info`, `gl_res_ver`, `GameTInfo.xml`); irrelevant without the installer.
21. `libStormGLOFT.so` is the repacker's patcher: fake GL strings (Adreno 205, ATC extension), CPU decompression of compressed textures, a fixed 854x480 internal resolution with touch rescaling, shader optimizer. Reproduce the first three in the runtime, drop the optimizer.
22. Texture data: DDS `ATC ` (584 + 4 + 5 files) and `ATCA` (119 + 308 + 5 + splash) plus 20 uncompressed RGBA8888 PVR containers; no DXT/PVRTC/ETC in use. Implement an ATC decoder and advertise `GL_AMD_compressed_ATC_texture`.
23. `glCompressedTexImage2D` formats to catch: 0x8C92 (ATC RGB), 0x8C93 (ATC RGBA explicit), 0x87EE (ATC interpolated); the game was never going to see anything else.
24. Language: `Game.d` (nativeStart arg) en 0, de 1, fr 2, it 3, es 4, ja 5, pt 6; `GetPhoneLanguage` en 0, fr 1, de 2, it 3, es 4, ja 5, pt 8, ru 9, zh-CN 10, zh-TW 11.
25. `GetManufacturer`/`GetModel` strings are cached by pointer and never freed: return interned lower-case strings; avoid `r800`, `lg-su660`, `lg-p990`, `nexus s` unless wanted.
26. `getGPUInfo()` returns 2 (ATC/Adreno) in the port; `isMoveMC` and `getMoveCamera_*` return -1.
27. `GetStringUTFChars` results must outlive `DeleteLocalRef`.
28. `NewGlobalRef(x)` returns `x`; jclass handles passed to `nativeInit`/`nativeStart` are cached and later used with `GetStaticMethodID` and `CallStatic*`; keep handle identity stable.
29. Online code (GLLive web portal, IGP ads, Gameloft servers) is dead or defunct: no-op `launchGLLive/launchIGP`, let sockets fail, rely on cached `serverConfig.sav`/`oconf.bar`.
30. Tooling: `tools/recomp/dex.py --natives --so` lists natives and checks exports; `--class/--method/--xref` show bodies and references; `dexdump -d` and `aapt dump xmltree` are in the SDK build-tools.

## Appendix: addresses in `librainbowsix.so`

JNI glue (Thumb): `JNI_OnLoad` 272404, `GameRenderer_nativeInit` 27241c, `nativeStart` 272430, `nativeRender` 272534, `nativeDestroy` 272544, keyboard
wrappers 2725f8-27270c, `GLResLoader_nativeInit` 272744, `GetNativeResource*` 272818/2728a4/27292c, `NVThreadInit` 27297c, `NVThreadGetCurrentJNIEnv` 272988,
`NVThreadSpawnJNIThread` 272a60, `_JNIEnv::Call*MethodV` thunks 272b28-272b88 / 273620 / 273640, `Game_nativeInit` 272c14, touch/key natives 272f24-273134,
`Game` wrappers 272ff8-2735d4, `GLMediaPlayer_nativeInit` 273660, `AndroidOS_Audio*` 273b50-2742c4, `Device_nativeInit` 2742e8, `ADevice::*` 2742f0-2746fc,
`GetConfigFilePath` 27472c, `GetSaveFilePath` 274750, `InitGL` 274798, `customizeSoure` 274844, `appInit` 27486c, `appPause` 274abc, `appOnTouch` 274b08, `appRender` 274c18,
`notifyTouchMoved/Press/Released` 274e44/274f4c/275054, `notifyKeyPressed/Released` 275320/27547c.
ALicenseCheck (ARM): `InitLicense` 53f2e0, `ValidateLicense` 53f304, `SetRMS_PATH` 53f328, `Init` 53f344, `ValidateServer` 53f8bc, `LoadConfig` 543118, `CallJNIFuncChar` 54294c.
Vox: `VoxSetJavaVM` 500a30, `VoxSetAndroidAPILevel` 500ad0, `DriverAndroid::Init` 500c74, `_InitAT` 500ce8, `_InitOSL` 500ff0, `_SuspendAT` 501440, `_ResumeAT` 501590,
`DoCallbackAT` 501758, `UpdateThreadedAT` 5019c8, `SoundManagerVox` calls `nativeSetDeviceMute` at 2b6500/2b6ed0/2b704a.
Globals: `g_env` 628958, Game class ref 628ba4 (method ids 628c60-628cb0), Device class ref 628de4 (ids 628de8-628e04), vox JavaVM 6645f8, AudioTrack class 6645fc and method ids
664600-664618, `g_androidApiLevel` 627390 (=3), `paused` 665ce0, `restart` 665ce8, `inited` 665cf4, `Application*` 62a578, `glitch::IDevice*` 62a57c.
