# Installing the arm64 build on a phone

APK: `build/android/r6sv-recomp.apk` (14,688,726 bytes, package `com.r6sv.recomp`, arm64-v8a only, Android 8.0 / API 26 or
newer, OpenGL ES 2.0, debug-signed). It contains no game data: the 765 MB data folder is pushed separately.
Tested on the Android emulator (Android 14 x86_64 image, arm64 through the image's ARM translation, AVD `r6test`): it
boots, loads the ERS Training mission, renders it and the look-drag works. On a real phone (Galaxy Z Fold7, Android 17)
it also runs into the training mission and takes touch input. Two things had stopped it there: data folders owned by
`shell` (crash at game address 0x2200000 after `Application::Init`, with "game data is missing" logged: see "Folder
ownership" below), and ART aborting in `AttachCurrentThread` (`thread.cc` "FindStackTop" check) because the system-bar
JNI call ran on the game's switched stack; that call now runs on a helper thread.

## Folder ownership (read before pushing data)

The app must own its data folder and everything in it. Folders created by `adb shell mkdir` or `adb push` belong to
`shell` with mode `drwxrws---`, and the app cannot enter them, so the game finds no data. Do not create the folder
yourself: install, start the app once so Android creates `/sdcard/Android/data/com.r6sv.recomp/files`, close it, then
copy. What worked on the emulator (root available): push one tar to `/data/local/tmp`, `adb root`, unpack it in
`/data/media/0/Android/data/com.r6sv.recomp/files`, then `chown -R <app uid>:ext_data_rw` that folder. Without root
(a phone): push, then copy the pushed subfolders as the app with
`adb shell run-as com.r6sv.recomp cp -r <files>/data <files>/data_app`, remove the pushed `data` and rename the copy.

## Build (if you need a fresh APK)

```
py -I -u tools/recomp/build_apk.py --so original/apk/lib/armeabi-v7a/librainbowsix.so --out build/android
```
About 35-65 s with the object cache (`build/recomp-android/`, `build/cache/`), a few minutes cold. Add `--install` to run
`adb install -r` afterwards. Do not run two builds on the same `--out`.

## Install

```
adb install -r build/android/r6sv-recomp.apk
```
Only a 64-bit phone can run it (arm64-v8a). USB debugging must be on.

## Data folder

Push the content of `original/data/gameloft/games/GloftR6HP` (the folder that held `data/`, `SaveGame.bin`, `user.dat`, ...)
into the app's own external folder, which needs no storage permission:

```
adb shell mkdir -p /sdcard/Android/data/com.r6sv.recomp/files
adb push original/data/gameloft/games/GloftR6HP/. /sdcard/Android/data/com.r6sv.recomp/files/
adb push original/apk/res/raw/igli.bin /sdcard/Android/data/com.r6sv.recomp/files/igli.bin
```
Afterwards `/sdcard/Android/data/com.r6sv.recomp/files/data/Res.array` must exist (the app checks that file and logs
"game data is missing" if not). The game's hard-coded `/sdcard/gameloft/games/GloftR6HP` (and the same path without the
leading slash, which Vox uses for `audio.zip`) is redirected to this folder, so saves (`SaveGame.bin`, `user.dat`,
`XPData.bin`) are written there too. The game's `/data/data/com.gameloft.android.ANMP.GloftR6HP` folder is redirected to the
app's internal folder (`pack.info`, preferences).

- Needed: everything except what is listed below. `data/audio.zip` is needed (sound), as are `data/2d`, `data/3d`,
  `data/menus`, `data/texts.zip`, `data/video` and the loose `*.bin`, `*.sav`, `user.dat`, `InsTime` files.
- `data/3d.zip` is not read by the runtime (`docs/DATA.md`): leaving it out saves about 80 MB.
- If `adb push` into `Android/data` is refused (scoped storage on some Android 11+ builds), start the app once so the folder
  exists, or push to `/sdcard/Download/r6sv` and move the files from there with a file manager or a shell that can
  write to the app folder. Do not push into `/sdcard/gameloft/...`: the app does not read it.
- Pushing 4,165 files takes several minutes. Close the app before pushing or replacing files.

## Start and expected first run

```
adb shell am start -n com.r6sv.recomp/android.app.NativeActivity
```
Landscape, full screen, screen kept on. On a good run, in order: a black screen during start-up (412 static constructors,
`JNI_OnLoad`, the Java start-up natives: a few seconds), the Ubisoft logo, the Rainbow Six title ("press any key or touch"),
then the main menu (Campaign / Multiplayer / Settings / Extras). Campaign, New Game, then the ERS Training briefing leads into
the first mission, the path verified on Windows. Sound goes through AAudio (44.1 kHz stereo).

Known and expected:
- The log shows `screen WxH, rendering at 854x480`. The game renders into an 854x480 buffer (what the menus are laid out for
  and what the Windows runner was tested at); Android stretches it to the screen and touches are scaled back. On a phone that
  is not 16:9 the picture is stretched. To keep square pixels with the game seeing the screen's aspect ratio instead, create
  `render_width.txt` containing `854` in the data folder (untested: the game's layout for other viewport shapes has not been checked).
- A warning `[GAMESWF]: error: can't find method push` at menu load, thin black lines in the menu background during
  transitions, and a missing `glsl.config` are known and harmless (`docs/PROGRESS.md`).
- ATC textures are decoded in software (`gl.c`), so level loading takes a while.
- Immersive (hidden navigation bar) mode is attempted through JNI from the native thread, which Android may refuse (logged
  as "hiding the system bars failed"). The window is full screen regardless.
- Back key: passed to the game, as the original Java did; `Game.Exit()` finishes the activity. Volume keys stay with Android.
- Pause (home, screen off) calls the game's pause; returning recreates the EGL surface and shows the pause menu.
  Tested on the emulator (home and back, screen off and on, then resume and play).

## Emulator test record (2026-11-22)

Android 14 emulator (AVD `r6test`), release APK, each mission from a fresh start on Recruit: briefing, loading screen,
intro cutscene (skipped), then a short walk and a few shots.

- All 12 missions load and play: Training, 1 Steel Wind, 2 Cold Thunder, 3 Angel Wire, 4, 5 Blue Sky, 6, 7 Razor Ice,
  8, 9, 10 Wild Arrow, 11 Mystic Tiger. Missions 2-11 were opened by setting the unlock bytes in `SaveGame.bin`
  (file offset 0x54 + level index, one byte each, up to 0x63), not by finishing the previous one.
- Pause menu, Options (Sounds, Controls), Help, home and return, screen off and on: work.
- Not covered: finishing a mission (end-of-mission screens, the save written by real progress), checkpoints reloaded
  after death, multiplayer, sound output (the emulator ran with `-no-audio`; the AAudio stream opens).
- One process death in mission 10 during the first pass, with no crash line logged; a second run of the same
  mission with the same inputs did not repeat it. Unexplained.
- Level loading takes 45-75 s on the emulator (software ATC decoding under ARM translation).

## If it fails: logcat

The runtime logs with tag `nova3`.

```
adb logcat -c
adb shell am start -n com.r6sv.recomp/android.app.NativeActivity
adb logcat -s nova3 AndroidRuntime DEBUG libc
```
or afterwards `adb logcat -d > r6sv.log` (`adb logcat -b crash -d` for native crash tombstones). What to look for:
- `game data folder: ...` and `game data is missing`: data path problem (see above).
- `screen ... rendering at ...`, `EGL surface failed (0x....)`, `no EGL config`, `eglCreateContext ... failed`: graphics.
- `audio: AAudio stream ...` or `AAudio openStream failed`: sound.
- `crash: signal N at game address 0x...` followed by the game call chain: a fault inside the translated game. A signal at a
  host address points to a runtime bug. Send these lines.
- `INSTALL_FAILED_NO_MATCHING_ABIS` at install time: the phone is not arm64.
- Black screen and nothing in logcat: wait 10-20 s (boot work happens before the first frame), then
  `adb logcat -d | grep -i nova3`.

For better crash reports build with `--checked` (bounds checks and shadow call stack, so the chain names game functions):
`py -I -u tools/recomp/build_apk.py --so ... --out build/android --checked` (slower; not built yet).
