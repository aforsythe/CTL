# IlmCtlMetal — Apple Silicon Metal GPU backend for CTL

`Ctl::MetalInterpreter` is a drop-in `Ctl::Interpreter` subclass that
transpiles the CTL syntax tree to Metal Shading Language, compiles an
`MTLLibrary` per module, and dispatches one thread per sample over
`MTLResourceStorageModeShared` buffers. The CPU SIMD backend is the
mathematical reference; known deviations are enumerated in
[`PRECISION.md`](PRECISION.md).

Built when `-DCTL_BUILD_METAL_BACKEND=ON` is passed and `APPLE` is true;
force-disabled otherwise.

## Status

| Phase | Deliverable | State |
|-------|-------------|-------|
| 1 | Skeleton — trivial program compiles+dispatches | done |
| 2 | Full CTL language (control flow, arrays, structs, VSArray) | done |
| 3 | Math stdlib — FreeBSD libm ports of `exp`, `log`, `log10`, `asin`, `acos`; remaining transcendentals at `metal::precise::` baseline under ULP-bounded thresholds | done |
| 4 | LUTs, colorspace matrices, limits, interpolators | done; `scatteredDataToGrid3D` ships as an MSL no-op stub — the only in-tree caller runs on the SIMD sidecar (see Deferred below) |
| 5 | `ctlrender-metal` executable with `--benchmark` and `--parity-check` | done |
| 6 | CPU-vs-GPU parity suite + CI benchmark with perf floor | done — see `testMetalParity`, `testMetalBenchmark`, `benchmark_results.json` |
| 7 | `mac_metal.yml` CI workflow, README/INSTALL docs | done |

## Design

- **Runtime MSL transpile** — walk the syntax tree at `Module` load,
  emit MSL, compile via `[MTLDevice newLibraryWithSource:options:]`.
  One compute kernel per callable CTL function.
- **Math-mode gate** — `MTLCompileOptions.mathMode = MTLMathModeSafe`
  on macOS 15+ / Metal 4 (via `respondsToSelector:@selector(setMathMode:)`),
  else `fastMathEnabled = NO` on older toolchains.
- **Unified memory** — `MTLResourceStorageModeShared` buffers read/write
  directly from the C++ caller's `FunctionArg::data()`; no explicit
  host↔device copies.
- **Dispatch shape** — `dispatchThreads:MTLSizeMake(numSamples,1,1)`
  with threadgroup width = `threadExecutionWidth` (32 on Apple 7/8).
- **FMA fusion control** — `FP_CONTRACT OFF` at MSL file scope; every
  `a*b+c` in ported libm kernels emits an explicit `metal::fma(...)`
  call to mirror Apple Clang's ARM64 CPU lowering.
- **Hardware gate** — `supportsFamily:MTLGPUFamilyApple7` at
  `MetalInterpreter` construction; Intel Macs and older Apple GPUs
  raise `Iex::BaseExc` with a clear error.

## Precision policy

The CPU SIMD backend defines correctness. The Metal backend must match
it bit-for-bit on all finite inputs wherever physically possible on
current Apple Silicon GPU hardware.

Where 0-ULP parity is blocked by a hardware-level cause — most commonly
Apple libm's FP64 "promote-compute-narrow" algorithm structure, which
cannot run on a GPU that lacks FP64 — the deviation is recorded in
[`PRECISION.md`](PRECISION.md) with root cause, measured max-ULP,
diverged-fraction on a 1024-sample sweep, and a CI threshold. If the
measured deviation grows past its threshold, `testMetalArithmetic`
fails the build.

Silent precision loss is never acceptable.

## Tests

| Test | Coverage |
|------|----------|
| `testMetalStub` | `MetalInterpreter` lifecycle, device gate |
| `testMetalLContext` | `MetalModule` + `MetalLContext` scaffolding |
| `testMetalHelloWorld` | End-to-end dispatch of a trivial kernel |
| `testMetalCodegen` | MSL source emission against golden fixtures |
| `testMetalDispatch` | Low-level `MetalPipeline` compile + dispatch |
| `testMetalIlmCtlFixtures` | All stdlib-free `unittest/IlmCtl/*.ctl` loaders |
| `testMetalLanguageRejections` | Recursion rejected at codegen time |
| `testMetalAssertFailure` | `assert` failures propagate as `Iex::BaseExc` |
| `testMetalArithmetic` | Per-stdlib-function parity with ULP thresholds |
| `testMetalParity` | End-to-end `.ctl` fixture parity (SIMD vs Metal) |
| `testMetalBenchmark` | CPU/GPU warm-dispatch timings + CI perf floor |

The benchmark writes `benchmark_results.json` to the CWD for CI
artifact upload; the floor guard fails the build if transcendental
warm-dispatch speedup at the largest config drops below
`CTL_METAL_BENCHMARK_FLOOR` (default 1.0×; measured ~3× on M4 Max).

Representative warm-dispatch speedups measured on M4 Max at 1 M
samples:

| Program            | CPU (ms) | GPU (ms) | Speedup |
|--------------------|---------:|---------:|--------:|
| `unity`            |    0.21  |    3.61  |  0.06×  |
| `matrix33`         |    2.69  |    4.10  |  0.66×  |
| `transcendental`   |   11.63  |    3.77  |  3.09×  |
| `aces_v2_jmh`      |  160.80  |    5.04  | 31.88×  |

`aces_v2_jmh` is a self-contained port of the Hellwig2022 JMh forward
pass from `Lib.Academy.OutputTransform.ctl` (ACES v2-dev-release-2) —
the compute-heaviest per-pixel stage of the ACES v2 ODT.
Memory-light programs (`unity`, `matrix33`) are bandwidth-bound and
lose to the CPU's cache hierarchy; the GPU wins decisively on the
transcendental-dense pipelines CTL is actually used for.

## PSO cache

Compiled compute pipeline states are persisted via `MTLBinaryArchive` at
`~/Library/Caches/CTL/metal/<fnv1a-digest>.binarchive`. On a warm start
the PSO step drops from ~26 ms per kernel to ~0.14 ms (measured on the
ACES parity kernel, M4 Max). The archive layer transparently invalidates
entries whose toolchain or GPU family no longer match; the file name is
just a content digest over `(MSL source, kernel name)` so we re-use
entries across modules whose emitted MSL happens to be identical.

The cache does **not** accelerate MSL source compile
(`newLibraryWithSource:`) — that path is unaffected by `MTLBinaryArchive`
and still dominates cold-start latency (~800 ms for ACES v2). Offline
`.metallib` precompile would be the next lever but is out of scope for v1.

Environment overrides:

- `CTL_METAL_CACHE_DIR=<path>` — use a different cache directory (tests
  use `/tmp/...` to avoid polluting the user cache).
- `CTL_METAL_DISABLE_CACHE=1` — bypass the cache entirely.
- `CTL_METAL_CACHE_DEBUG=1` — log hit/miss + PSO timing to stderr.

## Deferred stdlib functions

All CTL stdlib symbols are registered. One symbol ships an MSL no-op
stub rather than a real GPU implementation; its correctness on the
only in-tree fixture that exercises it is delivered by the SIMD
sidecar's module-init pass.

- **`scatteredDataToGrid3D`** — the CPU path runs a full RBF
  interpolator (`Ctl::RbfInterpolator`, see
  `lib/IlmCtlMath/CtlRbfInterpolator.cpp`) that solves an
  `(N+4)×(N+4)` linear system and samples it over a regular 3D grid.
  The CTL symbol is registered on Metal, so programs that call it
  parse successfully; the MSL helper is a no-op that does not touch
  the output grid. The only in-tree caller,
  `unittest/IlmCtl/testInterpolator.ctl`, wraps its two test
  functions in `const int t = runTest();` at module scope, so the
  real RBF solve and its `equalWithAbsErr` grid assertions run on
  the SIMD sidecar during module-init (see
  `memory/metal_module_init_limit.md`) rather than on the GPU.
  Calling the symbol from a kernel-dispatched path is **not
  supported** — the stub would return garbage. The real fix when a
  runtime caller appears is to detect it in `MetalCallNode`, run the
  solve on the host at `Module` init, and bind the resulting grid
  as a `device` MTLBuffer the kernel reads.

## Related files

- `PRECISION.md` — the deviation log (authoritative).
- `lib/IlmCtl/` — front-end interpreter contract this backend
  subclasses from.
- `lib/IlmCtlSimd/` — CPU SIMD backend; the mathematical reference.
- `ctlrender-metal/` — sibling CLI that reuses `ctlrender/`'s sources
  via `target_sources()` with `-DCTL_GPU_BACKEND=1`.
- `.github/workflows/mac_metal.yml` — CI job that builds this backend
  on an Apple Silicon runner.
