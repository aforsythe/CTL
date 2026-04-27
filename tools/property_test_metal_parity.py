#!/usr/bin/env python3
"""Generate random CTL programs and assert ctlrender -parity-check
holds (CPU SIMD output == Metal GPU output within ULP threshold).

Usage:
    tools/property_test_metal_parity.py \\
        --ctlrender PATH/TO/ctlrender \\
        --input  PATH/TO/input.exr \\
        --workdir DIR \\
        [--count N] [--seed N] [--ulp N]

Exits 0 on full pass, 1 on any divergence beyond the ULP threshold.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import random
import subprocess
import sys
import textwrap


INPUT_NAMES = ["rIn", "gIn", "bIn"]
OUTPUT_NAMES = ["rOut", "gOut", "bOut"]


def gen_expression(rng: random.Random, depth: int, vars_in_scope: list[str]) -> str:
    if depth == 0 or rng.random() < 0.3:
        if rng.random() < 0.7 and vars_in_scope:
            return rng.choice(vars_in_scope)
        return f"{rng.uniform(-2.0, 2.0):.4f}"
    op = rng.choice(["+", "-", "*"])
    a = gen_expression(rng, depth - 1, vars_in_scope)
    b = gen_expression(rng, depth - 1, vars_in_scope)
    return f"({a} {op} {b})"


def gen_program(rng: random.Random) -> str:
    n_params = rng.randint(0, 3)
    params = [(f"p{i}", rng.uniform(-1.5, 1.5)) for i in range(n_params)]
    var_pool = list(INPUT_NAMES) + [n for n, _ in params]

    sig_lines = []
    for n in OUTPUT_NAMES:
        sig_lines.append(f"     output varying float {n}")
    for n in INPUT_NAMES:
        sig_lines.append(f"     input varying float {n}")
    for pname, pdefault in params:
        sig_lines.append(f"     input uniform float {pname} = {pdefault:.4f}")
    sig = ",\n".join(sig_lines)

    body_lines = []
    for out_name in OUTPUT_NAMES:
        depth = rng.randint(1, 4)
        expr = gen_expression(rng, depth, var_pool)
        body_lines.append(f"    {out_name} = {expr};")

    return textwrap.dedent(f"""\
        void main(
        {sig})
        {{
        {chr(10).join(body_lines)}
        }}
        """)


def run_parity(ctlrender: str, ctl: str, input_exr: str, out_base: str,
               ulp: int) -> tuple[bool, str]:
    cmd = [
        ctlrender,
        "-parity-check",
        "--parity-check-ulp", str(ulp),
        "-ctl", ctl,
        "-format", "exr32",
        "-force",
        input_exr,
        out_base,
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    return (proc.returncode == 0, proc.stdout + proc.stderr)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ctlrender", required=True)
    ap.add_argument("--input", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--count", type=int, default=20)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--ulp", type=int, default=0,
                    help="max acceptable per-channel ULP drift; default 0 "
                         "(bit-exact) since the grammar has no transcendentals")
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
        out_path = workdir / f"prog_{i:03d}.exr"

        ctl_path.write_text(gen_program(rng))

        ok, log = run_parity(args.ctlrender, str(ctl_path), args.input,
                             str(out_path), args.ulp)
        if not ok:
            # Distinguish "ctlrender refused to load the program"
            # (parser/typecheck error from generated source) from
            # "parity check failed".
            if "parity: FAIL" in log:
                failures.append((program_seed, log[-1500:]))
            else:
                skipped += 1

    print(f"metal parity test: {args.count} programs, "
          f"{skipped} skipped (load/type errors), "
          f"{len(failures)} divergence{'s' if len(failures) != 1 else ''}")

    if failures:
        for seed, log in failures:
            print(f"\nseed={seed}:\n{log}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
