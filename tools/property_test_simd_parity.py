#!/usr/bin/env python3
"""Generate random CTL programs and assert ctlrender produces
byte-identical output at -threads 1 vs -threads N.

Usage:
    tools/property_test_simd_parity.py \\
        --ctlrender PATH/TO/ctlrender \\
        --input  PATH/TO/input.exr \\
        --workdir DIR \\
        [--count N] [--seed N] [--threads-vec N]

Exits 0 on full pass, 1 on any divergence.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import random
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from _random_ctl import gen_program  # noqa: E402


def run_ctlrender(ctlrender: str, ctl: str, input_exr: str, output: str,
                  threads: int) -> tuple[bool, str]:
    proc = subprocess.run(
        [ctlrender, "-threads", str(threads), "-ctl", ctl,
         "-format", "exr32", "-force", input_exr, output],
        capture_output=True, text=True)
    return (proc.returncode == 0, proc.stderr)


def files_equal(a: str, b: str) -> bool:
    if not (os.path.exists(a) and os.path.exists(b)):
        return False
    if os.path.getsize(a) != os.path.getsize(b):
        return False
    with open(a, "rb") as fa, open(b, "rb") as fb:
        return fa.read() == fb.read()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ctlrender", required=True)
    ap.add_argument("--input", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--count", type=int, default=20)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--threads-vec", type=int, default=4,
                    help="thread count for the vector run")
    args = ap.parse_args()

    workdir = pathlib.Path(args.workdir)
    workdir.mkdir(parents=True, exist_ok=True)

    failures: list[tuple[int, str]] = []
    skipped = 0
    master_rng = random.Random(args.seed)

    for i in range(args.count):
        program_seed = master_rng.randint(0, 2**31 - 1)
        rng = random.Random(program_seed)
        ctl_path = workdir / f"prog_{i:03d}.ctl"
        ref_path = workdir / f"prog_{i:03d}_ref.exr"
        out_path = workdir / f"prog_{i:03d}_out.exr"

        ctl_path.write_text(gen_program(rng))

        ok_ref, _ = run_ctlrender(args.ctlrender, str(ctl_path), args.input,
                                  str(ref_path), threads=1)
        if not ok_ref:
            skipped += 1
            continue

        ok_run, err_run = run_ctlrender(args.ctlrender, str(ctl_path),
                                        args.input, str(out_path),
                                        threads=args.threads_vec)
        if not ok_run:
            failures.append((program_seed,
                             f"vector run failed but scalar succeeded:\n{err_run}"))
            continue

        if not files_equal(str(ref_path), str(out_path)):
            failures.append((program_seed,
                             "output diverges between -threads 1 and "
                             f"-threads {args.threads_vec}"))

    print(f"property test: {args.count} programs, "
          f"{skipped} skipped (parse/type errors), "
          f"{len(failures)} divergence{'s' if len(failures) != 1 else ''}")

    if failures:
        for seed, msg in failures:
            print(f"  seed={seed}: {msg}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
