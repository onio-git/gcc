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


def run_unique(
    path: Path,
    sha256: str,
    embsim: str,
    objdump: str,
    model: Path,
    freq_hz: int,
    power_uw_per_mhz: float,
    cap: int,
) -> Measurement:
    try:
        starts, stops = timed_pcs(path, objdump)
        start_list = ",".join(f"0x{pc:x}" for pc in sorted(starts))
        stop_list = ",".join(f"0x{pc:x}" for pc in sorted(stops))
        commands = [
            f"monitor run_until_any_pc {start_list} {cap}",
            "monitor reset_counter",
            f"monitor run_until_any_pc {stop_list} {cap}",
            "monitor show_stats",
            "quit",
        ]
        process = subprocess.run(
            [
                embsim,
                "--quiet",
                "--debug",
                "--no-gdb-server",
                f"--stub={model}",
                str(path),
            ],
            input="\n".join(commands) + "\n",
            text=True,
            capture_output=True,
            check=False,
        )
        output = process.stdout + process.stderr
        hits = [
            int(value, 16) for value in re.findall(r"Reached PC=0x([0-9a-f]+)", output)
        ]
        if process.returncode != 0:
            raise RuntimeError(f"embsim exited {process.returncode}: {output.strip()}")
        if len(hits) < 2 or hits[0] not in starts or hits[1] not in stops:
            raise RuntimeError(
                f"timed window was not reached within the {cap}-instruction cap"
            )

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
        cm_at_freq = freq_hz / cycles
        power_mw = freq_hz / 1_000_000 * power_uw_per_mhz / 1000
        return Measurement(
            path=str(path),
            sha256=sha256,
            status="OK",
            start_pc=f"0x{hits[0]:08x}",
            stop_pc=f"0x{hits[1]:08x}",
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
            coremark_per_mhz=1_000_000 / cycles,
            modeled_coremark_per_mj=cm_at_freq / power_mw,
        )
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        return Measurement(
            path=str(path), sha256=sha256, status="ERROR", error=str(error)
        )


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
        "--power-uw-per-mhz", type=float, default=DEFAULT_POWER_UW_PER_MHZ
    )
    parser.add_argument(
        "--cap",
        type=int,
        default=1_000_000,
        help="instruction limit for reaching each edge of the timed window",
    )
    args = parser.parse_args()

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
        "path,sha256,status,instructions,cycles,fetch,branch,mispredictions,CoreMark/MHz"
    )
    for row in rows:
        print(
            f"{row.path},{row.sha256},{row.status},{row.instructions},{row.cycles},"
            f"{row.fetch_penalty},{row.branch_penalty},{row.mispredictions},"
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
