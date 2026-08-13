# ONiO.zero DejaGNU triage — 2026-08-13

This record triages the completed full GCC DejaGNU run against
`generic-sim/-mcpu=onio-zero`, identifies which failures are real, and
introduces a relevant RV32IMC/ONiO.zero gate.  It supplements, and does not
replace, `review-2026-08-09.md`.

The full run was not restarted and its result files were not modified.

## Inputs

- Run directory:
  `/home/veba/work/git/toolchain-src/build-gcc/gcc/testsuite-full-ws01-j8`
- Combined summary: `.../gcc/gcc.sum` (31 MB)
- Combined log: `.../gcc/gcc.log` (127 MB)
- Compiler: `xgcc (GCC) 17.0.0 20260602 (experimental)`, `riscv32-unknown-elf`,
  configured `--with-arch=rv32imc_zicsr_zba_zbb_zbs_zifencei --with-abi=ilp32`.
- Every test was compiled with a global `-mcpu=onio-zero`.
- Board: `generic-sim`, `DEJAGNU_SIM_OPTIONS='--no-gdb --run --fast --timeout=30'`.

Raw totals: 254,307 expected passes, 10,620 unexpected failures, 3 unexpected
successes, 1,230 expected failures, 153 unresolved, 22,712 unsupported.

## Method

`contrib/onio-zero/dejagnu-triage.py` opens both files read-only.  `gcc.sum`
supplies the canonical result lines; `gcc.log` supplies the evidence needed to
separate a simulator timeout from wrong code, an ICE from a scan mismatch, and
a missing toolchain component from a real defect.  Each result line in the log
is preceded by the commands and program output that produced it, so the log is
streamed once and every failing result keeps the block of lines that led to it.
Categories are assigned by the first matching rule, most specific first, so a
test that both targets another architecture and times out is counted once.

```sh
python3 contrib/onio-zero/dejagnu-triage.py \
  --sum  .../testsuite-full-ws01-j8/gcc/gcc.sum \
  --log  .../testsuite-full-ws01-j8/gcc/gcc.log \
  --srcdir gcc/testsuite --json triage.json
```

The tool triages 10,781 lines: the 10,620 unexpected failures plus the 153
unresolved testcases and 8 harness errors, which need the same explanation.

## Category counts

| Category | Lines | Distinct sources | Representative test |
|---|---:|---:|---|
| Vector (RVV) ISA absent from rv32imc | 7,080 | 489 | `gcc.target/riscv/rvv/autovec/sat/*`, `gcc.dg/vect/costmodel/riscv/rvv/pr113112-1.c` |
| Simulator timeout | 1,131 | 267 | `gcc.c-torture/execute/pr36765.c` |
| gcov/profile harness expectations | 1,113 | 28 | `gcc.misc-tests/gcov-4b.c` |
| Incompatible ISA/ABI or global `-mcpu=onio-zero` scan mismatch | 752 | 109 | `gcc.target/riscv/pr123278.c`, `gcc.dg/ifcvt-5.c` |
| Simulator memory-bound failure | 239 | 136 | `gcc.c-torture/execute/20030209-1.c` |
| Missing optional language, offload target, or component | 218 | 54 | `gcc.target/riscv/sched1-spills/spill1.cpp` |
| Unexplained | 180 | 56 | `gcc.dg/analyzer/mkostemp-1.c` |
| Execution wrong code, abort, or unexpected exit | 43 | 5 | `gcc.dg/torture/tls/run-ie.c` |
| Unresolved harness or environment failure | 25 | 9 | `gcc.dg/torture/bitint-37.c` |
| **Total** | **10,781** | | |

Two categories the handover asked for are empty, which is itself a result:

- **Compiler ICEs or assertions: 0.**  The scheduler assertion recorded in the
  2026-08-10 continuation no longer appears anywhere in the run.
- **Unsupported or unimplemented instructions: 0.**  The simulator did not
  reject a single instruction the compiler emitted.
- **Unrelated target-architecture tests: 0 failures.**  The `gcc.target/<arch>`
  suites for other architectures are selected but resolve to UNSUPPORTED rather
  than FAIL, so they inflate the unsupported count, not the failure count.

The raw 10,620 is therefore dominated by tests that cannot pass in this
configuration: 7,080 lines need a vector unit the ONiO.zero core does not have,
and 752 more need an ISA or ABI (mostly rv64/`lp64d`) this compiler is not
configured for, failing with `cc1: error: ABI requires '-march=rv32'` or
`Cannot find suitable multilib set`.

## Confirmed defect: unimplemented semihosting syscalls hang the program

**Category:** simulator timeout (1,131 lines, 267 distinct sources) and, in
part, simulator memory-bound failures.

**Responsible component:** `rv32sim.py`.

**Minimal reproducer:**

```c
int main (void) { __builtin_printf ("hi\n"); return 0; }
```

Compiled with `-mcpu=onio-zero -O2` and run under
`mikrosim --no-gdb --run --fast --timeout=20`, this program never terminates.

**Root cause.**  newlib's `printf` calls `fstat(1, ...)` to choose its
buffering before it writes anything.  libgloss issues that as `ECALL` with
`a7 = 80`.  The simulator implemented exactly two syscalls, `SYS_write` (64)
and `SYS_exit` (93), and raised an architectural ECALL trap for everything
else.  A bare-metal test program installs no trap handler, so the trap vectors
to an empty `mtvec` and the program spins until the harness timeout.  Any test
that produces output through stdio therefore consumed the full 30-second
budget and was recorded as a failure.

**Fix.**  `siminfra/isa/rv32_exec_scalar.py` now recognizes the complete set of
eighteen syscall numbers libgloss can emit and services them:

- `exit` (93) and `exit_group` (94) halt with the program's status;
- `write` (64) routes fd 1 to stdout and fd 2 to stderr;
- `read` (63) reports end of file on stdin;
- `close` (57) succeeds for the standard descriptors;
- `lseek` (62) reports `ESPIPE`;
- `fstat` (80) fills libgloss's `struct kernel_stat` describing a character
  device, which keeps newlib's stdio unbuffered so output appears in the order
  the program wrote it;
- every other recognized number returns `-ENOSYS`.

A syscall number outside that set keeps its architectural ECALL trap.  That
boundary matters: `rv32mi-p-scall` and the riscv-tests suite deliberately
execute `ecall` with an arbitrary `a7` and require the trap.  An earlier,
broader version of this fix that answered every ECALL broke 13 privilege,
nested-trap and compliance tests; the allowlist is what keeps both behaviours
correct.

`brk` (214) is recognized but returns `-ENOSYS`.  This toolchain's libgloss
uses the `_end`-based `_sbrk` that needs no syscall, so `malloc` works; a
libgloss built for the QEMU syscall path would need a real program break.  This
is a deliberate, documented limitation rather than an oversight.

**Regression tests:** `tests/test_cpu_core.py` gains coverage for the ENOSYS
path, the `fstat` character-device result and its `EBADF` case, `read`,
`close`, `lseek`, `write` to stderr and to a bad descriptor, and `exit_group`.
Two pre-existing tests encoded the old contract and were updated: a bad
descriptor for `write` now yields `-EBADF` rather than `-1`, and
`test_ecall_unknown_causes` now selects `semihosting_enabled=False` to keep
exercising privilege-dependent trap causes.

**Effect on the reproducers:** `gcc.c-torture/execute/pr36765.c`,
`gcc.c-torture/execute/pr79286.c` and `gcc.dg/pr84503-2.c` each ran to a clean
exit 0 after the fix, having previously exhausted a 240-second timeout.

## Confirmed harness limit: the default 128 KiB region

**Category:** simulator memory-bound failures (239 lines, 136 sources).

238 of the 239 diagnostics are `Memory write out of range at 0x00020000`, which
is exactly the ceiling of the simulator's default `flash` region
(`0x0`–`0x20000`).  DejaGNU links test programs at `0x10000` and lets newlib's
heap grow upward from `_end`, so any test needing more than the remainder of
that region writes past the end.

This is board configuration, not a simulator defect: adding
`--mem-region=0x20000:0x7E0000:heap` makes `gcc.c-torture/execute/20030209-1.c`
run to exit 0 unchanged.  The gate below sets it.

## Investigated and attributed: `gcc.target/riscv/ext-dce-4.c`

This was the only unexplained failure with a genuine code-generation
difference, so it was reproduced individually as the handover requires.

The test's negative case expects a sign-extending halfword load:

```c
int test_half_sign_needed (signed short *p) { return *p >> 8; }
```

The compiler emits `lb a0,1(a0)`, not `lh`.  The generated code is correct: on
little-endian rv32, byte 1 of the halfword is its high byte, and a
sign-extending byte load of it is exactly `(int)*p >> 8`.

It is **not** an ONiO regression.  `-mcpu=onio-zero`, `-mtune=generic`, and no
tune option at all produce the identical instruction, so the tune is not
involved.  The narrowing comes from commit `4a97237ac39` ("ext-dce: narrow
sign-extending loads to zero-extending when upper bits are dead") by an
upstream author, whose own test expectation this tree carries.  Per the
handover's instruction not to fix unrelated failures it is left alone and
recorded here; it is a stale upstream test expectation, not wrong code.

## Remaining unexplained failures

180 lines across 56 sources remain unexplained by an automatic rule.  Each was
inspected; none is an ONiO.zero code-generation defect.  They fall into
environment gaps that a bare-metal `--enable-languages=c` toolchain cannot
satisfy:

| Cause | Example | Evidence |
|---|---|---|
| No C++ compiler | `gcc.target/riscv/sched1-spills/spill1.cpp` | `C++ compiler not installed on this system` |
| No TLS runtime | `gcc.dg/torture/tls/run-gd.c` | `undefined reference to '__tls_get_addr'` |
| No libatomic | `gcc.dg/torture/bitint-95.c` | `undefined reference to '__atomic_fetch_sub_4'` |
| No gcov runtime | `gcc.dg/20020201-1.c` | `undefined reference to '__gcov_exit'` |
| No stack-protector runtime | `gcc.dg/ssp-1.c` | `ld returned 1 exit status` |
| No F registers in rv32imc | `gcc.dg/asm-hard-reg-4.c` | `register fa5 ... isn't suitable for data type` |
| Target defines no speculation barrier | `c-c++-common/spec-barrier-1.c` | compiler warning by design |
| Analyzer/target-specific expectations | `gcc.dg/analyzer/mkostemp-1.c`, `gcc.dg/ipa/pr122458.c` | diagnostic text differs on this target |

The 1,113 gcov lines have the same character: `gcc.misc-tests` needs a gcov
runtime and a filesystem to write `.gcda` files, neither of which exists here.

The 43 execution failures across 5 sources are all TLS tests
(`gcc.dg/tls/*`, `gcc.dg/torture/tls/*`) exiting 1 because bare-metal newlib
has no thread-local storage runtime.

Nothing in the full run is currently attributable to the ONiO.zero
optimizations.  All six ONiO-specific regression tests pass.

## The RV32IMC/ONiO.zero gate

`contrib/onio-zero/onio-dejagnu-gate.sh` runs the suites whose results actually
depend on RV32IMC code generation and on the simulator:

- `gcc.target/riscv/riscv.exp` — RISC-V target code generation.  This matches
  only `gcc.target/riscv/*.c`, so the RVV suites under `rvv/` stay out by
  construction rather than by an exclusion list.
- `gcc.c-torture/execute/execute.exp` — execution correctness, which is what
  exercises both the generated code and the simulator.

It sets the board options the triage established:

```sh
DEJAGNU_SIM_OPTIONS='--no-gdb --run --fast --timeout=60 --mem-region=0x20000:0x7E0000:heap'
```

Unrelated noise is excluded by **comparison, not by name**: the gate records
every `FAIL`, `UNRESOLVED`, `ERROR` and `XPASS` line in
`onio-dejagnu-gate-baseline.txt` and fails only when a line appears that the
baseline does not contain.  A test that is known-unsupported for ISA reasons
stays visible in the baseline, so a genuine RISC-V regression in the same file
is still reported rather than hidden behind a directory-level exclusion.  The
gate also prints tests that stopped failing, so the baseline cannot silently
drift.

```sh
source /home/veba/work/onio-zero-dev.env
contrib/onio-zero/onio-dejagnu-gate.sh                    # check
contrib/onio-zero/onio-dejagnu-gate.sh --update-baseline  # re-record
```

### The recorded baseline

`onio-dejagnu-gate-baseline.txt` holds 1,062 entries — 1,014 `FAIL` and 48
`UNRESOLVED` — recorded at commit `9e3ea11a539` with a clean working tree, as
its `.provenance` file records.  The full run:

| Suite | Passes | Failures | Unresolved | Unsupported |
|---|---:|---:|---:|---:|
| `gcc.target/riscv/riscv.exp` | 31,682 | 872 | 48 | 7,398 |
| `gcc.c-torture/execute` | 24,294 | 142 | 0 | 110 |

The execution suite passes 24,294 of 24,436 attempted, a 99.4% rate, which is
the useful number here: it exercises generated code and the simulator
together.  The `riscv.exp` failures are dominated by tests for hardware this
configuration does not have — `xtheadfmv-fmv.c` (48), `zbb-min-max-04.c` (36),
`zba-shNadd-07.c` (36), the `zicond`/`czero` group, and `arch-unset-5.c` (18,
rv64 ABI).  They stay visible in the baseline rather than being excluded by
directory, so a real regression in the same files is still reported.

An earlier attempt on 2026-08-13 was discarded: another session relinked `cc1`
twice while it ran (13:27 and 14:57), so it spanned three compilers.  That is
what the compiler-identity guard now prevents.  One claim made from that run
should be corrected — `riscv.exp` was described as having "completed with 25
failures out of 4,820 result lines"; it had not completed.  The suite runs to
roughly 40,000 result lines and 872 failures, and the low early count was
simply progress, not a result.

Two findings from the discarded run do hold, and the completed run confirms
them:

- **Zero simulator timeouts and zero memory-range errors**, against a full
  DejaGNU run in which those were the two largest failure causes.  That is the
  confirmation at scale that the semihosting fix and the `--mem-region` board
  option work.
- The execution failures are pre-existing and fail at `-O0`, so none is
  attributable to the ONiO optimizations.  Two are explained: `920501-8.c`
  needs `%f`, which newlib-nano omits unless linked with `-u _printf_float`
  (the test declares exactly that under a `newlib_nano_io` effective target
  this board does not claim), and `memcpy-1.c`/`memcpy-2.c` allocate two
  128 KiB stack buffers because the board sets no `STACK_SIZE`, then exceed
  the time limit on simulation throughput rather than on correctness.

## Relationship to the CoreMark follow-up screening

`results/2026-08-13-coremark-screening.md` records a parallel tuning effort
that ran against this same worktree while this triage was in progress.  The
two interact in three ways that matter for reading either document.

**The CoreMark reference point moved.**  Everything else in this report refers
to the preserved ws01 baseline, 197,051 modeled cycles at SHA-256
`77baa9a3…`, because that is what the changes described here were validated
against, and none of them alter code generation.  The screening then changed
three target defaults — dropping `loop-kernel-inline-growth-limit`, setting
`iv-always-prune-cand-set-bound=5`, and enabling `-fgcse-las` under a new
`gcse-las-generated-only` restriction — and moved the reference point:

| Metric | This report's baseline | After the screening |
|---|---:|---:|
| ELF SHA-256 | `77baa9a3…` | `b39a281d…` |
| text bytes | 14,768 | 14,704 |
| modeled cycles | 197,051 | 196,736 |
| CoreMark/MHz | 5.075 | 5.083 |

Those figures were reproduced independently for this record: building CoreMark
with the compiler currently in the build tree yields exactly
`b39a281daa94a30b2f2bcca2d00b2239bce65f7a78cd35ec3c68d856c6d86860`, 14,704
bytes of text and 196,736 cycles, matching the screening document.  The gain is
below the model's documented calibration error and is a board candidate, not a
hardware result.

**The focused ONiO suite grew.**  This report cites 78 passes and one
unsupported variant across four `gcc.target/riscv/onio-zero-*.c` tests.  The
screening adds three more — `onio-zero-gcse-las-generated.c`,
`onio-zero-inline-dispatcher.c` and `onio-zero-iv-prune.c` — for seven tests
reporting 158 passes, one unsupported variant and no failures.  A focused run
quoted from either document should say which set it used.

**Any baseline recorded now carries those changes.**  While this was written
the working tree held uncommitted edits to `gcc/config/riscv/riscv.cc`,
`gcc/gcse.cc` and `gcc/params.opt` from that screening, and the compiler in
the build tree was built from them.  A baseline recorded against that tree
describes `6b53b97` *plus* work-in-progress, not `6b53b97` alone.
`onio-dejagnu-gate.sh` therefore writes a `.provenance` file recording the
commit and a hash of the working-tree diff next to the baseline, and refuses
to write a baseline at all if the compiler is rebuilt mid-run.  Prefer to
record the baseline once the screening changes are committed.

## Commands and results

| Check | Result |
|---|---|
| `python3 -m pytest` (simulator) | 5,169 passed, 5 skipped, 158 subtests |
| coverage gate | 88.36% line, 82.74% branch — passed |
| `make gdb-smoke` | RV32 and ARM real-GDB smoke passed |
| `make performance-gate` | RV32 75.0, ARM 44.5 Python calls/step — unchanged |
| black, flake8, isort, mypy | clean |
| GCC C selftests | 8,438,570 / 8,438,570 |
| Focused ONiO DejaGNU (`riscv.exp=onio-zero-*.c`) | 78 pass, 1 unsupported, 0 fail |
| `gcc.dg/preunroll-1.c`, `gcc.dg/unroll-and-jam-reduction.c` | 3 passes each, 0 fail |
| CoreMark `build_ws01_baseline/coremark.elf` | SHA-256 unchanged, 197,051 cycles, 5.075 CoreMark/MHz |

## Remaining unknowns

- The gate covers RISC-V target code generation and C torture execution.  It
  does not yet cover `gcc.dg` or `gcc.dg/torture`, which are large and
  dominated by environment gaps; extending it needs the same
  baseline-comparison treatment.
- `brk` is not implemented, so a libgloss built for the QEMU syscall path would
  have no working `malloc` under this simulator.
- The gcov, TLS, stack-protector and libatomic gaps are properties of the
  bare-metal toolchain, not of ONiO.zero.  Whether to provide any of those
  runtimes is a product decision, not a compiler defect.
- `gcc.target/riscv/ext-dce-4.c` remains failing by design of this record: its
  expectation is stale upstream, and fixing it is out of the handover's scope.
