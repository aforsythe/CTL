#!/usr/bin/env python3
"""Apply each mutation in MUTATIONS, incrementally rebuild + run tests,
report survivors (mutations no test caught).

Targets lib/IlmCtlMetal/* — codegen helpers, dispatch binding,
sidecar/shader cache invalidation.

Usage:
    tools/mutation_test_metal.py --build-dir /path/to/build [--test-filter REGEX]

Exits 0 if every mutation was caught, 1 if any survived.
"""

from __future__ import annotations

import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from _mutation_harness import Mutation, run  # noqa: E402


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent


# Each `pattern` MUST uniquely match a single line in `file`; the
# harness aborts a mutation whose pattern matches 0 or >1 times.
MUTATIONS: list[Mutation] = [
    # MetalCodegen MSL stdlib helpers

    # Drops the M_LN2 split-precision recombination addend; any test
    # exercising exp_h drifts 1+ ULP from the CPU reference.
    Mutation(
        file="lib/IlmCtlMetal/CtlMetalCodegen.cpp",
        pattern=r"float fk = metal::fma\(invln2, x, \(xsb == 0 \? halF_p : halF_n\)\);",
        replacement="float fk = metal::fma(invln2, x, 0.0f);",
        description="halfExpLog: drop reduction-step half offset",
        target="ctlrender-metal",
    ),
    # Flipping FP_CONTRACT changes Metal-side fmadd fusion, breaking
    # ULP parity against the no-contraction CPU reference.
    Mutation(
        file="lib/IlmCtlMetal/CtlMetalCodegen.cpp",
        pattern=r'"#pragma STDC FP_CONTRACT OFF\\n"',
        replacement=r'"#pragma STDC FP_CONTRACT ON\\n"',
        description="MSL preamble: FP_CONTRACT OFF -> ON",
        target="ctlrender-metal",
    ),

    # MetalDispatch

    # MSL kernel signature expects out at index 0; binding at 1 yields
    # wrong values or a hard error.  Caught by every dispatch test.
    Mutation(
        file="lib/IlmCtlMetal/CtlMetalDispatch.mm",
        pattern=r"\[enc setBuffer:outBuf offset:0 atIndex:0\];",
        replacement="[enc setBuffer:outBuf offset:0 atIndex:1];",
        description="Dispatch: bind output buffer at wrong index",
        target="ctlrender-metal",
    ),

    # MetalSidecarCache

    # Skipping the stale-mtime check returns cached bytes after source
    # changes.  Caught by testMetalSidecarCache's mtime-bump check;
    # survives if that test is removed (useful tripwire).
    Mutation(
        file="lib/IlmCtlMetal/CtlMetalSidecarCache.cpp",
        pattern=r"if \(_mtimeNs != on_disk_mtime_ns\)\n",
        replacement="if (false /* mutation: skip mtime invalidation */)\n",
        description="SidecarCache: skip stale-mtime invalidation",
        target="IlmCtlMetalTest",
    ),
]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--test-filter", default="")
    args = ap.parse_args()
    return run(MUTATIONS, REPO_ROOT, pathlib.Path(args.build_dir).resolve(),
               args.test_filter)


if __name__ == "__main__":
    sys.exit(main())
