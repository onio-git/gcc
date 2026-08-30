# ONiO.zero selective-LTO and cache-phase screening, 2026-08-14

This screening continued from GCC commit
`a54f245a389b7f6fe62e81f19e2d13f2feba3122` and rv32sim commit
`54949e3e44ca10c52958b8835242e39cd6a8d064`.  The explicitly stopped full
DejaGNU run was not restarted.  Experiments used at most four compiler jobs
and used rv32sim and mikrosim, never QEMU.

## Baselines

The preserved ws01 baseline remains:

- ELF SHA-256:
  `77baa9a36f4ce11a7225bbbd3f447b2a0ad5f3a309c2493b7c330fcbda953b28`
- text: 14,768 bytes
- modeled cycles: 197,051
- CoreMark/MHz: 5.075
- modeled CoreMark/mJ: 230.7

The accepted target-default compiler improvements from the 2026-08-13 screen
are the immediate baseline for this work:

- ELF SHA-256:
  `b39a281daa94a30b2f2bcca2d00b2239bce65f7a78cd35ec3c68d856c6d86860`
- text: 14,704 bytes
- modeled cycles: 196,736
- timed instructions: 184,241
- fetch penalty: 7,107 cycles
- branch penalty: 5,388 cycles
- mispredictions: 666
- CoreMark/MHz: 5.083
- modeled CoreMark/mJ: 231.0

## Accepted code-generation strategy

Compile only `core_list_join.c` and `core_util.c` with `-flto=4`; compile the
remaining units with their accepted non-LTO per-file flags; then link with
`-flto=4 -flto-partition=none`.  This gives a phase-independent candidate:

| Metric | Target-default baseline | Selective LTO | Change |
|---|---:|---:|---:|
| text bytes | 14,704 | 13,576 | -1,128 |
| modeled cycles | 196,736 | 195,699 | -1,037 (-0.527%) |
| timed instructions | 184,241 | 183,205 | -1,036 |
| fetch penalty | 7,107 | 7,112 | +5 |
| branch penalty | 5,388 | 5,382 | -6 |
| mispredictions | 666 | 666 | unchanged |
| CoreMark/MHz | 5.083 | 5.110 | +0.527% |
| modeled CoreMark/mJ | 231.0 | 232.3 | +0.527% |

Candidate ELF SHA-256:
`d88ee4fc30e9382c0437d116a75ae27fc4358b82b66b7c97b0d51c6c36ff7125`.

The LTO group is deliberately narrow.  WPA can internalize the unused public
list helpers, specialize `core_list_mergesort` for its known comparators, and
inline the CRC chain from `core_util.c` into the timed list kernel.  The
ordinary build already performs same-unit comparator cloning, but it cannot
see the CRC bodies and must retain externally visible helper copies.  The
selective group therefore removes 1,036 timed instructions and 1,128 text
bytes together.  This is also why LTO of `core_list_join.c` alone regressed to
198,200 cycles: internalization without the cross-unit CRC opportunities is
not sufficient.

`-flto-partition=none` is important for the final layout.  The default
balanced partition produced the same text and timed instruction counts but
195,851 cycles because its CRC function ordering cost another 152 fetch
cycles.  `one` tied `balanced`, `1to1` produced 196,024 cycles, and `max`
regressed to 202,702 cycles.

Adding `core_main.c` to the LTO group reached 195,676 cycles, only 23 cycles
better than the phase-independent candidate, but expanded text to 15,512
bytes.  Adding matrix or state units regressed.  The main-unit result is
therefore rejected as an energy-sensitive code-size tradeoff.

### Fixed-layout 16-byte phase candidate

Linking `contrib/onio-zero/coremark-cache-pad-16.S` immediately after
`startup.o` places 16 zero bytes between `.text.startup` and the regular text.
With the selective-LTO strategy above, the exact result is:

- ELF SHA-256:
  `04ebff14f65ae858db478a1eabc726f4f68939d86526f7e9b2e6f5bfe933028d`
- text: 13,592 bytes
- modeled cycles: 195,537
- timed instructions: 183,205
- fetch penalty: 6,950 cycles
- branch penalty: 5,382 cycles
- mispredictions: 666
- CoreMark/MHz: 5.114
- modeled CoreMark/mJ: 232.5

This is 1,199 cycles (0.609%) faster and 1,112 text bytes smaller than the
target-default baseline.  Against the original preserved baseline it is
1,514 cycles (0.768%) faster and 1,176 bytes smaller.

The pad is a fixed-image optimization, not a compiler default.  The cache
phase is sharp: 14 bytes produced 195,683 cycles, 16 bytes produced 195,537,
and 18 bytes regressed to 196,982.  Coarse 32-byte phases across the full
1-KiB cache alias period found 512 bytes at 195,651 cycles, but that was both
slower and larger than the 16-byte candidate.  Any source, linker, or startup
change requires rescreening the phase.

## Reproduction

The common compile options are:

```text
-I. -DITERATIONS=1 -DTOTAL_DATA_SIZE=2000 -DCPU_FREQ_HZ=32000000
-march=rv32imc_zicsr_zba_zbb_zbs_zifencei -mabi=ilp32
-ffreestanding -fno-builtin -O2 -mcpu=onio-zero -std=c99
```

Compile `core_list_join.c` and `core_util.c` with those options plus
`-flto=4`.  Retain the accepted ordinary objects for all other sources:

```text
core_matrix.c: -fno-inline-functions -fno-inline-functions-called-once
               -floop-unroll-and-jam --param unroll-jam-min-percent=0
               --param unroll-jam-max-unroll=8
               --param max-unrolled-insns=6000
core_state.c:  --param max-inline-insns-auto=160
```

Link in the existing CoreMark object order with:

```text
-flto=4 -flto-partition=none -nostdlib -Wl,--gc-sections
-T linker.ld -lgcc
```

For the fixed-layout candidate, assemble
`contrib/onio-zero/coremark-cache-pad-16.S` with the common ISA/ABI options and
place its object immediately after `startup.o` on the link line.

## Validation

rv32sim reproduced the counters above.  mikrosim completed both the unpadded
and 16-byte candidates with an exit status of zero and reported:

```text
seedcrc      0xe9f5
crclist      0xe714
crcmatrix    0x1fd7
crcstate     0x8e3a
crcfinal     0xe714
```

mikrosim's timed count fell from 189,633 on the target-default baseline to
188,591 on both selective-LTO candidates, independently confirming the
cross-unit instruction saving.  The one-iteration harness still prints its
expected ten-second duration warning.

No GCC source change survives from the branch-layout or RVC experiments in
this screen, so the already-passed compiler selftests and focused DejaGNU set
remain the compiler-correctness evidence.  The stopped full DejaGNU run was
not duplicated.

## Rejected backend and layout experiments

- Rebuilt isolated branch profiles were byte-identical to the target-default
  baseline except for the two known list sites.  Those sites produced 196,895
  and 197,904 cycles; branch savings did not repay fetch cost.
- Software trace-cache duplication limits 0 through 16 had no winner.  The
  best nondefault value was six at 196,926 cycles.
- Twelve hot-object orders confirmed the current order as the unique optimum;
  all distinct orders regressed by 0.33% to 4.03%.
- Reserving either compressed callee-saved register in the list unit cost
  1,570 cycles.  Reserving any `s2` through `s11` cost 363 cycles.
- Exact whole-function callee-saved register swaps found only a 20-cycle
  cache-phase improvement for `s0`/`s2`, with no static rule that could select
  it robustly.
- An ONiO-only `TARGET_PREFERRED_RENAME_CLASS` prototype reduced text by 48
  bytes but regressed to 198,885 cycles through fetch cost.  Every changed
  translation unit regressed independently, so the prototype was removed.
- In selective LTO, inliner thresholds from 60 through 160 tied the accepted
  image.  `-O3`, `-Os`, broader LTO groups, IPA-PTA, IPA-CP cloning, and the
  tested IPA ablations had no further robust winner.
