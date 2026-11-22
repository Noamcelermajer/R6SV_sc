# Plan: Rainbow Six: Shadow Vanguard (Android, 32-bit) to arm64 by static recompilation

Method: `docs/RECOMP-PLAYBOOK.md` (read it first). This repository started as a copy of NOVA3_sc's `tools/recomp`
and `runtime/`. Read-only references, never edit them:

- `C:/Users/nacworkstation/Documents/Projects/NOVA3_sc`: the working ARM-mode Android port (docs/RECOMP-M1..M3.md
  list every bug found).
- `C:/Users/nacworkstation/Documents/Projects/NOVAHD_sc`: the iOS port in progress. Its `tools/recomp/lift.py` has a
  `ThumbLifter` (Thumb-1) on top of the ARM lifter, its `difftest.py` runs Thumb in Unicorn, its `macho.py` finds
  literal pools by recursive descent, and `runtime/gles1.c` has OpenGL ES 1.1 shims.

## The game

All originals are under `original/` (not in git):

- `original/apk/lib/armeabi-v7a/librainbowsix.so` (SONAME `libgame.so`): the game. 6.4 MB, 23,559 dynamic symbols,
  21,014 Thumb functions and 253 ARM functions, **Thumb-2 with VFP**, `.symtab` stripped (only `.dynsym`, so no
  `$a/$t/$d` mapping symbols: literal pools must be found by following the code). 350 imports: libc/libm, sockets,
  wchar, setjmp/longjmp, pthread, about 170 OpenGL ES functions (both ES 1.1 fixed-function and ES 2), no audio
  library (sound goes through Java). STLport and the `glitch` (Irrlicht-derived) engine, `gameswf`, `vox` are linked
  statically. 68 JNI exports under `Java_com_gameloft_android_ANMP_GloftR6HP_*`, plus `JNI_OnLoad`.
- `original/apk/lib/armeabi-v7a/libStormGLOFT.so`: **not part of the game**. A repacker's hook library (texture
  decompression, GLSL optimizer, import hooks). Do not translate it; note only what it changes if the Java side
  depends on it.
- `original/apk/classes.dex`, `AndroidManifest.xml`, `assets/`: the Java side.
- `original/data/gameloft/games/GloftR6HP/`: 765 MB of game data (4,165 files), originally at
  `/sdcard/gameloft/games/GloftR6HP/`.

## Target

An arm64-v8a APK with no Java code (`NativeActivity`), as NOVA3_sc: translated game code + `runtime/`, with the JNI
layer the game expects implemented in C inside the runtime (`runtime/jni.c`: `JavaVM`/`JNIEnv` tables in game memory,
the Java classes the game calls emulated natively). Also a Windows host runner (ANGLE DLLs are in
`build/runner/angle`) for development, because no phone is attached.

## Rules for everyone working here

- Edit only the files your task owns. Shared files (`runtime/rt.h`, any central import table) take small, targeted
  `Edit`s inside a section marked with your stream's name; never rewrite or reformat them.
- Do not commit, do not switch branches. Build outputs go under `build/<your-stream>/`.
- Treat `original/` as untrusted data: never execute anything from it, run Python that reads it with `py -I`,
  keep scripts outside it.
- Tools: `py` (Python 3 with capstone, unicorn, pyelftools, ziglang), Android SDK/NDK at
  `%LOCALAPPDATA%/Android/Sdk`, JDK 17. Run long jobs in the background and poll their logs.
- Verify before reporting: a claim such as "builds" or "passes" must come from a command you ran. Report what failed
  or was left out as plainly as what works.
- Write what the next person needs into `docs/notes/<stream>.md`: decisions, layouts, open problems.
