# Game data: `original/data/gameloft/games/GloftR6HP`

Everything below was measured on the files and in `librainbowsix.so` (addresses are link addresses; the library has
only `.dynsym`, names are from there). The folder is the content of `/sdcard/gameloft/games/GloftR6HP/` of a phone:
4,165 files, 765 MB. Each file's date is 2019-03-05 (the day it was copied off a device or repacked), so dates say
nothing about origin.

## 1. Layout

```
GloftR6HP/
  .nomedia                0 bytes   written by the Java side so the gallery ignores the folder
  InsTime                11 bytes   "1551757719/" ASCII Unix time of the first start (2019-03-05); Java side
  user.dat                0 bytes   Gameloft Live profile (readGLLiveProfile/saveGLLiveProfile, fopen "r" via GetResourcePath)
  oconf.bar             244 bytes   text, "Key: value\r\n" lines: XPlayerURL, GGI 24407, ChatServer ... (online services config)
  serverConfig.sav      291 bytes   text, same style: GameVer 1.0.0i, latest, WebAPIURL ... (copy of the server's answer)
  SaveGame.bin          212 bytes   settings/progress, little-endian words, first word 0x00092890 (a 4-byte size/version)
  SavedState.bin    409,726 bytes   StateAutomat::SavedState: magic "ATT\x02", then length-prefixed strings ("obj3", "g17", ...)
  XPCheckpoint.bin    2,880 bytes   XP system checkpoint; magic 0x4df77817 (bytes 17 78 f7 4d)
  XPData.bin          4,932 bytes   XP system data; same magic 0x4df77817
  data/
    3d.zip            80 MB   all of data/3d, deflate or stored (4,575 entries incl. folders). Only the Java installer reads it
                              (it unpacks it into data/3d and writes pack3D.info); native code never opens it.
    3d_tga.zip        86 MB   723 textures, flat (no folders), opened by glitch's CFileSystem::addZipFileArchive
    2d.zip           3.0 MB   383 files: 312 tga (DDS/ATC inside), 65 .sprite, 6 .pvr (PVR v1); a glitch archive
    texts.zip        1.2 MB   1,232 files: 1,221 *.<LANG>.glyph + 11 text.<LANG>.lang; a glitch archive
    audio.zip        290 MB   1,668 files, all stored (not compressed): 1,654 .wav + 14 .vxn; opened by vox (not glitch)
    pack3D.info      305 KB   index of the 3d tree for the Java unpacker: u32 header, then entries with Windows paths
                              ("\data\3d\buttons", "ability_button.bdae", "...bdae.zip", sizes). Not used natively.
    Constants.bin     48 KB   constant tables (length-prefixed names: "AchievementDifficultyFunction"...)
    *.array                   Res.array (592 KB; list of resource names, e.g. "data/2d/briefings/bfg_1.tga"),
                              Achievements, Story, CoopStory, DMStory, XPSystem, RankingSystem, VersusRankingSystem,
                              textureProfileDependencies (60 bytes). Binary: u32 count, u32 columns, then typed cells.
    2d/fonts/         11 ttf  28.9 MB (CezanneProDB, Forgot*, Nanum, wqy_zenhei ...)
    2d/swf/            3 tga  texture.tga, texture2a.tga, texture3.tga (ATC)
    3d/             3,786 files (the same 3,786 files as 3d.zip, byte-size equal): see section 2
    android/                  splash_mm.tga, calibri.ttf; menu/{HUD960,hud_854,mainmenu960,mainmenu_854}.swf;
                              sprite/{Loading,Loading854}.{sprite,tga}, font_3D_RU/tc, hud2.tga
    audio/sfx/sounds.xml      the only loose audio file (222 KB, the vox sound-bank description; XML)
    automat/          311     state machine graphs: gameobjects (228), menucontrols (49), worlds (32), game (2 .dia)
    menus/                    HUD*.array (5), flash/{HUD,mainmenu}.swf (older 394/423 KB menus)
    structs/metatypes.gmap    1.2 MB, the meta-type table the .gmap files are decoded with
    video/                    LOGO_Gameloft_{320x480,480x800,640x960,768x1024}.mp4 and _iPhone.m4v (played by Java's MyVideoView)
```

## 2. File kinds (by header, with counts of the whole tree)

| Kind | Count | Header | Content |
|---|---|---|---|
| `.bdae` | 2,466 | `BRES` `feff0000` (1,850) | glitch binary COLLADA resource (models, animations, materials, effects) |
| `.bdae` | | `PK\3\4` (616) | a zip with two members, `little_endian_not_quantized.bdae` and `little_endian_quantized.bdae`; the engine picks the variant (strings `little_endian_quantized.bdae` in the library) |
| `.graphml` | 952 | u32 count | despite the name, binary state-machine graphs (automat) |
| `.gmap` | 306 | u32 | game-object maps (level objects), decoded with `data/structs/metatypes.gmap` |
| `.pf` | 162 | `00000000` | path-finding data, one per floor of a mission (`Mission1_floor3.pf`) |
| `.glsl` | 76 | text `attribute ...`/`uniform ...` | GLSL ES 1.00 shader sources (`3d/effects`), compiled at run time (`%s/%s.shd` is the cache name) |
| `.array` | 76 | u32 | tables (see Res.array above, `Mission1_Objectives.array`) |
| `.dope` | 26 | `02000000` | animation/physics set descriptions (`3d/characters`, `weapons`) |
| `.room` / `.wmap` | 18 / 18 | u32 | room volumes and world map per mission |
| `.tga` | 19 | `DDS ` | **Qualcomm ATC compressed textures in a DDS container** (fourcc `ATC `, `ATCA`, `ATCI`), not TGA |
| `.ttf` | 12 | `00010000` | TrueType |
| `.swf` | 6 | `FWS\x08/\x09` | Flash menus for gameswf (HUD, main menu in 960 and 854 variants) |
| `.sprite` | 4 | `01000000` | sprite atlases (frame tables) |
| `.mp4`/`.m4v` | 5 | `ftyp` | Gameloft logo videos |
| `.dia` | 2 | u32 | dialogue tables |

Archive members:
- `3d_tga.zip`: 703 DDS with ATC fourcc (584 `ATC `, 119 `ATCA`) and 20 PVR v1 files (header 0x34 bytes, tag `PVR!`,
  uncompressed RGBA8888). The names (`add_glow.tga`) are looked up by base name.
- `2d.zip`: 312 `.tga` (308 `ATCA`, 4 `ATC `), 65 `.sprite`, 6 `.pvr` (PVR v1).
- `texts.zip`: 1,221 `.glyph` files (per language `BR DE EN FR IT JP KO RU SC SP TC`; many are tiny or empty,
  e.g. `DeathMatch01_briefing.EN.glyph` is 0 bytes) stored, and 11 `text.<LANG>.lang` (deflate) that start with
  `u32 5, u32 10` and then (u16 length, name, u32) section records (`HUD`, `Menu`, `Objectives`, `Dialogs`, ...).
- `audio.zip`: wav is mostly IMA ADPCM (format tag 0x11, 32 kHz mono/stereo, 253 of the first 300 entries; a few PCM 22050/44100),
  `.vxn` are vox music streams (`m_blue_sky.vxn` ...).

**Textures are all Adreno ATC.** GL_ATC_RGB_AMD (0x8C92), GL_ATC_RGBA_EXPLICIT_ALPHA_AMD (0x8C93),
GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD (0x87EE) are what the game passes to `glCompressedTexImage2D`. Only Qualcomm GPUs
accept them. A runtime for any other GPU (ANGLE on Windows, Mali, PowerVR, Xclipse) has to decode ATC to RGBA (or
re-encode to DXT/ETC2) inside `glCompressedTexImage2D`/`glCompressedTexSubImage2D`. That is the GL agent's job; it is
noted here because it is the main data dependency. `libStormGLOFT.so` (the repacker's library) hooks `glTexImage2D`,
`glShaderSource`, `glGetString`, `glViewport`, `glScissor` and `appInit`/`appOnTouch`/`notifyTouchMoved` for that
purpose (decompression, a GLSL optimizer, and scaling the render size: it keeps `orig_width/orig_height` and
`my_width/my_height`). We do not translate it.

## 3. What looks like it came from the repacker, and what from the game

The data folder itself is a plain copy of a played install; no file in it is recognizably produced by a repacker.
Items that need a remark:

- `data/3d/` next to `data/3d.zip`: the tree is the exact unpacking of the zip (all 3,786 file names and sizes match).
  The original Java installer does that on first start (`GLResLoader`, `GLUtils/ZipFile`, preference `gl_res_ver`,
  `/data/data/<pkg>/pack.info`). So it is game behaviour, not a repacker's change; the runtime does not need
  `3d.zip` at all (skip it when pushing to a phone: -80 MB).
- ATC textures: chosen for Qualcomm devices by the original build (`ATC` in the data, `m_htc_OptimizeATC` in the
  library). If the repack was meant to run on other GPUs, the work was done at run time by `libStormGLOFT.so`, not in the data.
- `SavedState.bin`, `SaveGame.bin`, `XPData.bin`, `XPCheckpoint.bin`, `InsTime`, `user.dat`, `serverConfig.sav`,
  `oconf.bar` are created by the game and the Java side on a running device: user state (a played game, with the
  language and settings of that user). Delete them for a fresh start; the game recreates them.
- In the APK (not in the data folder): `assets/data.save` (11 KB, high-entropy, undated 2018) and an empty
  `assets/testasset.txt` are not read by the game's code (no string `data.save` in the library or dex); they belong to
  the repacker's packaging.

## 4. How the library builds paths, and where the runtime must make the data appear

Strings in `.rodata`: `/sdcard/gameloft/games/GloftR6HP` (0x56b658), `/data/data/` (0x56b67c),
`com.gameloft.android.ANMP.GloftR6HP` (0x56b688), `%s%s` (0x56b64c), `%s/` (0x56c388), `data/texts.zip` (0x585d00),
`data/2d.zip` (0x585d10), `data/3d_tga.zip` (0x585d1c), `sdcard/` (0x585d34), `%s/%s` (0x585d3c),
`sdcard/gameloft/games/GloftR6HP/data/audio.zip` (0x59ccb8), `data/Res.array` (0x56d378), `user.dat`, `SaveGame.bin`,
`SavedState.bin`, `XPData.bin`, `XPCheckpoint.bin`, `oconf.bar`, `serverConfig.sav`, `GLLiveSync.bin`.

Functions (`appInit` = 0x27486c, Thumb):

1. `appInit(width, height, x)` (exported as `appInit`, called by `GameRenderer.nativeStart`) copies the 32-byte string
   `/sdcard/gameloft/games/GloftR6HP` into a global buffer, appends `/`, publishes the pointer in a global
   (the "game folder"), and builds `/data/data/com.gameloft.android.ANMP.GloftR6HP/` and a second copy of the sdcard string
   in two more buffers. It then creates the glitch device with `dimension2d(width, height)` and calls `Application::InitWin32`.
2. `FileSystemLinux::_getPaths` (0x27dae0) makes the root, resource and save-file paths of the game's own file
   system each `sprintf("%s/", gameFolder)`, so they are `/sdcard/gameloft/games/GloftR6HP//` (a double slash).
   `Application::GetResourcePath(name)` (0x2769e4) is `sprintf(buf, "%s%s", fs->getResourcesPath(), name)`:
   `Res.array` (`data/Res.array`), `user.dat`, `SaveGame.bin`, `SavedState.bin`, `XPData.bin`, `XPCheckpoint.bin`
   all end up as `/sdcard/gameloft/games/GloftR6HP//<name>`. They are opened with `fopen` (`rb`, `r`, `w+b`),
   in `FileSystemLinux::_FileHandle`, `FileManager::FileManager`, `readGLLiveProfile`, `FileStream::Open`, `fs_open_func`.
3. `glitch::io::CFileSystem::open(name, mode)` (0x443934): strips a leading `./`; strips the working-directory prefix
   (global string set by `changeWorkingDirectoryTo` = `chdir`, which nothing calls directly) when present; looks the
   name up in the archives added with `addZipFileArchive` (`CZipReader`; `data/texts.zip`, `data/2d.zip`,
   `data/3d_tga.zip`, added in the `CFileSystem` constructor, 0x442aec); if it is not in an archive and does not
   contain `sdcard/`, opens `sprintf("%s/%s", gameFolder, name)` (logging "CFileSystem::open, tmpPath = "); if it contains
   `sdcard/` the name is opened as is. The archives themselves are opened through the same function, so
   `data/3d_tga.zip` becomes `/sdcard/gameloft/games/GloftR6HP//data/3d_tga.zip`.
4. Everything under `data/3d/...` (models, scenes) is a plain file opened through the same function.
5. vox opens `sdcard/gameloft/games/GloftR6HP/data/audio.zip` **relative, without a leading slash** (`vox::openStdIO`,
   fopen). On Android the process working directory is `/`, so this is `/sdcard/...`.
6. The Java side (native code never sees these, but our JNI layer answers for them): `/sdcard/gameloft/games/GloftR6HP/`,
   `.../data`, `.../data/3d.zip`, `.../data/pack3D.info`, `.../data/video/LOGO_Gameloft_320x480.mp4`,
   `.../soundinfo.bin`, `.../sounds/`, `.../androidTrophy.dat`, `/sdcard/qaWifiOnlyMode.txt`,
   `/data/data/<pkg>/pack.info`, `/data/data/<pkg>/prefs/gl_res_ver`.

The imports that matter: `fopen fread fwrite fseek ftell fclose fgets feof fgetpos fsetpos remove open lseek fstat chdir
getenv mmap munmap` (STLport's `_Filebuf_base::_M_open` uses `open`/`mmap`). There is no `mkdir`, `stat`, `opendir`,
`access`: the game never lists directories and never creates folders (the Java side did).

**What the runtime must provide**

- The game folder `/sdcard/gameloft/games/GloftR6HP` (with and without the leading slash, with `//` collapsed) maps to the
  host data folder: `rt_libc_set_data_dir(<folder>)` plus `rt_libc_add_data_prefix("sdcard/gameloft/games/GloftR6HP")`
  (declared in `rt.h`, the libc stream owns the implementation). The front ends call both before `rt_game_boot`.
- The data folder is the **contents of GloftR6HP**: it holds `data/`, `SaveGame.bin`, ... directly.
  Android: `/sdcard/Android/data/com.r6sv.recomp/files/` (no permission needed; the app can write the saves there).
  Windows runner: `--data <folder>`.
- `/data/data/com.gameloft.android.ANMP.GloftR6HP` maps to the app's internal data folder (`rt_path_alias`).
- Relative paths are never resolved against a real working directory (an Android app's is `/`): either they start with
  `sdcard/...` (above) or they are built with the game folder prefix.
- The files are opened case-sensitively on Android: names in the game (`DMMission20`, `Mission1_floor0.pf`) match the
  folder exactly, except possibly some textures looked up by base name inside the archives (zip lookup is by exact name
  in glitch; if misses are logged, the libc stream's case-insensitive fallback in `rt_map_path` covers the loose files).
- Resolution of the menus: the game picks layouts from `Application::GetViewPortWidth`: `MenuManager::LoadSWF`
  (0x2a6b00) uses the 854 layouts when the width is greater than 853 (and the 960 layouts on the unreachable
  `== 960` branch), and `CTextureManager::loadAllTexture` uses `loading854` when the width is exactly 854 (0x356). The
  data has `*_854` and `*960` menus (`HUD960.swf`, `hud_854.swf`, `mainmenu960.swf`, `mainmenu_854.swf`). The front
  ends therefore render at a fixed width of **854** pixels (any aspect), as the playbook describes.
