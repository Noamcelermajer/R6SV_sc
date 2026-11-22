# Differential test (tools/recomp/difftest.py, models.py)

Runs each original function in Unicorn (ARM or Thumb-2 state) and its translation in a host DLL from the same memory
and CPU state, then compares r0-r15, N/Z/C/V, FPSCR N/Z/C/V, d0-d31 and every writable byte (image data, scratch,
stack, heap). NaN matches any NaN. Method and harness bugs: `docs/RECOMP-PLAYBOOK.md` section 4.

## Commands (run from the repository root; `py -I` as the plan asks)

    py -I tools/recomp/difftest.py --sample 1000 --seed 1 --runs 16               # random sample, all functions
    py -I tools/recomp/difftest.py --mode thumb --kind leaf --sample 500           # Thumb leaves only
    py -I tools/recomp/difftest.py --only 297c9c _ZN11MenuControl13GetTextHeightEPKc   # addresses (hex, link address) or names
    py -I tools/recomp/difftest.py --mutate mixed --mode thumb --sample 150        # check the checker
    py -I tools/recomp/difftest.py --unicorn-only --mode thumb --sample 300        # originals only, no lifter, no build
    py -I tools/recomp/difftest.py --isa arm --sample 0                            # the 274 ARM-mode entries

Options: `--sample N` (0 = whole pool), `--seed S`, `--runs R` (inputs per function), `--kind leaf|calls|all`,
`--mode any|arm|thumb`, `--isa auto|arm|all` (`arm` keeps Thumb code away from the lifter: for an ARM-only lift.py),
`--only`, `--mutate KIND|OLD=>NEW`, `--unicorn-only`, `--out DIR` (default `build/difftest/out`), `--work DIR` (objects, DLL,
lift cache; default `build/difftest`), `--report FILE`, `--lift-dir DIR` (import another lift.py, e.g. a snapshot).

The image takes about 30 s to load, lifting about 90 s (cached in `build/difftest/lift-cache.pkl.gz`, keyed by the text
of lift.py and image.py), the first full build several minutes (48 shards, object files cached by content).

## Output

One summary line: `RESULT pass P fail F no-coverage N untranslatable U (library-wide U of E) untranslated-callee C`.
`untranslatable` counts entries the lifter refused over the whole library (they are not sampled; `--only` on one reports
the reason). Files in `--out`: `results.csv`, `summary.json`, and `report.json` with every failure: function, address,
mode, input (all registers, NZCV, FPSCR, d registers, seed, scratch variant, run), the first differing register or
address with both values, the normalised mnemonics (condition, `s`, `.w` stripped; IT forms collapsed into `it`) of the
function, plus `failing_mnemonic_clusters` (mnemonics ranked by their share of the failures) and
`mnemonics_in_every_failure`. Cluster by instruction kind from there.

Statuses: `pass` needs at least 4 completed runs (completed = the original ran to its return in Unicorn); `no-coverage`
otherwise (the reasons are counted in `summary.json`: random pointers fault, an unmodelled import such as fopen or
socket, ...). `untranslated-callee`: a callee the build does not contain.

## Thumb

- Thumb functions start with CPSR.T set and LR = sentinel|1, so `bx lr` stays in Thumb state. IT state is cleared in
  the CPSR written before every run and read back (the harness raises if it is not clean).
- CPSR is rewritten completely each run (mode bits from the first read; flags, Q, GE, IT, T, E set explicitly), so a
  `setend` or stray IT state in one run cannot reach the next.
- r15 is compared without bit 0 (the Thumb bit belongs to the return address). The translation runs first and its final
  PC bounds the original (a mid-function entry may return somewhere else); host-side import errors and Unicorn faults
  make the run incomplete, not a failure.
- The dispatcher matches `t & ~1`, so Thumb call targets work, and `rc_is_entry` is generated for lifters that use it.
- The test glue defines `rt_setjmp_prepare` (a static jmp_buf), because `longjmp` is not modelled: a run that reaches
  it is incomplete.
- `Cpu` in difftest.py mirrors `cpu_t`; the harness compares `sizeof` and the offsets of `fault`, `vfp`, `trace` with
  the DLL at start and stops with a message if recomp.h changed the layout.

## Models (models.py)

Same code runs on both sides. Added for this library: `__aeabi_idiv/uidiv/idivmod/uidivmod/ldivmod/uldivmod`
(truncating division, quotient in r0 (r0:r1), remainder in r1 (r2:r3), division by zero gives quotient 0 and remainder = the
dividend as libgcc's `__div0` path does), `__aeabi_dadd/dsub/drsub/dmul/ddiv` (IEEE, division by zero gives +-inf or NaN),
`__aeabi_d2iz/d2uiz/d2lz/d2ulz` (truncate, saturate, NaN to 0), `__aeabi_i2d/ui2d/l2d/ul2d/f2d/d2f`, single-precision
math (softfp float in a core register), `_Znwj/_Znaj/_ZdlPv/_ZdaPv` (bump allocator), the printf family with the EABI
varargs rules (64-bit values on even register slots, 8-aligned in a va_list), wide-character functions (4-byte wchar),
`__cxa_guard_*`, `div`, `strlcpy`, `strncat`, `strcoll`, `basename`, pthread key functions. No model (the run is
incomplete): file and socket calls, `longjmp`, `exit`, `abort`, `pthread_create/join`, `qsort`, `strtok`, `strtod`.
Math results are Python's, not bionic's; they only have to be equal on both sides.

## Mutation mode

`--mutate carry|reg|itcond|branch|mixed` picks sampled functions where the pattern applies and runs a copy with one
defect (`fm_<addr>`, compiled in one extra file; the cached shards stay valid): carry flag inverted (or the carry-in of
adc/sbc), a source register replaced, the condition of a predicated instruction removed, a conditional branch
inverted. The patterns match the lifter's output text (`cf = ...;`, `if (z) {`, `if (cf) { goto L_...; }`); if the
lifter changes its style, adjust `MUTATIONS`. The summary line `MUTATION caught X of Y covered mutants` is the check of the checker; mutants that are
equivalent for the inputs tried (a flag nobody reads) show as `pass`. `--mutate OLD=>NEW` replaces text in every translation
and rebuilds all shards.

## Results so far (this machine, lift.py as of this session)

- ARM mode, `--isa arm --sample 0 --runs 16` with the ARM-only lift.py from git HEAD: 275 entries, 128 pass, 0 fail,
  119 no coverage, 28 call Thumb functions that are not in an ARM-only build (reported `untranslated-callee`).
- Mutation on the ARM entries: 130 of 141 covered mutants caught.
- Thumb originals in Unicorn alone: 91 of 300 sampled Thumb functions complete at least 4 of 8 random runs; the others
  fault on random pointers (not on the Thumb entry path: invalid instruction in 7 of 2,400 runs).
- Thumb translations (lift.py with `ThumbLifter`): see the numbers appended below.

### Thumb numbers (lift.py with ThumbLifter, 23,171 of 23,171 entries translated, 16 runs per function, seed 7)

- `--mode thumb --sample 600`: 269 pass, 0 fail, 331 no coverage (531 with calls: 211 pass, 320 no coverage; 69 leaves: 58 pass, 11 no coverage).
- `--mode thumb --kind leaf --sample 150 --runs 12`: 116 pass, 0 fail, 34 no coverage.
- `--only nativeIsLeftTouchPad 27ec90`: 2 pass.
- `--mutate mixed --mode thumb --sample 150 --runs 12`: 59 of 71 covered mutants caught (carry 8 of 14, reg 37 of 40, branch 9 of 9,
  itcond 5 of 8); the misses are mutants with no effect on the inputs tried.
- Most no-coverage runs are random pointers faulting in Unicorn, then unmodelled imports (fopen, longjmp, socket).
  Real coverage of functions with calls has to come from running the game.
