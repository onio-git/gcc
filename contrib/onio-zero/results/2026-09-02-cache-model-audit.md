# Aldebaran cache-model audit — 2026-09-02

## Outcome

The previous embsim model is invalid as a calibrated hardware-performance
model. Reported board results put GCC `6b53b974280438b49f44783753f92f90b4c00558`
at about 155 CoreMark iterations/s and
`9e3ea11a5397d96d86fb30cc3dae93bb6186ff9d` at about 138 iterations/s. The
old model predicted the newer compiler about 0.17% faster.

The cache implementation also had a concrete semantic error. The supplied
Aldebaran description says that the way used for the immediately preceding
valid instruction is probed first on the next fetch. embsim instead remembered
one preferred way per cache set. The corrected implementation carries one
cache-wide preferred way, preserves warmed cache state when timed-window
counters are reset, and reports the underlying cache events.

The correction gets the long-run direction right for locally reconstructed
ELFs, but not the magnitude: it predicts the newer compiler only 0.20% slower.
The model must therefore remain a structural screening model until exact board
artifacts and a versioned calibration dataset are available.

## Revisions and artifacts

| Item | Revision or SHA-256 |
|---|---|
| Corrected embsim branch | `origin/onio-cache-calibration` |
| Corrected embsim commit | `6277348b9a24bf1284b6108ac57f4b8bd08a223d` |
| Corrected local release executable | `1f378018eeea87f7388039102ef67650d4b08689adeafbe2edce6b36b3c34cb6` |
| Reconstructed 2,000-iteration `6b53b974…` ELF | `1de35143089abfb23a1ba881d2c3487f11257b3d3e2c0aedd6a1030c5852bb12` |
| Reconstructed 2,000-iteration `9e3ea11a…` ELF | `520e21d79c95321b8672224970e27892fa23e8482b71e4a890773e092126cb92` |

The two flashed board ELFs have not been supplied, so this audit does **not**
assert that either reconstructed ELF is byte-identical to the engineer's board
image. Git revisions are source provenance, not executable identities.

## Exact 2,000-iteration embsim comparison

Both reconstructed ELFs were measured between their `start_time` return and
`stop_time` call with the corrected embsim. The full rows are in
`2026-09-02-cache-model-audit.csv`.

| Counter | `6b53b974…` | `9e3ea11a…` | New minus old |
|---|---:|---:|---:|
| Instructions | 368,643,383 | 368,467,373 | -176,010 |
| Branch penalty | 10,772,549 | 10,772,549 | 0 |
| Soft misses | 765,614 | 416,200 | -349,414 |
| Hard misses | 1,248,202 | 1,378,094 | +129,892 |
| Sequential hard misses | 932,041 | 1,005,490 | +73,449 |
| Nonsequential hard misses | 316,161 | 372,604 | +56,443 |
| Modeled cycles | 392,663,566 | 393,437,062 | +773,496 (+0.197%) |
| Modeled CoreMark/MHz | 5.093419 | 5.083405 | -0.197% |

The corrected model therefore exposes a real adverse cache signature: about
65 extra hard misses per CoreMark iteration. The current ten-cycle hard-miss
placeholder underweights it, but changing one scalar cannot repair the model.
At any positive constant hard-miss cost, the limiting cycle ratio is the hard
miss count ratio, about 1.104. The reported board time ratio is approximately
`14.52 / 13.00 = 1.117`, already outside that limit. Giving sequential and
nonsequential fills independent positive costs also cannot fit both absolute
board times.

As cross-checks, a one-iteration dynamic trace has only 72 more immediate
load-use dependencies and two fewer jumps in the newer image. Those event
deltas cannot explain roughly 24,000 additional board cycles per iteration
without implausible penalties.

## What changed in embsim

- one cache-wide preferred-way state now follows every instruction fetch;
- `monitor reset_counter` zeros memory-interface counters without flushing a
  warmed cache;
- `monitor show_stats` reports code/data memory-interface counters;
- Aldebaran counters distinguish soft and hard misses, invalid fills,
  replacements, sequential and nonsequential hard misses;
- `track_sets=true` optionally reports hard misses per cache set; and
- the documentation now describes the performance model as screening rather
  than board evidence.

The entire embsim workspace test suite passes at the corrected commit.

## Required calibration input

The external engineer must return the two **actual ELF files that were
flashed**, not only GCC commit IDs, together with:

- `sha256sum` and section sizes for each ELF;
- complete compile and link commands plus compiler `-v` output;
- individual unrounded run times in alternating A/B order;
- board and silicon revision, CPU clock, voltage, and flash/cache configuration;
  and
- validation CRC output.

Until those inputs exist, `embsim-onio-zero-coremark.json` is explicitly marked
`uncalibrated-structural-model`; its `hard_cycles=10` value is a placeholder.
It can screen instruction/cache behavior, but it cannot approve a successor to
`6b53b974…`.
