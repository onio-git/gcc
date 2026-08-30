# ONiO.zero selective-LTO function-layout screening, 2026-08-26

## Scope and provenance

This screening continued from GCC commit
`a54f245a389b7f6fe62e81f19e2d13f2feba3122` and the selective-LTO result in
`2026-08-14-selective-lto.md`.  No GCC compiler source was changed.  The new
inputs are application-specific function sections, a linker section-ordering
file, and the existing optional startup/text pad.

The rv32sim checkout was at commit
`2bdb101a1d218a76f2dda806735a4ea332374f38`; its tracked CoreMark assets are
unchanged from the previously recorded
`54949e3e44ca10c52958b8835242e39cd6a8d064` revision.  The ONiO model SHA-256
remains
`5fffda831f7fe9dd3054cdbe816c088c21c9c16ea48b12e97bc5e7de9975dfdf`.
The compiler is `xgcc (GCC) 17.0.0 20260602 (experimental)`, configured for
`riscv32-unknown-elf`; the linker is GNU ld 2.43.  Experiments used at most
four concurrent jobs and used rv32sim and mikrosim, never QEMU.  The stopped
full DejaGNU run was not restarted.

The screen retained the one-iteration workload used for deterministic model
comparison:

```text
TOTAL_DATA_SIZE=2000 ITERATIONS=1 CPU_FREQ_HZ=32000000
```

## Result

The accepted target-default image and the two August 14 selective-LTO images
remain the comparison points:

| Metric | Target default | Selective LTO | Old 16-byte layout | New unpadded layout | New 16-byte layout |
|---|---:|---:|---:|---:|---:|
| text bytes | 14,704 | 13,576 | 13,592 | 12,936 | 12,952 |
| modeled cycles | 196,736 | 195,699 | 195,537 | 195,267 | 194,988 |
| timed instructions | 184,241 | 183,205 | 183,205 | 183,205 | 183,205 |
| fetch penalty | 7,107 | 7,112 | 6,950 | 6,680 | 6,401 |
| branch penalty | 5,388 | 5,382 | 5,382 | 5,382 | 5,382 |
| mispredictions | 666 | 666 | 666 | 666 | 666 |
| CoreMark/MHz | 5.083 | 5.110 | 5.114 | 5.121 | 5.129 |
| modeled CoreMark/mJ | 231.0 | 232.3 | 232.5 | 232.8 | 233.1 |

The unpadded ELF SHA-256 is
`32e5301104b85dfcab4a723799c654ea6cfd375be66d013b031ceb620e386ef8`.
The 16-byte ELF SHA-256 is
`11e9ac9629d47bae7ee27654901cbc7551f710641a7e77f5a892dd4ef7e86f48`.
A second link from the same fresh objects reproduced the padded ELF byte for
byte.

Against the target-default image, the padded result removes 1,748 cycles
(0.889%) and 1,752 text bytes (11.915%).  Against the preserved 197,051-cycle,
14,768-byte ws01 baseline, it removes 2,063 cycles (1.047%) and 1,816 text
bytes (12.297%).  Against the previous padded selective-LTO image, it removes
549 cycles and 640 text bytes.

The cycle gain over the previous padded image is entirely modeled fetch cost:
the timed instruction, branch-penalty, and misprediction counts are unchanged.
The largest local fetch reductions are 280 cycles in `matrix_sum`, 128 in
`crc16`, and 120 in `matrix_test`.  Some functions get slightly worse, which
is why this must be evaluated as a complete-image layout rather than as a
per-function compiler rule.

## Build strategy

The common compile options are:

```text
-I. -DITERATIONS=1 -DTOTAL_DATA_SIZE=2000 -DCPU_FREQ_HZ=32000000
-march=rv32imc_zicsr_zba_zbb_zbs_zifencei -mabi=ilp32
-ffreestanding -fno-builtin -Wall -Wextra -Wno-unused-parameter
-O2 -mcpu=onio-zero -std=c99
```

Compile `core_list_join.c` and `core_util.c` with the common options plus
`-flto=4 -ffunction-sections`.  Compile `core_matrix.c` with:

```text
-fno-inline-functions -fno-inline-functions-called-once
-floop-unroll-and-jam --param unroll-jam-min-percent=0
--param unroll-jam-max-unroll=8 --param max-unrolled-insns=6000
-ffunction-sections
```

Compile `core_state.c` with:

```text
--param max-inline-insns-auto=160 -ffunction-sections
```

Compile the remaining C and startup units with their existing options.
Assemble `contrib/onio-zero/coremark-cache-pad-16.S` with the common ISA and
ABI.  Link the objects in this order:

```text
core_list_join.o core_main.o core_matrix.o core_state.o core_util.o
core_portme.o ee_printf.o startup.o coremark-cache-pad-16.o
```

Use these link options:

```text
-march=rv32imc_zicsr_zba_zbb_zbs_zifencei -mabi=ilp32
-ffreestanding -fno-builtin -O2 -mcpu=onio-zero -ffunction-sections
-flto=4 -flto-partition=none -nostdlib
-Wl,--gc-sections
-Wl,--section-ordering-file=contrib/onio-zero/coremark-matrix-layout.order
-Wl,-Map=coremark.map -T linker.ld -lgcc
```

Omit `coremark-cache-pad-16.o` for the unpadded candidate.  GNU ld 2.43's
section-ordering file places the individual LTO list/CRC, matrix, and state
sections without duplicating the target linker's memory layout.  The link map
must confirm that the named LTO function sections immediately follow the
startup sections; `*lto.o(.text)` is only a fallback for residual LTO text.  A
toolchain that names or partitions its LTO output differently requires a new
map review.

`-ffunction-sections` on `core_state.c` lets `--gc-sections` discard the
618-byte externally visible `core_state_transition`.  Its calls have already
been inlined into `core_bench_state`, so the timed instruction stream and CRCs
do not change.  The list/CRC LTO section order is:

```text
core_bench_list, core_list_init, crcu16, get_seed_32, crcu32, crc16,
check_data_types
```

Matrix function sections enable the explicit caller-first order recorded in
`coremark-matrix-layout.order`:

```text
core_bench_matrix, matrix_test, matrix_add_const, matrix_mul_vect,
matrix_mul_const, matrix_mul_matrix, matrix_mul_matrix_bitextract,
matrix_sum, core_init_matrix
```

## Search bounds

The bounded search covered:

- ten source-, call-, size-, and hotness-derived matrix orders;
- the four promising orders at padding phases 0 through 32 in four-byte steps;
- two complete adjacent-swap rounds around the best caller-first order;
- eight list/CRC seed orders and two adjacent-swap rounds around their best
  order;
- the neighboring 14-, 16-, and 18-byte phases; and
- all 32 cache-set aliases at `16 + 32*n` bytes across the full 1-KiB alias
  period.

The 16-byte result was the unique winner at 194,988 cycles.  The next same
intra-line phase, 48 bytes, produced 195,072 cycles; the remaining aliases
ranged from 195,235 to 195,498 cycles.  Fourteen and 18 bytes produced 195,079
and 195,108 cycles.  Hot-first and descending-size layouts were rejected at
202,936 and 202,566 cycles.  Every adjacent swap from the final order
regressed or tied the prior order.

## Validation and limits

rv32sim reproduced 194,988 cycles, 6,401 fetch-penalty cycles, 5,382 branch
penalty cycles, and 666 mispredictions for the fresh padded ELF.  mikrosim
completed both the unpadded and padded ELFs with exit status zero and 188,591
ticks.  Both printed:

```text
seedcrc      0xe9f5
crclist      0xe714
crcmatrix    0x1fd7
crcstate     0x8e3a
crcfinal     0xe714
```

The one-iteration harness prints its expected ten-second duration warning.
No compiler source changed, so the existing compiler selftests, focused ONiO
DejaGNU set, and gate baseline remain the compiler-correctness evidence.

Both layouts are application link recipes, not compiler defaults.  The
unpadded order is already 270 modeled cycles faster and 656 text bytes smaller
than the previous padded candidate, but its absolute addresses still form part
of the result.  The 16-byte candidate is explicitly fixed-image: any source,
compiler, linker, startup, library, or object-order change requires rebuilding
the exact ELF and rescreening its phase.

Neither result is a hardware performance or energy claim.  Flash the exact
candidate and comparison ELFs on the same Aldebaran board and retain the raw
power trace, voltage, frequency, temperature, flash state, board/silicon
revision, command lines, hashes, section sizes, and CRC output before accepting
either layout.
