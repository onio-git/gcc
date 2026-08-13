#!/usr/bin/env python3
"""Categorize a RISC-V DejaGNU run for ONiO.zero triage.

The script is read-only with respect to the run: it opens ``gcc.sum`` and
``gcc.log`` for reading and writes only to the requested output paths.

``gcc.sum`` supplies the canonical result lines.  ``gcc.log`` supplies the
evidence needed to tell a simulator timeout from wrong code, an ICE from a
scan mismatch, and a missing toolchain component from a real defect.  Every
result line in the log is preceded by the commands and program output that
produced it, so the log is streamed once and each failing result keeps the
block of lines that led to it.

Categories are assigned by the first matching rule, most specific first, so a
test that both targets another architecture and times out is reported once.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

RESULT_RE = re.compile(
    r"^(PASS|XPASS|FAIL|XFAIL|UNSUPPORTED|UNRESOLVED|ERROR|WARNING|UNTESTED):"
    r" ?(.*)$"
)

# Result kinds this triage reports on.
FAILING = ("FAIL", "UNRESOLVED", "ERROR")

# Architecture directories under gcc.target that are unrelated to a
# riscv32-unknown-elf compiler.  Their tests are selected by .exp files that
# frequently ignore the target triplet.
RISCV_TARGET_DIRS = ("riscv",)

MAX_EVIDENCE_LINES = 240

# Evidence signatures, in priority order.  The first match wins.
SIGNATURES: Tuple[Tuple[str, re.Pattern[str]], ...] = (
    ("ice", re.compile(r"internal compiler error|in \w+, at \S+:\d+")),
    ("sim_timeout", re.compile(r"\[mikrosim\] INFO: Timeout reached")),
    ("sim_memory", re.compile(r"\[mikrosim\] ERROR: Memory (write|read) out of range")),
    (
        "unimplemented_insn",
        re.compile(
            r"unimplemented|illegal instruction|unknown instruction"
            r"|Unsupported instruction|Illegal instruction"
        ),
    ),
    (
        "isa_abi_config",
        re.compile(
            r"ABI requires '-march=|Cannot find suitable multilib set"
            r"|isn't suitable for data type"
            r"|target does not define a speculation barrier"
        ),
    ),
    (
        "missing_component",
        re.compile(
            r"is not configured to support|valid '-foffload=' arguments"
            r"|not configured with|unrecognized command-line option"
            r"|C\+\+ compiler not installed|Fortran compiler not installed"
            r"|undefined reference to `__tls_get_addr'"
            r"|undefined reference to `__atomic_"
            r"|undefined reference to `__gcov_"
            r"|undefined reference to `__stack_chk"
            r"|cannot find -l"
        ),
    ),
    ("sim_crash", re.compile(r"Segmentation fault|Aborted \(core dumped\)")),
)

EXIT_RE = re.compile(r"\[mikrosim\] INFO: Halted: exit: (\d+)")

CATEGORY_ORDER = (
    "cross_target",
    "vector_isa",
    "isa_abi_scan_mismatch",
    "missing_component",
    "ice",
    "sim_timeout",
    "sim_memory",
    "unimplemented_insn",
    "execution_wrong_code",
    "coverage_harness",
    "unresolved_harness",
    "onio_optimization_regression",
    "unexplained",
)

CATEGORY_TITLES = {
    "cross_target": "Unrelated target-architecture tests",
    "vector_isa": "Vector (RVV) ISA not present in rv32imc",
    "isa_abi_scan_mismatch": "Incompatible ISA/ABI or global -mcpu=onio-zero scan mismatch",
    "missing_component": "Missing optional language, offload target, or component",
    "ice": "Compiler ICE or assertion",
    "sim_timeout": "Simulator timeout",
    "sim_memory": "Simulator memory-bound failure",
    "unimplemented_insn": "Unsupported or unimplemented instruction",
    "execution_wrong_code": "Execution wrong code, abort, or unexpected exit",
    "coverage_harness": "gcov/profile harness expectations",
    "unresolved_harness": "Unresolved harness or environment failure",
    "onio_optimization_regression": "Candidate ONiO optimization regression",
    "unexplained": "Unexplained",
}


def parse_result_line(text: str) -> Tuple[str, str]:
    """Split a DejaGNU result body into (testcase, description)."""
    stripped = text.strip()
    if not stripped:
        return "", ""
    testcase = stripped.split(None, 1)[0]
    description = stripped[len(testcase) :].strip()
    return testcase, description


def scan_log(path: Path) -> Dict[Tuple[str, str], List[str]]:
    """Map each failing result line to the log block that produced it.

    Duplicate result lines keep the first block, which is enough to classify
    the failure and to name a reproducer.
    """
    evidence: Dict[Tuple[str, str], List[str]] = {}
    buffer: List[str] = []
    with path.open("r", errors="replace") as handle:
        for line in handle:
            match = RESULT_RE.match(line)
            if not match:
                buffer.append(line.rstrip("\n"))
                if len(buffer) > MAX_EVIDENCE_LINES:
                    del buffer[:-MAX_EVIDENCE_LINES]
                continue
            kind, body = match.group(1), match.group(2)
            if kind in FAILING:
                key = (kind, body.rstrip())
                if key not in evidence:
                    evidence[key] = list(buffer)
            buffer = []
    return evidence


def evidence_signature(lines: Iterable[str]) -> Optional[str]:
    for name, pattern in SIGNATURES:
        for line in lines:
            if pattern.search(line):
                return name
    return None


def nonzero_exit(lines: Iterable[str]) -> Optional[int]:
    code: Optional[int] = None
    for line in lines:
        match = EXIT_RE.search(line)
        if match:
            code = int(match.group(1))
    return code


def target_arch(testcase: str) -> Optional[str]:
    if not testcase.startswith("gcc.target/"):
        return None
    rest = testcase[len("gcc.target/") :]
    return rest.split("/", 1)[0] if "/" in rest else None


def is_vector_test(testcase: str) -> bool:
    return (
        "/rvv/" in testcase
        or testcase.endswith("/rvv")
        or "vect/costmodel/riscv/rvv" in testcase
        or "/riscv/zvl" in testcase
    )


def is_scan_check(description: str) -> bool:
    return bool(re.search(r"\bscan-(assembler|tree-dump|rtl-dump|ipa-dump|lto)", description))


def is_coverage_check(testcase: str, description: str) -> bool:
    if testcase.startswith("gcc.misc-tests/gcov") or testcase.startswith("gcc.misc-tests/g"):
        if re.search(r"coverage summary|uncovered term|unexpected summary|should be", description):
            return True
    return bool(re.search(r"coverage summary not found|is #####:should be", description))


def classify(
    kind: str,
    testcase: str,
    description: str,
    lines: List[str],
    onio_tests: frozenset,
) -> Tuple[str, str]:
    """Return (category, reason)."""
    arch = target_arch(testcase)
    if arch is not None and arch not in RISCV_TARGET_DIRS:
        return "cross_target", f"gcc.target/{arch} is not a riscv32 target directory"

    if is_vector_test(testcase):
        return "vector_isa", "RVV test; the tested ISA is rv32imc without V"

    signature = evidence_signature(lines)
    if signature == "isa_abi_config":
        return (
            "isa_abi_scan_mismatch",
            "test requires an ISA/ABI this rv32imc compiler is not configured for",
        )
    if signature == "ice":
        return "ice", "compiler ICE or assertion in the log"
    if signature == "sim_timeout":
        return "sim_timeout", "simulator reported 'Timeout reached'"
    if signature == "sim_memory":
        return "sim_memory", "simulator reported a memory range error"
    if signature == "unimplemented_insn":
        return "unimplemented_insn", "unimplemented or illegal instruction in the log"
    if signature == "missing_component":
        return "missing_component", "toolchain component or option unavailable"

    if is_coverage_check(testcase, description):
        return "coverage_harness", "gcov/profile expectation, not a code-generation result"

    if kind == "UNRESOLVED" or kind == "ERROR":
        return "unresolved_harness", f"{kind} result from the harness"

    if is_scan_check(description):
        if testcase in onio_tests:
            return (
                "onio_optimization_regression",
                "scan check in an ONiO-specific regression test",
            )
        return (
            "isa_abi_scan_mismatch",
            "scan expectation written for a different CPU/ISA than -mcpu=onio-zero",
        )

    if "execution test" in description or re.search(r"\bexecute\b", description):
        code = nonzero_exit(lines)
        if code:
            return "execution_wrong_code", f"program halted with exit status {code}"
        if signature == "sim_crash":
            return "execution_wrong_code", "simulator process crashed"
        return "execution_wrong_code", "execution test failed without a recognized diagnostic"

    return "unexplained", "no rule matched"


def source_size(testcase: str, srcdir: Optional[Path]) -> Optional[int]:
    if srcdir is None:
        return None
    candidate = srcdir / testcase
    try:
        return candidate.stat().st_size
    except OSError:
        return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sum", type=Path, required=True, help="combined gcc.sum")
    parser.add_argument("--log", type=Path, required=True, help="combined gcc.log")
    parser.add_argument(
        "--srcdir",
        type=Path,
        default=None,
        help="gcc/testsuite directory, used to rank candidates by source size",
    )
    parser.add_argument("--json", type=Path, default=None, help="write full classification")
    parser.add_argument(
        "--examples", type=int, default=5, help="representative tests printed per category"
    )
    parser.add_argument(
        "--smallest",
        type=int,
        default=25,
        help="smallest unexplained/relevant sources to list",
    )
    args = parser.parse_args()

    onio_tests = frozenset(
        line.strip()
        for line in (
            "gcc.target/riscv/onio-zero-no-unroll.c",
            "gcc.target/riscv/onio-zero-spill-motion-live-header.c",
            "gcc.target/riscv/onio-zero-spill-motion-volatile.c",
            "gcc.target/riscv/onio-zero-vector-override.c",
            "gcc.dg/preunroll-1.c",
            "gcc.dg/unroll-and-jam-reduction.c",
        )
    )

    evidence = scan_log(args.log)

    counts: Counter = Counter()
    per_category: Dict[str, List[dict]] = defaultdict(list)
    records: List[dict] = []

    with args.sum.open("r", errors="replace") as handle:
        for line in handle:
            match = RESULT_RE.match(line)
            if not match:
                continue
            kind, body = match.group(1), match.group(2).rstrip()
            if kind not in FAILING:
                continue
            testcase, description = parse_result_line(body)
            lines = evidence.get((kind, body), [])
            category, reason = classify(kind, testcase, description, lines, onio_tests)
            counts[category] += 1
            record = {
                "kind": kind,
                "testcase": testcase,
                "description": description,
                "category": category,
                "reason": reason,
                "has_evidence": bool(lines),
            }
            records.append(record)
            per_category[category].append(record)

    total = sum(counts.values())
    print(f"triaged {total} failing result lines from {args.sum}")
    print()
    width = max(len(CATEGORY_TITLES[c]) for c in CATEGORY_ORDER)
    print(f"{'category':<32} {'count':>7}  title")
    for category in CATEGORY_ORDER:
        if not counts[category]:
            continue
        print(f"{category:<32} {counts[category]:>7}  {CATEGORY_TITLES[category]:<{width}}")
    unknown = set(counts) - set(CATEGORY_ORDER)
    for category in sorted(unknown):
        print(f"{category:<32} {counts[category]:>7}  (uncategorized)")
    print(f"{'TOTAL':<32} {total:>7}")

    for category in CATEGORY_ORDER:
        rows = per_category.get(category)
        if not rows:
            continue
        print()
        print(f"== {category}: {CATEGORY_TITLES[category]} ({len(rows)}) ==")
        seen = set()
        shown = 0
        for row in rows:
            if row["testcase"] in seen:
                continue
            seen.add(row["testcase"])
            print(f"  {row['testcase']}  [{row['reason']}]")
            shown += 1
            if shown >= args.examples:
                break
        distinct = len({row["testcase"] for row in rows})
        print(f"  ({distinct} distinct test sources)")

    relevant = [
        row
        for row in records
        if row["category"] in ("unexplained", "execution_wrong_code", "ice", "sim_memory")
    ]
    if relevant and args.srcdir:
        sized = []
        for testcase in {row["testcase"] for row in relevant}:
            size = source_size(testcase, args.srcdir)
            if size is not None:
                sized.append((size, testcase))
        sized.sort()
        print()
        print(f"== smallest relevant unexplained/execution sources ({len(sized)} with sources) ==")
        for size, testcase in sized[: args.smallest]:
            print(f"  {size:>8}  {testcase}")

    if args.json:
        args.json.write_text(json.dumps(records, indent=1))
        print()
        print(f"wrote {args.json}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
