# Loader and function discovery (tools/recomp/image.py, survey.py)

Owner: loader stream. Numbers are in `docs/ASSESSMENT.md` (regenerate with `py -I tools/recomp/survey.py`, about 45 s).

## What `Image(path)` gives you

Unchanged from NOVA3: `memory` (bytearray indexed by link address, relocated words already have `IMAGE_BASE` added),
`segments`, `writable`, `imports` (name -> stub address), `got_imports`, `plt`, `functions`, `by_address`, `code(f)`.

`Function(address, size, name, data_ranges, thumb)` plus defaulted extras:

| field | meaning |
|---|---|
| `address` | link address, Thumb bit cleared |
| `thumb` | 5th field, same meaning as in NOVAHD_sc (True: Thumb-2 code) |
| `data_ranges` | `(start, end)` ranges inside the function that are not instructions: literal pools, table data, alignment padding |
| `aliases` | other dynsym names for the same body (the shortest name with a size wins `name`) |
| `source` | `dynsym`, `exidx`, `init_array`, `call`, `data_pointer`, `pic_pointer` |
| `tables` | `{instruction address: JumpTable}` for the computed jumps in this function |

New on `Image`:

- `jump_tables`: `{address of the branching instruction: JumpTable}` for every function.
  `JumpTable(kind, insn, table, count, targets, end)`. `targets` are the case targets in table order (duplicates kept),
  Thumb bit cleared, all inside the function. The lifter should emit a `switch` over `set(targets)` and must not decode
  the table bytes again. Kinds:
  - `tbb` / `tbh`: `tbb [pc, rX]`; table starts at `insn + 4`; target = `insn + 4 + 2 * entry`. `count` comes from the
    `cmp rX, #N` before the guard branch (N+1 entries) when it is on the same register, otherwise from the first
    forward target. `end` is rounded up to a halfword.
  - `thumb_word`: `adr rB, table; ldr.w rT, [rB, rI, lsl #2]; add rB, rT; bx rB`. target = `table + word`, bit 0
    cleared (the word carries the Thumb bit). The `bx` is at `insn`.
  - `arm_add`: `add pc, pc, rX, lsl #2` followed by a row of `b`. `targets[0]` is the instruction after the add
    (executed when the add is conditional and falls through); the rest are the `b` rows. The rows are code, so
    `end == table` (no data).
  - `arm_ldr`: `ldr pc, [pc, rX, lsl #2]` followed by absolute addresses (not present in this library).
- `insn_size`: bytearray over `.text`; `insn_size[a - text_start]` is the size of the instruction starting at `a`
  (0 elsewhere). `is_insn(a)`, `instructions(f)`. Everything the descent did not reach is data by definition.
- `walks[address]`: the raw descent result per function (`reached`, `calls`, `literals`, `tables`, `ptrs`, `bad`,
  `indirect`, `falls_off`, `adrs`). `calls` holds direct call targets and branch targets outside the function (tail
  calls) as `(address, thumb)`.
- `inner_entries`: call or branch targets that land in the middle of another function (shared entry points); none of
  them is a function of its own. In this library there are 2 (`0x2bb366`, `0x2bb49c`).
- `init_array` `[(address, thumb)]` (412, all Thumb), `jni_exports` `{name: address}` (`JNI_OnLoad` and 33 `Java_*`),
  `text_start`, `text_end`, `in_text(a)`, `u16`, `u32`.
- `plt_range_set`, `reloc_pointers` (address of a relocated word -> link address it points to), `reloc_kinds`.

## How functions are found

1. Seeds: `.dynsym` STT_FUNC defined (21,265 bodies; Thumb bit from `st_value & 1`; size-0 symbols run to the next
   start), `.ARM.exidx` entries (the unwind table: 1,886 starts that have no dynsym symbol, i.e. static functions),
   `.init_array`. An exidx start inside a sized dynsym function is an inner entry, not a seed. exidx has no mode:
   halfword-aligned starts are Thumb, word-aligned ones are decoded both ways and the clean one wins (ARM only on a tie
   with an ARM prologue and more bytes covered).
2. Every seed is walked (recursive descent, below). Direct call targets and tail-branch targets outside every known
   function become new functions (`sub_<addr>`), as do `R_ARM_RELATIVE`/`ABS32` pointers in data that point into
   `.text` and PC-relative code pointers built with `ldr rX,=off; add rX,pc` or `movw/movt; add rX,pc`. A target inside
   a known function but not at its start goes to `inner_entries`.
3. Result for this library: 23,169 functions; no data pointer or PIC pointer was outside a known function; only 18 new
   functions (ARM long-branch veneers) came from call targets. `.text` is 99.57% inside functions, the rest is
   alignment padding plus one 16-byte constant block at `0x3b10e0`.

## How data inside functions is found

Recursive descent from the entry, one capstone `disasm` call per straight-line run (capstone only tracks IT-block
conditions inside one call, so instructions are never decoded one at a time):

- follows `b`, conditional `b`, `cbz`/`cbnz`, falls through IT blocks, stops at `bx lr`, `pop {..pc}`, `ldr pc,[sp],#4`,
  `mov pc,lr`, unconditional `b`, `tbb`/`tbh`, and calls to noreturn imports;
- a branch outside `[address, address+size)` is a tail call (recorded in `calls`);
- every `ldr`/`ldrb`/`ldrh`/`ldrd`/`vldr` with base `pc` marks its literal word(s) as data; a walk that ran into such a
  word is repeated with those bytes as barriers (this handles code that falls through a noreturn call into its pool);
- `adr`, `addw/subw rX, pc` and `add rX, pc, #n` targets are recorded in `walks[..].adrs` but are not barriers (they
  may point at code);
- everything the descent did not reach inside `[address, address+size)` becomes `data_ranges` (so nop/zero alignment
  padding is in there too).

Verification (in `survey.py`, all passing): every maximal instruction run is re-decoded independently and must agree on
every boundary and have no undecodable byte (0 disagreements over 1,098,619 instructions); all 301 jump tables have
every target on an instruction boundary and end exactly on an instruction start (300) or at the function end (1);
every literal word loaded by code lies in a data range; every byte of a data range is a literal, table data, adr
target, constant continuation, nop padding, or (one case) dead code after `pthread_exit`. Spot checks by hand:
`to_number` (tbb, 7 entries, matches `cmp r3,#6`), `OnUpdateSuccess` (tbh, 153 entries, matches `cmp r3,#0x98`),
`OnRequestSuccess` (thumb_word), `nativeStart`, `setBinary`, `CollideRay`, `ObjectivePointTransition` ctor and others
(pool words are PIC offsets loaded by the `ldr [pc]`/`add pc` pairs).

## PLT and relocations

`.plt` is a 20-byte PLT0 and 340 ARM-mode stubs `add ip,pc,#a; add ip,ip,#b; ldr pc,[ip,#c]!`; no Thumb veneers in
front. Thumb code calls them with `blx imm`. `Image.plt[addr] = (import name, GOT slot link address)` for all 340.
Relocation types in the file: `R_ARM_RELATIVE` 42,988, `R_ARM_ABS32` 4,120, `R_ARM_GLOB_DAT` 11, `R_ARM_JUMP_SLOT` 340;
all handled, none touches `.text`. 348 distinct imports (PLAN.md says 350); the 8 not behind a PLT stub are data
imports (`__sF`, `_ctype_`, ...). The loader raises if a PLT entry uses a GOT slot without an import relocation.

At the end of `.text` there are 18 ARM veneers (`ldr ip,[pc,#4]; add ip,pc,ip; bx ip; .word off`, 16 bytes each at
`0x56ad70`..`0x56ae80`). The loader treats `bx ip` after that sequence as a tail call to `pc + word`, so a veneer is a
Function whose only call is its target. They are reached from Thumb code with `blx`.

## Things the lifter author must know

- Thumb PC is `address + 4`; word loads use `align4(pc)` as base. The descent already resolved all literals; treat
  `data_ranges` as data, or use `walks[f.address].literals`.
- `jump_tables` replaces any decoding of `tbb`/`tbh` and of the `bx rB` of `thumb_word` tables in the lifter.
- Functions with a size from the symbol table may include their pool; functions found via exidx without size run to
  the next start, trimmed to the last instruction/literal/table.
- ARM-mode functions are `thumb=False`: 251 from dynsym at `0x53ef68`+ (the `_JNIEnv` wrappers and `ALicenseCheck`),
  5 more from exidx, 18 veneers.
- Dead code after calls to noreturn imports (`pthread_exit` in `_Z9threadfunPv`) is not decoded and sits in a data range.
- No NEON in the whole library; VFP only.
- Building `Image` takes about 40 s (Python walk over 23,169 functions); construct it once per process.

## Open points

- `tbb`/`tbh` count: when no `cmp` on the index register precedes the table, the bound comes from the first forward
  target. All 301 tables end on an instruction boundary, but a table whose first forward target is not at its end would
  be over-read; nothing in this library showed it.
- The mode of exidx-only word-aligned starts (5 ARM, 1,881 Thumb) is decided by the both-ways decode and checked only by
  the clean-decode criterion.
- The lifter files (`lift.py`, `difftest.py`) were not touched; they still need to read `Function.thumb`, `jump_tables`
  and `plt` as documented here.
