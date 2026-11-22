# The lifter (tools/recomp/lift.py, tools/recomp/runtime/recomp.h)

Owner: lifter stream. Translates every function of `librainbowsix.so` (23,171 entries: 22,897 Thumb including 2
inner entries, 274 ARM mode) into C on the model of the NOVA3 port: one C function `void f_<addr>(cpu_t *c, uint8_t *m)` per entry,
core registers `r0..r14` and the flags `n z cf v` as locals (loaded on entry, stored on exit), VFP state in `c->vfp`,
every instruction a labelled block `L_<addr>: { ... }`, branches inside the function are `goto`s.

## Interface (unchanged for the callers)

```python
from lift import Lifter, Unsupported, entries, branch_targets
ents = entries(image)           # {link address: Function}; Thumb and ARM; includes the loader's inner entries
text = Lifter(image, ents).lift(f)   # f.thumb selects ThumbLifter or the ARM translator; raises Unsupported
```

* `entries(image)` starts from `image.functions`, adds `image.inner_entries` (entries into the middle of a function, with
  the function's mode and data ranges) and then every call/branch target that leaves its function and is not an entry
  yet (a loop head that an inner entry jumps back to, for example): 23,171 entries, 2 of them inner
  (`World::Load+0x9a`, `+0x170`). The loader's `0x2bb366` is inside a literal pool (a false inner entry); it is dropped
  because it is data.
* `branch_targets(image, f)` yields `(address, target, is_call)` for both modes (difftest.py uses it).
* Nothing outside lift.py changed: the cache key of `build_common.translate` already covers lift.py.

## Conventions the generated code relies on (what runtime and test harness must know)

* **Thumb bit.** Return addresses created by Thumb code carry bit 0 (`r14 = (ret | 1)`); ARM code creates them without.
  A callee returns with `pcv = r14`, so the caller's `RC_AFTER_CALL(ret)` compares the exact value it stored. Anything that
  calls into guest code with `lr` set by hand (runtime `rt_call`, thread start) must pass a link register that the
  callee simply returns to: the runtime's sentinel without bit 0 works for both modes.
* **Dispatch.** Translated code reaches `rc_dispatch` only through `RC_DISPATCH(c, m, t)` = `rc_dispatch(c, m, t & ~1u)`;
  the generated dispatcher names functions by link address + IMAGE_BASE without the Thumb bit. A target with bit 0 set
  (Thumb function pointer, `bx`/`blx` register) is therefore accepted. The existing dispatcher needs no change.
* **PLT calls.** `bl`/`blx <plt stub>` becomes `r14 = ret; r12 = IMAGE_BASE + GOT slot; RC_SAVE(); rc_import(c, m, slot);
  RC_LOAD(); RC_AFTER_CALL(ret)`, i.e. `ip` holds what the PLT stub leaves (`add ip,pc,#a; add ip,ip,#b; ldr pc,[ip,#c]!`).
* **Non-local returns, tail calls, falling off the end**: as in NOVA3 (see `RC_AFTER_CALL`, `RC_TAIL`).
* **ARM-mode functions** may be called from Thumb (`blx`) and call Thumb (`blx <label>` in the ARM translator now lifts to
  a normal call with an ARM return address).

## Thumb translator: what was done and why

The Thumb translator decodes the encodings itself (ARMv7-A Thumb-2 + VFPv3-D32). Capstone is used only for the comment
at the end of each line and for the text of `Unsupported` messages. Reasons: IT blocks, flag setting, writeback bits and
modified-immediate carries are cheaper to get right from the bits than from capstone's detail (which on 16-bit encodings
reports `update_flags` as if the instruction were outside an IT block, and which loses the IT state when decoding one
instruction at a time).

* **IT blocks** are resolved statically while decoding the function (`ThumbLifter.decode`): the `it` halfword gives
  firstcond and mask, each following instruction (up to 4) gets its condition; the IT instruction itself emits nothing.
  Inside the block a 16-bit data-processing instruction does not set flags (`S = not in_it` for the encodings that set
  flags outside), a 32-bit one sets flags from its S bit. Conditions are evaluated on the live flag locals, so an
  instruction in the block that sets flags (32-bit `adds.w`, `cmp`) changes the conditions of the following ones exactly as
  in hardware. A conditional `b`, `bx lr`, `pop {pc}`, call or VFP instruction in a block is just `if (cond) { body }`.
* **Modified immediates** (`ThumbExpandImm_C`): the value and, for logical S instructions, the carry-out (bit 31 of the rotated
  value when imm12 >= 0x100 after the pattern forms, unchanged `cf` for the `00XY00XY`... patterns).
* **Shifts**: constant shifts are emitted as direct C expressions with their carry-out (`lsl #0` leaves C alone; `lsr/asr
  #0` in the encoding means 32; `ror #0` means `rrx`); register shifts use `shc` with the low byte of the register.
* **Flags** for `adds/subs/adcs/sbcs/rsbs/cmp/cmn` via `addc` (carry = not borrow). 16-bit `muls` sets N and Z only.
* **Loads/stores**: P/U/W taken from the bits (imm12, imm8 pre/post-index with writeback, negative offsets, register index with
  `lsl #0..3`, ldrd/strd with all three forms, sign-extending loads, literal loads). A load into the base register keeps the
  loaded value (writeback happens first). `ldr pc,[sp],#4`, `pop {..pc}`, `ldm sp!,{..pc}` are returns (`pcv = value; goto
  out`); any other write to pc goes through `RC_DISPATCH` and ends the function (tail call).
* **PC-relative literal loads** (`ldr/ldrb/ldrh/ldrsb/ldrsh/ldrd/vldr` with Rn = pc, `Align(PC,4)`): when the literal lies in
  `.text` the constant is folded into the instruction (`r3 = 0x00123456u;`); `adr`/`addw rX,pc`/`subw` are constants too.
  `add rX, pc` (16-bit) reads PC = address + 4 without alignment, as the hardware does.
* **Multiple transfers**: `ldm/stm` (ia/db), `push/pop`, with pc in the list, with the base in the list (a stored base is its
  value before writeback; a loaded base suppresses writeback). `vldm/vstm/vpush/vpop` for S and D registers.
* **tbb/tbh/thumb_word tables**: the loader's `Image.jump_tables` is used, nothing is decoded again. `tbb/tbh [pc, rX]` becomes
  `switch (rX)` with one `case` group per distinct target (an index outside the table faults with RC_FAULT_DISPATCH);
  the `adr; ldr.w; add; bx` form becomes a `switch` over the table's target addresses with a dispatcher default.
* **Wide integer ops**: movw/movt, addw/subw, ubfx/sbfx/bfi/bfc, clz, rbit, rev/rev16/revsh, uxt*/sxt* and the add forms
  (uxtab, sxtah..., rotation), mul/mla/mls, umull/smull/umlal/smlal/umaal, sdiv/udiv (ARM semantics: /0 = 0), smulxy,
  smlaxy, smulwy, smlawy, smuad/smusd/smlad/smlsd, smmul/smmla/smmls (+R), smlalxy, ssat/usat, qadd/qsub/qdadd/qdsub,
  pkhbt/pkhtb, ldrex/strex family. The Q flag and the GE flags are not modelled.
* **ldrex/strex/clrex**: the monitor is two locals of the function (`exa`, `exv`); `strex` is a real atomic compare-exchange
  (`rc_strex8/16/32/64` in recomp.h), status 0 on success, 1 when the address differs from the `ldrex` or the value changed.
  (Not used by the game code; present for completeness, not exercised by the Unicorn test.)
* **VFP** is decoded from the 32-bit word like the ARM encoding: vldr/vstr, vldm/vstm/vpush/vpop (S and D, 32 D registers),
  vmov (core<->S, two core<->D or two S, S<->S, D<->D, immediate), vadd/vsub/vmul/vdiv/vmla/vmls/vnmla/vnmls/vnmul (the
  multiply-accumulate forms round the product first: build with `-ffp-contract=off`), vabs/vneg (bit operations, NaN safe),
  vsqrt, vcmp/vcmpe (with a register or #0) and `vmrs APSR_nzcv, fpscr` / `vmrs rX, fpscr` / `vmsr`, vcvt f32<->f64,
  int->float (s32/u32), float->int (truncating, `vcvtr` rounds to nearest), saturating like the hardware with NaN = 0.
  Not translated: VFPv4 fused multiply-add, fixed-point vcvt, half precision, scalar `vmov.32`, fldmx/fstmx.
* **NEON**: the assessment found none in the library and the lifter does not translate NEON; any NEON or other coprocessor
  encoding raises `Unsupported("thumb NEON or coprocessor encoding ...")`. None of the 23,171 entries hits it.

## setjmp / longjmp

`bl/blx` to the PLT entry of `setjmp`, `_setjmp`, `sigsetjmp` or `__sigsetjmp` is emitted as

```c
r14 = <ret|1>; r12 = <got>; RC_SAVE(); RC_SETJMP();
```

with, in recomp.h (this is exactly the form runtime/libc.c and its unit test `test_setjmp` expect):

```c
void *rt_setjmp_prepare(struct cpu *c, uint32_t guest_jmp_buf);       /* runtime/libc.c */
#define RC_SETJMP() do { \
    if (setjmp(*(jmp_buf *)rt_setjmp_prepare(c, c->r[0])) == 0) { r0 = 0; } else { RC_LOAD(); } } while (0)
```

First return: `r0 = 0`, execution continues after the call with the locals untouched. A later `longjmp` (runtime) fills `c`
(r0 = value, r4-r11, sp, lr, d8-d15, r15 = lr), restores `c->depth` from the record made by `rt_setjmp_prepare`, and does the
host `longjmp` into this very C frame; the second return reloads every register local and the flags from `c` and goes on
after the call. The macro runs `setjmp` in the translated function's own frame, which is why the call is not an ordinary
`rc_import`. The runtime's `setjmp` import handler stays as a (logged) fallback for calls through a pointer. 8 functions
of the library call `setjmp` (`0x3909e0`, `0x47c0ac`, `0x47c374`, `0x47d604`, `0x4a9d10` and 3 more); all are Thumb.
Needs `rt_setjmp_prepare` at link time: the x86 test DLL of difftest.py must define it (a stub returning a static
`jmp_buf` is enough while no test function calls `setjmp`).

## Unsupported

Nothing in the library: all 23,171 entries lift. Anything the Thumb decoder does not know raises
`Unsupported("thumb <what> (<mnemonic>) at 0x...")`; the caller groups by the text before ` at `. The things that raise:
NEON and other coprocessor encodings, VFPv4 fma, fixed-point vcvt, parallel add/subtract (`uadd8`...), `sel`, `usad8`, `sxtb16/uxtb16`,
`ssat16/usat16`, dual 16-bit multiplies (smlald/smlsld), `mrs/msr/cps`, `svc/udf/bkpt`, `srs/rfe`, `ldrt` family is treated as the
plain access. None occur.

## Verification

Both checks use Unicorn (Thumb or ARM, picked by `Function.thumb`) against the translation built with zig for x86-64
(`-DRECOMP_CHECKED -fwrapv -fno-strict-aliasing -ffp-contract=off -O1`), same memory snapshot, same random registers,
flags, FPSCR NZCV and D registers; they compare r0-r14, pc, NZCV, FPSCR NZCV, d0-d31 and every writable byte (NaN against NaN
accepted: x86's default NaN is negative). They are `tools/recomp/lift_check_insn.py` and `tools/recomp/lift_check_lib.py` (work files under `build/lifter/`):

* `lift_check_insn.py --seed S --count N [--runs R] [--it share] [--only mnemonics] [--words h1:h2,...]`: random Thumb encodings (16 and 32 bit, 25-30% wrapped in random IT blocks of 1-4
  instructions), each as `[it] insn...; bx lr`.
* `lift_check_lib.py build` then `lift_check_lib.py run --sample N --kind leaf|calls|all [--arm] [--only addr...]`: the whole library in one
  DLL, sampled functions from the real code with the imports modelled by `models.py` on both sides.

Gotchas found on the way (all harness issues, kept here for whoever rewrites difftest): Unicorn lets a random store hit the code
page it executes from, which corrupts every later case (map the image read+exec only); a random register can point
into the sentinel page the Unicorn side has mapped, so the host memory must cover it; `ldrd/strd` with writeback and
`rt == rn` is unpredictable; a random 16-bit word can be an `it` that swallows the `bx lr` that ends the test.

Results are in the section "Results" at the end of this file.

## Results (all produced by the commands above on this machine)

**Lifting** (`Lifter(image, entries(image)).lift(f)` over every entry, about 10-30 s):

| | entries | lifted | Unsupported |
|---|---:|---:|---:|
| Thumb (incl. 2 inner entries) | 22,897 | 22,897 | 0 |
| ARM mode | 274 | 274 | 0 |

Table of `Unsupported` reasons by count: empty (0 entries raise). 8 entries call `setjmp` (inline `RC_SETJMP`), 258 contain a
`tbb`/`tbh` switch, 21 a `thumb_word` switch, 4,297 contain an IT block; no ldrex/strex, no NEON.

**Compiles**: the C of all 23,171 entries (131 MB, 64 shards + dispatcher) compiles and links with zig
(`x86_64-windows-gnu -O1 -DRECOMP_CHECKED`, 438 s on a busy 8-core machine, no error); 200 of them including two `setjmp`
callers also compile with `aarch64-linux-musl` and `aarch64-linux-gnu` at `-O2`.

**Instruction fuzz vs Unicorn** (`lift_check_insn.py`): seed 2, 3000 random encodings (2,655 lifted; the rest are NEON, VFPv4,
parallel add, `ldrex`... and raise `Unsupported`), 4 runs each from random state: 2,324 cases completed in Unicorn
and matched, 0 differences (the other 331 never completed in Unicorn: random addresses fault). Earlier seeds produced only
harness bugs listed above. Check of the checker: `--mutate "cf = co;=>cf = !co;"` makes 43 of 447 cases fail, as it must.

**Real functions vs Unicorn** (`lift_check_lib.py`, imports modelled by models.py on both sides, 8 random states each):

| sample | pass | fail | no coverage |
|---|---:|---:|---:|
| 600 random Thumb leaf functions (no calls) | 438 | 0 | 162 |
| 600 random Thumb functions with calls | 156 | 0 | 444 |
| all 274 ARM-mode functions | 113 | 0 | 161 |

"No coverage" = fewer than 4 of the 8 random states completed in Unicorn (faults, unmodelled import, loop limit); as in NOVA3
the functions with calls get less coverage from random inputs. Larger runs (5,000 leaf, 5,000 call functions) were started but
starved by other jobs on the machine and not finished, so they are not counted.

**Not verified by a comparison**: ldrex/strex/clrex (compiled, never executed; the library has none), the `setjmp` resume path
(only the macro's compilation; runtime/libc.c's `test_setjmp` exercises the runtime half by hand), real `tbh` tables beyond the
sampled functions, VFP `vcvtr` with a non-default FPSCR rounding mode (not modelled: rounds to nearest), FPSCR cumulative flags
and the Q/GE flags (not modelled).
