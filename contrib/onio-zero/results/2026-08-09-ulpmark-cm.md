# ULPMark-CM board baseline — 2026-08-09 archive

This record transcribes the supplied ULPMark-CM result window.  It is a board
measurement, but it is not yet a fully reproducible benchmark record because
the screenshot does not contain the binary, compiler command lines, firmware
revision, board revision, or raw power data.

Screenshot: `unnamed (1).png`, 512 by 645 pixels, 72,500 bytes.

SHA-256: `16a00d5bebf97d31e6c1d0f0a3dcdee5ecc2eb042604821b27837406c409a9ed`

GCC tree at archive time: `2f483fec4bb69ac42343e9a98e72bd165cbf607b`

The screenshot alone does not prove which compiler revision produced the
binary.  The revision above is therefore context, not an asserted binary
provenance field.

| Metric | Performance run | Energy-efficiency run | Energy-efficiency run at 3 V |
|---|---:|---:|---:|
| CoreMark | 341.75 | 156.23 | 156.23 |
| CoreMark/MHz | 4.88 | 4.88 | 4.88 |
| Power (uW) | 2324.58 | 720.79 | 724.29 |
| Voltage (V) | 3.000 | 3.000 | 3.000 |
| Frequency (MHz) | 70.00 | 32.00 | 32.00 |
| Estimated frequency (MHz) | 70.85 | 31.81 | 31.88 |
| Library | example | example | example |
| Iterations | 4500 | 3500 | 3500 |
| Temperature (C) | 32 | 32 | 32 |
| ULPMark-CM | 148.81 | 215.49 | 214.89 |
| Derived CoreMark/mJ | 147.02 | 216.75 | 215.70 |

The observed 32 MHz power is 22.52 uW/MHz for the energy-efficiency run and
22.63 uW/MHz for its 3 V column.  Those ratios are descriptive only; fixed and
frequency-dependent power must be separated before extrapolating energy to a
different clock.  The CoreMark/mJ row is CoreMark divided by displayed power
in mW and is therefore derived from rounded screen values, not raw samples.

## Missing provenance to capture on the next run

- Exact benchmark and firmware source revisions.
- Full per-file compile flags, link flags, linker script, and library build.
- Compiler `--version`, configured target, and GCC revision.
- ELF and flash-image SHA-256 hashes plus section sizes.
- EEMBC validation CRC/output and benchmark seed/data-size configuration.
- Board and silicon revision, flash configuration, and measurement equipment.
- Raw voltage/current samples, sampling interval, warm-up, and idle-baseline
  treatment.
