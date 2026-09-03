#!/usr/bin/env bash
# Build the board-callable cache-policy and instruction-word probes.

set -euo pipefail

usage() {
  cat >&2 <<'EOF'
Usage: build-probes.sh COREMARK_SOURCE_DIR OUTPUT_DIR

COREMARK_SOURCE_DIR is tests/coremark from rv32sim.py commit
324adf10be1886ab74e8abd04b36e17e8e11369e.  OUTPUT_DIR must not exist.

Environment:
  ONIO_CC          installed compiler (default: riscv32-unknown-elf-gcc)
  ONIO_GCC_BUILD   optional GCC build directory; uses gcc/xgcc and its libgcc
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
command -v "$size_path" >/dev/null || {
  printf 'error: size tool not found: %s\n' "$size_path" >&2
  exit 2
}

(cd -- "$source_dir" && \
  sha256sum --check "$script_dir/../coremark-source-324adf10.sha256")
[[ $("${cc[@]}" -dumpmachine) == riscv32-unknown-elf ]] || {
  printf 'error: compiler target is not riscv32-unknown-elf\n' >&2
  exit 2
}

export LC_ALL=C
export TZ=UTC
commands_log=$output_dir/commands.log
: >"$commands_log"

run() {
  local label=$1
  shift
  local command=("$@")
  {
    printf '# %s\n' "$label"
    printf '%q' "${command[0]}"
    printf ' %q' "${command[@]:1}"
    printf '\n'
  } >>"$commands_log"
  "${command[@]}"
}

common=(
  "-I$source_dir" -DITERATIONS=1 -DTOTAL_DATA_SIZE=2000
  -DCPU_FREQ_HZ=32000000
  -march=rv32imc_zicsr_zba_zbb_zbs_zifencei -mabi=ilp32
  -ffreestanding -fno-builtin -ffunction-sections -fdata-sections
  -O2 -mcpu=onio-zero
)

compile() {
  local input=$1
  local output=$2
  shift 2
  run compile "${cc[@]}" "${common[@]}" "$@" -c "$input" -o "$output"
}

compile "$source_dir/startup.S" "$output_dir/startup.o"
compile "$source_dir/core_portme.c" "$output_dir/core_portme.o" -std=c99
compile "$source_dir/ee_printf.c" "$output_dir/ee_printf.o" -std=c99
compile "$script_dir/replacement-probe.S" "$output_dir/replacement-probe.o"
compile "$script_dir/split-word-probe.S" "$output_dir/split-word-probe.o"
compile "$script_dir/probe-main.c" "$output_dir/replacement-main.o" -std=c99
compile "$script_dir/probe-main.c" "$output_dir/split-main.o" \
  -std=c99 -DONIO_SPLIT_WORD_PROBE

link_common=(
  -march=rv32imc_zicsr_zba_zbb_zbs_zifencei -mabi=ilp32
  -nostdlib -Wl,--gc-sections -T "$source_dir/linker.ld" -lgcc
)
run link-replacement "${cc[@]}" \
  "$output_dir/startup.o" "$output_dir/replacement-main.o" \
  "$output_dir/replacement-probe.o" "$output_dir/core_portme.o" \
  "$output_dir/ee_printf.o" "${link_common[@]}" \
  "-Wl,-Map=$output_dir/replacement-probe.map" \
  -o "$output_dir/replacement-probe.elf"
run link-split-word "${cc[@]}" \
  "$output_dir/startup.o" "$output_dir/split-main.o" \
  "$output_dir/split-word-probe.o" "$output_dir/core_portme.o" \
  "$output_dir/ee_printf.o" "${link_common[@]}" \
  "-Wl,-Map=$output_dir/split-word-probe.map" \
  -o "$output_dir/split-word-probe.elf"

{
  printf 'compiler-command:'
  printf ' %q' "${cc[@]}"
  printf '\n'
  "${cc[@]}" -v 2>&1
  printf '\nSHA-256 of source inputs:\n'
  sha256sum \
    "$script_dir/build-probes.sh" \
    "$script_dir/probe-main.c" \
    "$script_dir/replacement-probe.S" \
    "$script_dir/split-word-probe.S" \
    "$source_dir/startup.S" \
    "$source_dir/core_portme.c" \
    "$source_dir/ee_printf.c" \
    "$source_dir/linker.ld"
  printf '\nSHA-256 of compiler inputs:\n'
  if resolved=$(command -v "${cc[0]}" 2>/dev/null); then
    sha256sum "$resolved"
  else
    sha256sum "${cc[0]}"
  fi
  if [[ -n ${gcc_build:-} ]]; then
    sha256sum "$gcc_build/gcc/cc1" "$target_libgcc/libgcc.a"
  fi
} >"$output_dir/provenance.txt"

"$size_path" "$output_dir/replacement-probe.elf" \
  "$output_dir/split-word-probe.elf"
sha256sum "$output_dir/replacement-probe.elf" \
  "$output_dir/split-word-probe.elf"
