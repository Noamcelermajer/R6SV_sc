# Status (independent audit, 2026-11-22)

Everything below comes from commands run during the audit, in `build/audit/` (logs, screenshots, WAV, a fresh lift and
a separate runner build). Earlier reports were not trusted. Nothing was committed; the working tree has many
uncommitted changes (24 modified files), so the audited state is the working tree, not a commit.

## What works (verified)

1. **Translation.** `difftest.py` lifted all 23,171 entries from scratch (22,895 Thumb, 274 ARM): 0 refused.
   Random sample seed 4242, 1,500 functions, 16 inputs each: **676 pass, 0 fail, 824 no coverage** (3,887 s).
   By kind: leaf 131 pass of 157; with calls 545 pass of 1,343 (8 of 15 ARM-mode entries pass).
   Mutation check, 300 mutants: **134 of 161 covered mutants caught**, 139 uncovered. Register substitution 85/86,
   inverted branch 24/25, inverted carry 22/38, removed IT condition 3/12.
   Files: `build/audit/out/{summary.json,results.csv,report.json}`, `build/audit/mut/report.json`, `build/audit/mutate.log`.
2. **Host runner.** Built to `build/audit/runner/` (lifted C from `build/cache`, 49 objects, 87 s link step). Run on the
   real data folder, offscreen 854x480, 8,400 frames in 286 s, with taps at frames 1450, 1800, 2000, 2300, 3300, 6300
   (the sequence in `docs/PROGRESS.md`). Screenshots in `build/audit/shots/`, log `build/audit/run.log`:
   - `f30.png`, `f300.png`: key art, Tom Clancy's Rainbow Six Shadow Vanguard, three operatives (frame 300, no prompt yet).
   - `f1400.png`: same art with "PRESS ANY KEY OR TOUCH THE SCREEN TO CONTINUE".
   - `f2390.png`, `f3000.png`: "SELECT DIFFICULTY" (Recruit, Operative, Elite, Rainbow Six greyed) over a 3D scene of
     armoured operatives in a hangar, with a Back button. The black line streaks at the top of the 3D background are
     visible here too (open issue below).
   - `f5000.png`: fully black (a loading phase between the menu and the mission).
   - `f6700.png`, `f8390.png`: the training mission in 3D: a hangar with a RAINBOW floor emblem, three red-lit targets on
     tripods, cyan arrows, the text "MOVE YOUR HEAD" and "COMPLETE YOUR TRAINING", pause button top left.
   - `drag7590.png` (second run, `build/audit/run2.log`): after `--drag 6800:427:240:200:240:20` the view has turned to
     a wall and floor with the arrows still drawn: look input works.
   Log evidence: `btn_TouchScreen` menu events, then `CHECKPOINT`, `Recreate effect`.
3. **Audio.** `build/audit/audit.wav`: 44.1 kHz stereo 16-bit, 286.07 s, 248 s non-silent; max +32767 and min -32768
   (clipping on 0.004% of samples).
4. **APK file.** `build/android/r6sv-recomp.apk` exists: 14,688,726 bytes; 7 entries: `AndroidManifest.xml`,
   `resources.arsc`, `assets/image.bin` (6.75 MB), `lib/arm64-v8a/libr6sv.so` (24.7 MB, ELF64 AArch64, 262 undefined
   symbols, needs liblog, libandroid, libaaudio, libEGL, libGLESv1_CM, libGLESv2), signature files. No dex. Package
   `com.r6sv.recomp`, minSdk 26, targetSdk 35, `NativeActivity`, landscape, `application-debuggable`. `apksigner verify`:
   verifies with the v2 and v3 schemes; signer `CN=r6sv-recomp debug` (a debug key, RSA 2048). **It has not been run on a
   phone or emulator: none is attached.** `libr6sv.so` is the same C as the runner but compiled for arm64; the
   differential test runs the x86-64 build, so the arm64 compilation itself is untested.

## Furthest point the game really reaches

Title, menus (main, campaign, new game confirmation, difficulty), loading, and the first training mission rendering and
reacting to a touch drag, on Windows only (ANGLE over Direct3D 11, Intel Arc). Not reached: a mission beyond the first
training room, firing, movement, any gameplay objective, saving a progress milestone, multiplayer, pause/resume,
any run on Android.

## What does not work, or is not verified (known gaps)

- **Test coverage is 45% of the sample, not 100%.** 824 of 1,500 sampled functions never completed 4 random runs in
  Unicorn (invalid memory reads from random pointers: 9,008 runs; unmodelled imports such as memcpy, strlen, strcmp, fopen,
  abort). Those are untested, not passed. Functions with calls are covered worst (41% of the sampled ones pass).
  Weak mutation results for carry (58%) and IT conditions (25%).
- **No phone run.** The APK is unproven: `android_main.c`, `audio_android.c` (AAudio) and the arm64 compile were never
  executed. The APK is debuggable and debug-signed.
- **`igli.bin` is not in the APK and not in `docs/INSTALL.md`.** The runtime reads it from the data folder, the app's home
  folder or the current directory (`runtime/jni.c:174-219`). On the Windows runner it is found at
  `original/apk/res/raw/igli.bin` only because the working directory is the repository root. On a phone it is missing
  unless copied next to the data; then `Game.db()` returns zeros, which the licence code gets as its key blob.
- Log line `[GAMESWF]: error: can't find method push` appears 44 times in the 8,400 frame run (`docs/PROGRESS.md` says
  once), plus four gameswf "heap still has bytes used" warnings. Not compared with the original.
- Black line streaks over the 3D menu background (`f2390.png`, `f3000.png`). Cause unknown.
- 7 of 348 imports are not implemented (`runtime/core.c:157-175`: logged and return 0 when called). None was called in the
  runs, but the names were not listed.
- Real-time pacing: the audited runs take 4.8 minutes to reach the first mission.
- Network defaults to offline (`runtime/net.c:186`, `R6SV_NET`): the licence server URL is read and logged but not
  contacted; online features (Gameloft Live, IGP, trophies) are not provided.

## Shortcuts and substitutions (none changes the game's code or data)

Checked: `OVERRIDES` in `tools/recomp/lift.py:61` is empty; a grep of `runtime/` and the build scripts for patches,
hard-coded answers and writes into the loaded image found none beyond the table below (a grep, not a full read); difftest excludes nothing by name (only entries the lifter refuses, and there are none), but
its sample is dominated by functions with calls and no-coverage ones are not retried.

| Where | What is substituted | Does the game use the result? |
|---|---|---|
| `runtime/game.c:83-96` | A fake `/proc/cpuinfo` (ARMv7, VFP, no NEON) so vox picks C mixers; the NEON assembly is not translated. | Yes, deliberately: the game takes a slower code path than on a real phone. |
| `runtime/game.c:109` | The Gameloft logo video the Java side plays first is skipped. | No. |
| `runtime/jni.c:292`, `:174-219` | `Game.db()` returns the original `igli.bin` verbatim (real data, from the original APK). | Yes (licence code). Not patched, but needs the file at run time. |
| `runtime/jni.c:340-342`, `:424` | `TelephonyManager.getDeviceId` returns `"000000000000000"`. | Licence "dead path" per the comments. |
| `runtime/jni.c:297-307`, `:399-406` | `GLUtils.Device` getters return `"\0"`, `""`, `"00"`, `"1.1.6"`, host name from the model, user agent `GL_EMU_001`. | Yes, device-identification strings, no behaviour seen to depend on them. |
| `runtime/jni.c:267-269`, `:352,363-367`, `:370-373` | `IsWifiEnabled` returns 1; `isMoveMC` and `getMoveCamera_*` return -1; `getGPUInfo` returns 2; `Game.d()` nop; `dc()` returns `"0"`; `da()` returns an empty Activity object. | Yes; constant answers. |
| `runtime/jni.c:374-379`, `:385-396` | `GLResLoader` resource getters and all 34 `GLMediaPlayer` methods return 0/NULL (lookup only). | Not called in a 400-frame `-vv` run (`build/audit/run3.log`); not checked later in the game. |
| `runtime/jni.c:465-468` | `FindClass("android/view/ViewRoot")` fails on purpose. | Game copes (comment), as on current Android. |
| `runtime/core.c:175` | Unimplemented imports return 0 with a log line. | Possible; none called so far. |
| `tools/recomp/models.py` | The differential test models imports (allocator, printf, math from Python) identically on both sides; file, socket, longjmp, exit, qsort, strtok, strtod have no model so those runs are incomplete. | Test only: this is why 55% of the sample has no coverage. |

## Next steps, in order

1. Get the APK onto a phone (or an arm64 emulator) and record the first boot; fix what `adb logcat` shows. Add `igli.bin`
   to the APK assets (or to `docs/INSTALL.md`) first, otherwise the licence blob is wrong.
2. Raise test coverage: model `memcpy`/`strlen`/`strcmp`/`fopen`/`abort` and valid pointer arguments in the harness so the
   824 no-coverage functions in the sample run; rerun seed 4242 and then a full pass over all 23,171 functions.
   Strengthen the carry and IT mutants.
3. Run an arm64 build of the differential test (or run the Android build against recorded traces) so the arm64
   compilation is covered.
4. Investigate the `can't find method push` message and the black line artifact; compare with the original behaviour in
   Unicorn (difftest `--only` on the gameswf Array lookup; GL line primitives in `runtime/gles1.c`).
5. Play further on the runner: movement, fire, mission end, save, pause and resume, a long session, so the next gaps
   are found beyond the first training room.
6. Implement the 7 missing imports, list them, and run with `-vvv` to see which the game calls.
7. Commit the work (24 modified files are uncommitted) and replace the debug signing key before any distribution to
   one's own devices.
