# OpenGL ES layer (stream: GL)

Files: `tools/recomp/gen_gl.py` (signature table, generates `runtime/gl.c`), `runtime/gles1.c` (hand-written parts,
included by the generated `gl.c`; nothing to add to `RUNTIME_SRC`), this note. Tests and probes: `build/gl/`.

## 1. Which driver the game uses, and how it chooses

The 'glitch' engine (Irrlicht-derived) has three video drivers in `glitch::video`:

| class | what it is |
|---|---|
| `COpenGLES2Driver` (= `CCommonGLDriver<CProgrammableGLDriver<CGLSLShaderHandler>, CProgrammableGLFunctionPointerSet>`) | ES 2, shaders; the `.glsl` files in `3d.zip` (`ProfileCOMMON_emul_VS.glsl` / `_FS.glsl`) |
| `COpenGLESDriver` (= `CCommonGLDriver<CFixedGLDriver<COpenGLESDriver>, CFixedGLFunctionPointerSet>`) | ES 1.1 fixed function: matrix stack, client arrays, lights, fog, `glTexEnv` combine, `glClipPlanef`, point parameters |
| `CNullDriver` | headless |

Selection (all addresses are Thumb entry points in `librainbowsix.so`):

* `appInit` (0x274850 area) calls `glitch::createDevice(driverType = 1, ...)` (0x42f9b1), which builds a
  `CAndroidDevice` (0x44da6d). `CAndroidDevice::createDriver` (0x44dbc5) switches on the type:
  `1` -> try **ES 2 first, ES 1 only if that fails**; `0` -> null driver; `0x80`/`0x100` (OpenGL/Direct3D types) ->
  "not available". Nothing in the Java or JNI side picks it; it is hard-coded.
* `createOpenGLES2Driver` (0x4960bd) news a `COpenGLES2Driver` and calls its `initDriver` (vtable slot 5, 0x497675)
  -> `genericDriverInit` (0x49815d). That function: reads `glGetString(GL_VERSION)`, skips to the first digit
  (using `_ctype_`), `sscanf("%d.%d")` -> `major*100+minor`, then calls `CGLSLShaderHandler::doVersionCheck(version)`
  (0x4c1155), which **fails when the GL version is below 2.00**. It then reads `GL_SHADING_LANGUAGE_VERSION`
  (`fast_atof` on the part after the first '.', must be >= 1.00), `glGetIntegerv(GL_NUM_SHADER_BINARY_FORMATS /
  GL_SHADER_BINARY_FORMATS)` (only logged), `glGetString(GL_EXTENSIONS)` -> `initExtensions` (0x4bd799) and then
  builds the texture-format table from the extension bits. `GL_VENDOR`/`GL_RENDERER` are only logged (no vendor
  string comparison anywhere in the library).
* If that returns false the engine drops the ES 2 driver and calls `createOpenGLES1Driver` (0x48b265), which runs
  the fixed-function init (`genericDriverInit` 0x48ff25). logcat shows which way it went: the engine prints
  `video::createOpenGLES2Driver AAAA()`, `BBBB()`, `... failed!`, then `video::createOpenGLES1Driver()` (strings at
  0x586988..0x586a18).

Java side (dex): `GameGLSurfaceView$ContextFactory.createContext` logs "creating OpenGL ES 2.0 context" and calls
`eglCreateContext` with `{EGL_CONTEXT_CLIENT_VERSION (0x3098), 2, EGL_NONE}`. `ConfigChooser` asks for
`EGL_RENDERABLE_TYPE = EGL_OPENGL_ES2_BIT (4)` with RGBA 8888, depth 24 and no stencil (constants of the six
constructor arguments; the chooser scores configs by distance to those), and the surface format is
`PixelFormat.TRANSLUCENT` (so the window needs an alpha channel).

### What the JNI/boot code must do

* **Create an ES 2 context** (`EGL_CONTEXT_CLIENT_VERSION 2`, `EGL_OPENGL_ES2_BIT`). That is what `host_main.c` and
  `android_main.c` already do. On ANGLE and on modern phones this returns an ES 3.x context that is backward
  compatible; the shims report `OpenGL ES 2.0` to the game regardless (see section 3). Stencil size is not required
  by the game (it asks for 0) but harmless.
* The choice **can be steered**, but there is normally no reason to: an ES 1 context (`EGL_CONTEXT_CLIENT_VERSION 1`,
  `EGL_OPENGL_ES_BIT`) makes `GL_VERSION` read `OpenGL ES-CM 1.1`, the ES 2 init fails and the ES 1 driver runs.
  A context that does not match the driver does not work: the fallback needs a real ES 1 context, because after
  an ES 2 driver failure the ES 1 driver calls `glMatrixMode` & co. on the same context. Both sets of shims are
  generated, so either way links.
* After re-creating the EGL context (Android destroys it with the surface) call `rt_gl_context_changed()` (defined
  in `gl.c`, not yet declared in `rt.h`): it forgets the cached host entry points, the buffer bindings and any
  buffer mapping.
* No EGL or `dlsym` import exists in the game: all GL goes through the 145 `gl*` imports.
* `Game.getGPUInfo()` (static, returns int) feeds the native `customizeSoure(int)` (0x274845) via `getGPUType`
  (0x2734e5): a device-table lookup (an XML raw resource) returning 0..3. Return **0**. Only the value 2 does
  something: it sets `m_htc_OptimizeATC = 1` and `m_htc_OptimizeCulling = 0` (globals 0x62a89f / 0x62a8a0, read in
  `Application::_Draw`); it has no effect on GL.

## 2. Texture formats the data needs

Survey of `original/data/.../data/{3d.zip, 3d_tga.zip, 2d.zip}` (all `.tga` files are not TGA):

| content | count | format |
|---|---|---|
| `3d_tga.zip` | 584 | DDS (fourcc `ATC `) = `GL_ATC_RGB_AMD` 0x8C92 |
| | 119 | DDS (fourcc `ATCA`) = `GL_ATC_RGBA_EXPLICIT_ALPHA_AMD` 0x8C93 |
| | 20 | PVR v2, pixel type 0x12 = RGBA8888 (uncompressed) |
| `3d.zip` | 10 | DDS ATC / ATCA |
| `2d.zip` | 312 | DDS ATCA / ATC; plus 6 `mipmap_*.pvr` = PVR v2 type 0x18 = **PVRTC 2bpp** (RGBA) |

So the game is an **ATC build** (Adreno; the AMD/Qualcomm format). No ETC1, no S3TC/DXT, no ASTC data. ATC exists
only on Adreno GPUs; desktop ANGLE, Mali and PowerVR phones have none of it (ANGLE here lists S3TC, ETC2, BPTC, RGTC;
no ATC, no PVRTC, no ETC1). `libStormGLOFT.so` (the repacker's hook) is the same story: it reports the renderer as
"Adreno (TM) 205", carries an ETC decoder, a Mesa GLSL preprocessor (shader optimizer) and hooks
`glCompressedTexImage2D`/`glTexImage2D`/`glShaderSource`/`glViewport`/`glScissor`/`glGetString`; nothing of it is
translated.

The ES 2 driver enables the ATC formats only if `GL_AMD_compressed_ATC_texture` is in the extension string (extension
bit 404 -> word at driver+0x7a8, bit 20), PVRTC with `GL_IMG_texture_compression_pvrtc` (bit 414), ETC1 and S3TC have
their own bits. Hence:

* `glGetString(GL_EXTENSIONS)` always lists `GL_AMD_compressed_ATC_texture`, `GL_IMG_texture_compression_pvrtc` and
  `GL_OES_mapbuffer`, plus `GL_OES_texture_3D` when the host can do 3D textures, plus the ES 3-core names
  (`GL_OES_depth24`, `packed_depth_stencil`, `rgb8_rgba8`, `element_index_uint`, `texture_npot`) on an ES 3 host.
  The rest is the host's list (ANGLE's is 3.6 KB; the game ignores names it does not know).
* `glCompressedTexImage2D`/`SubImage2D` with ATC (3 formats; the interpolated-alpha one too) or PVRTC (4 formats) is
  **decoded in software to RGBA8** and uploaded with `glTexImage2D`/`glTexSubImage2D` **when the host does not list the
  extension** (on an Adreno phone the data goes straight through). ETC1 (0x8D64) is sent to `GL_COMPRESSED_RGB8_ETC2`
  when the host has ETC2 but no ETC1 (not used by this data). Anything else passes through.
* `glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS / GL_COMPRESSED_TEXTURE_FORMATS)` includes the decoded formats.
* Decoders are in `gles1.c` (pure functions). ATC: DXT1-like block with the AMD palette (mode flag in bit 15 of
  colour0: `{c0, 5/8 c0 + 3/8 c1, 3/8 c0 + 5/8 c1, c1}` or `{black, c1 - c0/4, c0, c1}`), explicit 4-bit alpha, or
  DXT5-style interpolated alpha. Checked on real textures of the game: decoded images (a character, ammunition box,
  armory panel, glass, wires, particles) are clean (`build/gl/sheet.png`); and an ATC level uploaded through the shim
  and rendered by ANGLE is bit-identical to the decoder's output. The ATC palette formula for mode 1 is from memory of
  the AMD spec; those blocks are 0-5% of a texture and look right, but no hardware reference was available.
  PVRTC is the decoder from `NOVAHD_sc/runtime/gles1.c` (validated there); the six PVR files decode to flat colour
  debug textures (`mipmap_128.pvr` is red, `mipmap_256.pvr` black), not independently verified.
* Mipmaps: the files carry them (`dwMipMapCount`), the loader uploads level by level; every level is decoded on its own.

## 3. Strings the game sees

| name | value |
|---|---|
| `GL_VERSION` | host ES 2+ -> `OpenGL ES 2.0`; ES 1 host -> unchanged (`OpenGL ES-CM 1.1...`) |
| `GL_SHADING_LANGUAGE_VERSION` | host ES 2+ -> `OpenGL ES GLSL ES 1.00` (the game's version parser mis-reads "ES 3.00 (ANGLE ...)"; the shaders are GLSL ES 1.00); ES 1 host -> unchanged |
| `GL_EXTENSIONS` | host list + the additions above |
| `GL_VENDOR`, `GL_RENDERER` | host's, unchanged (only logged by the game) |

All are copied into game memory once (`rt_static_string`) and cached by name. Nothing is returned while no context is
current (the game checks for NULL).

## 4. The shims

148 shims: the 145 `gl*` imports of the library plus 3 more names this build does not import (checked by `gen_gl.py --check <lib.so>`: "imports
without a shim: none"). All GL names are shared by `libGLESv1_CM` and `libGLESv2` where both define them.

* **Host libraries.** Android: `rt_gl_getproc` (set by `android_main.c`: dlsym on libGLESv2, then `eglGetProcAddress`)
  first; if that fails `gl.c` does `dlopen` + `dlsym` on `libGLESv2.so` then `libGLESv1_CM.so` itself, so the ES 1 calls
  need no change in the platform layer (and no link flag: both libraries are only opened, never linked). Windows:
  ANGLE's `libGLESv2.dll` exports **both** the ES 2/3 and the ES 1.1 + OES entry points (tested: a context created with
  `EGL_CONTEXT_CLIENT_VERSION 1` reports `OpenGL ES 1.1.0 (ANGLE 2.1.23105)` and the fixed-function calls render), so
  the same DLL serves both drivers. Entry points are looked up lazily at the first call, with the context current.
* **Core names for the OES calls.** `glBindFramebufferOES`, `glGenRenderbuffersOES`, `glBlendEquationOES`,
  `glGenerateMipmapOES` and the other 9 framebuffer OES imports exist only as ES 1 extensions; on an ES 2+ host the
  core call (`glBindFramebuffer`, ...) is used (checked `GL_VERSION` for "ES-C"), on an ES 1 host the OES name.
  `glTexImage3DOES`/`SubImage3DOES`/`CompressedTexImage3DOES`/`CompressedTexSubImage3DOES` use the OES name if the host
  has it, else the ES 3 core call (same signature).
* **Client arrays.** `glVertexAttribPointer`, `glVertexPointer`, `glColorPointer`, `glNormalPointer`,
  `glTexCoordPointer`: while a buffer is bound to `GL_ARRAY_BUFFER` the pointer argument is an offset and passes
  unchanged, otherwise it is a game address and becomes `rt_mem + address`. That pointer stays valid until the draw
  call because game memory is one fixed host reservation (the game's own array is what GL reads), so nothing is copied
  or translated at draw time. Same for `glDrawElements` indices and `GL_ELEMENT_ARRAY_BUFFER`. The bindings are
  tracked in `glBindBuffer` and `glDeleteBuffers` (single context, single render thread). Tested on ANGLE ES 3.0 and
  ES 1.1: client arrays, VBO offsets, element buffer offsets, game-address indices, unsigned-byte colour arrays.
  On a real ES 3 phone context client arrays only work with the default VAO bound; the game never creates a VAO
  (no `glGenVertexArrays*` import).
* `glShaderSource`: array of game addresses of strings. `glGetShaderSource`, `glGetActiveAttrib/Uniform`,
  `glGet*iv/fv`, `glReadPixels`, `glGen*/glDelete*`: output pointers are 32-bit-typed GL data and go straight into
  game memory.
* `glMapBufferOES`/`glUnmapBufferOES`: a zeroed shadow block in game memory (`rt_calloc` of `GL_BUFFER_SIZE`), uploaded
  with `glBufferSubData` on unmap (a host mapping would be a 64-bit pointer). One mapping per target; write-only.
* `glGetIntegerv`, `glGetString`, `glCompressedTex*2D`, `glBindBuffer`, `glDeleteBuffers`, `glShaderSource` are
  hand-written; the rest is generated from the table.
* A function the host lacks is logged once ("GL: X is not available on this host") and ignored (returns 0).
  Headless runs (`rt_gl_getproc` NULL) do nothing and `glGetString` returns 0.

## 5. Tests (build/gl)

* `angle_probe.c`: creates ES 1/2/3 contexts on `build/runner/angle` and prints version, extensions, compressed formats
  and which entry points exist (`angle_probe.txt` is the output). ANGLE 2.1.23105 on D3D11: ES 1 -> 1.1.0; version 2 and 3
  -> 3.0.0 (requesting 2 does not give a 2.0 context); no ATC/PVRTC/ETC1; `glMapBufferOES` present natively.
* `decode_test.c`: decodes level 0 of an extracted DDS/PVR file with the game decoders; `rgba2png.py` makes a contact
  sheet (`sheet.png`).
* `gl_test.c`: includes the generated `gl.c`, a stub runtime (guest memory, `rt_arg` from a fake `cpu_t`), creates an
  ANGLE pbuffer context and calls the imports as the game would. ES 2 run: 30 checks (strings, shaders through guest
  string arrays, client array / VBO offset / element-buffer offset / game-address index draws, map+unmap, the
  compressed-format list, real ATC level -> glCompressedTexImage2D -> textured draw bit-identical to the decoder,
  OES framebuffer names on ES 2+, `glBlendEquationOES`, 3D texture OES names -> ES 3 core). ES 1 run: matrix stack,
  client vertex/colour arrays, VBO offset, lights/fog/material/texenv/clip plane/point/alpha calls without GL error,
  real OES renderbuffer FBO. Both: 0 failures.

  Build: `py -I -m ziglang cc -target x86_64-windows-gnu -O1 -Itools/recomp/runtime -Iruntime build/gl/gl_test.c -o build/gl/gl_test.exe`;
  run: `gl_test.exe <angle dir> es2|es1 build/gl/tex` (needs the extracted `tex/atc__ammobox.tga`).

## 6. Not done / open

* Never run with the real game (no boot yet): whether `COpenGLES2Driver` init succeeds on ANGLE ES 3.0 with the strings
  above is the first thing to check (logcat/`rt_log` lines `video::createOpenGLES2Driver ... failed!`).
* ATC/PVRTC **3D** compressed uploads (`glCompressedTexImage3DOES`) pass through (no data uses them); S3TC and ETC1
  decoders are not written (not in the data; ETC1 maps to ETC2 where available).
* No ES 1 `GLfixed` (`*x`) entry points: the library imports none.
* Software ATC decoding costs CPU on every compressed upload (a 512x512 level decodes in well under 100 ms, measured with process start included); an Adreno phone does not need it.
  If load times matter, decode on a worker thread or cache decoded levels; not done.
* `rt_gl_context_changed()` is not declared in `rt.h` (shared file; ask the owner to add the prototype) and nobody calls
  it yet.
* If the Mali/Adreno host driver returns an ES 3.x context and a game shader fails to compile, the GLSL version
  string is the first suspect (see section 3).
