# ONiO.zero loop memory-extension experiment, 2026-09-02

> **Hardware status:** this report's embsim ranking is superseded by
> `2026-09-02-cache-model-audit.md`. Reported board measurements show that the
> `9e3ea11a…` base is substantially slower than `6b53b974…`; no candidate in
> this report is a validated successor to `6b53b974…`.

## Result

A general RTL liveness bug prevented `ext-dce` from narrowing some
sign-extending memory loads inside loops.  The pass handled the source of a
memory-load `SET`, then descended into the whole `SET` and conservatively
treated its destination as another input.  That false use circulated around a
loop backedge, made all bits appear live, and blocked a valid `lh` to `lhu`
rewrite.

The fix marks registers in the memory address live and then skips the complete
`SET`.  In CoreMark's inlined `cmp_idx`, each hot list comparison now uses
`lhu` directly instead of `lh` followed by `zext.h`.  This removes two
instructions per comparison, 418 instructions from the one-iteration timed
window and 832,194 instructions from the 2,000-iteration timed window.  Text
falls by eight bytes in both target-default and final-layout images.

The phase-independent final-layout image is the candidate.  The old 16-byte
pad is rejected: changing the code invalidates its cache phase and makes it
slower than the unpadded image.

## Provenance

- GCC compiler change:
  `04b0bc8e8272e9b0a73bffcb0b9e5cf572ad3ff9`, based on
  `d7130792407`.
- CoreMark source: `tests/coremark` from rv32sim.py commit
  `324adf10be1886ab74e8abd04b36e17e8e11369e`, content-checked by
  `coremark-source-324adf10.sha256`.  rv32sim.py supplied source only; it was
  not used as a simulator.
- Simulator: `embsim` commit
  `03bffbf82b1dbe0b874ed9a1d8048bb369467c07`, executable SHA-256
  `f5557a4321ea296befc9c7ab541b955ba96e33f70a5ad412468ceff7ad272903`.
- Model: `embsim-onio-zero-coremark.json`, SHA-256
  `9736eb2ac1132d02b7e89ba2970d649f050800431752397fa3a7c3b87118bd1f`.
- Timed-window runner: `run-coremark-embsim.py`, SHA-256
  `06e899e587f1834e7dd4ecc7d1140bc25c582ea7a2725851ffd9ef47a72c9892`.
- Rebuild script: `rebuild-coremark-candidates.sh`, SHA-256
  `116ded8df3928378f5beb99dcf5ce511e623d5c3bd69c6ecac8dac8f2cb37f7a`.
- Compiler: `xgcc (GCC) 17.0.0 20260602 (experimental)` from
  `/home/veba/work/git/toolchain-src/build-gcc`.
- Linker: GNU ld 2.43.

All performance and correctness simulation in this experiment used `embsim`.
The complete counters are archived in `2026-09-02-ext-dce-coremark-1.csv` and
`2026-09-02-ext-dce-coremark-2000.csv`.

## One-iteration screen

The historical one-iteration workload remains useful for quick deterministic
compiler screening.  Lower cycles are better.

| Image | Text | Instructions | Cycles | Fetch | Branch | Mispredicts | Change |
|---|---:|---:|---:|---:|---:|---:|---:|
| Old target `b39a281d...` | 14,704 | 184,241 | 196,736 | 7,107 | 5,388 | 666 | baseline |
| New target `9b49f59e...` | 14,696 | 183,823 | 196,648 | 7,437 | 5,388 | 666 | -88 (-0.045%) |
| Old unpadded `32e53011...` | 12,936 | 183,205 | 195,267 | 6,680 | 5,382 | 666 | baseline |
| New unpadded `edf20b1c...` | 12,928 | 182,787 | 194,945 | 6,776 | 5,382 | 666 | -322 (-0.165%) |
| Old pad-16 `11e9ac96...` | 12,952 | 183,205 | 194,988 | 6,401 | 5,382 | 666 | baseline |
| New pad-16 `9846d1bc...` | 12,944 | 182,787 | 196,033 | 7,864 | 5,382 | 666 | +1,045 (+0.536%) |

Full hashes for the new one-iteration comparison images are:

```text
9b49f59e6f1a47dd682765ec399885b0bc89e9ef0037af3960a7c96eff9ab3e7  coremark-target-default-9b49f59e.elf
edf20b1c59b488f00e2e00b9f4874ce6836539335a975e628f3fdd999827c4ce  coremark-layout-unpadded-edf20b1c.elf
9846d1bcd14bc39ac1904eec2d1e6f3b3592cec2576985670989d953795393b8  coremark-layout-pad16-9846d1bc.elf
```

## 2,000-iteration confirmation

The same source, flags, object order, and link recipes were rebuilt with
`-DITERATIONS=2000`.  This checks that the one-iteration screening result is
not an artifact of initialization or the benchmark harness.

| Image | Instructions | Cycles | Fetch | Branch | Mispredicts | CoreMark/MHz | Change |
|---|---:|---:|---:|---:|---:|---:|---:|
| Old target | 368,467,373 | 393,108,825 | 13,868,903 | 10,772,549 | 1,326,267 | 5.087650 | baseline |
| New target | 367,635,179 | 392,856,671 | 14,448,943 | 10,772,549 | 1,326,267 | 5.090915 | -252,154 (-0.064%) |
| Old unpadded | 366,397,869 | 390,140,039 | 12,983,620 | 10,758,550 | 1,326,267 | 5.126364 | baseline |
| New unpadded | 365,565,675 | 389,577,872 | 13,253,647 | 10,758,550 | 1,326,267 | 5.133762 | -562,167 (-0.144%) |
| Old pad-16 | 366,397,869 | 389,634,103 | 12,477,684 | 10,758,550 | 1,326,267 | 5.133021 | baseline |
| New pad-16 | 365,565,675 | 391,817,279 | 15,493,054 | 10,758,550 | 1,326,267 | 5.104420 | +2,183,176 (+0.560%) |

The new 2,000-iteration hashes are:

```text
9862f7eafa841ac0656bd6644a4ac22c5379e479caac4535ed84e7e89635f08e  coremark-target-default-iterations-2000.elf
8193476f2039417f313706d2bc8c6c5669446973a51beacd5f31102f997fa59b  coremark-layout-unpadded-iterations-2000.elf
259d8ae99dfdcce1e1c6cb067a1071b4157b30a9480bacc5efcfe79eb14b20bf  coremark-layout-pad16-iterations-2000.elf
```

## External reproduction and board A/B

Check out the published GCC tag `onio-zero-ext-dce-2026-09-02`, build that
compiler using the toolchain procedure in `2026-08-31-reproduction.md`, then
run:

```sh
ONIO_GCC_BUILD=/path/to/build-gcc \
ONIO_AS=/path/to/prefix/bin/riscv32-unknown-elf-as \
ONIO_LD=/path/to/prefix/bin/riscv32-unknown-elf-ld \
ONIO_SIZE=/path/to/prefix/bin/riscv32-unknown-elf-size \
contrib/onio-zero/rebuild-coremark-candidates.sh \
  /path/to/rv32sim.py/tests/coremark /path/to/output
```

For the next board A/B, test only these two newly compiled files from that
output directory:

1. `target-default/coremark-target-default-9b49f59e.elf`
2. `layout/coremark-layout-unpadded-edf20b1c.elf`

The hash checks in the rebuild script must pass before flashing.  Do not use
the generated pad-16 image as the optimized candidate; it is retained only to
demonstrate that the old cache phase was invalidated.

## Correctness and compiler gates

Full `embsim --run --fast` execution of the new one-iteration comparison
images exited with status zero and produced the expected `e714` final CRC.
The new target and unpadded 2,000-iteration images also exited with status zero
and reported `Correct operation validated`, including:

```text
seedcrc      0xe9f5
crclist      0xe714
crcmatrix    0x1fd7
crcstate     0x8e3a
crcfinal     0x4983
```

`gcc.target/riscv/ext-dce-5.c` covers the compiler defect.  The old compiler
emits `lh` plus `zext.h`; the fixed compiler emits `lhu`.  Its 40 configured
optimization variants pass.  The wider ONiO DejaGNU gate, pinned to the same
build-tree compiler and `embsim`, passes with no new failures against 1,062
baseline entries.  The run has 942 remaining known failures; 120 baseline
execution failures no longer reproduce with the pinned simulator.

## Rejected hypotheses

Outlining the duplicated state transition reduced target text from 14,704 to
13,752 bytes, but increased timed instructions from 184,241 to 207,361 and
cycles from 196,736 to 220,126.  It is rejected.

An explicit final-state switch exposed constant counter addresses and reduced
timed instructions to 181,893, but added 788 branches, expanded text to 15,120
bytes, and increased cycles to 197,969.  It establishes an instruction-count
opportunity but is not an acceptable implementation.

A second branch-free oracle moved state counting onto direct parser exits.  On
top of the `ext-dce` fix it reduced timed instructions from 183,823 to 183,157,
branch penalty from 5,388 to 4,796, and mispredictions from 666 to 606.  The
duplicated exits expanded text from 14,696 to 15,368 bytes, however; fetch
penalty rose from 7,437 to 10,650 and total cycles regressed from 196,648 to
198,603.  Path specialization of this state machine is therefore rejected as
an instruction-cache tradeoff.

## Hardware requirement

The model predicts only a 0.144% gain for the 2,000-iteration unpadded image,
far smaller than the previously observed board reversal between target and
optimized images.  This compiler fix does not explain or resolve that hardware
gap.  Board A/B testing must use the exact new target and unpadded hashes on
the same hardware and archive raw timing and power data.  The simulator result
is explanatory evidence, not hardware acceptance.
