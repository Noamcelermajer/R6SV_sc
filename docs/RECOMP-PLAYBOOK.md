# Playbook: bringing an old 32-bit Android app to arm64 by static recompilation

A method for an agent (or a person) to take a native Android app whose code exists only as an ARM32 shared library
and make it run natively on a 64-bit-only device, without the source code and without rewriting it by hand. It was
worked out on N.O.V.A. 3 (Gameloft, 2013; 39,000 functions), which went from APK to a playable arm64 build in about two days of agent work.
Everything below is generic; N.O.V.A. 3 specifics appear only as examples and are marked as such. Section 0 covers
other kinds of software (iOS, desktop, consoles, firmware, closed libraries) and when this method is the best choice.

The short version: **translate the machine code mechanically into C, check every translated function against an
emulator running the original, then supply the outside world the library expects (libc, GL, audio, JNI) from a
runtime of your own.** No hand-written reconstruction of the app's logic is needed.

---

## 0. Beyond Android apps: where the method applies, and when it is the best choice

Nothing in the core idea is specific to Android or to apps. It works for any **compiled program whose code is
fixed** (not generated at run time) and whose original platform is gone, or which you want to run natively
somewhere else. The parts that change from case to case are the instruction set the lifter reads, and the "outside
world" the runtime has to supply (an OS API, a JVM, or hardware). The method itself does not change: lift, verify
against an emulator, provide the runtime.

### Where it fits

| Kind of software | Fit | What changes |
|---|---|---|
| 32-bit Android native apps and games (this case) | very good | nothing: this playbook |
| A closed library or plugin without source (one `.so`/`.dll` used by a host program) | very good | the interface is just the exported functions, so the runtime is small |
| Old 32-bit iOS apps (armv7; iOS 11 dropped 32-bit) | good | Mach-O instead of ELF; the Objective-C runtime and UIKit make a much larger import surface |
| Old 32-bit Windows or Linux x86 programs (to x86-64, or to arm64 laptops and servers) | good, harder lifter | x86 has variable-length instructions, more flags and segment quirks; Win32 is a large API surface |
| Console games (N64, PlayStation 2, Xbox 360, …) | proven by others | the runtime emulates hardware (GPU, audio, controllers) instead of an OS; timing matters more. Community projects such as N64Recomp and XenonRecomp take this route. |
| Embedded firmware, controllers, industrial or scientific binaries | good | peripherals and memory-mapped I/O become the runtime; often no symbols |
| Programs that generate code at run time (JIT engines, emulators, scripting VMs with JIT) | poor | the generated code never existed for the lifter to see; translate the interpreter path only, or emulate |
| Packed, encrypted or self-modifying binaries | poor until unpacked | dump the decrypted code first; anti-tamper checks may need the same treatment as licensing (section 7) |

### What decides how hard it is

1. **Function boundaries.** With symbols (as here), free. Without them, take the function list from IDA or Ghidra and
   accept a longer tail of misses found by testing.
2. **Indirect control flow.** Jump tables, vtables and function pointers are handled by a dispatcher over all entries
   (section 3), which is the same on every platform.
3. **Size of the outside world.** A few hundred imports (as here) take days to provide; a whole OS API or a GPU takes
   much longer. Count the imports first.
4. **Timing and hardware.** Apps that only call an OS barely notice that time passes differently. Console and embedded
   code that counts cycles or polls hardware needs a timing model.
5. **Memory model.** Translating x86 code (strong ordering) to ARM (weak ordering) needs barriers around shared data;
   ARM to ARM, or anything to x86, does not.
6. **Code you cannot translate.** Hand-written SIMD assembly, run-time generated code. Often it can be avoided
   (section 7, CPU feature detection) or replaced by a hand-written function (the override table, section 3).

### Choosing between the three ways to revive a binary

| Approach | Best when | Costs |
|---|---|---|
| **Dynamic translation or emulation** (QEMU user mode, box64, FEX, Rosetta 2, Android's ARM translation) | you need it running now; many different binaries; the code is generated at run time | slower; needs the translator on the target, and often none exists there (64-bit-only phones have no 32-bit ARM translator); per-app fixes are hard |
| **Static recompilation** (this playbook) | one fixed binary; native speed wanted; the target has no translator; you want every function checked against the original; you will port the platform layer anyway | per-binary work (days to weeks); a runtime of shims |
| **Source reconstruction** (decompiling and rewriting by hand) | you need readable, maintainable source to change the program deeply (gameplay, remasters, new platforms beyond reach of shims) | very large effort; error-prone where data layouts must be guessed; hard to prove equal to the original |

**The best path is usually a hybrid:** recompile statically first, so the whole program runs and every function has a
verified reference. Then replace the functions you want to change, or that are too slow, with reconstructed source,
and test each replacement against the translated version it replaces. The DH_sc project (Dungeon Hunter 2) did the
reconstruction first and spent most of its effort on layouts; N.O.V.A. 3 did the recompilation first and was playable
within days.

**Rule of thumb:** if the code is fixed, its boundaries can be found, and its outside world is a few hundred
functions, static recompilation is the fastest path to a native, trustworthy result, whatever the platform.

---

## 1. When to use this, and when not

Use it when:
- the app's code is a native library (`lib/armeabi-v7a/*.so`), and the Java/Kotlin side is thin (an activity, GL
  surface, input forwarding);
- the target device has no 32-bit support (most arm64 phones from 2023 on) or you want the code on another platform;
- correctness matters more than readable source.

Prefer something else when:
- the app is mostly Java/Kotlin (dex runs on arm64 already; only re-package);
- a source reconstruction already exists and links;
- the library is heavily self-modifying or JIT-based (rare in apps).

**Assess first** (an hour of work decides feasibility). Record each number in a `docs/` file:

| Check | Why | How |
|---|---|---|
| Is the `.so` stripped? | Symbols give every function's start and size; recovering function boundaries is the hardest part of static recompilation, and an unstripped library removes it. | `pyelftools`: count `STT_FUNC` symbols in `.symtab` |
| Instruction mix | The lifter only needs what is used. | Disassemble every function with capstone (code ranges from the `$a`/`$t`/`$d` mapping symbols), histogram the mnemonics |
| ARM vs Thumb | Thumb-2 adds IT blocks and mixed widths. | Mapping symbols `$t` vs `$a`; the low bit of function symbols |
| Imports | This is the runtime you must write. | `.dynsym` undefined symbols; group into libc/libm, GL, audio, pthread, other |
| JNI surface | The calls the Java side makes and the Java methods native code calls back. | Exported `Java_*` symbols, `JNI_OnLoad`; strings with JNI signatures `(…)…`; the dex (section 9) |
| Hand-written assembly | Untranslatable regions (often NEON). | Gaps between function symbols that contain code; untyped (`STT_NOTYPE`) labels |
| Data files | The app's assets (OBB, asset packs) must be provided at run time. | List the OBB/assets; note formats |

N.O.V.A. 3 example: unstripped (39,134 functions), ARM mode only, 93 instruction kinds, no Thumb, 336 imports (202
libc/libm, 100 GLES2, 29 pthread, 3 OpenSL ES), 57 JNI exports. That profile is close to ideal.

---

## 2. Architecture

```
original lib*.so ──► image loader ──► lifter (ARM → C, one C function per entry)
                                        │
                     ┌──────────────────┴──────────────────┐
             differential test                          builds
   (Unicorn runs the original; the translation      x86-64 test DLL (checked)
    runs in a DLL; compare registers and memory)    x86-64 host runner (Windows/Linux)
                                                     arm64 Android .so (optimized)
                                        │
                                   runtime/  (game memory, import shims, threads, JNI, GL, audio, platform layer)
```

Design decisions that make it work:

1. **Keep the original 32-bit memory layout.** Reserve a 4 GB region; a guest pointer is an offset into it
   (`host = base + guest32`). Load the ELF image at a fixed base (e.g. 0x01000000, so null pointers fault), apply its
   own relocations, put the heap, thread stacks and TLS inside the region. Every struct, vtable and pointer keeps its
   original size, so nothing about data layout has to be guessed. This single decision removes the main source of
   bugs in hand ports to 64-bit.
2. **One C function per entry, CPU state in a struct.** `void f_<addr>(cpu_t *c, uint8_t *m)`. Core registers become
   locals (loaded at entry, stored at exit); flags are locals; VFP registers live in the struct. Branches inside a
   function become `goto`; conditional instructions become `if`. Let the C compiler optimize.
3. **Calls are C calls.** Save the register locals to the struct, call, reload. Indirect calls and computed jumps go
   through one generated dispatcher (`switch` over every entry address, plus import stubs and kernel helpers).
4. **Imports are host functions.** Each import call becomes `rc_import(slot)`; the runtime implements it by reading
   arguments with the 32-bit ABI and translating pointers.
5. **Host functions callable from guest code.** Reserve an address range whose "calls" land in runtime C code. Use it
   for anything the guest calls through a pointer: JNI function tables, OpenSL ES interfaces, callbacks.
6. **Verify against the original, always.** The emulator is the specification.

---

## 3. The lifter

Write it in Python with capstone; generate C text.

**Read encoding bits yourself where capstone is unreliable.** On N.O.V.A. 3, capstone reported `update_flags` on a
plain `adc` and wrong post-index/writeback flags for some `ldrb`/`ldrh`/`strd` register forms. Decode S (bit 20) and
P/U/W (bits 24/23/21) from the instruction word.

Things that must be exact (each was a real bug found by the differential test):
- The shifter carry-out for logical S-instructions, including the rotated-immediate case (carry = bit 31 of the
  immediate when the rotation is non-zero).
- `STM`/`PUSH` that stores its own base register stores the value *before* writeback (`push {sp, lr}` in libgcc's
  `__aeabi_uldivmod`).
- PC reads as instruction address + 8 (ARM) or + 4 (Thumb).
- Float operations without fused multiply-add: compile with `-ffp-contract=off`; float-to-int conversion saturates
  and maps NaN to 0, as ARM does.
- Compile with `-fwrapv -fno-strict-aliasing`.

Control flow (milestone 2):
- **Entries.** Start from the symbol table's functions. Add every branch or call target that lands inside another
  function as its own entry (libgcc assembly has shared entry points) and repeat until no new ones appear. Size-0
  function symbols (libgcc's `__aeabi_*`) run to the next function.
- **Calls.** `BL` to an entry: direct C call. `BL` to the PLT: an import call; set `ip` to the GOT slot as the PLT
  stub would, so register state matches the original exactly. `BLX reg` and GCC 4.x's `mov lr, pc` followed by a write
  to `pc` (virtual calls): call through the dispatcher.
- **Tail calls.** `B` out of the function, or falling off its end: call the target and return.
- **Jump tables.** `add pc, pc, rX, lsl #2` followed by a row of `B` instructions, and `ldr pc, [pc, rX, lsl #2]`
  followed by an address table in a literal pool: emit a `switch` over **only the table's targets**. A switch over
  every label in a large function makes clang's compile time explode (an 8,824-instruction function never finished).
- **Non-local returns.** If a call comes back with a PC other than its return address, the callee has already
  returned for its caller (libgcc's `__muldf3` calls a helper with `bleq` that pops `__muldf3`'s own frame). The caller
  must return at once without writing its locals back; its own caller does the same check. A wrong translation still
  shows, because the outermost return address is compared.
- **Kernel user helpers.** `__kuser_cmpxchg` (0xffff0fc0), `__kuser_memory_barrier`, `__kuser_get_tls` are how old
  ARM code does atomics and TLS. Implement them in the dispatcher (with a real atomic compare-exchange).

Untranslatable code: give the lifter an **override table** (address → hand-written C body) for routines such as NEON
assembly. Often you can avoid them entirely (section 8).

---

## 4. Differential testing (the part that makes it trustworthy)

For each function, run the original in Unicorn (ARM32) and the translation in a host DLL, from the same memory
snapshot and the same random register state, and compare **all core registers, flags, FPSCR flags, all VFP registers
and every writable byte**.

- Build the whole library into one DLL so callees run translated too. Shard the generated C (≈50 files), compile in
  parallel, cache objects by a hash of their text plus the header plus the flags.
- Inputs: random register values biased towards pointers into a scratch area; fill the scratch area with words that
  are themselves pointers into it, so random structures can be followed a few levels deep.
- Imports: write small deterministic models (memcpy, strlen, malloc on a bump heap, math, mutexes…) and run the
  **same model code on both sides**, so the test checks only the translation.
- Count a function as passing only when enough inputs complete in Unicorn (e.g. 4); report "no coverage" otherwise.
- Build the test DLL "checked": bounds-check every guest access and cap loop iterations, so a wrong translation
  reports a fault instead of crashing the test.

**Check the checker.** Deliberately break the generated code (invert a carry, read the wrong register) and confirm
the run reports failures. Expect harness bugs that look like translation bugs; on N.O.V.A. 3 every one of these was
real:
- partial CPSR restore between runs let a run that executed garbage leave the big-endian bit set (`SETEND BE`):
  byte-swapped values in later runs;
- Unicorn's `mem_write` from a hook ignores page protection, so import models wrote into code pages and the writes
  persisted: code words off by one;
- the host side lacked memory the emulator had (import stub words);
- NaN sign: x86's default NaN is negative, ARM's and arm64's positive. Accept NaN against NaN.

Expected numbers: leaf functions pass almost all (8,339 of 9,330 translatable leaves on N.O.V.A. 3, none failing;
the rest lacked coverage). Functions with calls get less coverage from random inputs (788 of 3,000 passing, none
failing); real coverage comes from running the app.

---

## 5. The runtime

Split it into a platform-independent part and thin front ends (Windows/Linux host runner, Android activity).

**Game memory.** `VirtualAlloc`/`mmap` 4 GB reserved; commit the image, heap growth, stacks. A simple thread-safe
allocator inside it (size classes plus a large-block list) is enough.

**Calling conventions.** ARM softfp (armeabi-v7a): arguments in r0-r3 then the stack; doubles in an even/odd register
pair; float results in r0. A helper to call guest code from the runtime (`rt_call(fn, args…)`) is needed for callbacks
(`qsort` comparators, `pthread_once`, thread entry points, audio callbacks).

**libc with bionic's 32-bit layouts.** Data imports (`__sF`, `_ctype_`, `_tolower_tab_`, `__stack_chk_guard`,
OpenSL `SL_IID_*`) need real bytes in guest memory, and their GOT slots patched. Layouts that bit on N.O.V.A. 3:
- `FILE`: bionic's `fileno()` is a **macro** that reads `_file` (a short at offset 14). Apps size files with
  `fstat(fileno(f))`; put the host descriptor there.
- `struct stat` (104 bytes), `dirent`, `struct tm`, 4-byte `wchar_t`.
- printf/scanf families need a formatter that reads guest varargs and `va_list` (a pointer into the stack; 64-bit
  values 8-byte aligned).

**Threads.** Each guest thread is a host thread with its own CPU state. Map guest mutexes/conditions/semaphores
(4-byte bionic objects) to host objects through a table keyed by guest address, created on first use (static
initializers are just zero).

**GL.** Generate the GLES2 shims from a signature table. Special cases: vertex/index pointers are offsets when a
buffer is bound and guest pointers otherwise; `glShaderSource` takes an array of guest strings; `glGetString` must
return a guest address; `glMapBufferOES` needs a shadow buffer. On Windows, ANGLE provides GLES2 (Electron/CEF apps
ship `libEGL.dll`/`libGLESv2.dll`).

**JNI.** A small JVM: `JavaVM` and `JNIEnv` tables in guest memory whose entries are host functions; Java objects as
handles in an unmapped address range; an emulation of the Java methods the native code calls, logging every unknown
one once. That log is your to-do list. Watch for native code that calls back into Java to continue its own
initialization (N.O.V.A. 3's `init()` calls the Java `setupPaths()`, which calls the native `setPaths`).

**Android front end.** A `NativeActivity` (no Java at all) with `android_native_app_glue`: EGL on the window, input
via `AInputQueue`, the guest image shipped as an APK asset, app data in the app's external files folder (no storage
permission needed), the app's hard-coded `/data/data/<package>` paths aliased to the new package's folder. Build
everything with the NDK's clang; link against `liblog`, `libandroid`, `libEGL`, `libGLESv2`, `libaaudio`.

**Debugging aids worth building early:**
- a shadow call stack in checked builds (each translated function records its address by call depth) and a crash
  handler that prints the guest call chain with symbol names;
- log every unimplemented import and JNI call once;
- an offscreen mode (EGL pbuffer of an exact size, paced to real time) and scripted input (`--tap FRAME:X:Y`) with
  screenshots at given frames, for unattended runs;
- an audio capture to WAV.

---

## 6. Bring-up order

1. Constructors (`.init_array`), then `JNI_OnLoad`.
2. The Java side's startup calls, in the order the dex makes them (find it in the bytecode, section 9).
3. One frame, then many frames, then input, then audio.

At each crash: read the guest call chain, disassemble the function, find which global or import result it expected,
and supply what the platform would have.

---

## 7. Platform behaviour the app depended on (N.O.V.A. 3, likely in others of its era)

- **Touch codes.** Do not assume Android's `MotionEvent` codes. The game's Java side passed its own (1 down, 0 up,
  2 move); with Android's, every press arrived as "touch ended", so taps still worked and drags (stick, camera) did
  nothing. Read the native touch handler to see how codes map to began/moved/ended.
- **Screen sizes.** Old apps lay themselves out for resolutions they know. At 2418×1080 N.O.V.A. 3 drew at 40%
  and its touch zones were wrong; at 1280 wide (any aspect) it was correct. Render into a fixed-width buffer
  (`ANativeWindow_setBuffersGeometry`) and scale the touches; test sizes with the offscreen mode, not a desktop window
  (Windows shrinks windows larger than the monitor, which looks like an app bug).
- **Audio buffer queues.** OpenSL ES users may re-enqueue the same buffer while it is still queued, relying on the
  platform copying it out early. Copy at `Enqueue`, and reject enqueues beyond the locator's `numBuffers`. Symptom
  otherwise: distortion with discontinuities at your output block boundaries.
- **CPU feature detection.** Libraries choose NEON assembly paths from `/proc/cpuinfo`. If that assembly is not
  translated, give the app a cpuinfo for an ARMv7 CPU without NEON; it then uses its C paths, which are.
- **Licensing/DRM.** Old apps may refuse to start without their store's license answer. If the service no longer
  exists, set up the state a successful check leaves, using the values the app's own functions store, without
  patching code; document it. Expect deliberate traps (functions that fault unless the expected state exists).
- **Frame timing.** Splash screens and loading are often timed in seconds; a run without vsync does not advance.

---

## 8. Tooling notes

- zig (`pip install ziglang`) is a convenient clang for Windows DLLs and arm64 objects; it ships no Android libc
  headers, so use the NDK's clang for Android code that includes libc.
- The NDK, build-tools and a platform come from `sdkmanager` (now the "Android CLI"). The JDK's AF_UNIX loopback
  socket fails under long temp paths on Windows; set `-Djdk.net.unixdomain.tmpdir` to a short folder.
- A sandboxed or virtualized `AppData` can make files invisible to some processes; keep the SDK at a plain path.
- Expect long builds: let the object cache do its work, and run long jobs in the background.

---

## 9. Reading the Java side without a decompiler

A 150-line dex reader (string, type, proto and method tables, class data, and a scan of `invoke-*` and
`const-string` in each method body) is enough to list native methods, find who calls them, and see the order of the
startup calls and the strings they pass. No full decompilation is needed.

---

## 10. Checklist

1. Assess (section 1); write the numbers down.
2. Loader + lifter for leaf functions; differential test with mutation checks.
3. Calls, entries closure, jump tables, non-local returns, kernel helpers; whole library builds.
4. Runtime: memory, imports (log missing ones), threads, JNI; host runner; crash reports with the guest call chain.
5. Boot: constructors, `JNI_OnLoad`, startup calls in Java's order; first frame.
6. Input (check the app's codes), audio (copy at enqueue), resolution (fixed-width buffer).
7. Android front end, APK, device.
8. Write down every platform assumption you had to satisfy; that list is the most reusable output.
