#!/usr/bin/env python3
"""Mutation testing harness for the SIMD register / interpreter code.

For each mutation in a hand-curated list, apply it to the source,
incrementally rebuild ONE target, run the test suite, and record
whether any test caught the mutation.  Survivors (mutations no test
caught) indicate gaps in the test suite — they're places where the
implementation could silently regress and CI would not notice.

This is INTENTIONALLY targeted, not exhaustive.  Real exhaustive
mutation testing (every binary operator, every constant, every
branch) would require building tens of thousands of variants which
is not practical for an interactive QA tool.  The mutations here are
chosen to:

  - Land in code paths the test suite actually exercises (verified
    via llvm-cov beforehand)
  - Test specific invariants we care about (lane addressing, ref
    counting, ownership transitions)
  - Be syntactically valid after substitution (no compile-failure
    spam)

Usage:
    tools/mutation_test_simd.py --build-dir /path/to/build [--test-filter REGEX]

Builds incrementally per mutation (only the mutated translation unit
+ link), so the per-iteration cost is ~10-30s on cpu-perf.

Exits 0 if every mutation was caught (every test failed for at least
one mutation), 1 if any survived (no test caught the change).
"""

from __future__ import annotations

import argparse
import dataclasses
import os
import pathlib
import re
import subprocess
import sys


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent


@dataclasses.dataclass
class Mutation:
    file: str            # path relative to repo root
    pattern: str         # regex that uniquely matches the line to mutate
    replacement: str     # replacement string
    description: str     # human-readable description
    target: str          # cmake target to (re)build before running tests


# Targeted mutations.  Each one changes one line in cpu-perf SIMD code
# in a way that should make at least one test fail.  Picked to cover:
#   - SimdReg setVarying lane[0] preservation
#   - SimdReg operator[] varying-vs-uniform offset arithmetic
#   - SimdReg arena ownership flag
#   - SimdBoolMask varying transition
#   - SimdInst arithmetic opcode behaviour (one representative)
#   - SimdInterpreter maxSamples reporting
#
# The `pattern` MUST uniquely match its target line; the harness will
# refuse to apply a mutation that matches multiple lines.
MUTATIONS: list[Mutation] = [
    # SimdBoolMask::setVarying widening: memset broadcasts inline value across
    # all lanes.  Mutate the broadcast value to a constant 0 — testSimdRegAddr
    # checks lane[i] == seed after widen.
    Mutation(
        file="lib/IlmCtlSimd/CtlSimdReg.h",
        pattern=r"memset\(data, _data\[0\], MAX_REG_SIZE\);",
        replacement="memset(data, 0, MAX_REG_SIZE);",
        description="SimdBoolMask widen: broadcast 0 instead of inline value",
        target="IlmCtlTest",
    ),
    # SimdReg::reference: when _dataOwned is true, the assignment delete[]s
    # the old data before reassignment.  Skip the delete -> leak.  The smoke
    # tests won't notice (no heap accounting), but a heap-instrumented build
    # (ASan) would.  Expected to SURVIVE under plain unit tests — illustrates
    # an inherent limit of unit-test-driven mutation testing.
    Mutation(
        file="lib/IlmCtlSimd/CtlSimdReg.cpp",
        pattern=r"if \(_dataOwned\)\n\tdelete \[\] _data;\n\n    //",
        replacement="if (_dataOwned)\n\t{ /* leak: _data; */ }\n\n    //",
        description="reference(): leak _data instead of deleting (ASan-only catch)",
        target="IlmCtlTest",
    ),
    # SimdReg::setVarying narrow: should preserve _data[0] in _inlineData.
    # Mutate to preserve _data[1] instead — testSimdRegAddr asserts lane[0]
    # is preserved across narrow.
    Mutation(
        file="lib/IlmCtlSimd/CtlSimdReg.h",
        pattern=r"_inlineData = _data\[0\];",
        replacement="_inlineData = _data[1];",
        description="SimdBoolMask narrow: preserve lane[1] instead of lane[0]",
        target="IlmCtlTest",
    ),
    # SimdInterpreter::maxSamples returns MAX_REG_SIZE.  Mutate to half.
    # testConcurrentCalls asserts maxSamples >= shape.samples for every
    # configured shape (8192 samples specifically); halving breaks that.
    Mutation(
        file="lib/IlmCtlSimd/CtlSimdInterpreter.cpp",
        pattern=r"return MAX_REG_SIZE;",
        replacement="return MAX_REG_SIZE / 2;",
        description="maxSamples() returns half MAX_REG_SIZE",
        target="IlmCtlTest",
    ),
]


def find_unique_match(content: str, pattern: str) -> tuple[int, int] | None:
    """Return (start, end) of the unique regex match, or None if not unique."""
    matches = list(re.finditer(pattern, content))
    if len(matches) != 1:
        return None
    return matches[0].span()


def apply_mutation(file_path: pathlib.Path, m: Mutation) -> str:
    """Apply mutation; return the original file content for restoration."""
    original = file_path.read_text()
    span = find_unique_match(original, m.pattern)
    if span is None:
        raise RuntimeError(
            f"mutation pattern matched 0 or >1 times in {m.file}: {m.pattern}")
    start, end = span
    mutated = original[:start] + m.replacement + original[end:]
    file_path.write_text(mutated)
    return original


def restore(file_path: pathlib.Path, original: str) -> None:
    file_path.write_text(original)


def run_build(build_dir: pathlib.Path, target: str) -> tuple[bool, str]:
    proc = subprocess.run(
        ["cmake", "--build", str(build_dir), "--target", target, "-j"],
        capture_output=True, text=True)
    return (proc.returncode == 0, proc.stderr or proc.stdout[-2000:])


def run_tests(build_dir: pathlib.Path, test_filter: str) -> tuple[bool, str]:
    cmd = ["ctest"]
    if test_filter:
        cmd += ["-R", test_filter]
    proc = subprocess.run(
        cmd, cwd=str(build_dir), capture_output=True, text=True)
    return (proc.returncode == 0, proc.stdout[-2000:])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", required=True,
                    help="cmake build dir (must already be configured)")
    ap.add_argument("--test-filter", default="",
                    help="ctest -R regex (default: all tests)")
    args = ap.parse_args()

    build_dir = pathlib.Path(args.build_dir).resolve()
    if not (build_dir / "CMakeCache.txt").exists():
        print(f"error: {build_dir} is not a configured cmake build dir",
              file=sys.stderr)
        return 2

    survivors: list[Mutation] = []
    caught: list[Mutation] = []
    uncompilable: list[Mutation] = []

    for i, m in enumerate(MUTATIONS, start=1):
        file_path = REPO_ROOT / m.file
        print(f"\n[{i}/{len(MUTATIONS)}] {m.description}", flush=True)
        print(f"  file: {m.file}", flush=True)

        try:
            original = apply_mutation(file_path, m)
        except RuntimeError as e:
            print(f"  SKIP: {e}", flush=True)
            continue

        try:
            ok_build, build_log = run_build(build_dir, m.target)
            if not ok_build:
                print("  uncompilable (mutation did not produce valid C++)",
                      flush=True)
                uncompilable.append(m)
                continue

            ok_tests, test_log = run_tests(build_dir, args.test_filter)
            if ok_tests:
                print("  SURVIVED — no test caught the mutation", flush=True)
                survivors.append(m)
            else:
                print("  caught", flush=True)
                caught.append(m)
        finally:
            restore(file_path, original)

    # Final report.
    print("\n" + "=" * 60)
    print(f"caught:       {len(caught)}/{len(MUTATIONS)}")
    print(f"survived:     {len(survivors)}/{len(MUTATIONS)}")
    print(f"uncompilable: {len(uncompilable)}/{len(MUTATIONS)}")
    if survivors:
        print("\nSurvivors (no test caught these — gaps in coverage):")
        for m in survivors:
            print(f"  - {m.description}  [{m.file}]")

    # The build dir has the LAST mutation's object files baked in.  Rebuild
    # all targets we touched so a follow-up `ctest` against this build dir
    # reflects the (now-restored) original source rather than the last
    # mutation.  Without this, the user gets confusing failures from a
    # stale incremental build.
    print("\nrestoring build (rebuilding mutated targets against original source)")
    targets = sorted({m.target for m in MUTATIONS})
    for t in targets:
        run_build(build_dir, t)

    return 1 if survivors else 0


if __name__ == "__main__":
    sys.exit(main())
