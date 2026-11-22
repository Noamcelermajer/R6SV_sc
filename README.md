# Rainbow Six: Shadow Vanguard, recompiled for 64-bit

Gameloft's **Tom Clancy's Rainbow Six: Shadow Vanguard** (Android, 2011) shipped as a 32-bit ARM library
(`librainbowsix.so`, Thumb-2 with VFP). Phones sold today are arm64-only and cannot run it. This project translates that
library, all of its machine code, into C by static recompilation, checks the translation against an emulator running the
original, and runs it on a runtime of its own (libc, OpenGL ES, audio and a small JVM for the JNI calls, in C). The
method is the one of the N.O.V.A. 3 port (`docs/RECOMP-PLAYBOOK.md`); no game logic was rewritten by hand.

**Status in one line:** on Windows the translated game boots, shows the title screen, the menus, starts a new campaign and
renders the first training mission in 3D, with sound and touch input. The arm64 APK builds and verifies but has never run
on a phone.

## Status (audited 2026-11-22, numbers from commands run during the audit)

| Part | Result |
|---|---|
| Lifter (`tools/recomp/lift.py`) | 23,171 of 23,171 entries translate (22,895 Thumb, 274 ARM, 2 inner entries); none refused. No hand-written replacements (`OVERRIDES` is empty). |
| Differential test, random sample, seed 4242, 16 inputs each | 1,500 functions drawn: **676 pass, 0 fail, 824 no coverage** (55%: random inputs fault in Unicorn or reach an unmodelled import). By kind: 131 of 157 leaf functions pass, 545 of 1,343 functions with calls pass. |
| Mutation check (`--mutate mixed`, 300 mutants) | 134 of 161 covered mutants caught; 139 had no coverage. By kind: wrong source register 85 of 86, inverted branch 24 of 25, inverted carry 22 of 38, removed IT condition 3 of 12. The carry and IT checks are weak: 25 mutants of those two kinds passed (equivalent for the inputs tried or not exercised). |
| Windows host runner | Builds (checked build, 49 translated objects, 42 MB exe). 8,400 frames offscreen at 854x480 in 286 s: 412 constructors, `JNI_OnLoad`, title, menus, new game, training mission rendering. 341 of 348 imports implemented. |
| Audio | WAV capture of 286 s, 44.1 kHz stereo; 248 of 286 seconds are not silent; peaks reach full scale (0.004% of samples clip). |
| Android APK | `build/android/r6sv-recomp.apk`, 14,688,726 bytes, arm64-v8a only, debug-signed (v2 and v3 verify), `NativeActivity`, no Java code. **Never run on a phone or emulator** (none attached). |

Furthest point reached (on the Windows runner): the first training mission is loaded and rendered in 3D with HUD and
pause button, and a touch drag turns the camera. Nothing beyond the first mission has been tried.

## What it does

- **Lifter** (`tools/recomp/lift.py`): ARM and Thumb-2 plus VFP to C, one C function per entry.
- **Differential test** (`tools/recomp/difftest.py`): each original function runs in Unicorn and its translation in a
  DLL from the same state; registers, flags, VFP registers and all writable memory are compared.
- **Runtime** (`runtime/`): 4 GB game memory with the original 32-bit layout, libc, pthread, sockets (offline by
  default), OpenGL ES 1.1 and 2 on ANGLE, audio, and `runtime/jni.c`, the JNI layer (JavaVM and JNIEnv tables in game
  memory, the game's Java classes emulated in C). Front ends: Windows host runner and Android `NativeActivity`.
- **APK** (`tools/recomp/build_apk.py`): arm64-v8a, no Java.

## Building

You need your own copy of the game: the APK (for `lib/armeabi-v7a/librainbowsix.so` and `res/raw/igli.bin`) and the data
folder `GloftR6HP`. Neither is in this repository. Put them under `original/` as in `docs/PLAN.md`.

```
py -I tools/recomp/build_runner.py --so original/apk/lib/armeabi-v7a/librainbowsix.so --out build/runner   # Windows runner, 5 to 6 minutes
build\runner\r6sv_runner.exe --image build\runner\image.bin --data original\data\gameloft\games\GloftR6HP --offscreen --frames 400 --shot 300:shot.png
py -I -u tools/recomp/build_apk.py --so original/apk/lib/armeabi-v7a/librainbowsix.so --out build/android       # needs the Android SDK, NDK, JDK 17
py -I tools/recomp/difftest.py --sample 1500 --seed 4242 --runs 16                                              # about 65 minutes
```

Run the runner from the repository root: it looks for `igli.bin` at `original/apk/res/raw/igli.bin` relative to the
working directory (or `R6SV_IGLI`, or `<data>/igli.bin`). Phone installation: `docs/INSTALL.md` (see `docs/STATUS.md`:
the guide does not yet mention `igli.bin`).

## Documents

`docs/PLAN.md` (goal and rules), `docs/RECOMP-PLAYBOOK.md` (the method), `docs/PROGRESS.md` (the runner milestone),
`docs/STATUS.md` (audit: what works, what does not, next steps), `docs/INSTALL.md`, `docs/JAVA-SIDE.md`, `docs/DATA.md`,
`docs/notes/*.md` (per component).

## Rights

The game's code and data belong to Gameloft and Ubisoft. This repository holds tools and a runtime, plus analysis data
derived from the binary. Builds contain the game's code and are for your own copy only; do not redistribute them or the
game data. The game's licence check can no longer be answered by its servers (gone); the runtime hands the game its own
original `igli.bin` and runs offline, and the game's code is not modified.
