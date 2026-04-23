# Full Metal port of `scatteredDataToGrid3D`

## What the function does (CPU reference)

Signature (from `CtlSimdStdLibInterpolator.cpp:120`):

```
scatteredDataToGrid3D(float data[n][2][3],
                     float pMin[3], float pMax[3],
                     int gridSize[3],
                     output float grid[gridSize[0]][gridSize[1]][gridSize[2]][3])
```

Given `n` scattered (x → y) sample pairs, build an RBF interpolant `g(x) ≈ y`
and evaluate it on a regular 3D lattice between `pMin` and `pMax`.

### CPU algorithm

1. **Affine solve** — 12-unknown sparse CG to fit `y = A·x + b` as a global
   linear trend over the `n` samples (3 output channels × 4 affine coeffs).
2. **Build k-d tree** for fast spatial queries.
3. **Per-sample σ** — for each sample, find 4 nearest neighbors, σᵢ = ½·√Σ‖Δ‖².
   Track `maxSigma`.
4. **Residual RBF solve** — 3 independent sparse CG solves (one per output
   channel), `n × n` matrix where entries are `kernel(‖pᵢ - pⱼ‖, σⱼ)` non-zero
   only within `2·maxSigma`. RHS is `(y - A·x - b)`.
5. **Grid loop** — for each grid point `p`, evaluate `value(p) = Σⱼ λⱼ·kernel(‖p-pⱼ‖, σⱼ) + A·p + b`.

The `kernel` is piecewise cubic with compact support (zero beyond `2σ`).

## Porting challenges specific to MSL

| Concern | CPU | MSL | Mitigation |
|---|---|---|---|
| Linear algebra | `LSSCG<double>` + `CRSOperator<double>` | nothing built-in | Port CG inline, FP32 |
| Precision | `double` throughout | FP32 only | Larger CG tolerance budget; verify numerical stability |
| Dynamic sizing | `std::vector` with runtime `n` | arrays must have compile-time size, or device-buffer pointers | Cap `n ≤ kMaxRbfSamples = 256`, error out above |
| Neighbor search | `PointTree` (k-d tree) | pointer-heavy, avoid on GPU | Brute-force — `n ≤ 256`, linear scan fits in L1 |
| Scratch memory | `std::vector` from heap | thread memory is small (~32 KB); threadgroup is limited | Pre-allocate `kMaxRbfSamples`-sized fixed arrays in `thread` storage; refuse larger |
| Sparse-matrix CG | CRS format, sparse matvec | no sparse BLAS | Dense matrix since we already bounded `n ≤ 256` — matrix is ≤256×256 = 64 K float32 = 256 KB; does not fit in thread memory. Need `threadgroup` or `device` scratch |
| Varying-input case | per-lane RBF eval | same per-lane cost | Support, document cost, matches CPU semantics |

## Proposed MSL structure

Emit four new static inline helpers in `CtlMetalCodegen.cpp`'s preamble:

1. **`ctl_stdlib_rbf_kernel(float val, float sigma)`** — direct port of the
   CPU `kernel()` function. ~15 lines of FMA + branch.

2. **`ctl_stdlib_rbf_solve_cg(...)`** — conjugate-gradient solver.
   Operates on a dense `n × n` float32 matrix + RHS + initial guess, in
   `threadgroup` storage. Parameterized by `n` and `maxIter`.

3. **`ctl_stdlib_rbf_setup(data, n, out_lambdas, out_sigmas, out_affine, out_maxSigma)`** —
   orchestrator. Runs the three sub-solves (affine + σ loop + λ solve).
   All FP32. Hard-caps `n` to `kMaxRbfSamples`; above that, sets
   `__ctl_err_flag |= 4u` and returns a sentinel.

4. **`ctl_stdlib_scatteredDataToGrid3D(...)`** — replaces the current stub.
   Calls `rbf_setup`, then runs the 3-deep grid loop, writing each
   grid-point value via the RBF eval inline.

### Error-flag extension

Current flag bits:

- bit 0: `assert(cond)` failure
- bit 1: kernel-reachable `scatteredDataToGrid3D` stub (will be repurposed or deprecated)

Propose:

- bit 2 (new): `scatteredDataToGrid3D` input `n > kMaxRbfSamples`.
- bit 1: retained for legacy binaries; the new impl doesn't set it, but callers still check both.

`MetalFunctionCall::callFunction` grows one more case to report
`TooManyRbfSamplesExc` (or similar clean NoImplExc) on bit 2.

## Scope estimate

| Phase | What | Time |
|---|---|---|
| 0 | Set up `kMaxRbfSamples`, wire error-flag bit 2 through | 0.5 d |
| 1 | Port `rbf_kernel`, `rbf_setup` (with CG), full MSL emission for no-op-free codegen | 1 d |
| 2 | Port the grid loop + integration with existing VSArray codegen | 0.5 d |
| 3 | Numerical stability validation: match CPU within N-ULP on the golden ACES v2 gamut-cusp table (which uses scatteredDataToGrid3D at init) | 1 d |
| 4 | Kernel-reachable test fixture: a small CTL program that builds an RBF interp at runtime and evaluates it on a tiny grid, parity-check against CPU | 0.5 d |
| 5 | Documentation + PRECISION.md entry, build tidy-up | 0.5 d |

**Total: ~4 engineer-days.** Most risk in phase 3 — FP32 CG convergence vs
the CPU's FP64 may require looser tolerances or more iterations, or
numeric reconditioning (Jacobi preconditioner is easy to add).

## Open questions before I start coding

1. **`kMaxRbfSamples` cap**. Does ACES v2 currently pass `n` larger than
   ~256? I saw `REACH_GAMUT_TABLE` and `GAMUT_CUSP_TABLE` in the sidecar
   cache but didn't compare sizes against the RBF input `n`. Need to
   confirm before fixing the cap. A bigger cap = more threadgroup memory
   (which is limited on Apple Silicon to ~32 KB per threadgroup).

2. **Varying-input case**. Current stub sets bit 1 on every varying call.
   CPU runs N evaluations (one per lane). Do we want to support varying
   inputs, or is it always uniform in practice? Non-varying-only keeps
   the implementation much cleaner; varying adds a `regSize`-deep outer
   loop that pays full RBF setup cost per lane.

3. **FP32 vs FP32-with-Kahan**. The CPU code is `double` throughout.
   The affine solve is cheap enough to not worry, but the residual CG
   over `n` points can suffer cancellation at n > 64. Options:
   (a) plain FP32 — ship, hope tolerance budget absorbs it;
   (b) Kahan summation on the CG accumulator — ~30% more ops per step;
   (c) treat as double-float via paired `(hi, lo)` floats — invasive.

3 seems mainly like option (a) unless measurements show drift.

## Recommendation

Answer the three open questions, then proceed phase-by-phase. At phase
3 I'll report parity numbers; if they're poor we have a decision point
between Kahan (fast to add) or double-float (full day of work).
