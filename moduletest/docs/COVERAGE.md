# CTL module code coverage

ctltest can report which lines of your `.ctl` modules were executed by your
test suite, in the lcov v1 `.info` format consumed by Codecov, Coveralls,
Codacy, SonarQube, `genhtml`, the VS Code "Coverage Gutters" extension,
and most other coverage tooling.

Statement-level coverage only — branch coverage (taken / not-taken on each
conditional) is not reported. See "Known limitations" below.

## Local use

Build with coverage enabled:

```
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCTL_ENABLE_COVERAGE=ON
cmake --build build -j8
```

Run ctltest with `--coverage`:

```
./build/moduletest/cli/ctltest \
    --coverage coverage.info \
    path/to/your/suite.yaml
```

You can also point at a directory; ctltest walks it for `*.yaml` / `*.yml`
suite files and accumulates coverage across the whole run:

```
./build/moduletest/cli/ctltest \
    --coverage coverage.info \
    moduletest/tests/selftest/
```

Generate HTML:

```
genhtml coverage.info \
    --output-directory coverage-html \
    --title "CTL module coverage" \
    --legend \
    --filter missing

open coverage-html/index.html       # macOS
xdg-open coverage-html/index.html   # Linux
```

`--filter missing` is defensive — `flushToLcov` already drops synthetic
file contexts (e.g. internal testkit shims, module-init instructions
without a source file) before writing the .info, but the flag also
tolerates any path that names a real file the runner can't currently
locate (e.g. moved fixtures), which keeps `genhtml` working on shared
CI runners.

## CI

The `Ubuntu-Coverage` GitHub Actions workflow
(`.github/workflows/ubuntu_coverage.yml`) runs on every push to
`main` / `master` and every pull request. It produces:

- **`coverage.info`** — lcov format, attached as a workflow artifact (30-day
  retention). Machine-readable; consumable by any lcov tool.
- **`coverage-html/`** — `genhtml` output, attached as a workflow artifact.
  Click-through HTML report.
- **Codecov upload** (if `CODECOV_TOKEN` is configured in repo settings) —
  posts a PR comment with file-level coverage deltas vs the base branch
  and updates the project badge.

Download the workflow artifact from the Actions run page to view the
report locally without depending on Codecov. The artifact is the durable
record; Codecov is a convenience layer on top.

## Backend scope

Only the SIMD interpreter is instrumented. The Metal backend is not:
once a CTL function is lowered to MSL and handed to the Metal compiler,
there is no per-CTL-line hook on the GPU. Metal correctness is verified
by the existing output-parity tests against the SIMD backend.

This means coverage reports reflect the code paths that the SIMD
interpreter exercised. Code that runs only on Metal (none today, but in
principle possible if Metal-only optimisations grow) would not appear
hit even when tests pass.

## Off-build behaviour

Building without `-DCTL_ENABLE_COVERAGE=ON` makes ctltest's `--coverage`
flag a no-op: the flag is still parsed (so scripts don't break), but
`SimdCoverage::flushToLcov` returns false and ctltest prints a clear
diagnostic to stderr:

```
warning: --coverage ignored: built without CTL_ENABLE_COVERAGE
```

No `.info` file is written. This means CI scripts that always pass
`--coverage` work fine on non-coverage builds — they just don't get a
report.

## Known limitations

These are deliberate scope cuts. None block the basic CI workflow; each
is a candidate for a future follow-up.

Functions that are loaded but never called appear as `DA:N,0` in the
report — `SimdCoverage::discoverModule` runs eagerly from
`SimdModule::runInitCode` at load time, so fully-uncalled functions still
show up in the coverage tree.

### Statement coverage only, no branch coverage

The dispatch-loop hook records which lines were executed; it does not
distinguish "if branch taken vs not taken" beyond the line-level signal.
A `then`-branch that runs on one SIMD lane out of four still shows as
fully hit in lcov. Branch-aware reporting would require instrumenting
`SimdCondJumpInst` and `SimdBranchInst` to record per-branch lane masks.

### Per-instruction mutex makes coverage builds 5–15× slower

`SimdCoverage::record` takes a global mutex on every executed SIMD
instruction. This is acceptable for the moduletest selftest corpus
(small fixtures, runs in seconds even with the slowdown) but means
coverage runs are **not** appropriate for production-sized images
or perf benchmarking. If a future workload needs faster coverage, swap
the global mutex-protected map for thread-local hit tables merged at
flush time (gcov-style). See the comment in
`lib/IlmCtlSimd/CtlSimdCoverage.h`.

### Hit counts are SIMD-lane multiples

A line that runs once per logical call shows a count equal to the SIMD
lane width (typically 4 or 5 on x86, varies by build). lcov does not
distinguish "call count" from "instruction-execution count," so this
is harmless for percentage / hit-vs-miss reporting but makes raw
counters unsuitable as a proxy for "how many times did this line run."

### Hit counts are not exact under multi-threading

The interpreter is generally single-threaded for ctltest, but if a
future workload calls CTL functions from a thread pool, the per-line
counters race (no atomic increments — only "hit at all" is guaranteed
correct). For statement coverage this is fine; for any consumer that
cares about exact counts, fold in `std::atomic<uint64_t>` or per-thread
accumulators.
