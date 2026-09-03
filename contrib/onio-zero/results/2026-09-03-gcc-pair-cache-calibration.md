# GCC 6b53/9e3e CoreMark and cache-calibration handoff — 2026-09-03

## Outcome

Clean builds of both GCC revisions reproduced the 2,000-iteration CoreMark
ELFs byte for byte.  The comparison is now pinned to source commits an
external engineer can check out, while the ELF hashes are verification outputs.

embsim 0.2.0 still cannot explain the board regression.  More importantly,
changing only the unmeasured cache policy changes the compiler ranking:

- documented predicted-way replacement scores `9e3e` 0.197% slower;
- predicted empty-way filling makes the two builds effectively equal; and
- LRU replacement gives `9e3e` 6.483% fewer modeled cycles (6.93% higher
  modeled throughput).

The reported board times, 13.00 s and 14.52 s, make `9e3e` about 11.69%
slower.  No tested cache hypothesis predicts that result.  Do not tune a
cycle-price scalar to these two rounded measurements and do not accept a GCC
successor to `6b53` on embsim alone.

## Reproducible compiler and ELF pair

| Name | GCC source commit | text/data/BSS | CoreMark ELF SHA-256 |
| --- | --- | ---: | --- |
| Board-good reference | `6b53b974280438b49f44783753f92f90b4c00558` | 14,768/12/20 | `1de35143089abfb23a1ba881d2c3487f11257b3d3e2c0aedd6a1030c5852bb12` |
| Regressed candidate | `9e3ea11a5397d96d86fb30cc3dae93bb6186ff9d` | 14,704/12/20 | `520e21d79c95321b8672224970e27892fa23e8482b71e4a890773e092126cb92` |

Both commits are reachable from `https://github.com/onio-git/gcc.git` branches
`onio-zero` and `onio-zero-next`.  Both clean toolchains used:

```text
--target=riscv32-unknown-elf
--with-arch=rv32imc_zicsr_zba_zbb_zbs_zifencei --with-abi=ilp32
--disable-multilib --with-newlib --without-headers --disable-shared
--disable-threads --disable-nls --disable-libssp --disable-libquadmath
--disable-libgomp --enable-languages=c,lto
```

They were built with `make -j4 all-gcc all-target-libgcc`, the same host,
and the same GNU Binutils 2.43 executables.  The CoreMark source is
`mikro-design/rv32sim.py` commit
`324adf10be1886ab74e8abd04b36e17e8e11369e`, with every input checked by
`coremark-source-324adf10.sha256`.  `rebuild-coremark-candidates.sh` supplied
the identical flags, `TOTAL_DATA_SIZE=2000`, `CPU_FREQ_HZ=32000000`, and
`ONIO_ITERATIONS=2000`.

Local tool evidence:

| Input | 6b53 build SHA-256 | 9e3e build SHA-256 |
| --- | --- | --- |
| `xgcc` | `c86a9e2b22d7f7fcd579658666fb0e75b6962c0a7f4507e961b62bf13aa89010` | `8cc8ef72f4d4328c29c63e5f15e006c7aa346afc3928adf6ea38eacbc21d4329` |
| `cc1` | `d10b7a99d3220727d011da01177d366a00f12914438bbce81cf017f1fe91d87f` | `c23e385a0e81ee6e398c0c964062bd12f9205cf4d7b078f1c82da6b67f0fee10` |
| target `libgcc.a` | `613af673ae0d078bb70d47ae7b70a298e250c3a754fcc462b1e4a9c26ecf398e` | `5aa6d40d71450460d4c5ca69ceffed15ca9289b962d7d1e6fa06cf3eec011a3d` |

The common assembler SHA-256 was
`cdf22c12ee42aa51ee21c161b2e340d066db6d71ba401ce511e249b22cea9a4e`;
the common linker SHA-256 was
`9267cb063a6eacd8f5d9f48fe715dab4c9fe7bb2a7c8d621b61f57259d62c02b`.

The two other images emitted as reconstruction controls were:

| GCC | Selective-LTO unpadded SHA-256 | Selective-LTO pad-16 SHA-256 |
| --- | --- | --- |
| 6b53 | `3a90ab7c4f8851d293c9f944cdaff5ff7af6d6d4a6f75ad37443b18389831e5d` | `b2e185d57ff4a95d8c7f054116a0a9dc22e2b54a8b80798b3a715d08a005c456` |
| 9e3e | `6159c24dc63ceae118bc73c06f6dc3c800e28afd9e3b743b1d8db7dad8fe2141` | `58d98982ea44bdaf6a34c3d393e00f974c8ddcfd01488523e9584df2139350a5` |

## Pinned embsim matrix

All rows came from the published `embsim 0.2.0` Linux x86-64 executable,
SHA-256
`41210159d05177ad70e4b364a4df2e11f1995cacc7f7e58154bd964824a3f037`.
The full counters, per-set hard misses, model hashes, ELF hashes, and simulator
hash are in `2026-09-03-cache-hypotheses.csv`.

| Cache hypothesis | 6b53 cycles | 9e3e cycles | 9e3e minus 6b53 | Key event change |
| --- | ---: | ---: | ---: | --- |
| predicted replacement, lowest fill | 392,663,566 | 393,437,062 | +773,496 (+0.197%) | hard misses +129,892 (+10.41%) |
| predicted replacement, predicted fill | 392,640,650 | 392,639,342 | -1,308 (-0.0003%) | hard -178,216; soft +1,956,862 |
| LRU replacement, lowest fill | 440,209,028 | 411,671,089 | -28,537,939 (-6.483%) | hard -52,946; soft -27,832,469 |
| predicted/lowest plus one split cycle | 531,577,187 | 537,544,681 | +5,967,494 (+1.123%) | split fetches +5,193,998 (+3.74%) |

The one-cycle split-word control is internally exact: its cycle total exceeds
the predicted/lowest row by `code_split_word_fetches` for each ELF.  The
runner also verifies that every reported per-set count sums to the hard-miss
total.

Under the documented policy, the largest 9e3e hard-miss increases are cache
sets 14 (+35,998), 18 (+29,998), 13 (+27,994), 21 (+18,086), and 31
(+17,998).  Set 16 decreases by 57,976.  Those iteration-shaped counts show
real layout conflicts and identify where to focus later placement work, but
they do not account for the board magnitude.

No single constant hard-miss or split-word price repairs this result.  If hard
misses dominate, the documented-model ratio approaches only
`1,378,094 / 1,248,202 = 1.1041`; if split fetches dominate, it approaches
`144,107,619 / 138,913,621 = 1.0374`.  The rounded board time ratio is
`14.52 / 13.00 = 1.1169`.  Alternative replacement/fill policies reduce or
reverse the modeled regression.  At least one material hardware event or
configuration difference remains absent from the model.

## Board-discriminating probes

`cache-calibration/build-probes.sh` builds two freestanding reference ELFs.
The script checks the pinned CoreMark source manifest, records every command
and input hash, and works with an installed compiler or `ONIO_GCC_BUILD`.
Built with the clean 6b53 compiler, it reproducibly generated:

| Probe | text/data/BSS | ELF SHA-256 |
| --- | ---: | --- |
| replacement policy | 100,208/0/16 | `960a05d957a60e344444b33d835984a864de77b5b2b9de801de623337ea69356` |
| split instruction word | 2,272/0/16 | `8f822071317ad6487fb2284fbb3d0b0520e744be23eec8590f217f7760c0018f` |

The replacement probe emits 31 groups across all 32 cache sets.  Every trial
fetches fresh same-set lines in the order A, B, A, C, B.  embsim observes:

| Policy | Timed cycles | Soft misses | Hard misses |
| --- | ---: | ---: | ---: |
| predicted replacement | 65,017 | 55 | 4,908 |
| predicted fill | 65,026 | 54 | 4,909 |
| LRU replacement | 56,584 | 992 | 3,971 |

The 937-hard-miss separation makes this a much cleaner replacement-policy
measurement than CoreMark.  `onio_cache_replacement_probe()` is board-callable
and returns its own `rdcycle` delta.  The returned embsim values were 64,999
cycles for predicted replacement and 56,557 for LRU; the table includes the
18-cycle outer measurement wrapper as well.

The split probe runs identical warm loops of 100,000 iterations, first with
32-bit instructions at `pc % 4 == 0`, then at `pc % 4 == 2`.  With no split
cost configured, its internal counts were 200,015 and 200,005 cycles.  With
`split_word_cycles=1`, they were 200,015 and 400,006: exactly 200,001 extra
cycles for the split loop's 200,001 split instructions.
`onio_split_word_probe()` writes the two `rdcycle` results through caller
pointers.

The reference ELFs use the rv32sim CoreMark ecall port so they can be verified
under embsim.  For real hardware, link the two assembly functions into the
engineer's known-good UART/debug board harness; the assembly interfaces do not
depend on ecall output.

## Exact external test request

With an existing GNU Binutils 2.43 prefix in `$prefix`, the source side of the
handoff is concretely reproducible as follows:

```sh
git clone --branch onio-zero-next https://github.com/onio-git/gcc.git gcc-control
git -C gcc-control worktree add ../gcc-6b53 \
  6b53b974280438b49f44783753f92f90b4c00558
git -C gcc-control worktree add ../gcc-9e3e \
  9e3ea11a5397d96d86fb30cc3dae93bb6186ff9d
(cd gcc-6b53 && ./contrib/download_prerequisites)
(cd gcc-9e3e && ./contrib/download_prerequisites)

for name in 6b53 9e3e; do
  mkdir "build-$name"
  (
    cd "build-$name"
    PATH="$prefix/bin:$PATH" "../gcc-$name/configure" \
      --target=riscv32-unknown-elf --prefix="$PWD/toolchain-$name" \
      --with-arch=rv32imc_zicsr_zba_zbb_zbs_zifencei --with-abi=ilp32 \
      --disable-multilib --with-newlib --without-headers --disable-shared \
      --disable-threads --disable-nls --disable-libssp --disable-libquadmath \
      --disable-libgomp --enable-languages=c,lto
    PATH="$prefix/bin:$PATH" make -j4 all-gcc all-target-libgcc
  )
done

git clone https://github.com/mikro-design/rv32sim.py.git rv32sim.py
git -C rv32sim.py checkout 324adf10be1886ab74e8abd04b36e17e8e11369e

for name in 6b53 9e3e; do
  PATH="$prefix/bin:$PATH" \
  ONIO_GCC_BUILD="$PWD/build-$name" \
  ONIO_AS="$prefix/bin/riscv32-unknown-elf-as" \
  ONIO_LD="$prefix/bin/riscv32-unknown-elf-ld" \
  ONIO_SIZE="$prefix/bin/riscv32-unknown-elf-size" \
  ONIO_ITERATIONS=2000 \
    gcc-control/contrib/onio-zero/rebuild-coremark-candidates.sh \
      rv32sim.py/tests/coremark "coremark-$name"
done

PATH="$prefix/bin:$PATH" \
ONIO_GCC_BUILD="$PWD/build-6b53" \
ONIO_SIZE="$prefix/bin/riscv32-unknown-elf-size" \
  gcc-control/contrib/onio-zero/cache-calibration/build-probes.sh \
    rv32sim.py/tests/coremark cache-probes
```

Use an absolute installation prefix in practice.  `ONIO_GCC_BUILD` selects
the just-built `xgcc` and target `libgcc`; no installed GCC is substituted.

1. Check out and build GCC commits
   `6b53b974280438b49f44783753f92f90b4c00558` and
   `9e3ea11a5397d96d86fb30cc3dae93bb6186ff9d` with the same configure line.
2. Build the pinned CoreMark source with `ONIO_ITERATIONS=2000` and
   `rebuild-coremark-candidates.sh`.  Report the produced target ELF SHA-256;
   matching the values above confirms the exact images, while a mismatch must
   be accompanied by `toolchain.txt` and `commands.log`.
3. Alternate the two target images on one board for at least five retained
   runs each.  Record raw, unrounded cycle counts, run order, validation CRCs,
   board/silicon revision, clock, voltage, flash/cache configuration,
   temperature, and flash/debug command.
4. Link and run the replacement and split-word probe functions in that same
   board harness.  Reset before every replacement-probe run and retain at
   least ten cycle readings.  For the split probe, retain both individual
   readings from every run.
5. Return the actual flashed ELFs, their SHA-256 values and section sizes, all
   raw readings, and the complete build logs.  Do not return only compiler
   commit IDs or screenshots rounded to two decimals.

Only those board results can select the cache policy/costs and decide whether
a future compiler revision is better than 6b53.
