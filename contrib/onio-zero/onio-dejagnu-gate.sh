#!/bin/sh
# Run the relevant RV32IMC/ONiO.zero DejaGNU gate.
#
# The full GCC testsuite is not a meaningful gate for this configuration: it
# schedules RVV, rv64, and other-target tests that a scalar rv32imc compiler
# can never satisfy.  This gate runs only the suites whose results depend on
# RV32IMC code generation and on the simulator, then compares the run against
# a recorded baseline of known-failing tests.  Filtering is done by comparison
# rather than by name, so a newly broken test is reported even if it lives
# beside known-unsupported ones.
#
# Usage:
#   onio-dejagnu-gate.sh [--update-baseline] [--jobs N] [--outdir DIR]
#
# Requires the environment from onio-zero-dev.env.

set -eu

: "${ONIO_GCC_SRC:?source onio-zero-dev.env first}"
: "${ONIO_BUILD:?source onio-zero-dev.env first}"
: "${ONIO_EMBSIM:?source onio-zero-dev.env first}"

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BASELINE="$HERE/onio-dejagnu-gate-baseline.txt"
OUTDIR="$ONIO_BUILD/gcc/testsuite-onio-gate"
UPDATE=0
JOBS=4

while [ $# -gt 0 ]; do
  case "$1" in
    --update-baseline) UPDATE=1 ;;
    --jobs) JOBS=$2; shift ;;
    --outdir) OUTDIR=$2; shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done

# Suites relevant to RV32IMC/ONiO.zero.  gcc.target/riscv/riscv.exp matches
# only gcc.target/riscv/*.c, so the RVV suites under rvv/ stay out by
# construction.

# DejaGNU links test programs at 0x10000 and lets newlib's heap grow from _end.
# Explicit regions replace embsim's automatic ELF coverage, so describe both
# the original 64-KiB program range and the enlarged heap range.
DEJAGNU_SIM_OPTIONS="--no-gdb-server --run --fast --timeout=60 --mem-region=0x10000:0x10000:program --mem-region=0x20000:0x7E0000:heap"
export DEJAGNU_SIM_OPTIONS
export DEJAGNU_SIM="${DEJAGNU_SIM:-$ONIO_EMBSIM/target/release/embsim}"

if [ ! -x "$DEJAGNU_SIM" ]; then
  echo "embsim executable not found: $DEJAGNU_SIM" >&2
  echo "build it with: cargo build --release --manifest-path $ONIO_EMBSIM/Cargo.toml --bin embsim" >&2
  exit 2
fi

mkdir -p "$OUTDIR"

# A baseline is only meaningful if one compiler produced every result in it.
# Anything that rebuilds cc1 mid-run - a concurrent session, an editor-driven
# make - silently mixes compilers, so identify the compiler up front and
# verify it afterwards.
compiler_id () {
  sha256sum "$ONIO_BUILD/gcc/cc1" "$ONIO_BUILD/gcc/xgcc" 2>/dev/null | awk '{print $1}'
}
COMPILER_BEFORE=$(compiler_id)
if [ -z "$COMPILER_BEFORE" ]; then
  echo "cannot identify the compiler under $ONIO_BUILD/gcc" >&2
  exit 2
fi
GCC_UNDER_TEST="$ONIO_BUILD/gcc/xgcc -B$ONIO_BUILD/gcc/"

# GCC's testsuite supplies a cooperative parallelizer for DejaGNU. Each worker
# enumerates the complete suite; marker files assign each batch to exactly one
# worker. Run the suites in turn with JOBS workers and merge their summaries.
case "$JOBS" in
  ''|*[!0-9]*|0) echo "--jobs must be a positive integer" >&2; exit 2 ;;
esac

RUN_ID="embsim-$(date +%Y%m%d-%H%M%S)-$$"
RESULT_DIRS=
run_suite () {
  label=$1
  exp=$2
  markers="$OUTDIR/$RUN_ID-$label-markers"
  mkdir -p "$markers"
  worker=1
  while [ "$worker" -le "$JOBS" ]; do
    dir="$OUTDIR/$RUN_ID-$label-worker$worker"
    RESULT_DIRS="$RESULT_DIRS $dir"
    mkdir -p "$dir"
    {
      sed "s|set tmpdir .*|set tmpdir $dir|" "$ONIO_BUILD/gcc/site.exp"
      echo "set GCC_UNDER_TEST {$GCC_UNDER_TEST}"
    } > "$dir/site.exp"
    echo "running $exp worker $worker/$JOBS in $dir"
    (
      export GCC_RUNTEST_PARALLELIZE_DIR="$markers"
      cd "$dir"
      runtest --tool gcc --srcdir "$ONIO_GCC_SRC/gcc/testsuite" \
        --target_board=generic-sim/-mcpu=onio-zero "$exp" > runtest.out 2>&1 || true
    ) &
    worker=$((worker + 1))
  done
  wait
}

run_suite riscv gcc.target/riscv/riscv.exp
run_suite execute gcc.c-torture/execute/execute.exp

found=0
for dir in $RESULT_DIRS; do
  [ -f "$dir/gcc.sum" ] || continue
  found=1
done
if [ "$found" -eq 0 ]; then
  echo "no gcc.sum was produced; see $OUTDIR/$RUN_ID-*/runtest.out" >&2
  exit 2
fi

# Normalize to result lines that are stable between runs, dropping timing and
# summary noise.
for dir in $RESULT_DIRS; do
  [ -f "$dir/gcc.sum" ] && cat "$dir/gcc.sum"
done > "$OUTDIR/gcc.sum"
grep -E '^(FAIL|UNRESOLVED|ERROR|XPASS):' "$OUTDIR/gcc.sum" | sort > "$OUTDIR/gate-results.txt" || true

if [ "$COMPILER_BEFORE" != "$(compiler_id)" ]; then
  echo >&2
  echo "the compiler was rebuilt while this run was in progress, so its" >&2
  echo "results span more than one compiler and cannot be trusted." >&2
  echo "results kept for inspection: $OUTDIR/gate-results.txt" >&2
  exit 3
fi

if [ "$UPDATE" -eq 1 ]; then
  cp "$OUTDIR/gate-results.txt" "$BASELINE"
  {
    echo "# Recorded from $(cd "$ONIO_GCC_SRC" && git rev-parse --short HEAD)"
    echo "# Working-tree diff sha256: $(cd "$ONIO_GCC_SRC" && git diff | sha256sum | cut -d' ' -f1)"
  } > "$BASELINE.provenance"
  echo "baseline updated: $BASELINE ($(wc -l < "$BASELINE") known failures)"
  cat "$BASELINE.provenance"
  exit 0
fi

if [ ! -f "$BASELINE" ]; then
  echo "no baseline at $BASELINE; rerun with --update-baseline" >&2
  exit 2
fi

NEW=$(comm -13 "$BASELINE" "$OUTDIR/gate-results.txt" || true)
FIXED=$(comm -23 "$BASELINE" "$OUTDIR/gate-results.txt" || true)

if [ -n "$FIXED" ]; then
  echo
  echo "tests that no longer fail (update the baseline once this is intended):"
  echo "$FIXED"
fi

if [ -n "$NEW" ]; then
  echo
  echo "GATE FAILED: newly failing tests:"
  echo "$NEW"
  exit 1
fi

echo "gate passed: no new failures against $(wc -l < "$BASELINE") baseline entries"
