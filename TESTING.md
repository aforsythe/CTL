# Testing the CTL performance branches

The ship branches form a linear stack on top of `master`.  Each branch
adds one capability to the one below it; pick the branch with the work
you want and it includes everything below it.

```
ship/gpu-metal            ← + Apple Silicon Metal GPU backend (Apple Silicon only)
ship/moduleTestFramework  ← this branch: + YAML-driven module test framework
ship/ctl-debugger         ← + ctldb (REPL) + ctldap (DAP server)
ship/cpu-perf             ← CPU performance work
master                    ← pre-branch baseline
```

`ship/moduleTestFramework` is the recommended branch on Linux, Windows,
and Intel Macs — it has every platform-agnostic capability.  Apple
Silicon users who additionally want the Metal GPU backend should pull
`ship/gpu-metal`.

## Build and test

```bash
git clone <your-fork>/CTL.git && cd CTL
git checkout ship/moduleTestFramework
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
(cd build && ctest)
```

On Apple Silicon, the same build will exercise the CPU path; switch to
`ship/gpu-metal` for the GPU backend.

### Run from the build tree — do not `make install`

Test against your own workloads using the binaries in the build
directory directly:

```bash
./build/ctlrender/ctlrender -ctl <your.ctl> in.exr out.exr
```

`make install` is not necessary and not recommended while you're
evaluating; it would place the binaries in system directories
(`/usr/local/bin/...`) where they could shadow a production install.

## What's in each branch

### `ship/cpu-perf`

CPU SIMD interpreter performance work:

- Vectorized stdlib transcendentals (Apple Accelerate on macOS, sleef
  cross-platform).
- Threaded tile dispatch + file-level `-jobs N`.
- Host-native ISA (`-mcpu=native`), thin LTO, hidden visibility, and
  profile-guided optimization as defaults.
- `-pixel r,g,b[,a]` CLI mode — apply a CTL chain to a single pixel
  and print the result, no file I/O.
- Inline-substitution of idiomatic `Lib.Academy.Utilities` helpers
  (min/max/clip/copysign/wrap_to_360/radians_to_degrees/degrees_to_radians)
  routed through the SIMD interpreter's batched-math path.

### `ship/ctl-debugger`

Single-pixel debugger for CTL programs.  Two binaries gated on
`-DCTL_ENABLE_DEBUGGER=ON` (OFF by default; normal builds pay no cost):

- `ctldb` — REPL with breakpoints, step, locals inspector.
- `ctldap` — Debug Adapter Protocol server, consumed by the
  [vscode-ctl-debug](https://github.com/aforsythe/vscode-ctl-debug)
  extension.

```bash
cmake -B build-dbg -DCMAKE_BUILD_TYPE=Debug -DCTL_ENABLE_DEBUGGER=ON
cmake --build build-dbg -j
./build-dbg/ctldb/ctldb -ctl path/to/transform.ctl --break transform.ctl:42
```

### `ship/moduleTestFramework` (this branch)

`ctltest` is a declarative conformance framework for CTL modules.
Tests are YAML specs referencing a CTL module plus one of three
input modes; the runner marshals typed CTL values in and out, compares
outputs against a chosen oracle, and reports TAP or JUnit XML for CI.

```bash
./build/moduletest/cli/ctltest --help
./build/moduletest/cli/ctltest path/to/your/tests/
./build/moduletest/cli/ctltest --junit junit.xml path/to/tests/
./build/moduletest/cli/ctltest --tap path/to/tests/ > results.tap
```

Authoring references live in `moduletest/examples/`; the spec format,
oracle behaviour, and marshaling rules are in `moduletest/docs/`.

### `ship/gpu-metal` (Apple Silicon only)

Adds the Apple Silicon Metal GPU backend and a sibling
`ctlrender-metal` CLI.  Requires macOS 14+ on an M1 or newer.  See
`lib/IlmCtlMetal/PRECISION.md` for end-to-end parity bounds.

## Code coverage

ctltest can produce statement-level coverage of `.ctl` modules in lcov
format:

```
cmake -B build -DCTL_ENABLE_COVERAGE=ON
cmake --build build -j8
./build/moduletest/cli/ctltest --coverage out.info path/to/suite.yaml
genhtml out.info -o out-html --filter missing
```

The `Ubuntu-Coverage` GitHub Actions workflow runs on every push and
PR, uploads `coverage.info` + the genhtml HTML as workflow artifacts,
and (if `CODECOV_TOKEN` is configured) posts a Codecov comment with
file-level deltas.

See [`moduletest/docs/COVERAGE.md`](moduletest/docs/COVERAGE.md) for
local + CI usage and known limitations.

## Opt-outs

```bash
-DCTL_PGO=OFF                                  # disable profile-guided optimization
-DCTL_NATIVE_ARCH=OFF                          # portable-arch build
-DCTL_LTO=OFF                                  # disable thin LTO
-DCTL_HIDDEN_VISIBILITY=OFF                    # if building SHARED libs for an external ABI
-DCTL_USE_ACCELERATE=OFF -DCTL_USE_SLEEF=OFF   # scalar libm (bit-exact to pre-branch master)
```

## Reporting results

Please include:

```bash
/usr/bin/time -p ./build/ctlrender/ctlrender -ctl … in.exr out.exr
```

plus your hardware, input image size + format, CTL transform used, and
the measured wall-time.

## Known limits

- **CPU path** builds and runs on every tier-1 target.  Bit-exact
  against pre-branch master with both vector libraries OFF; ≤1 ULP
  per transcendental on the vectorized path.
- **Module test framework** has no platform restrictions.
- **Metal backend** (on `ship/gpu-metal`) requires Apple Silicon GPU
  family 7+.
