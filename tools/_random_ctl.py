"""Random CTL-program generator shared by the property-test harnesses.

Grammar is intentionally constrained: function signature matches
ctlrender's rIn/gIn/bIn convention; bodies use only +, -, *
(no division → no /0, no transcendentals → no domain errors).
"""

from __future__ import annotations

import random
import textwrap


INPUT_NAMES = ["rIn", "gIn", "bIn"]
OUTPUT_NAMES = ["rOut", "gOut", "bOut"]


def gen_expression(rng: random.Random, depth: int,
                   vars_in_scope: list[str]) -> str:
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

    sig_lines = [f"     output varying float {n}" for n in OUTPUT_NAMES]
    sig_lines += [f"     input varying float {n}" for n in INPUT_NAMES]
    sig_lines += [f"     input uniform float {pname} = {pdefault:.4f}"
                  for pname, pdefault in params]
    sig = ",\n".join(sig_lines)

    body = "\n".join(
        f"    {out} = {gen_expression(rng, rng.randint(1, 4), var_pool)};"
        for out in OUTPUT_NAMES)

    return textwrap.dedent(f"""\
        void main(
        {sig})
        {{
        {body}
        }}
        """)
