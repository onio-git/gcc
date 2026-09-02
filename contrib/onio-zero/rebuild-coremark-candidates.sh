#!/usr/bin/env bash
# Rebuild the hash-controlled ONiO.zero CoreMark comparison images.

set -euo pipefail

usage() {
  cat >&2 <<'EOF'
Usage: rebuild-coremark-candidates.sh COREMARK_SOURCE_DIR OUTPUT_DIR

COREMARK_SOURCE_DIR must be tests/coremark from rv32sim.py commit
324adf10be1886ab74e8abd04b36e17e8e11369e.  File contents are checked before
building.  OUTPUT_DIR must not already exist.

Environment:
  ONIO_CC          installed compiler (default: riscv32-unknown-elf-gcc)
  ONIO_GCC_BUILD   optional GCC build directory; uses gcc/xgcc -B.../gcc/
  ONIO_AS          assembler recorded with the build provenance
                   (default: riscv32-unknown-elf-as)
  ONIO_LD          linker checked for GNU ld 2.43 and ordering-file support
                   (default: riscv32-unknown-elf-ld)
  ONIO_SIZE        size tool (default: riscv32-unknown-elf-size)
  ONIO_ITERATIONS  compile-time CoreMark iteration count (default: 1)
EOF
  exit 2
}

[[ $# -eq 2 ]] || usage

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source_dir=$(cd -- "$1" && pwd)
output_dir=$2
iterations=${ONIO_ITERATIONS:-1}
if [[ ! $iterations =~ ^[1-9][0-9]*$ ]]; then
  printf 'error: ONIO_ITERATIONS must be a positive integer: %s\n' \
    "$iterations" >&2
  exit 2
fi

if [[ -e $output_dir ]]; then
  printf 'error: output directory already exists: %s\n' "$output_dir" >&2
  exit 2
fi
mkdir -p -- "$output_dir"
output_dir=$(cd -- "$output_dir" && pwd)

cc_path=${ONIO_CC:-riscv32-unknown-elf-gcc}
as_path=${ONIO_AS:-riscv32-unknown-elf-as}
ld_path=${ONIO_LD:-riscv32-unknown-elf-ld}
size_path=${ONIO_SIZE:-riscv32-unknown-elf-size}

if [[ $iterations == 1 ]]; then
  target_hash=9b49f59e6f1a47dd682765ec399885b0bc89e9ef0037af3960a7c96eff9ab3e7
  unpadded_hash=edf20b1c59b488f00e2e00b9f4874ce6836539335a975e628f3fdd999827c4ce
  padded_hash=9846d1bcd14bc39ac1904eec2d1e6f3b3592cec2576985670989d953795393b8
  target_elf=coremark-target-default-${target_hash:0:8}.elf
  unpadded_elf=coremark-layout-unpadded-${unpadded_hash:0:8}.elf
  padded_elf=coremark-layout-pad16-${padded_hash:0:8}.elf
else
  target_elf=coremark-target-default-iterations-$iterations.elf
  unpadded_elf=coremark-layout-unpadded-iterations-$iterations.elf
  padded_elf=coremark-layout-pad16-iterations-$iterations.elf
fi

cc=("$cc_path")
if [[ -n ${ONIO_GCC_BUILD:-} ]]; then
  gcc_build=$(cd -- "$ONIO_GCC_BUILD" && pwd)
  target_libgcc=$gcc_build/riscv32-unknown-elf/libgcc
  [[ -f $target_libgcc/libgcc.a ]] || {
    printf 'error: target libgcc is not built: %s\n' \
      "$target_libgcc/libgcc.a" >&2
    exit 2
  }
  cc=(
    "$gcc_build/gcc/xgcc"
    "-B$gcc_build/gcc/"
    "-B$target_libgcc/"
  )
fi

for tool in sha256sum "$as_path" "$ld_path" "$size_path"; do
  command -v "$tool" >/dev/null || {
    printf 'error: required tool not found: %s\n' "$tool" >&2
    exit 2
  }
done
if [[ ${cc[0]} == */* ]]; then
  [[ -x ${cc[0]} ]] || {
    printf 'error: compiler is not executable: %s\n' "${cc[0]}" >&2
    exit 2
  }
else
  command -v "${cc[0]}" >/dev/null || {
    printf 'error: compiler not found: %s\n' "${cc[0]}" >&2
    exit 2
  }
fi

manifest=$script_dir/coremark-source-324adf10.sha256
(cd -- "$source_dir" && sha256sum --check "$manifest")

target=$("${cc[@]}" -dumpmachine)
[[ $target == riscv32-unknown-elf ]] || {
  printf 'error: compiler target is %s, expected riscv32-unknown-elf\n' \
    "$target" >&2
  exit 2
}
"$ld_path" --version | head -n 1 | grep -F 'GNU ld (GNU Binutils) 2.43' >/dev/null || {
  printf 'error: ONIO_LD must be GNU ld 2.43\n' >&2
  exit 2
}
"$ld_path" --help | grep -F -- '--section-ordering-file' >/dev/null || {
  printf 'error: linker lacks --section-ordering-file\n' >&2
  exit 2
}

export LC_ALL=C
export TZ=UTC

commands_log=$output_dir/commands.log
: >"$commands_log"

run_in() {
  local directory=$1
  shift
  {
    printf '(cd %q &&' "$directory"
    printf ' %q' "$@"
    printf ')\n'
  } >>"$commands_log"
  (cd -- "$directory" && "$@")
}

common=(
  -I. "-DITERATIONS=$iterations" -DTOTAL_DATA_SIZE=2000 -DCPU_FREQ_HZ=32000000
  -march=rv32imc_zicsr_zba_zbb_zbs_zifencei -mabi=ilp32
  -ffreestanding -fno-builtin -Wall -Wextra -Wno-unused-parameter
  -O2 -mcpu=onio-zero -std=c99
)
matrix=(
  -fno-inline-functions -fno-inline-functions-called-once
  -floop-unroll-and-jam --param unroll-jam-min-percent=0
  --param unroll-jam-max-unroll=8 --param max-unrolled-insns=6000
)
state=(--param max-inline-insns-auto=160)
objects=(
  core_list_join.o core_main.o core_matrix.o core_state.o core_util.o
  core_portme.o ee_printf.o startup.o
)

compile_target_default() {
  local build=$output_dir/target-default
  mkdir -p -- "$build"
  run_in "$source_dir" "${cc[@]}" "${common[@]}" -c core_list_join.c \
    -o "$build/core_list_join.o"
  run_in "$source_dir" "${cc[@]}" "${common[@]}" -c core_main.c \
    -o "$build/core_main.o"
  run_in "$source_dir" "${cc[@]}" "${common[@]}" "${matrix[@]}" \
    -c core_matrix.c -o "$build/core_matrix.o"
  run_in "$source_dir" "${cc[@]}" "${common[@]}" "${state[@]}" \
    -c core_state.c -o "$build/core_state.o"
  run_in "$source_dir" "${cc[@]}" "${common[@]}" -c core_util.c \
    -o "$build/core_util.o"
  run_in "$source_dir" "${cc[@]}" "${common[@]}" -c core_portme.c \
    -o "$build/core_portme.o"
  run_in "$source_dir" "${cc[@]}" "${common[@]}" -c ee_printf.c \
    -o "$build/ee_printf.o"
  run_in "$source_dir" "${cc[@]}" "${common[@]}" -c startup.S \
    -o "$build/startup.o"

  run_in "$build" "${cc[@]}" "${objects[@]}" \
    -march=rv32imc_zicsr_zba_zbb_zbs_zifencei -mabi=ilp32 \
    -ffreestanding -fno-builtin -Wall -Wextra -Wno-unused-parameter \
    -O2 -mcpu=onio-zero -nostdlib -Wl,--gc-sections \
    -Wl,-Map=coremark.map -T "$source_dir/linker.ld" -lgcc \
    -o "$target_elf"
}

compile_layout_candidates() {
  local build=$output_dir/layout
  mkdir -p -- "$build"
  run_in "$source_dir" "${cc[@]}" "${common[@]}" -flto=4 \
    -ffunction-sections -c core_list_join.c -o "$build/core_list_join.o"
  run_in "$source_dir" "${cc[@]}" "${common[@]}" -c core_main.c \
    -o "$build/core_main.o"
  run_in "$source_dir" "${cc[@]}" "${common[@]}" "${matrix[@]}" \
    -ffunction-sections -c core_matrix.c -o "$build/core_matrix.o"
  run_in "$source_dir" "${cc[@]}" "${common[@]}" "${state[@]}" \
    -ffunction-sections -c core_state.c -o "$build/core_state.o"
  run_in "$source_dir" "${cc[@]}" "${common[@]}" -flto=4 \
    -ffunction-sections -c core_util.c -o "$build/core_util.o"
  run_in "$source_dir" "${cc[@]}" "${common[@]}" -c core_portme.c \
    -o "$build/core_portme.o"
  run_in "$source_dir" "${cc[@]}" "${common[@]}" -c ee_printf.c \
    -o "$build/ee_printf.o"
  run_in "$source_dir" "${cc[@]}" "${common[@]}" -c startup.S \
    -o "$build/startup.o"
  run_in "$script_dir" "${cc[@]}" \
    -march=rv32imc_zicsr_zba_zbb_zbs_zifencei -mabi=ilp32 \
    -c coremark-cache-pad-16.S -o "$build/coremark-cache-pad-16.o"

  local link_flags=(
    -march=rv32imc_zicsr_zba_zbb_zbs_zifencei -mabi=ilp32
    -ffreestanding -fno-builtin -O2 -mcpu=onio-zero -ffunction-sections
    -flto=4 -flto-partition=none -nostdlib -Wl,--gc-sections
    "-Wl,--section-ordering-file=$script_dir/coremark-matrix-layout.order"
    -T "$source_dir/linker.ld" -lgcc
  )
  run_in "$build" "${cc[@]}" "${objects[@]}" "${link_flags[@]}" \
    -Wl,-Map=coremark-unpadded.map -o "$unpadded_elf"
  run_in "$build" "${cc[@]}" "${objects[@]}" coremark-cache-pad-16.o \
    "${link_flags[@]}" -Wl,-Map=coremark-pad16.map \
    -o "$padded_elf"
}

record_tools() {
  {
    printf 'compiler-command:'
    printf ' %q' "${cc[@]}"
    printf '\n'
    "${cc[@]}" -v 2>&1
    printf '\n'
    "$ld_path" --version | head -n 2
    "$size_path" --version | head -n 1
    printf '\nSHA-256 of executable tool inputs:\n'
    local tool
    local tools=("${cc[0]}" "$as_path" "$ld_path" "$size_path")
    if [[ -n ${gcc_build:-} ]]; then
      tools+=(
        "$gcc_build/gcc/cc1"
        "$gcc_build/gcc/lto1"
        "$gcc_build/gcc/lto-wrapper"
        "$gcc_build/gcc/collect2"
        "$gcc_build/gcc/liblto_plugin.so"
        "$target_libgcc/libgcc.a"
      )
    fi
    for tool in "${tools[@]}"; do
      if resolved=$(command -v "$tool" 2>/dev/null); then
        sha256sum "$resolved"
      elif [[ -f $tool ]]; then
        sha256sum "$tool"
      fi
    done
  } >"$output_dir/toolchain.txt"
}

check_hash() {
  local file=$1
  local expected=$2
  local actual
  actual=$(sha256sum "$file")
  actual=${actual%% *}
  if [[ $actual != "$expected" ]]; then
    printf 'FAIL  %s\n  expected %s\n  actual   %s\n' \
      "$file" "$expected" "$actual" >&2
    return 1
  fi
  printf 'OK    %s  %s\n' "$expected" "$file"
}

record_tools
compile_target_default
compile_layout_candidates

"$size_path" \
  "$output_dir/target-default/$target_elf" \
  "$output_dir/layout/$unpadded_elf" \
  "$output_dir/layout/$padded_elf"

if [[ $iterations == 1 ]]; then
  check_hash \
    "$output_dir/target-default/$target_elf" \
    "$target_hash"
  check_hash \
    "$output_dir/layout/$unpadded_elf" \
    "$unpadded_hash"
  check_hash \
    "$output_dir/layout/$padded_elf" \
    "$padded_hash"
else
  sha256sum \
    "$output_dir/target-default/$target_elf" \
    "$output_dir/layout/$unpadded_elf" \
    "$output_dir/layout/$padded_elf"
fi
