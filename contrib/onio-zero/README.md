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
