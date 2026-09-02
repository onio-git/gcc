#!/usr/bin/env python3
"""Measure CoreMark's timed window with the ONiO model in embsim.

Directories are searched recursively for ``*.elf``. Identical ELFs are
simulated once by SHA-256 and still receive one CSV row per input path.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, fields
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_MODEL = SCRIPT_DIR / "embsim-onio-zero-coremark.json"
DEFAULT_FREQ_HZ = 32_000_000
DEFAULT_POWER_UW_PER_MHZ = 22.0
MONITOR_PROMPT = b"(embsim) "
MAX_MONITOR_CHUNK = 1_000_000


@dataclass(frozen=True)
class Measurement:
    path: str
    sha256: str
    status: str
    start_pc: str = ""
    stop_pc: str = ""
    instructions: int = 0
    cycles: int = 0
    fetch_penalty: int = 0
    branch_penalty: int = 0
    mispredictions: int = 0
    branches: int = 0
    branches_taken: int = 0
    branches_not_taken: int = 0
    loads: int = 0
    stores: int = 0
    code_accesses: int = 0
    code_hits: int = 0
    code_soft_misses: int = 0
    code_hard_misses: int = 0
    code_invalid_fills: int = 0
    code_replacements: int = 0
    code_sequential_hard_misses: int = 0
    code_nonsequential_hard_misses: int = 0
    data_accesses: int = 0
    coremark_per_mhz: float = 0.0
    modeled_coremark_per_mj: float = 0.0
    error: str = ""


def executable(value: str, description: str) -> str:
    path = shutil.which(value)
    if path is None:
        raise SystemExit(f"{description} not found: {value}")
    return path


def inputs(paths: list[Path]) -> list[Path]:
    found: set[Path] = set()
    for path in paths:
        if path.is_dir():
            found.update(candidate.resolve() for candidate in path.rglob("*.elf"))
        elif path.is_file():
            found.add(path.resolve())
        else:
            raise SystemExit(f"input does not exist: {path}")
    return sorted(found)


def digest(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(block)
    return hasher.hexdigest()


def timed_pcs(path: Path, objdump: str) -> tuple[set[int], set[int]]:
    disassembly = subprocess.check_output(
        [objdump, "-dr", str(path)], text=True, errors="replace"
    )
    starts: set[int] = set()
    stops: set[int] = set()
    in_main = False
    instruction = re.compile(
        r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{4,8}\s+)+)(.*)$", re.IGNORECASE
    )
    call = re.compile(r"\bjal\s+[0-9a-f]+\s+<(start_time|stop_time)>", re.IGNORECASE)
    for line in disassembly.splitlines():
        if "<main>:" in line:
            in_main = True
            continue
        if in_main and line and not line.startswith(" "):
            break
        if not in_main:
            continue
        decoded = instruction.match(line)
        if decoded is None:
            continue
        target = call.search(decoded.group(3))
        if target is None:
            continue
        pc = int(decoded.group(1), 16)
        width = sum(len(token) for token in decoded.group(2).split()) // 2
        if target.group(1).lower() == "start_time":
            starts.add(pc + width)
        else:
            stops.add(pc)
    if not starts or not stops:
        raise RuntimeError("could not find start_time/stop_time calls in main")
    return starts, stops


def integer(output: str, pattern: str) -> int:
    match = re.search(pattern, output, re.MULTILINE)
    if match is None:
        raise RuntimeError(f"missing simulator output matching {pattern!r}")
    return int(match.group(1))


def optional_integer(output: str, pattern: str) -> int:
    match = re.search(pattern, output, re.MULTILINE)
    return int(match.group(1)) if match is not None else 0


def monitor_reply(process: subprocess.Popen[bytes], command: str | None = None) -> str:
    if command is not None:
        if process.stdin is None:
            raise RuntimeError("embsim debugger stdin is unavailable")
        process.stdin.write(command.encode() + b"\n")
        process.stdin.flush()
    if process.stdout is None:
        raise RuntimeError("embsim debugger stdout is unavailable")
    reply = bytearray()
    while not reply.endswith(MONITOR_PROMPT):
        byte = process.stdout.read(1)
        if not byte:
            raise RuntimeError(
                f"embsim debugger closed while waiting for {command or 'initial prompt'}"
            )
        reply.extend(byte)
    return reply[: -len(MONITOR_PROMPT)].decode(errors="replace")


def run_until_any_pc(
    process: subprocess.Popen[bytes], targets: set[int], cap: int
) -> tuple[int, str]:
    target_list = ",".join(f"0x{pc:x}" for pc in sorted(targets))
    executed = 0
    replies: list[str] = []
    while executed < cap:
        chunk = min(MAX_MONITOR_CHUNK, cap - executed)
        reply = monitor_reply(
            process, f"monitor run_until_any_pc {target_list} {chunk}"
        )
        replies.append(reply)
        reached = re.search(r"Reached PC=0x([0-9a-f]+) after (\d+) steps", reply)
        if reached is not None:
            return int(reached.group(1), 16), "".join(replies)
        incomplete = re.search(
            r"(?:not reached|No requested PC reached) in (\d+) steps", reply
        )
        if incomplete is None:
            raise RuntimeError(f"embsim stopped before a target PC: {reply.strip()}")
        executed += int(incomplete.group(1))
    raise RuntimeError(f"timed window was not reached within the {cap}-instruction cap")


def run_unique(
    path: Path,
    sha256: str,
    embsim: str,
    objdump: str,
    model: Path,
    iterations: int,
    freq_hz: int,
    power_uw_per_mhz: float,
    cap: int,
) -> Measurement:
    process: subprocess.Popen[bytes] | None = None
    try:
        starts, stops = timed_pcs(path, objdump)
        process = subprocess.Popen(
            [
                embsim,
                "--quiet",
                "--debug",
                "--no-gdb-server",
                "--fast",
                f"--stub={model}",
                str(path),
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        monitor_reply(process)
        start_pc, start_output = run_until_any_pc(process, starts, cap)
        reset_output = monitor_reply(process, "monitor reset_counter")
        stop_pc, stop_output = run_until_any_pc(process, stops, cap)
        stats_output = monitor_reply(process, "monitor show_stats")
        output = start_output + reset_output + stop_output + stats_output
        if process.stdin is None:
            raise RuntimeError("embsim debugger stdin is unavailable")
        process.stdin.write(b"quit\n")
        process.stdin.flush()
        process.wait(timeout=5)
        if process.returncode != 0:
            raise RuntimeError(f"embsim exited {process.returncode}: {output.strip()}")

        instructions = integer(output, r"Instructions: (\d+)")
        cycles = integer(output, r"Cycles: (\d+)")
        fetch = integer(output, r"Fetch penalty cycles: (\d+)")
        branch = integer(output, r"Branch penalty cycles: (\d+)")
        load_penalty = integer(output, r"Load penalty cycles: (\d+)")
        store_penalty = integer(output, r"Store penalty cycles: (\d+)")
        if cycles != instructions + fetch + branch + load_penalty + store_penalty:
            raise RuntimeError(
                "cycle accounting mismatch: "
                f"{cycles} != {instructions}+{fetch}+{branch}+{load_penalty}+{store_penalty}"
            )
        branch_row = re.search(
            r"Branches: (\d+) \(taken (\d+), not taken (\d+), mispredicted (\d+)\)",
            output,
            re.MULTILINE,
        )
        if branch_row is None:
            raise RuntimeError("missing branch counters")
        cm_at_freq = iterations * freq_hz / cycles
        power_mw = freq_hz / 1_000_000 * power_uw_per_mhz / 1000
        return Measurement(
            path=str(path),
            sha256=sha256,
            status="OK",
            start_pc=f"0x{start_pc:08x}",
            stop_pc=f"0x{stop_pc:08x}",
            instructions=instructions,
            cycles=cycles,
            fetch_penalty=fetch,
            branch_penalty=branch,
            mispredictions=int(branch_row.group(4)),
            branches=int(branch_row.group(1)),
            branches_taken=int(branch_row.group(2)),
            branches_not_taken=int(branch_row.group(3)),
            loads=integer(output, r"Loads: (\d+)"),
            stores=integer(output, r"Stores: (\d+)"),
            code_accesses=optional_integer(output, r"Code memory accesses: (\d+)"),
            code_hits=optional_integer(output, r"Code memory hits: (\d+)"),
            code_soft_misses=optional_integer(
                output, r"Code memory soft_misses: (\d+)"
            ),
            code_hard_misses=optional_integer(
                output, r"Code memory hard_misses: (\d+)"
            ),
            code_invalid_fills=optional_integer(
                output, r"Code memory invalid_fills: (\d+)"
            ),
            code_replacements=optional_integer(
                output, r"Code memory replacements: (\d+)"
            ),
            code_sequential_hard_misses=optional_integer(
                output, r"Code memory sequential_hard_misses: (\d+)"
            ),
            code_nonsequential_hard_misses=optional_integer(
                output, r"Code memory nonsequential_hard_misses: (\d+)"
            ),
            data_accesses=optional_integer(output, r"Data memory accesses: (\d+)"),
            coremark_per_mhz=iterations * 1_000_000 / cycles,
            modeled_coremark_per_mj=cm_at_freq / power_mw,
        )
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        return Measurement(
            path=str(path), sha256=sha256, status="ERROR", error=str(error)
        )
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


def write_csv(path: Path, rows: list[Measurement]) -> None:
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(
            stream,
            fieldnames=[field.name for field in fields(Measurement)],
            lineterminator="\n",
        )
        writer.writeheader()
        writer.writerows(row.__dict__ for row in rows)


def display_path(path: Path, root: Path | None) -> str:
    if root is None:
        return str(path)
    try:
        return str(path.relative_to(root))
    except ValueError:
        return str(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "input", nargs="+", type=Path, help="ELF file or directory tree"
    )
    parser.add_argument("--csv", type=Path, help="write one result row per input ELF")
    parser.add_argument(
        "--path-root",
        type=Path,
        help="store input paths relative to this directory when possible",
    )
    parser.add_argument("--model", type=Path, default=DEFAULT_MODEL)
    parser.add_argument("--embsim", default=os.environ.get("EMBSIM", "embsim"))
    parser.add_argument(
        "--objdump",
        default=os.environ.get("RISCV_OBJDUMP", "riscv32-unknown-elf-objdump"),
    )
    parser.add_argument("--freq-hz", type=int, default=DEFAULT_FREQ_HZ)
    parser.add_argument(
        "--iterations",
        type=int,
        default=1,
        help="CoreMark iterations inside the measured window (default: 1)",
    )
    parser.add_argument(
        "--power-uw-per-mhz", type=float, default=DEFAULT_POWER_UW_PER_MHZ
    )
    parser.add_argument(
        "--cap",
        type=int,
        default=1_000_000,
        help="instruction limit for reaching each edge of the timed window",
    )
    args = parser.parse_args()

    if args.iterations < 1:
        parser.error("--iterations must be a positive integer")

    embsim = executable(args.embsim, "embsim")
    objdump = executable(args.objdump, "RISC-V objdump")
    paths = inputs(args.input)
    path_root = args.path_root.resolve() if args.path_root else None
    if not paths:
        raise SystemExit("no ELF files found")

    cache: dict[str, Measurement] = {}
    rows: list[Measurement] = []
    for index, path in enumerate(paths, 1):
        sha256 = digest(path)
        if sha256 not in cache:
            print(f"[{index}/{len(paths)}] {path}", file=sys.stderr)
            cache[sha256] = run_unique(
                path,
                sha256,
                embsim,
                objdump,
                args.model.resolve(),
                args.iterations,
                args.freq_hz,
                args.power_uw_per_mhz,
                args.cap,
            )
        result = cache[sha256]
        rows.append(
            Measurement(**{**result.__dict__, "path": display_path(path, path_root)})
        )

    if args.csv:
        write_csv(args.csv, rows)
    print(
        "path,sha256,status,instructions,cycles,fetch,branch,mispredictions,"
        "soft_misses,hard_misses,CoreMark/MHz"
    )
    for row in rows:
        print(
            f"{row.path},{row.sha256},{row.status},{row.instructions},{row.cycles},"
            f"{row.fetch_penalty},{row.branch_penalty},{row.mispredictions},"
            f"{row.code_soft_misses},{row.code_hard_misses},"
            f"{row.coremark_per_mhz:.6f}"
        )
    errors = sum(row.status != "OK" for row in rows)
    for row in rows:
        if row.status != "OK":
            print(f"ERROR: {row.path}: {row.error}", file=sys.stderr)
    print(
        f"Measured {len(cache)} distinct ELFs across {len(rows)} paths; {errors} errors",
        file=sys.stderr,
    )
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
