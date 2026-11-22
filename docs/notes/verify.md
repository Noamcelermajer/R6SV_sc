# Verification, Thumb-2 round 1 (lift.py, difftest.py, models.py, image.py, recomp.h)

Date 2026-11-21. Seed 1001, 16 inputs per function, library `librainbowsix.so`. Every number below comes from a run
made in this round; logs and reports are in `build/verify/` (`leaf/`, `calls/`, `mut/`, `armleaf/`, `*.log`).
No source file was changed: no lifter bug and no harness bug turned up, so there are no bug entries (one entry per bug
is the format for the next rounds).

## 1. Lifting

`build/verify/liftall.py` lifts all 23,171 entries (22,897 Thumb, 274 ARM): 23,171 lifted, 0 Unsupported, 0 other
exceptions (loading 32 s, lifting 20 s). Nothing to cluster, and no sign of data decoded as code: the only
"Invalid instruction" in Unicorn were 141 of about 41,000 runs of tiny functions that call random virtual-call targets.

## 2. Differential test, seed 1001

| run | pool | tested | pass | fail | no coverage |
|---|---:|---:|---:|---:|---:|
| `--mode thumb --kind leaf --sample 3000` | 2,591 (all Thumb leaves in the library) | 2,591 | 2,041 | 0 | 550 |
| `--mode arm --kind leaf` | 1 (the only ARM leaf) | 1 | 0 | 0 | 1 |
| `--mode any --kind calls --sample 3000` | 20,579 | 3,000 | 1,250 | 0 | 1,750 |

* The library has only 2,592 functions without calls, so "3,000 leaves" cannot be met; the whole leaf pool was tested.
  Functions with calls: 3,000 sampled (the request was 1,000) because random inputs complete in Unicorn for only 42 %.
* No-coverage reasons (leaf run, counted in runs): invalid memory read 6,840, write 724, fetch 474, invalid instruction
  141, unmodelled import (`__cxa_pure_virtual`, `abort`) 9. They are random pointers faulting in Unicorn, not
  translation differences. Functions with fewer than 4 completed runs are untested; they are listed in
  `results.csv` with their reason and none was excluded by hand.
* 0 failures, so there was nothing to prove against single-stepping.

## 3. Mutation check

`--mutate mixed --mode thumb --sample 150 --runs 12 --seed 1001`: 62 of 74 covered mutants caught (76 more without
coverage). By kind: carry 13 caught / 9 passed, reg 36 / 1, itcond 2 / 2, branch 11 / 0. The 12 that pass are mutants
whose effect the inputs never observe (a flag no later instruction reads, a predicated instruction whose condition
always holds for the inputs). So the harness reports failures for broken code.

## 4. Whole-library compile (cold, empty work directories)

| target | objects | time | result |
|---|---:|---:|---|
| x86_64-windows-gnu, `-O1 -DRECOMP_CHECKED`, variant `runtime` (host runner) | 48 shards + dispatcher | 476 s | no error |
| aarch64-linux-musl, `-O2 -fPIC`, variant `runtime` (Android) | 48 shards + dispatcher | 402 s | no error |

Both on 8 cores shared with other jobs, 131 MB of C (130,867,359 characters). Shards are address-interleaved, objects
are cached by content hash, so a rebuild after a lifter change recompiles only changed shards (the full cache hit takes
about 30 s). No duplicate labels and no function too large for the compiler (the biggest generated function is below 2 MB
of C). The final executable/shared object was not linked in this round (it needs the runtime streams' files).

## Open points

* Coverage of functions with calls is 42 %; real coverage has to come from running the game with the checked build.
* Not exercised by any comparison: ldrex/strex, the `setjmp` resume path, FPSCR rounding modes, Q/GE flags
  (unchanged from `lifter.md`).

# Verification, Thumb-2 round 2 (seed 1002)

Date 2026-11-22. Logs and reports in `build/verify2/` (`liftall.log`, `leaf/`, `calls/`, `mut/`, `cold-x86.log`,
`cold-arm64.log`). No source file was changed in this round: again no lifter bug, harness bug or compile problem
turned up, so there are no per-bug entries. Every number below is from a run of this round.

* Lifting (`liftall.py`, all entries): 23,171 of 23,171 lifted, 0 Unsupported, 0 other exceptions (load 34 s, lift 20 s).
  Nothing to fix in discovery.
* `--mode thumb --kind leaf --sample 3000 --seed 1002 --runs 16`: the library has only 2,591 Thumb leaves, so the whole
  pool was tested: pass 2,029, fail 0, no coverage 562 (random pointers fault in Unicorn, unmodelled imports).
* `--mode any --kind calls --sample 3000 --seed 1002 --runs 16`: pool 20,579, tested 3,000: pass 1,238, fail 0,
  no coverage 1,762. Untranslatable 0, untranslated callee 0. Nothing excluded by hand.
* Mutation `--mutate mixed --mode thumb --sample 150 --runs 12 --seed 1002`: 61 of 72 covered mutants caught
  (carry 10 caught / 5 passed, reg 38 / 1, itcond 2 / 4, branch 11 / 1; 78 without coverage). The 11 that pass are
  mutants no input observes. The harness reports failures for broken code.
* Whole-library cold compile (empty work dirs, 49 objects = 48 shards + dispatcher, 130,867,359 characters of C, variant
  `runtime`, no errors, no duplicate labels, no oversized function): x86_64-windows-gnu `-O1 -DRECOMP_CHECKED` 189 s;
  aarch64-linux-musl `-O2 -fPIC` 198 s (lift came from the content cache; a cold lift adds about 55 s). Machine was less
  loaded than in round 1 (476 s / 402 s).
* Still unexercised by comparison: ldrex/strex, the setjmp resume path, FPSCR rounding modes, Q/GE flags. Coverage of
  functions with calls is 41 %; the rest has to come from running the game.
