# Recreating the ONiO.zero CoreMark board candidates, 2026-08-31

The ELF SHA-256 values are verification results, not reproduction
instructions.  Reproduction requires pinned source inputs, the compiler and
linker source/configuration, and the complete compile/link commands.  This
record supplies those missing pieces.

## Pinned inputs

- GCC: `https://github.com/onio-git/gcc.git`, tag
  `onio-zero-coremark-repro-2026-08-31`.  The tag includes the compiler changes,
  layout inputs, content manifest, and executable rebuild script.
- CoreMark port and sources: `https://github.com/mikro-design/rv32sim.py.git`,
  commit `324adf10be1886ab74e8abd04b36e17e8e11369e`, directory
  `tests/coremark`.
- Every source, header, startup, and linker-script input is independently
  pinned by `contrib/onio-zero/coremark-source-324adf10.sha256`.  The rebuild
  script verifies that manifest before invoking the compiler.
- GNU Binutils 2.43 release tarball, SHA-256
  `b53606f443ac8f01d1d5fc9c39497f2af322d99e14cea5c0b4b124d630379365`.
- GCC prerequisites are the versions and SHA-512 values pinned by the tagged
  GCC tree's `contrib/download_prerequisites`: GMP 6.3.0, MPFR 4.2.2, MPC
  1.3.1, and ISL 0.24.

The public rv32sim repository was republished as source snapshot `324adf10`.
Older rv32sim commit identifiers in the historical screening notes are no
longer present in that repository.  The content manifest, rather than a claim
that those vanished identifiers remain fetchable, pins the actual benchmark
inputs used here.

## Clean toolchain build

The commands below use at most four build jobs.  Choose a new empty working
directory and a writable installation prefix:

```sh
mkdir onio-coremark-reproduction
cd onio-coremark-reproduction
work=$PWD
prefix=$work/toolchain
```

Build and install GNU Binutils 2.43:

```sh
curl -LO https://sourceware.org/pub/binutils/releases/binutils-2.43.tar.xz
echo 'b53606f443ac8f01d1d5fc9c39497f2af322d99e14cea5c0b4b124d630379365  binutils-2.43.tar.xz' |
  sha256sum --check
tar -xf binutils-2.43.tar.xz
mkdir build-binutils
cd build-binutils
../binutils-2.43/configure \
  --target=riscv32-unknown-elf --prefix="$prefix" \
  --disable-nls --disable-werror --disable-gdb --disable-sim
make -j4
make install
cd "$work"
```

Clone the tagged GCC source, fetch its checksum-pinned prerequisites, and
build the compiler plus target `libgcc`:

```sh
git clone https://github.com/onio-git/gcc.git gcc
git -C gcc checkout onio-zero-coremark-repro-2026-08-31
(cd gcc && ./contrib/download_prerequisites)
mkdir build-gcc
cd build-gcc
PATH="$prefix/bin:$PATH" ../gcc/configure \
  --target=riscv32-unknown-elf --prefix="$prefix" \
  --with-arch=rv32imc_zicsr_zba_zbb_zbs_zifencei --with-abi=ilp32 \
  --disable-multilib --with-newlib --without-headers --disable-shared \
  --disable-threads --disable-nls --disable-libssp --disable-libquadmath \
  --disable-libgomp --enable-languages=c,lto
PATH="$prefix/bin:$PATH" make -j4 all-gcc all-target-libgcc
cd "$work"
```

No newlib objects are linked into these freestanding ELFs.  `libgcc` is the
only target library used.

## One-command ELF rebuild

Fetch the source snapshot and invoke the tagged rebuild script.  The output
directory must not already exist:

```sh
git clone https://github.com/mikro-design/rv32sim.py.git rv32sim.py
git -C rv32sim.py checkout 324adf10be1886ab74e8abd04b36e17e8e11369e

PATH="$prefix/bin:$PATH" \
ONIO_GCC_BUILD="$work/build-gcc" \
ONIO_AS="$prefix/bin/riscv32-unknown-elf-as" \
ONIO_LD="$prefix/bin/riscv32-unknown-elf-ld" \
ONIO_SIZE="$prefix/bin/riscv32-unknown-elf-size" \
"$work/gcc/contrib/onio-zero/rebuild-coremark-candidates.sh" \
  "$work/rv32sim.py/tests/coremark" "$work/coremark-output"
```

The script:

1. validates every CoreMark input against the committed source manifest;
2. records compiler/linker versions and tool hashes in `toolchain.txt`;
3. records every shell-escaped compilation and link command in
   `commands.log`;
4. builds the target-default image and both selective-LTO layout images; and
5. exits unsuccessfully unless all three resulting ELF SHA-256 values match.

Expected output:

```text
b39a281daa94a30b2f2bcca2d00b2239bce65f7a78cd35ec3c68d856c6d86860  coremark-target-default-b39a281d.elf
32e5301104b85dfcab4a723799c654ea6cfd375be66d013b031ceb620e386ef8  coremark-layout-unpadded-32e53011.elf
11e9ac9629d47bae7ee27654901cbc7551f710641a7e77f5a892dd4ef7e86f48  coremark-layout-pad16-11e9ac96.elf
```

## Local independent rebuild evidence

The script was run from a new output directory using the source snapshot and
the GCC build tree.  It recreated all three ELFs byte for byte:

| Image | text/data/BSS | SHA-256 |
|---|---:|---|
| Target default | 14,704/12/20 | `b39a281daa94a30b2f2bcca2d00b2239bce65f7a78cd35ec3c68d856c6d86860` |
| New unpadded | 12,936/12/20 | `32e5301104b85dfcab4a723799c654ea6cfd375be66d013b031ceb620e386ef8` |
| New 16-byte layout | 12,952/12/20 | `11e9ac9629d47bae7ee27654901cbc7551f710641a7e77f5a892dd4ef7e86f48` |

As a negative control, the previously installed compiler had the same
`17.0.0 20260602` version text but recreated the older 14,768-byte baseline
with SHA-256 `77baa9a36f4ce11a7225bbbd3f447b2a0ad5f3a309c2493b7c330fcbda953b28`.
This confirms why version text and an ELF hash alone were not an adequate
handoff; the tagged GCC source and executable build procedure are required.

## Full public-source end-to-end verification

After publishing the input tag, the complete procedure above was run in a new
temporary directory on `x86_64` Ubuntu 22.04 with the system GCC 11.4.0 host
compiler.  The verification:

- downloaded the Binutils 2.43 tarball and passed its recorded SHA-256 check;
- configured, built, and installed Binutils into a new empty prefix;
- shallow-cloned the public GCC tag at
  `a18c92d150277e9233b3e4c1fac2e719b0a1243d`;
- downloaded the checksum-pinned GCC prerequisites;
- configured GCC with the commands above and built `all-gcc` plus
  `all-target-libgcc` with four jobs;
- shallow-cloned the public rv32sim snapshot at
  `324adf10be1886ab74e8abd04b36e17e8e11369e`; and
- ran the tagged rebuild script with only the newly built RISC-V tools.

The clean compiler and target-library executables had different host-file
hashes from the earlier incremental build, as expected for separately built
host tools, but generated all three target ELFs byte for byte with the expected
SHA-256 values and section sizes.  This is the independent reconstruction
evidence; the ELF hashes are its verification outputs.
