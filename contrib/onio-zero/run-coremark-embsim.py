#!/usr/bin/env python3
"""Measure CoreMark's timed window with the ONiO model in embsim.

Directories are searched recursively for ``*.elf``. Identical ELFs are
simulated once by SHA-256 and still receive one CSV row per input path.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time
from dataclasses import dataclass, fields
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_MODEL = SCRIPT_DIR / "embsim-onio-zero-coremark.json"
DEFAULT_FREQ_HZ = 32_000_000
DEFAULT_POWER_UW_PER_MHZ = 22.0
GDB_PROMPT = b"(gdb) "
MAX_MONITOR_CHUNK = 1_000_000
CACHE_KINDS = frozenset({"cache", "wp-icache"})
CACHE_COUNTERS = frozenset(
    {
        "accesses",
        "hard_misses",
        "hits",
        "invalid_fills",
        "misses",
        "nonsequential_hard_misses",
        "replacements",
        "sequential_hard_misses",
        "soft_misses",
        "split_word_fetches",
        "word_fetches",
        "writebacks",
    }
)


@dataclass(frozen=True)
class Measurement:
    path: str
    sha256: str
    status: str
    embsim_version: str = ""
    embsim_sha256: str = ""
    model_sha256: str = ""
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
    code_word_fetches: int = 0
    code_split_word_fetches: int = 0
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


def embsim_version(embsim: str) -> str:
    result = subprocess.run(
        [embsim, "--version"],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    lines = result.stdout.splitlines()
    if not lines:
        raise RuntimeError(f"{embsim} --version produced no output")
    return lines[0]


def configured_cache_labels(model: Path) -> set[str]:
    try:
        document = json.loads(model.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"cannot read model {model}: {error}") from error
    memif = document.get("memif", {})
    if not isinstance(memif, dict):
        raise RuntimeError(f"model {model}: memif must be an object")

    labels: set[str] = set()
    for key, label in (("code", "Code"), ("data", "Data")):
        spec = memif.get(key, "flat:penalty=0")
        if not isinstance(spec, str):
            raise RuntimeError(f"model {model}: memif.{key} must be a string")
        if spec.partition(":")[0].strip() in CACHE_KINDS:
            labels.add(label)
    return labels


def memory_counters(output: str, label: str, required: bool) -> dict[str, int]:
    counters: dict[str, int] = {}
    pattern = re.compile(rf"^{re.escape(label)} memory ([A-Za-z0-9_]+): (\d+)$")
    for line in output.splitlines():
        match = pattern.fullmatch(line)
        if match is None:
            continue
        name = match.group(1)
        if name in counters:
            raise RuntimeError(f"duplicate {label.lower()} memory counter {name!r}")
        counters[name] = int(match.group(2))

    if required:
        missing = sorted(CACHE_COUNTERS - counters.keys())
        if missing:
            raise RuntimeError(
                f"missing required {label.lower()} memory counters: {', '.join(missing)}"
            )
    return counters


def unused_tcp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def gdb_reply(process: subprocess.Popen[bytes], command: str | None = None) -> str:
    if command is not None:
        if process.stdin is None:
            raise RuntimeError("GDB stdin is unavailable")
        process.stdin.write(command.encode() + b"\n")
        process.stdin.flush()
    if process.stdout is None:
        raise RuntimeError("GDB stdout is unavailable")
    reply = bytearray()
    while not reply.endswith(GDB_PROMPT):
        byte = process.stdout.read(1)
        if not byte:
            raise RuntimeError(
                f"GDB closed while waiting for {command or 'initial prompt'}"
            )
        reply.extend(byte)
    return reply[: -len(GDB_PROMPT)].decode(errors="replace")


def run_until_any_pc(
    process: subprocess.Popen[bytes], targets: set[int], cap: int
) -> tuple[int, str]:
    target_list = ",".join(f"0x{pc:x}" for pc in sorted(targets))
    executed = 0
    replies: list[str] = []
    while executed < cap:
        chunk = min(MAX_MONITOR_CHUNK, cap - executed)
        reply = gdb_reply(process, f"monitor run_until_any_pc {target_list} {chunk}")
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
    gdb: str,
    objdump: str,
    model: Path,
    cache_labels: set[str],
    version: str,
    embsim_hash: str,
    model_hash: str,
    iterations: int,
    freq_hz: int,
    power_uw_per_mhz: float,
    cap: int,
) -> Measurement:
    simulator: subprocess.Popen[bytes] | None = None
    debugger: subprocess.Popen[bytes] | None = None
    try:
        starts, stops = timed_pcs(path, objdump)
        port = unused_tcp_port()
        simulator = subprocess.Popen(
            [
                embsim,
                "--quiet",
                "--fast",
                f"--stub={model}",
                f"--port={port}",
                str(path),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        debugger = subprocess.Popen(
            [gdb, "--quiet", "--nx", str(path)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        gdb_reply(debugger)
        gdb_reply(debugger, "set pagination off")
        gdb_reply(debugger, "set confirm off")

        connected = False
        connection_output = ""
        for _ in range(50):
            if simulator.poll() is not None:
                detail = (
                    simulator.stdout.read().decode(errors="replace")
                    if simulator.stdout
                    else ""
                )
                raise RuntimeError(
                    f"embsim exited before GDB connected: {detail.strip()}"
                )
            connection_output = gdb_reply(debugger, f"target remote 127.0.0.1:{port}")
            if not re.search(
                r"(?:Connection refused|Connection timed out|No route to host)",
                connection_output,
                re.IGNORECASE,
            ):
                connected = True
                break
            time.sleep(0.02)
        if not connected:
            raise RuntimeError(
                f"GDB could not connect to embsim: {connection_output.strip()}"
            )

        monitor_help = gdb_reply(debugger, "monitor help")
        if "show_stats" not in monitor_help or "reset_counter" not in monitor_help:
            raise RuntimeError(
                f"embsim monitor commands unavailable: {monitor_help.strip()}"
            )

        start_pc, start_output = run_until_any_pc(debugger, starts, cap)
        reset_output = gdb_reply(debugger, "monitor reset_counter")
        stop_pc, stop_output = run_until_any_pc(debugger, stops, cap)
        stats_output = gdb_reply(debugger, "monitor show_stats")
        output = start_output + reset_output + stop_output + stats_output
        gdb_reply(debugger, "detach")
        if debugger.stdin is None:
            raise RuntimeError("GDB stdin is unavailable")
        debugger.stdin.write(b"quit\n")
        debugger.stdin.flush()
        debugger.wait(timeout=5)
        if debugger.returncode != 0:
            raise RuntimeError(f"GDB exited {debugger.returncode}: {output.strip()}")

        instructions = integer(stats_output, r"Instructions: (\d+)")
        cycles = integer(stats_output, r"Cycles: (\d+)")
        fetch = integer(stats_output, r"Fetch penalty cycles: (\d+)")
        branch = integer(stats_output, r"Branch penalty cycles: (\d+)")
        load_penalty = integer(stats_output, r"Load penalty cycles: (\d+)")
        store_penalty = integer(stats_output, r"Store penalty cycles: (\d+)")
        if cycles != instructions + fetch + branch + load_penalty + store_penalty:
            raise RuntimeError(
                "cycle accounting mismatch: "
                f"{cycles} != {instructions}+{fetch}+{branch}+{load_penalty}+{store_penalty}"
            )
        branch_row = re.search(
            r"Branches: (\d+) \(taken (\d+), not taken (\d+), mispredicted (\d+)\)",
            stats_output,
            re.MULTILINE,
        )
        if branch_row is None:
            raise RuntimeError("missing branch counters")
        code = memory_counters(stats_output, "Code", "Code" in cache_labels)
        data = memory_counters(stats_output, "Data", "Data" in cache_labels)
        cm_at_freq = iterations * freq_hz / cycles
        power_mw = freq_hz / 1_000_000 * power_uw_per_mhz / 1000
        return Measurement(
            path=str(path),
            sha256=sha256,
            status="OK",
            embsim_version=version,
            embsim_sha256=embsim_hash,
            model_sha256=model_hash,
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
            loads=integer(stats_output, r"Loads: (\d+)"),
            stores=integer(stats_output, r"Stores: (\d+)"),
            code_accesses=code.get("accesses", 0),
            code_hits=code.get("hits", 0),
            code_soft_misses=code.get("soft_misses", 0),
            code_hard_misses=code.get("hard_misses", 0),
            code_invalid_fills=code.get("invalid_fills", 0),
            code_replacements=code.get("replacements", 0),
            code_sequential_hard_misses=code.get("sequential_hard_misses", 0),
            code_nonsequential_hard_misses=code.get("nonsequential_hard_misses", 0),
            code_word_fetches=code.get("word_fetches", 0),
            code_split_word_fetches=code.get("split_word_fetches", 0),
            data_accesses=data.get("accesses", 0),
            coremark_per_mhz=iterations * 1_000_000 / cycles,
            modeled_coremark_per_mj=cm_at_freq / power_mw,
        )
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        return Measurement(
            path=str(path),
            sha256=sha256,
            status="ERROR",
            embsim_version=version,
            embsim_sha256=embsim_hash,
            model_sha256=model_hash,
            error=str(error),
        )
    finally:
        if debugger is not None and debugger.poll() is None:
            debugger.terminate()
            try:
                debugger.wait(timeout=5)
            except subprocess.TimeoutExpired:
                debugger.kill()
                debugger.wait()
        if simulator is not None and simulator.poll() is None:
            simulator.terminate()
            try:
                simulator.wait(timeout=5)
            except subprocess.TimeoutExpired:
                simulator.kill()
                simulator.wait()


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
    parser.add_argument("--gdb", default=os.environ.get("RISCV_GDB", "gdb-multiarch"))
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
    gdb = executable(args.gdb, "GDB")
    objdump = executable(args.objdump, "RISC-V objdump")
    model = args.model.resolve()
    try:
        version = embsim_version(embsim)
        embsim_hash = digest(Path(embsim))
        model_hash = digest(model)
        cache_labels = configured_cache_labels(model)
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error)) from error
    print(
        f"Simulator: {version}; SHA-256 {embsim_hash}; model SHA-256 {model_hash}",
        file=sys.stderr,
    )
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
                gdb,
                objdump,
                model,
                cache_labels,
                version,
                embsim_hash,
                model_hash,
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
        "soft_misses,hard_misses,word_fetches,split_word_fetches,CoreMark/MHz"
    )
    for row in rows:
        print(
            f"{row.path},{row.sha256},{row.status},{row.instructions},{row.cycles},"
            f"{row.fetch_penalty},{row.branch_penalty},{row.mispredictions},"
            f"{row.code_soft_misses},{row.code_hard_misses},"
            f"{row.code_word_fetches},{row.code_split_word_fetches},"
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
