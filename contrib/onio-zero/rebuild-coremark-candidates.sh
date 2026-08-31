#!/usr/bin/env bash
# Rebuild the three hash-controlled ONiO.zero CoreMark board candidates.

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
EOF
  exit 2
}

[[ $# -eq 2 ]] || usage

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source_dir=$(cd -- "$1" && pwd)
output_dir=$2

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
  -I. -DITERATIONS=1 -DTOTAL_DATA_SIZE=2000 -DCPU_FREQ_HZ=32000000
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
    -o coremark-target-default-b39a281d.elf
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
    -Wl,-Map=coremark-unpadded.map -o coremark-layout-unpadded-32e53011.elf
  run_in "$build" "${cc[@]}" "${objects[@]}" coremark-cache-pad-16.o \
    "${link_flags[@]}" -Wl,-Map=coremark-pad16.map \
    -o coremark-layout-pad16-11e9ac96.elf
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
  "$output_dir/target-default/coremark-target-default-b39a281d.elf" \
  "$output_dir/layout/coremark-layout-unpadded-32e53011.elf" \
  "$output_dir/layout/coremark-layout-pad16-11e9ac96.elf"

check_hash \
  "$output_dir/target-default/coremark-target-default-b39a281d.elf" \
  b39a281daa94a30b2f2bcca2d00b2239bce65f7a78cd35ec3c68d856c6d86860
check_hash \
  "$output_dir/layout/coremark-layout-unpadded-32e53011.elf" \
  32e5301104b85dfcab4a723799c654ea6cfd375be66d013b031ceb620e386ef8
check_hash \
  "$output_dir/layout/coremark-layout-pad16-11e9ac96.elf" \
  11e9ac9629d47bae7ee27654901cbc7551f710641a7e77f5a892dd4ef7e86f48
