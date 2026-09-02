# ONiO.zero tuning records

This directory keeps the hardware assumptions and measured baselines used by
the `onio-zero` GCC tuning.  It is intended to make performance changes
reproducible and to keep simulator assumptions separate from facts supplied by
the CPU design.

- `aldebaran-cache-bp.md` records the instruction-cache and branch-predictor
  description supplied by ONiO.
- `results/2026-08-09-ulpmark-cm.md` records the current board result.
- `review-2026-08-09.md` records the compiler-safety review and reproducible
  simulator screening performed against that baseline.
- `results/2026-08-13-coremark-screening.md` records the follow-up compiler
  search, the rejected candidates, and the new hash-controlled CoreMark
  candidate awaiting board validation.
- `results/2026-08-14-selective-lto.md` records the selective list/CRC LTO
  improvement, the rejected RVC-renaming work, and an optional fixed-layout
  cache-phase candidate.
- `results/2026-08-26-function-layout.md` records the follow-up function
  section, matrix-order, and full cache-alias screen built on selective LTO.
- `results/2026-08-30-board-ab.md` is the external-board handoff for the
  three hash-controlled CoreMark images.
- `results/2026-08-31-reproduction.md` and
  `rebuild-coremark-candidates.sh` provide the pinned source/toolchain
  procedure that recreates all three images; the source manifest is
  `coremark-source-324adf10.sha256`.
- `results/2026-09-02-embsim-resimulation.md` supersedes the simulator side of
  the historical records with a complete `embsim` rerun. The machine-readable
  results are in `results/2026-09-02-embsim-coremark-all.csv`; the runner and
  translated model are `run-coremark-embsim.py` and
  `embsim-onio-zero-coremark.json`.
- `results/2026-09-02-ext-dce-loop-memory.md` records the next compiler
  experiment: a loop liveness fix that removes redundant extensions, its
  one- and 2,000-iteration `embsim` results, and the invalidated 16-byte pad.
- `results/2026-09-02-cache-model-audit.md` records the board/model
  contradiction, the corrected cache-wide preferred-way implementation, and
  the exact calibration data still required from the flashed images.

All new ONiO simulation must use `embsim`. The `rv32sim.py` repository remains
the pinned public location of the CoreMark source snapshot and archived ELF
artifacts; it is not the simulator used by the current workflow.

## Acceptance order

Compiler changes should be accepted in this order:

1. The generated program produces the expected EEMBC validation CRCs.
2. GCC's testsuite and the ONiO-specific regressions do not regress.
3. The exact benchmark binary is measured on hardware at controlled voltage,
   frequency, temperature, and iteration count.
4. CoreMark/MHz is compared at equal frequency and CoreMark/mJ at equal
   voltage and measurement method.
5. Simulator counters are used to explain a result, not as a substitute for
   the board measurement.  Any assumed hard-miss latency or energy must be
   called out explicitly.

Every archived result should include the GCC revision, full compiler and
linker command lines, source revision, binary SHA-256, validation output,
board/firmware revision, and raw power trace.  The first imported result lacks
some of that provenance; those fields are marked as unknown rather than being
reconstructed from memory.

The source tree retains GCC's generic RISC-V tune when no CPU or tune is
selected.  ONiO toolchain builds must configure with
`--with-arch=rv32imc_zicsr_zba_zbb_zbs_zifencei`, `--with-abi=ilp32`, and
`--with-tune=onio-zero`, or compile individual files with `-mcpu=onio-zero`.
