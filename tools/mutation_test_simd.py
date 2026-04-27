#!/usr/bin/env python3
"""Apply each mutation in MUTATIONS, incrementally rebuild + run tests,
report survivors (mutations no test caught).

Targets lib/IlmCtlSimd/* and ctlrender/* (CPU-side parallel paths).

Usage:
    tools/mutation_test_simd.py --build-dir /path/to/build [--test-filter REGEX]

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
    # SimdReg / SimdBoolMask register-class invariants

    # testSimdRegAddr's lane[i]==seed-after-widen check pins this.
    Mutation(
        file="lib/IlmCtlSimd/CtlSimdReg.h",
        pattern=r"memset\(data, _data\[0\], MAX_REG_SIZE\);",
        replacement="memset(data, 0, MAX_REG_SIZE);",
        description="SimdBoolMask widen: broadcast 0 instead of inline value",
        target="IlmCtlTest",
    ),
    # testSimdRegAddr's lane-discrimination check pins this.
    Mutation(
        file="lib/IlmCtlSimd/CtlSimdReg.h",
        pattern=r"_inlineData = _data\[0\];",
        replacement="_inlineData = _data[1];",
        description="SimdBoolMask narrow: preserve lane[1] instead of lane[0]",
        target="IlmCtlTest",
    ),
    # Documented expected survivor: skipping the delete leaks but
    # doesn't change observable values.  Only ASan catches it.
    Mutation(
        file="lib/IlmCtlSimd/CtlSimdReg.cpp",
        pattern=r"if \(_dataOwned\)\n\tdelete \[\] _data;\n\n    //",
        replacement="if (_dataOwned)\n\t{ /* leak */ }\n\n    //",
        description="reference(): leak _data instead of deleting (ASan-only catch)",
        target="IlmCtlTest",
    ),

    # Interpreter dispatch invariants

    # testConcurrentCalls REQUIRE()s shape.samples <= maxSamples for
    # every shape (8192 specifically); halving breaks that.
    Mutation(
        file="lib/IlmCtlSimd/CtlSimdInterpreter.cpp",
        pattern=r"return MAX_REG_SIZE;",
        replacement="return MAX_REG_SIZE / 2;",
        description="maxSamples() returns half MAX_REG_SIZE",
        target="IlmCtlTest",
    ),

    # SimdInst branch + loop semantics

    # Drop the active-mask AND in trueMask construction — branches
    # silently take the true path on lanes the outer mask filtered out.
    Mutation(
        file="lib/IlmCtlSimd/CtlSimdInst.cpp",
        pattern=r"const bool t  = mi & ci;",
        replacement="const bool t  = ci;",
        description="SimdBranchInst: ignore mask in trueMask construction",
        target="IlmCtlTest",
    ),
    # Symmetric falseMask version.
    Mutation(
        file="lib/IlmCtlSimd/CtlSimdInst.cpp",
        pattern=r"const bool f  = mi & !ci;",
        replacement="const bool f  = !ci;",
        description="SimdBranchInst: ignore mask in falseMask construction",
        target="IlmCtlTest",
    ),
    # Neither-branch lanes get a non-zero bit pattern.  Survives
    # under black-box CTL tests (lanes are mask-filtered downstream)
    # but useful as a tripwire if a future test reads the merge reg.
    Mutation(
        file="lib/IlmCtlSimd/CtlSimdInst.cpp",
        pattern=r"memset \(\(\*outReg\)\[i\], 0, eSize\);",
        replacement="memset ((*outReg)[i], 1, eSize);",
        description="SimdBranchInst: neither-branch lanes get pattern 0x01 not 0x00",
        target="IlmCtlTest",
    ),
    # `&=` -> `=` on the per-lane loop mask: lanes that should have
    # exited keep iterating whenever the condition is true.
    Mutation(
        file="lib/IlmCtlSimd/CtlSimdInst.cpp",
        pattern=r"loopMask\[i\] \&= \*\(bool\*\)\(condition\[i\]\);",
        replacement="loopMask[i] = *(bool*)(condition[i]);",
        description="SimdLoopInst: condition mask assignment forgets prior mask state",
        target="IlmCtlTest",
    ),

    # ctlrender threading + parallel float->half

    # fetch_add -> load: workers race on the same tile, output diverges
    # from the -threads 1 reference.
    Mutation(
        file="ctlrender/transform.cc",
        pattern=r"size_t idx = next_tile\.fetch_add\(1, std::memory_order_relaxed\);",
        replacement="size_t idx = next_tile.load(std::memory_order_relaxed);",
        description="transform.cc: tile fetch_add -> load (workers race on same tile)",
        target="ctlrender",
    ),
    # Documented expected survivor: -threads 0 autoselect collapses to
    # 1 worker; output stays byte-equal to -threads 1 reference.
    Mutation(
        file="ctlrender/transform.cc",
        pattern=r"if \(hw > 1\) worker_count = hw;",
        replacement="worker_count = 1;",
        description="transform.cc: -threads 0 autoselect always serial (byte-parity equivalent)",
        target="ctlrender",
    ),
    # One unprocessed element per chunk.  Caught by parallel-half-cmp
    # against the gen_serial_half_ref reference.
    Mutation(
        file="ctlrender/exr_file.cc",
        pattern=r"uint64_t end = begin \+ kChunk;",
        replacement="uint64_t end = begin + kChunk - 1;",
        description="exr_file.cc parallel half: chunk one element short",
        target="ctlrender",
    ),
    # Sanity-check mutation: zero output everywhere.  Trivially caught
    # by every parallel-half test.
    Mutation(
        file="ctlrender/exr_file.cc",
        pattern=r"out\[i\] = half\(fIn\[i\]\);",
        replacement="out[i] = half(0.0f);",
        description="exr_file.cc parallel half: emit zero instead of converted value",
        target="ctlrender",
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
