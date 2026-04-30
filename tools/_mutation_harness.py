"""Shared mutation-testing driver.

Apply each mutation in `mutations` to its source file, incrementally
rebuild + run ctest, restore.  Report survivors (mutations no test
caught).  Pattern miss / uncompilable mutations are tracked separately.
"""

from __future__ import annotations

import dataclasses
import pathlib
import re
import subprocess
import sys


@dataclasses.dataclass
class Mutation:
    file: str
    pattern: str
    replacement: str
    description: str
    target: str


def _find_unique_match(content: str, pattern: str) -> tuple[int, int] | None:
    matches = list(re.finditer(pattern, content))
    if len(matches) != 1:
        return None
    return matches[0].span()


def _apply(file_path: pathlib.Path, m: Mutation) -> str:
    original = file_path.read_text()
    span = _find_unique_match(original, m.pattern)
    if span is None:
        raise RuntimeError(
            f"mutation pattern matched 0 or >1 times in {m.file}: {m.pattern}")
    start, end = span
    file_path.write_text(original[:start] + m.replacement + original[end:])
    return original


def _run_build(build_dir: pathlib.Path, target: str) -> bool:
    proc = subprocess.run(
        ["cmake", "--build", str(build_dir), "--target", target, "-j"],
        capture_output=True, text=True)
    return proc.returncode == 0


def _run_tests(build_dir: pathlib.Path, test_filter: str,
               timeout_s: int) -> bool:
    cmd = ["ctest", "--timeout", str(timeout_s)]
    if test_filter:
        cmd += ["-R", test_filter]
    try:
        proc = subprocess.run(
            cmd, cwd=str(build_dir), capture_output=True, text=True,
            timeout=timeout_s * 4)
    except subprocess.TimeoutExpired:
        return False
    return proc.returncode == 0


def run(mutations: list[Mutation],
        repo_root: pathlib.Path,
        build_dir: pathlib.Path,
        test_filter: str = "",
        timeout_s: int = 60) -> int:
    """Run every mutation; return 1 if any survived, 0 if all caught."""
    if not (build_dir / "CMakeCache.txt").exists():
        print(f"error: {build_dir} is not a configured cmake build dir",
              file=sys.stderr)
        return 2

    survivors: list[Mutation] = []
    caught: list[Mutation] = []
    uncompilable: list[Mutation] = []

    for i, m in enumerate(mutations, start=1):
        file_path = repo_root / m.file
        print(f"\n[{i}/{len(mutations)}] {m.description}", flush=True)
        print(f"  file: {m.file}", flush=True)

        try:
            original = _apply(file_path, m)
        except RuntimeError as e:
            print(f"  SKIP: {e}", flush=True)
            continue

        try:
            if not _run_build(build_dir, m.target):
                print("  uncompilable", flush=True)
                uncompilable.append(m)
                continue
            if _run_tests(build_dir, test_filter, timeout_s):
                print("  SURVIVED", flush=True)
                survivors.append(m)
            else:
                print("  caught", flush=True)
                caught.append(m)
        finally:
            file_path.write_text(original)

    print("\n" + "=" * 60)
    print(f"caught:       {len(caught)}/{len(mutations)}")
    print(f"survived:     {len(survivors)}/{len(mutations)}")
    print(f"uncompilable: {len(uncompilable)}/{len(mutations)}")
    if survivors:
        print("\nSurvivors:")
        for m in survivors:
            print(f"  - {m.description}  [{m.file}]")

    # Restore mutated build artifacts to the original source.
    print("\nrestoring build")
    for t in sorted({m.target for m in mutations}):
        _run_build(build_dir, t)

    return 1 if survivors else 0
