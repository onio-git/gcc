# ONiO.zero embsim resimulation — 2026-09-02

> **Superseded performance model:** the Aldebaran implementation used here
> incorrectly kept one preferred way per cache set. See
> `2026-09-02-cache-model-audit.md` and embsim
> `6277348b9a24bf1284b6108ac57f4b8bd08a223d`. The archived measurements below
> remain useful as historical evidence but are not board-calibrated rankings.

## Outcome

Every surviving CoreMark ELF artifact was reprocessed with `embsim`, and the
two GCC DejaGNU gate suites were moved to `embsim`. No current simulation in
this workflow invokes `rv32sim.py` or `mikrosim`.

The CoreMark archive contains 181 ELF paths representing 164 distinct SHA-256
values. Of those, 176 paths (162 distinct binaries) reached and completed the
timed window. Five paths (two distinct binaries) are old diagnostic builds
that repeatedly trap and do not reach a timed window within the one-million
instruction cap; they are retained as explicit errors in the CSV and excluded
from performance comparisons.

The published comparison points reproduce exactly:

| Image | SHA-256 | Instructions | Cycles | Fetch penalty | Branch penalty | Mispredictions | CoreMark/MHz |
|---|---|---:|---:|---:|---:|---:|---:|
| Preserved baseline | `77baa9a36f4ce11a7225bbbd3f447b2a0ad5f3a309c2493b7c330fcbda953b28` | 184,339 | 197,051 | 7,324 | 5,388 | 666 | 5.074828 |
| Target default | `b39a281daa94a30b2f2bcca2d00b2239bce65f7a78cd35ec3c68d856c6d86860` | 184,241 | 196,736 | 7,107 | 5,388 | 666 | 5.082954 |
| Selective LTO | `d88ee4fc30e9382c0437d116a75ae27fc4358b82b66b7c97b0d51c6c36ff7125` | 183,205 | 195,699 | 7,112 | 5,382 | 666 | 5.109888 |
| Final layout, unpadded | `32e5301104b85dfcab4a723799c654ea6cfd375be66d013b031ceb620e386ef8` | 183,205 | 195,267 | 6,680 | 5,382 | 666 | 5.121193 |
| Final layout, 16-byte pad | `11e9ac9629d47bae7ee27654901cbc7551f710641a7e77f5a892dd4ef7e86f48` | 183,205 | 194,988 | 6,401 | 5,382 | 666 | 5.128521 |

The archived `04ebff14…` old padded ELF is no longer present. Two surviving
relinks, `85789d7f…` and `b5431e18…`, reproduce its published 183,205
instructions, 195,537 cycles, 6,950 fetch-penalty cycles, 5,382 branch-penalty
cycles, and 666 mispredictions. They are equivalent counter checks, not claims
to recreate the missing exact bytes.

Across the complete archive, the final 16-byte layout remains the winner at
194,988 cycles. A second, symbolically different pad-16 ELF (`d4acb8b8…`)
ties it. No previously rejected layout became better under `embsim`.

## Model and measurement window

The model is a direct data-format translation of historical
`tests/coremark/onio_model.json` from rv32sim commit
`b4fd02cb3324cd0df0cb4df5e472520722931890`, SHA-256
`5fffda831f7fe9dd3054cdbe816c088c21c9c16ea48b12e97bc5e7de9975dfdf`.
It retains:

- one base cycle per instruction;
- the eight-entry backward-only, word-aliased branch predictor;
- zero load and store penalties; and
- the 2-KiB, two-way Aldebaran instruction cache with a zero-cycle
  preferred-way hit, one-cycle soft miss, ten-cycle hard miss, and MRU-way
  replacement on a full set.

Ordinary LRU is not equivalent to this cache. `embsim` therefore gained an
explicit `aldebaran-icache` memory-interface model rather than approximating
the result with its generic cache.

`run-coremark-embsim.py` discovers every `start_time` return and `stop_time`
call in `main`, runs until the dynamically reached start return, resets only
the instruction/cycle/performance counters while preserving warmed machine
state, and stops before executing the matching `stop_time` call. This is the
same interval used by the historical `onio_window_bench.py` helper. Every row
also checks that total cycles equal instructions plus all modeled penalties.

The three externally handed-off images were additionally run to program exit
with `embsim --run`. All exit with status zero and print the expected CRCs:

```text
seedcrc      0xe9f5
crclist      0xe714
crcmatrix    0x1fd7
crcstate     0x8e3a
crcfinal     0xe714
```

Their end-to-end reported ticks are also unchanged: 189,633 for `b39a281d…`
and 188,591 for both `32e53011…` and `11e9ac96…`.

## DejaGNU migration

The first `embsim` gate exposed a simulator-side harness gap rather than a GCC
regression: newlib's `_sbrk` received `-ENOSYS` from syscall 214, so tests that
needed `malloc` timed out. The final simulator commit implements a stateful,
memory-map-bounded `brk`, initializes it from the ELF `_end` symbol (or the
loadable-image end for a stripped ELF), and preserves it in snapshots.
`20000914-1.c` and `20051113-1.c`, including their `-O0` variants, then passed.

The complete four-worker gate was rerun with the clean simulator executable
listed below and passed with no new failures against the 1,062-entry checked-in
baseline:

| Suite | PASS | FAIL | XFAIL | UNRESOLVED | UNSUPPORTED |
|---|---:|---:|---:|---:|---:|
| `gcc.target/riscv` | 31,682 | 872 | 58 | 48 | 7,398 |
| `gcc.c-torture/execute` | 24,414 | 22 | 0 | 0 | 110 |

The RISC-V target-suite counts are unchanged. In the execute suite, 120
previous baseline failures now pass because `brk` and the configured heap are
available; the 22 remaining failures are all present in the baseline. The
sorted unexpected-result set contains 942 entries, has SHA-256
`176c1d221ddbc7fa5fc9026abeaa65d8b292d8e53acf012a7c6667d64b70613c`,
and has zero additions and 120 removals relative to the baseline.

## Reproduction

Check out the public simulator commit, build it, place the ONiO RISC-V
binutils in `PATH`, and run:

```sh
git clone https://github.com/mikro-design/embsim.git /path/to/embsim
git -C /path/to/embsim checkout 03bffbf82b1dbe0b874ed9a1d8048bb369467c07
cargo build --release --manifest-path /path/to/embsim/Cargo.toml --bin embsim

EMBSIM=/path/to/embsim/target/release/embsim \
RISCV_OBJDUMP=riscv32-unknown-elf-objdump \
  contrib/onio-zero/run-coremark-embsim.py \
    --path-root=/path/to/rv32sim.py \
    --csv=contrib/onio-zero/results/2026-09-02-embsim-coremark-all.csv \
    /path/to/rv32sim.py/tests/coremark

DEJAGNU_SIM=/path/to/embsim/target/release/embsim \
  contrib/onio-zero/onio-dejagnu-gate.sh --jobs 4 \
    --outdir /path/to/gate-results
```

`rv32sim.py` in this command is only the pinned CoreMark source/artifact
archive from `https://github.com/mikro-design/rv32sim.py.git` at commit
`324adf10be1886ab74e8abd04b36e17e8e11369e`.
Every ELF execution is performed by the `EMBSIM` executable.

The files used for this run have these SHA-256 values:

| File | SHA-256 |
|---|---|
| Clean `embsim` release executable used for the authoritative run | `f5557a4321ea296befc9c7ab541b955ba96e33f70a5ad412468ceff7ad272903` |
| GCC `cc1` tested by the DejaGNU gate | `1e7804747ae6801abeee405b430a336b2f840b049ef6e330cd292852e6036dcd` |
| GCC `xgcc` tested by the DejaGNU gate | `12686630549aa9ec1d7c9798a935edfd7b44a00e7783b2dd15d8c5bca9d89982` |
| `embsim-onio-zero-coremark.json` | `9736eb2ac1132d02b7e89ba2970d649f050800431752397fa3a7c3b87118bd1f` |
| `run-coremark-embsim.py` | `77eca601649f4aed852b3934a79f51d69a8bab9247c4b4d7aaa7881308284b97` |
| `2026-09-02-embsim-coremark-all.csv` | `0069e9c8b73b7841aed19744072a7dbcf37310207f8a1d0e38e9c42b08a9d7f1` |
