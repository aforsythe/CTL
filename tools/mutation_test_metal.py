#!/usr/bin/env python3
"""Apply each mutation in MUTATIONS, incrementally rebuild + run tests,
report survivors (mutations no test caught).

Mutations target lib/IlmCtlMetal/* — codegen helpers, dispatch
binding, sidecar/shader cache invalidation.

Usage:
    tools/mutation_test_metal.py --build-dir /path/to/build [--test-filter REGEX]

Exits 0 if every mutation was caught, 1 if any survived.
"""

from __future__ import annotations

import argparse
import dataclasses
import pathlib
import re
import subprocess
import sys


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent


@dataclasses.dataclass
class Mutation:
    file: str
    pattern: str
    replacement: str
    description: str
    target: str


# Each `pattern` MUST uniquely match a single line in `file`; the
# harness aborts a mutation whose pattern matches 0 or >1 times.
MUTATIONS: list[Mutation] = [
    # --- MetalCodegen MSL stdlib helpers ---

    # halfExpLog reduction step: replace the FMA's addend with 0.
    # Catches regressions in the M_LN2 split-precision recombination —
    # any test that exercises exp_h on a non-trivial input observes a
    # 1+ ULP drift relative to the CPU reference.
    Mutation(
        file="lib/IlmCtlMetal/CtlMetalCodegen.cpp",
        pattern=r"float fk = metal::fma\(invln2, x, \(xsb == 0 \? halF_p : halF_n\)\);",
        replacement="float fk = metal::fma(invln2, x, 0.0f);",
        description="halfExpLog: drop reduction-step half offset",
        target="ctlrender-metal",
    ),

    # FP_CONTRACT pragma in the MSL preamble.  Flipping it to ON changes
    # how the Metal compiler fuses scalar `a*b+c` into fmadd, which
    # affects ULP drift on every test that compares against a CPU
    # reference computed without contraction.
    Mutation(
        file="lib/IlmCtlMetal/CtlMetalCodegen.cpp",
        pattern=r'"#pragma STDC FP_CONTRACT OFF\\n"',
        replacement=r'"#pragma STDC FP_CONTRACT ON\\n"',
        description="MSL preamble: FP_CONTRACT OFF -> ON",
        target="ctlrender-metal",
    ),

    # --- MetalDispatch ---

    # Buffer binding index: bind output buffer at index 1 instead of 0.
    # The MSL kernel signature expects out at index 0; the mismatch
    # produces wrong values or a hard error.  Caught by every dispatch
    # test (testMetalHelloWorld and downstream).
    Mutation(
        file="lib/IlmCtlMetal/CtlMetalDispatch.mm",
        pattern=r"\[enc setBuffer:outBuf offset:0 atIndex:0\];",
        replacement="[enc setBuffer:outBuf offset:0 atIndex:1];",
        description="Dispatch: bind output buffer at wrong index",
        target="ctlrender-metal",
    ),

    # --- MetalSidecarCache ---

    # Skip the stale-mtime invalidation entry — the cache returns
    # bytes from a previous run regardless of whether the source has
    # changed.  Caught by testMetalSidecarCache's mtime-bump check.
    # If the test was disabled or removed, this survives — useful as
    # a tripwire.
    Mutation(
        file="lib/IlmCtlMetal/CtlMetalSidecarCache.cpp",
        pattern=r"if \(_mtimeNs != on_disk_mtime_ns\)\n",
        replacement="if (false /* mutation: skip mtime invalidation */)\n",
        description="SidecarCache: skip stale-mtime invalidation",
        target="IlmCtlMetalTest",
    ),
]


def find_unique_match(content: str, pattern: str) -> tuple[int, int] | None:
    matches = list(re.finditer(pattern, content))
    if len(matches) != 1:
        return None
    return matches[0].span()


def apply_mutation(file_path: pathlib.Path, m: Mutation) -> str:
    original = file_path.read_text()
    span = find_unique_match(original, m.pattern)
    if span is None:
        raise RuntimeError(
            f"mutation pattern matched 0 or >1 times in {m.file}: {m.pattern}")
    start, end = span
    file_path.write_text(original[:start] + m.replacement + original[end:])
    return original


def restore(file_path: pathlib.Path, original: str) -> None:
    file_path.write_text(original)


def run_build(build_dir: pathlib.Path, target: str) -> tuple[bool, str]:
    proc = subprocess.run(
        ["cmake", "--build", str(build_dir), "--target", target, "-j"],
        capture_output=True, text=True)
    return (proc.returncode == 0, proc.stderr or proc.stdout[-2000:])


def run_tests(build_dir: pathlib.Path, test_filter: str,
              timeout_s: int = 60) -> tuple[bool, str]:
    cmd = ["ctest", "--timeout", str(timeout_s)]
    if test_filter:
        cmd += ["-R", test_filter]
    try:
        proc = subprocess.run(
            cmd, cwd=str(build_dir), capture_output=True, text=True,
            timeout=timeout_s * 4)
    except subprocess.TimeoutExpired:
        return (False, "harness timeout")
    return (proc.returncode == 0, proc.stdout[-2000:])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--test-filter", default="")
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
            ok_build, _ = run_build(build_dir, m.target)
            if not ok_build:
                print("  uncompilable", flush=True)
                uncompilable.append(m)
                continue
            ok_tests, _ = run_tests(build_dir, args.test_filter)
            if ok_tests:
                print("  SURVIVED", flush=True)
                survivors.append(m)
            else:
                print("  caught", flush=True)
                caught.append(m)
        finally:
            restore(file_path, original)

    print("\n" + "=" * 60)
    print(f"caught:       {len(caught)}/{len(MUTATIONS)}")
    print(f"survived:     {len(survivors)}/{len(MUTATIONS)}")
    print(f"uncompilable: {len(uncompilable)}/{len(MUTATIONS)}")
    if survivors:
        print("\nSurvivors:")
        for m in survivors:
            print(f"  - {m.description}  [{m.file}]")

    # Restore mutated build artifacts to the original source.
    print("\nrestoring build")
    for t in sorted({m.target for m in MUTATIONS}):
        run_build(build_dir, t)

    return 1 if survivors else 0


if __name__ == "__main__":
    sys.exit(main())
