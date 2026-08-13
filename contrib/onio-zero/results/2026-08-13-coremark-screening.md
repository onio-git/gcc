# ONiO.zero CoreMark follow-up screening — 2026-08-13

## Scope and baseline

This screening continued from GCC commit `6b53b974280438b49f44783753f92f90b4c00558`
and the preserved ws01 CoreMark baseline.  The explicitly stopped full
DejaGNU run was not restarted; only compiler selftests and named focused tests
were used.  Experiments used no more than four build jobs and used rv32sim and
mikrosim, never QEMU.

The baseline was rebuilt after first forcing a stale experimental `riscv.o`
out of the build tree.  The restored build matched the preserved ELF byte for
byte:

- SHA-256: `77baa9a36f4ce11a7225bbbd3f447b2a0ad5f3a309c2493b7c330fcbda953b28`
- text/data/BSS: 14,768/12/20 bytes
- modeled cycles: 197,051
- CoreMark/MHz: 5.075
- modeled CoreMark/mJ: 230.7
- fetch penalty: 7,324 cycles
- branch penalty: 5,388 cycles
- branch mispredictions: 666

The common build remained:

```text
make -j4 BUILD_DIR=<dir> \
  CC="<build>/gcc/xgcc -B<build>/gcc/" \
  SIZE=riscv32-unknown-elf-size \
  ARCH=rv32imc_zicsr_zba_zbb_zbs_zifencei ABI=ilp32 \
  CONFIG_OPTIMIZATION=-mcpu=onio-zero \
  TOTAL_DATA_SIZE=2000 ITERATIONS=1 CPU_FREQ_HZ=32000000
```

## Accepted compiler candidate

ONiO.zero previously set `loop-kernel-inline-growth-limit=8`.  Removing that
target default lets the ordinary inliner inline both calls to CoreMark's local
`iterate` dispatcher into `main`; section garbage collection then removes the
out-of-line dispatcher.  The general small-cache heuristic and its parameter
remain intact and can still be enabled explicitly.

ONiO.zero now also sets `iv-always-prune-cand-set-bound=5`, down from the
generic value of 10.  This limits unconditional candidate-set pruning and
retains better initial address inductions in the unrolled
`matrix_mul_vect` and `matrix_mul_matrix_bitextract` loops.  Relative to the
inline-only candidate, it removes 84 dynamic instructions and 32 more text
bytes.  The eliminated work includes three matrix-vector stack temporaries
and their associated moves and update chains.

Finally, ONiO.zero enables GCSE load-after-store elimination with the new
`gcse-las-generated-only=1` restriction.  Unrestricted load-after-store GCSE
extends user pointer live ranges and is severely unprofitable for the list
kernel.  The selective mode considers only compiler-generated, non-pointer
stores.  It therefore recovers the reaching values of loop-store-motion
counter temporaries in the state kernel without changing the list or main
translation units.  Relative to the IV candidate it removes four timed moves
and reduces modeled fetch cost by 60 cycles, for a 64-cycle gain at a cost of
eight text bytes.  The final image remains 64 bytes smaller than the preserved
baseline.

| Metric | Preserved baseline | New default | Change |
|---|---:|---:|---:|
| text bytes | 14,768 | 14,704 | -64 (-0.433%) |
| dynamic instructions | 184,339 | 184,241 | -98 (-0.053%) |
| modeled cycles | 197,051 | 196,736 | -315 (-0.160%) |
| CoreMark/MHz | 5.075 | 5.083 | +0.160% |
| modeled CoreMark/mJ | 230.7 | 231.0 | +0.160% |
| fetch penalty | 7,324 | 7,107 | -217 |
| branch penalty | 5,388 | 5,388 | unchanged |
| mispredictions | 666 | 666 | unchanged |

Candidate ELF SHA-256:
`b39a281daa94a30b2f2bcca2d00b2239bce65f7a78cd35ec3c68d856c6d86860`.
The target-default build reproduced the explicitly selected, generated-store
LAS prototype byte for byte.  The inline-only intermediate was 14,728 bytes
and 196,883 cycles; adding the IV bound produced the 14,696-byte,
196,800-cycle intermediate.

The combined improvement comes from fewer dynamic instructions, lower modeled
instruction-fetch cost, and smaller text, so it is directionally favorable
for both speed and flash-fetch energy.  Its magnitude remains below the
documented model calibration error; it is a board candidate, not a hardware
result.

### Correctness and focused validation

- GCC C selftests: 8,438,570 passes.
- Seven `gcc.target/riscv/onio-zero-*.c` tests: 158 PASS,
  1 UNSUPPORTED torture variant, 0 FAIL.  This includes 20 passes for
  `onio-zero-gcse-las-generated.c`, 20 passes for
  `onio-zero-inline-dispatcher.c`, and 40 passes for the new
  `onio-zero-iv-prune.c` regression.
- The two existing generic `-fgcse-las` regressions, `pr45101.c` and
  `pr45107.c`, both passed.
- `gcc.dg/preunroll-1.c`: 3 PASS, 0 FAIL.
- `gcc.dg/unroll-and-jam-reduction.c`: 3 PASS, 0 FAIL.
- mikrosim completed with `seedcrc=e9f5`, `crclist=e714`,
  `crcmatrix=1fd7`, `crcstate=8e3a`, and `crcfinal=e714`.  Its standard
  CoreMark harness reports the expected duration warning because this screen
  deliberately uses one iteration rather than a ten-second certification run.
- `-Q -O2 -mcpu=onio-zero` reports `-fgcse-las` enabled and
  `gcse-las-generated-only=1`; `--help=params` also reports
  `loop-kernel-inline-growth-limit=0` and
  `iv-always-prune-cand-set-bound=5`.  Generic RISC-V and `-Os` retain
  `-fno-gcse-las` and `gcse-las-generated-only=0`.  Explicit values of 8 and
  10 restore the respective old dispatcher and matrix-vector decisions, and
  explicit `-fno-gcse-las` suppresses the new target default.

## Rejected compiler candidates

All absolute cycle counts below are deterministic.  Unless a subsection names
an immediate comparison point, changes are against the 197,051-cycle baseline.
Rejected source experiments were fully removed.

### Exact branch layout

An experimental late-RTL profile keyed each conditional by source basename,
assembler function name, and post-layout ordinal.  This matched all 215
executed CoreMark branches and avoided the earlier GIMPLE key collisions, but
rerunning software-trace-cache layout with the exact probabilities was not
profitable.

| Profile selection | Text | Cycles | Regression |
|---|---:|---:|---:|
| all 215 executed conditionals | 14,920 | 208,241 | +5.68% |
| forward-taken, penalty >= 200 (6 branches) | 14,776 | 198,336 | +0.65% |
| forward-taken, penalty >= 100 (14 branches) | 14,808 | 199,481 | +1.23% |
| forward-taken, penalty >= 50 (25 branches) | 14,824 | 202,424 | +2.73% |
| forward-taken, penalty >= 20 (38 branches) | 14,824 | 205,114 | +4.09% |
| all 79 executed forward-taken branches | 14,840 | 205,146 | +4.11% |

Single-site attribution found that the four hottest duplicated
`core_bench_state` conditionals did not change layout.  The two isolated
`core_bench_list` changes regressed to 198,336 and 197,217 cycles.  The full
profile increased fetch penalty from 7,324 to 9,605 cycles and branch penalty
from 5,388 to 15,651 cycles.  The RTL hook and its generator were therefore
removed rather than retained as an attractive nuisance.

The earlier full GIMPLE profile also remains rejected: text fell to 13,920
bytes, but cycles rose to 249,833 (+26.79%).  Two isolated list keys produced
197,279 and 197,601 cycles.

### Inlining and loop transforms

- With the loop-kernel cap disabled, `max-inline-insns-auto` values from 100
  through 300 all produced the accepted 196,883-cycle ELF.  Values 60 through
  90 reduced text to 14,328 bytes but regressed to 198,005 cycles.
- Pre-unroll factors 3, 4, and 8 produced 200,023, 206,527, and 197,566 cycles.
  Per-source pre-unroll variants also regressed.
- Matrix unroll-and-jam factors 2 through 16 produced no winner; factor 8
  remained the unique best setting.
- Increasing or decreasing `max-unroll-times` did not change the ELF.
- `-O3`, loop splitting, path splitting, tracer, dynamic vector cost,
  stride versioning, and related generic loop switches either regressed or
  were byte-identical.  Representative results were 200,927 cycles for `-O3`,
  197,361 for loop splitting, 197,387 for path splitting, and 198,593 for
  tracer.

### Register allocation, propagation, and spill motion

- `-fira-algorithm=priority`: 198,161 cycles.
- IRA loop pressure: 197,524 cycles globally.  It saved 9 cycles in the list
  translation unit but lost 502 in `core_main`, so the per-file signal did not
  compose.
- LRA inheritance cutoff 20: 197,291 cycles.
- Disabling caller saves, hard-register copy propagation, register renaming,
  shrink wrapping, separated shrink wrapping, or web construction regressed;
  the worst of these was `-fno-web` at 204,839 cycles.
- Extending spill motion from innermost loops to all loops was byte-identical.
  Relaxing its unknown-store guards was neutral.  Both experiments were
  reverted.
- A deeper live-out-copy sink found one additional state-machine move but
  regressed to 198,465 cycles and was reverted.

### Addressing, costs, scheduling, and layout

- The IAR-style pointer/address induction experiment regressed by 0.676%.
  RV32 has no memory post-increment operation, and the attempted transformation
  lengthened lifetimes without removing enough address work.
- Disabling late combine regressed by 1.645%.
- Setting only the ONiO memory cost to 1 exposed an invalid, incoherent cost
  combination and triggered an IRA checking failure.  Setting memory and
  integer register-move costs coherently to 1 built 14,760 bytes but regressed
  to 197,487 cycles.  Register-move cost 1 alone built 14,784 bytes and
  regressed to 197,503 cycles.  All costs were restored.
- Branch cost 3 was neutral and branch cost 4 regressed to 198,907 cycles.
- Function/label/jump/loop alignment of four bytes regressed; two-byte
  alignment was byte-identical.
- Disabling the second scheduler regressed to 198,431 cycles.  Speculative
  scheduling and the other cache-aware scheduling toggles were neutral or
  regressed.
- Classification-probability confidence from 50% through 60% tied the
  baseline; 62% began to regress, and values at or above 70% lost as much as
  7.05%.  The existing exact classifier remains unchanged.

### Follow-up pass and IV screens

These screens used the inline-only 196,883-cycle candidate as their immediate
baseline.  Loop interchange, predictive commoning, store motion, loop
distribution, split-wide-types-early, and unroller variable-expansion limits
from 0 through 8 were byte-identical.  Other representative results were:

| Candidate | Text | Cycles | Regression versus 196,883 |
|---|---:|---:|---:|
| version loops for strides | 14,808 | 197,783 | +0.46% |
| IPA points-to analysis | 14,744 | 197,012 | +0.07% |
| unrestricted GCSE load-after-store | 14,736 | 205,740 | +4.50% |
| modulo scheduling | 14,728 | 197,125 | +0.12% |
| simple block reordering | 14,432 | 200,130 | +1.65% |
| block partitioning | 14,744 | 197,112 | +0.12% |
| Graphite identity/nest optimization | 14,736 | 196,905 | +0.01% |
| disable first scheduler | 14,736 | 201,444 | +2.32% |
| disable partial PRE | 14,640 | 197,111 | +0.12% |
| disable classification probabilities | 14,752 | 207,915 | +5.60% |

The IV pruning bound produced four distinct plateaus.  Values 4 and 5 tied at
the accepted 14,696 bytes and 196,800 cycles.  Values 6 through 9 produced
14,704 bytes and 196,868 cycles; the generic value 10 produced 14,728 bytes
and 196,883 cycles.  More aggressive values 2 and 3 regressed to 197,173 and
196,939 cycles.  Five was selected as the least aggressive value on the best
plateau.  Changing `iv-max-considered-uses` from 50 through 1,000 was byte
identical; lowering `iv-consider-all-candidates-bound` to 20 regressed to
197,249 cycles.

Against the 196,800-cycle IV candidate, reassociation width 1 was byte
identical and widths 2 through 4 regressed to 196,811 cycles.  More aggressive
tail sharing was especially unfavorable to the instruction cache:
`min-crossjump-insns` values 1, 2, and 3 reduced text to 14,392, 14,464, and
14,488 bytes but regressed to 207,470, 200,266, and 199,006 cycles.  Value 4
was byte-identical to the accepted default.

The initially rejected global GCSE load-after-store result hid one useful
translation unit.  Applying unrestricted LAS only to `core_state.c` produced
196,736 cycles, while applying it only to the pointer-chasing list unit
regressed to 204,492.  Restricting LAS to non-pointer stores made the list unit
byte-identical, but still changed user initialization stores in `core_main.c`;
that unit alone regressed to 198,139 through code placement and fetch cost.
The accepted generated-store restriction leaves both units unchanged and
retains the state-machine result.  The new parameter defaults to zero, so the
meaning of an explicit `-fgcse-las` is unchanged for other targets.

Predictor upper-bound experiments also rejected targeted branch-word padding:
removing every same-word compressed-branch conflict saved no cycles, while an
unrealistic 256-entry predictor saved only 46.  Allowing that predictor to
store forward branches saved only 245 cycles before accounting for the code
and cache cost required to change layout.

## Hardware acceptance requirement

The candidate must be flashed and measured against the preserved baseline on
the same Aldebaran board at controlled voltage, frequency, temperature, flash
state, and iteration count.  Archive raw current/voltage samples and both ELF
hashes.  Do not interpret the modeled 231.0 CoreMark/mJ value as a physical
energy measurement: the simulator uses a fixed 22 uW/MHz assumption, making
that metric proportional to modeled CoreMark/MHz.
