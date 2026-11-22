# Progress (Windows host runner)

## Milestone reached: in_game (round 1, 2026-11-22)

The runner built and ran the whole chain with no code changes needed in this round: constructors (412), JNI_OnLoad (0x10004),
Java-side natives in order, appInit, first frame, title screen, menus, touch, a mission loading and rendering, audio.

Evidence (all in `build/boot/`, logs `run1..run7.log`):
- `shots/f30.png`: first frame (Ubisoft logo, frame 30). `shots/a1400.png`: Rainbow Six title ("press any key or touch").
- `shots/b2390.png`: main menu (Campaign / Multiplayer / Settings / Extras) after a tap at 1450 (log: `MenuBase::OnEvent btn_touchscreen`).
- `shots/c1950.png` campaign menu; `c2300.png` new-game confirmation; `d3600.png` difficulty; log `btn_Campaign`, `btn_NewGame`, `btn_ok`.
- `shots/e7400.png`: ERS Training briefing after "Create World ... cutscene manager" in the log; `g8390.png` / `g6700.png`: the training
  mission rendering in 3D ("MOVE YOUR HEAD", HUD, pause button) after a tap at frame 6300.
- Audio: build/boot/g.wav (265 s) and a.wav, 44.1 kHz stereo, non-silent; g.wav peaks reach full scale (+-32767) so check for clipping later.

Build: `py -I tools/recomp/build_runner.py --so original/apk/lib/armeabi-v7a/librainbowsix.so --out build/runner` (about 5.5 min with
the lifted-C cache, 49 translated objects). Checked build (bounds checks), so about 30 frames/s offscreen.

Command that reproduces the in-game screenshot (about 270 s):
```
build\runner\r6sv_runner.exe --image build\runner\image.bin --data original\data\gameloft\games\GloftR6HP --offscreen --frames 8400 ^
  --tap 1450:427:240 --tap 1800:640:165 --tap 2000:640:165 --tap 2300:322:302 --tap 3300:640:165 --tap 6300:427:240 ^
  --shot 8390:build\boot\shots\g8390.png --wav build\boot\g.wav
```
Taps: title (any), Campaign (640,165), New Game (640,165), YES (322,302), Recruit (640,165), "touch to continue" after loading (about frame 6300).

## What was fixed this round
Nothing: earlier work (lifter, loader, jni.c, game.c, libc, gl/gles1 ATC software decode) was enough. The licence check passes
through `Game.db()` (igli.bin) plus the runtime's config; no code patched.

## Open observations (next work)
1. Log line `[GAMESWF]: error: can't find method push` once at menu load. Unknown whether the original prints it too; if it is a
   mistranslation, run `tools/recomp/difftest.py --only` on the gameswf Array/ActionScript method lookup (search the string in the
   library) and compare. Menus work, so low priority.
2. Menu 3D background shows thin black lines with white ticks during transitions (`c1950.png`, `d5500.png`); likely a geometry or
   blending artifact (GL stream: check line primitives / ES1 shim in gles1.c) or a transition effect. Not investigated.
3. In-game input beyond the first tap not tested: drags for look (`--drag`), fire button, movement. Not run: accelerometer, key events.
4. Runs paced in real time, one screenshot run needs 4 minutes to reach the mission; a savegame or a `--skip` would speed iteration.
5. `glsl.config` is opened and missing (harmless, game falls back). Imports: 341 of 348 implemented; the missing 7 were not listed
   this round (`-vvv` shows them).
6. Not tested: Android (other agent), fullscreen window mode, long play sessions (stability beyond ~8400 frames), pause/resume.
