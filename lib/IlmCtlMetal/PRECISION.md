# CTL Metal backend — precision deviations

This file tracks every stdlib function whose Metal-backend output does
*not* match the CPU SIMD backend at 0 ULP on all finite inputs. The
plan's merge gate is 0-ULP parity; any function listed here is
explicitly accepted under a documented root cause and a
CI-enforced upper-bound threshold. Silent precision loss is never
acceptable — a function is either 0-ULP, or it appears below.

## How a deviation is recorded

Each entry states the function, the measured max-ULP and divergence
rate on the standard 1024-sample `testMetalArithmetic` input range, the
root cause, and the threshold enforced by the corresponding parity
test. If a deviation grows past its threshold, the build fails.

## Root cause common to all single-argument transcendentals: Apple libm computes in FP64

Disassembly of `libsystem_m.dylib`'s `expf` on macOS 15 / Apple Silicon
(`0x1aaad5a70`, 2026-04-19) shows the algorithm is:

1. `fcvt d0, s0` — promote `float` argument to `double`.
2. `fmadd d1, d3, d0, d2` — compute `128/ln2 * x + 1.5·2^52` (FP64 magic
   round).
3. Bit-extract the integer index `k`, look up `2^(k/128)` from a
   128-entry `double` table at `_FE_DFL_DISABLE_DENORMS_ENV + 48`.
4. Evaluate a degree-2 polynomial in `double` precision.
5. `fcvt s0, d0` — narrow back to `float`.

Every step between the two `fcvt`s runs in **FP64**. `logf`, `sinf`,
`cosf`, `tanf`, and the other single-argument transcendentals follow
the same "promote-compute-narrow" pattern — it's the standard way to
deliver correctly-rounded single-precision transcendentals.

**Apple Silicon GPU MSL does not support `double`.** There is no
supported path to 64-bit floating-point on current Apple GPU hardware
(neither native nor via MSL type). Software FP64 via double-float pairs
would be 50–100× slower than native single-precision, erasing the
entire reason this backend exists.

Consequence: **0-ULP parity with Apple's libm single-precision
transcendentals is physically unachievable** on current Apple Silicon
GPU hardware. Every entry in the table below inherits this same
root cause. The per-function rows below document what each
implementation *does* achieve, and the CI threshold that guards it.

Precision strategy: deviations with hardware-level causes are
accepted under documentation and a regression-guarding upper bound —
"precision as close to the CPU path as physically possible," not a
bit-exact mandate that the hardware cannot satisfy.

## Current deviations

### `exp` — hand-rolled FreeBSD `s_expf.c` port

- **Measured** (macOS 15.x / M4 Max, 2026-04-19, 1024 samples uniform
  over `[-16, 16]`): max **1 ULP**, **99 / 1024** samples diverge.
- **Baseline** (`metal::precise::exp`, same input): max 1 ULP, 382 / 1024.
  The port cuts divergence ~4×; the remaining 1-ULP floor is the FP64
  gap described above.
- **Implementation:** `ctl_stdlib_exp` in `CtlMetalCodegen.cpp` — a
  byte-for-byte single-precision port of FreeBSD `msun/src/s_expf.c`,
  with explicit `metal::fma` calls at the three fusion points Apple
  Clang emits on ARM64 (`invln2*x + halF`, `x − t*ln2HI`, polynomial).
  `FP_CONTRACT OFF` at file scope keeps every other `a*b + c` discrete.
- **Root cause of residual 1 ULP:** Apple libm's `expf` computes the
  reduction, table lookup, and polynomial in FP64 (see section above).
  Our single-precision port matches Apple's algorithm structure but
  cannot reproduce FP64 rounding in single precision.
- **Threshold:** max ≤ 1 ULP. Enforced by `testMetalArithmetic`'s
  `stdlib exp` fixture.
- **Plan-to-close:** none. Blocked by Apple Silicon GPU FP64
  unavailability. Row stays until Apple ships FP64 on GPU (not expected)
  or a faster-than-software double-float emulation is justified.

### `log10` — hand-rolled FreeBSD `s_log10f.c` port

- **Measured** (macOS 15.x / M4 Max, 2026-04-19, 1024 samples,
  `fabs(a)+1.0` ∈ `[1, 17]`): max **1 ULP**, **37 / 1024** diverge.
- **Baseline** (`metal::precise::log10`, same input): max 2 ULP,
  261 / 1024. The port is a ~7× reduction in diverged fraction AND
  drops the max from 2 ULP to 1 ULP.
- **Implementation:** `ctl_stdlib_log10` in `CtlMetalCodegen.cpp` — a
  single-precision port of FreeBSD `msun/src/s_log10f.c`. Shares the
  range reduction and `k_log1pf` polynomial kernel with the `log` port
  (substitution `s = f/(2+f)`, 4-term polynomial in `s²`). Key trick:
  split `log(1+f) = hi + lo` where `hi = (f - hfsq)` has its low 12
  mantissa bits zeroed, making `hi * ivln10hi` exact (ivln10hi's low
  13 bits are zero by construction). The final accumulator
  `y*log10_2lo + (hi+lo)*ivln10lo + lo*ivln10hi + hi*ivln10hi +
  y*log10_2hi` sums smallest-to-largest.
- **Root cause of residual 1 ULP:** FP64 floor (see top-of-file).
- **Threshold:** max ≤ 1 ULP.
- **Plan-to-close:** none (FP64 floor).

### `pow` — hand-rolled FreeBSD `e_powf.c` port

- **Measured** (macOS 15.x / M4 Max, 2026-04-19, 1024 samples,
  `pow(fabs(a)+1, b*0.25)`): max **1 ULP**, **110 / 1024** diverge.
- **Baseline** (`metal::precise::pow`, same input): max 1 ULP, 382 / 1024.
  The port is a ~3.5× reduction in diverged fraction.
- **Implementation:** `ctl_stdlib_pow` in `CtlMetalCodegen.cpp` — a
  single-precision port of FreeBSD `msun/src/e_powf.c`. Two-stage
  algorithm: (1) compute `log2(|x|) = n + (p_h + p_l)` via a 6-term
  Horner polynomial in `s = (ax-bp)/(ax+bp)` with `bp ∈ {1.0, 1.5}`,
  using Dekker-style hi/lo splits for the intermediate `s_h/s_l`,
  `t_h/t_l`, `p_h/p_l` pairs; (2) compute `2^(y * log2(|x|))` via a
  degree-5 polynomial in the fractional part plus direct exponent
  bit-fiddle. Explicit `metal::fma` at every Horner step (six L-coeff
  steps + five P-coeff steps) plus at the fused `cp_l*p_h + p_l*cp`
  and `(y-y1)*t1 + y*t2` patterns where Apple Clang emits `fmadd`
  on ARM64.
- **Root cause of residual 1 ULP:** FP64 floor (see top-of-file).
- **Threshold:** max ≤ 1 ULP. Enforced by `testMetalArithmetic`'s
  `stdtrans_pow` fixture.
- **Plan-to-close:** none (FP64 floor).
- **Downstream benefit:** `XYZtoLuv` diverged count dropped from
  209/1024 to 47/1024; `XYZtoLab` from 568/1024 to 263/1024 with
  max ULP 7 → 5. ACES v2 `aces_to_JMh` / `JMh_to_output_XYZ`
  composition on marci-512.exr (200 704 pixels) dropped from 87 %
  diverged at max 293 ULP to 27 % diverged at max 293 ULP — the
  divergence rate tracks pow's individual improvement 1:1; the
  residual 293-ULP tail concentrates on 42 pixels (0.02 %) where
  CAM16 opponent-axis cancellation (`a = R - 12/11·G + 1/11·B`
  with near-neutral inputs) amplifies pow's 1-ULP floor. Absolute
  error at the worst pixel is 2.7e-7 on a 0.015 output — 25× below
  a half-precision ULP, so invisible at display precision.

### `log` — hand-rolled FreeBSD `s_logf.c` port

- **Measured** (macOS 15.x / M4 Max, 2026-04-19, 1024 samples,
  `fabs(a)+1.0` ∈ `[1, 17]`): max **1 ULP**, **68 / 1024** diverge.
- **Baseline** (`metal::precise::log`, same input): max 2 ULP,
  437 / 1024. The port is a ~6.4× reduction in diverged fraction AND
  drops the max from 2 ULP to 1 ULP.
- **Implementation:** `ctl_stdlib_log` in `CtlMetalCodegen.cpp` — a
  single-precision port of FreeBSD `msun/src/s_logf.c`. Range reduction
  `x = 2^k*(1+f)`, substitution `s = f/(2+f)`, then the 4-term
  polynomial in `s²`: `R(z) = z*(Lg1 + z*(Lg2 + z*(Lg3 + z*Lg4)))`.
  Result is `hfsq - (hfsq - R(s²) - s*(hfsq + R(s²))) + dk*ln2_lo
  - f + dk*ln2_hi`, with the hi/lo split of `ln2` keeping the
  exponent-restore addition exact. `metal::fma` at every fusion point
  Apple Clang emits on ARM64.
- **Root cause of residual 1 ULP:** FP64 floor (see top-of-file).
- **Threshold:** max ≤ 1 ULP.
- **Plan-to-close:** none (FP64 floor).

### `asin` — hand-rolled FreeBSD `s_asinf.c` port

- **Measured** (macOS 15.x / M4 Max, 2026-04-19, 1024 samples,
  `a * 0.03125` ∈ `[-0.5, 0.5]`): max **1 ULP**, **48 / 1024** diverge.
- **Baseline** (`metal::precise::asin`, same input): max 1 ULP,
  372 / 1024. Port is a ~7.7× reduction in diverged fraction.
- **Implementation:** `ctl_stdlib_asin` in `CtlMetalCodegen.cpp` — a
  single-precision port of FreeBSD `msun/src/s_asinf.c`. Shares the
  (3,1) rational `R(z) = z*(pS0+z*(pS1+z*pS2)) / (1+z*qS1)` with the
  `acos` port. Primary regime `|x| < 0.5` is `x + x*R(x²)`.
- **Root cause of residual 1 ULP:** FP64 floor (see top-of-file).
- **Threshold:** max ≤ 1 ULP.
- **Plan-to-close:** none (FP64 floor).

### `acos` — hand-rolled FreeBSD `e_acosf.c` port

- **Measured** (macOS 15.x / M4 Max, 2026-04-19, 1024 samples,
  `a * 0.03125` ∈ `[-0.5, 0.5]`): max **1 ULP**, **86 / 1024** diverge.
- **Baseline** (`metal::precise::acos`, same input): max 2 ULP,
  668 / 1024. The port is a ~7.8× reduction in diverged fraction AND
  drops the max from 2 ULP to 1 ULP.
- **Implementation:** `ctl_stdlib_acos` in `CtlMetalCodegen.cpp` — a
  single-precision port of FreeBSD `msun/src/e_acosf.c`. Primary
  regime `|x| < 0.5` uses the (3,1) rational `z*(pS0+z*(pS1+z*pS2)) /
  (1+z*qS1)`; the outer two regimes add the sqrt-based reduction. FMA
  fusion applied at every `a*b + c` the FreeBSD source contains.
- **Root cause of residual 1 ULP:** FP64 floor (see top-of-file
  section).
- **Threshold:** max ≤ 1 ULP.
- **Plan-to-close:** none (FP64 floor).

### `sinh` / `cosh` / `tanh` — Taylor + `ctl_stdlib_exp` composition

- **Measured** (macOS 15.x / M4 Max, 2026-04-19, 1024 samples):
  | Function | Input domain | Max ULP | Diverged |
  |----------|--------------|--------:|---------:|
  | `sinh` | `a * 0.1` ∈ `[-1.6, 1.6]` | **1** | 130 / 1024 |
  | `cosh` | `a * 0.1` ∈ `[-1.6, 1.6]` | **1** | 172 / 1024 |
  | `tanh` | `a`       ∈ `[-16, 16]`   | **1** |  18 / 1024 |
- **Baseline** (`metal::precise::sinh/cosh/tanh`, same inputs):
  all three at max 2 ULP, diverged 355 / 212 / 241. The ports drop
  max from 2 ULP to 1 ULP and reduce divergence by 2.7× / 1.2× / 13×
  respectively.
- **Implementation:** `ctl_stdlib_sinh` / `ctl_stdlib_cosh` /
  `ctl_stdlib_tanh` in `CtlMetalCodegen.cpp`. Domain-split strategy:
  - `|x| < 1.0` (sinh) or `|x| < 0.5` (cosh, tanh): Taylor polynomial
    evaluated via `metal::fma`-Horner. Avoids the 1–2 ULP cancellation
    that `(exp(x) ± 1/exp(x))/2` accumulates when the two exponentials
    are similar magnitude (sinh near zero) or when the result is
    1.0-dominated (cosh near zero).
    - `sinh`: through `x¹¹/11!` — truncation at |x|=1 is ~1.6e-10.
    - `cosh`: through `x¹⁰/10!` — truncation at |x|=0.5 is ~5e-13.
    - `tanh`: through `x¹³` using the Bernoulli-series rational
      coefficients (1, -1/3, 2/15, -17/315, 62/2835, -1382/155925,
      21844/6081075).
  - `1.0 ≤ |x| < 22` (sinh) / `0.5 ≤ |x| < 22` (cosh):
    `(ctl_stdlib_exp(|x|) ± 1/ctl_stdlib_exp(|x|)) / 2`. Leverages
    the 1-ULP exp port directly.
  - `0.5 ≤ |x| < 9.0103` (tanh): `1 - 2/(exp(2|x|)+1)` — FreeBSD
    `s_tanhf.c`'s formulation, no cancellation because `exp(2|x|)+1 > 2`.
  - `|x| < 88.72`: `0.5 * ctl_stdlib_exp(|x|)` (1/exp underflows).
  - `88.72 ≤ |x| < 89.42` (cosh only): `0.5 * w * w` where
    `w = exp(|x|/2)`, avoiding mid-computation overflow.
  - `|x| ≥ 89.42` (sinh/cosh) / `|x| ≥ 9.0103` (tanh): return
    ±infinity / ±1 respectively.
- **Root cause of residual 1 ULP:** identical to the FP64 floor that
  bounds `ctl_stdlib_exp` — any composition inherits exp's 1-ULP tail
  on inputs where Apple libm's FP64 polynomial rounds differently.
- **Threshold:** max ≤ 1 ULP per function. Enforced by
  `stdtrans_sinh/cosh/tanh` fixtures in `testMetalArithmetic`.
- **Plan-to-close:** none below 1 ULP (FP64 floor).

### Other `metal::precise::` transcendentals — baseline

All wired through `metal::precise::fn`; all diverge from Apple libm for
the same polynomial-mismatch reason as `exp` / `log`. Measured under
the same conditions (macOS 15.x / M4 Max 2026-04-18, 1024 samples)
over the per-function input domain noted below. Each entry lists
**max ULP** and **diverged fraction**, and is guarded by a matching
`ulpBoundedFixture` threshold in `testMetalArithmetic`.

| Function | Input domain (CTL expression) | Max ULP | Diverged |
|----------|-------------------------------|--------:|---------:|
| `atan`   | `a`                            | 1 | 376 / 1024 |
| `atan2`  | `atan2(a, b)`                  | 2 | 353 / 1024 |
| `hypot`* | `hypot(a, b)`                  | 1 | 173 / 1024 |
| `pow10`  | `pow10(a * 0.25)` ∈ `[10⁻⁴,10⁴]` | 1 | 410 / 1024 |

*`hypot` is hand-rolled as `precise::sqrt(x*x + y*y)` because MSL does
not expose `metal::hypot` at all. This loses CPU libm's overflow-safe
scaling — inputs whose squares exceed `FLT_MAX` produce `+inf` on the
GPU where CPU would still return finite. See the source comment in
`CtlMetalCodegen.cpp`'s preamble for the path to a full libm port
(`e_hypotf.c`) if a CTL program ever hits that regime.

### `sin` / `cos` / `tan` — FreeBSD `k_sinf.c` / `k_cosf.c` FP32 kernels + Cody-Waite

- **Measured** (macOS 15.x / M4 Max, 2026-04-19, 1024 samples):
  * `sin(a)`,  `a` ∈ `[-16, 16]`: max **1 ULP**, **161 / 1024** diverge.
  * `cos(a)`,  `a` ∈ `[-16, 16]`: max **1 ULP**, **186 / 1024** diverge.
  * `tan(a*0.1)`, `a*0.1` ∈ `[-1.6, 1.6]`: max **2 ULP**, **402 / 1024** diverge.
- **Baseline** (`metal::precise::sin/cos/tan`, same inputs):
  * `sin`: 2 ULP / 441 diverged
  * `cos`: 2 ULP / 455 diverged
  * `tan`: 3 ULP / 581 diverged
  The precise:: trio mirrors the single-argument polynomial-in-FP64 path
  documented above, amplified in `tan` because it's sin/cos plus a
  division (any 1-ULP error in the operands can round up to 2–3 ULP in
  the quotient).
- **Implementation:** Cody-Waite 2/π reduction with `PIO2_HI = 0x3FC90F00`
  (8 trailing mantissa zeros — `n*PIO2_HI` is exact for `|n| <= 256`,
  covering the `[-16, 16]` domain's max `|n| = 11` with large margin) +
  `PIO2_LO = π/2 − PIO2_HI`. The FreeBSD pre-double `k_sinf.c` `S1..S5`
  and `k_cosf.c` `C1..C5` coefficients are evaluated via Horner `metal::
  fma` so the rounding count matches ARM64 Clang's fmadd sequence. Cos
  kernel reassociated as `1 − (0.5*z − w*r)` to preserve precision when
  the result approaches 1. `tan(y)` is composed as `sin(y)/cos(y)` on
  the reduced argument — a direct `k_tanf.c` port with rational
  approximation is feasible but sin/cos composition already cuts the
  max bound by 33% and the diverged fraction by 31%.
- **Root cause of residual 1-ULP gap:** same FP64 floor as `exp` / `log`.
  Apple `sinf` / `cosf` disassembly shows `fcvt d0, s0` at entry,
  FP64-tuned polynomial, `fcvt s0, d0` at exit. Metal GPU has no FP64,
  so one ULP of single-precision rounding at the polynomial step cannot
  be recovered. `tan` is 2 ULP because the sin/cos division compounds
  that floor.
- **Threshold:** max ≤ 1 ULP for `sin` / `cos`, ≤ 2 ULP for `tan`.
  Enforced by the `stdtrans_sin` / `stdtrans_cos` / `stdtrans_tan`
  fixtures in `testMetalArithmetic`.
- **Plan-to-close:** none at 0 ULP (FP64 floor). `tan` could be pushed
  to 1 ULP with a direct `k_tanf.c` rational-approximation port; not
  pursued because the sin/cos composition is simpler, the diverged
  fraction already dropped 31%, and no current ACES workload hits tan
  in the chroma-angle hot path.

### `aces log2` (user-level, not stdlib) — inherits `log` floor

ACES v2 `Lib.Academy.Utilities.ctl` defines `log2(x) = log(x) / log(2.0)`.
Not a stdlib entry — listed here because the composition is a common
ACES call site and its drift is enforced by `testMetalArithmetic`'s
`aud_log2` fixture.

- **Measured** (macOS 15.x / M4 Max, 2026-04-19, 1024 samples,
  `fabs(a) + 1.0` ∈ `[1, 17]`): max **2 ULP**, **60 / 1024** diverge.
- **Root cause:** `ctl_stdlib_log` is the FreeBSD-ported 1-ULP kernel.
  The constant-fold on `log(2.0)` does not apply (both CTL backends
  evaluate it each call), so the numerator and denominator each carry
  up to 1 ULP of FP64-gap drift and the division can round up to one
  additional ULP.
- **Threshold:** max ≤ 2 ULP. Enforced by `aud_log2::compute` in
  `testMetalArithmetic`.
- **Plan-to-close:** none at this level — collapses to 1 ULP only if
  the underlying `log` port reaches 0 ULP (blocked by the FP64 floor
  at the top of this file).

**Plan-to-close** (for all of the above): **none at the 0-ULP level.**
The FP64 root cause documented above applies to every single-argument
transcendental Apple libm ships — each promotes to `double`, runs a
table+polynomial scheme in FP64, narrows back. A pure single-precision
port on the GPU can at best approach but not reach the FP64 reference
result. What *is* achievable is reducing the 1-ULP diverged-fraction
by porting Apple's algorithm structure (as done for `exp`). Each such
port is its own judgement call: is the ~4× reduction in diverged-sample
count worth the maintenance cost of a libm port per function? Rows
below stay until either (a) Apple exposes FP64 on Silicon GPU (not
announced), or (b) a specific function's diverged-fraction is judged
too high and gets its own port landed.

## ROI decisions log

These ports were attempted and reverted, or deliberately not attempted.
Recording them here so a future iteration does not re-run the same
experiment and arrive at the same dead end.

### `atan` — FreeBSD `s_atanf.c` port **regressed** (2026-04-19)

Measured max 25 ULP vs `metal::precise::atan`'s 1 ULP on the same
1024-sample sweep. Root cause: FreeBSD's `aT[0..7]` coefficients are a
double-precision minimax for the polynomial
`aT[0]*z + aT[1]*z² + ... + aT[7]*z⁸`, and the degree-8 evaluation in
single precision amplifies coefficient roundoff well past 1 ULP. The
`metal::precise::atan` baseline is better because Apple's libm runs the
same polynomial in FP64 and narrows — something we cannot reproduce on
GPU. Re-derive a single-precision-tuned minimax before trying again;
direct port of the FreeBSD source is **not the answer** for `atan`.

### `pow10` — `exp(x*ln10)` composition **regressed** (2026-04-19)

Composition `ctl_stdlib_exp(x * ln10)` with a hi/lo split of `ln10`
regressed from 410 / 1024 at 1 ULP (baseline `metal::precise::pow(10, x)`)
to 698 / 1024 at **8 ULP**. Apple's `__pow10f` is a direct table-based
10^x algorithm in FP64 — not `exp(x*ln10)` — so reducing via `ln10` and
re-exponentiating injects a second source of single-precision roundoff
that dwarfs any gain from the better `exp` kernel. Composition is **not
a viable strategy** for pow10. A dedicated `s_exp10f.c`-style port
(table of `10^(k/N)`) is the only remaining path; not pursued in this
iteration.

### `sin` / `cos` / `tan` — **landed** (2026-04-19)

Initially parked as "not attempted" on the assumption that FreeBSD's
double-based `__kernel_sindf` / `__kernel_cosdf` / `__kernel_tandf` were
the only available reference. The pre-double FreeBSD FP32 kernels
(`k_sinf.c` / `k_cosf.c` with `S1..S5` / `C1..C5`) are in fact tuned for
single-precision accumulation and port directly. Combined with a
2-piece Cody-Waite π/2 reduction (`PIO2_HI` with 8 trailing mantissa
zeros so `n*PIO2_HI` is exact for `|n| <= 256`), `sin` / `cos` drop from
2 ULP to 1 ULP and `tan` drops from 3 ULP to 2 ULP via sin/cos
composition. See the dedicated section above for measurements and the
CI-enforced thresholds. Moved out of this deferred log.

### `sinh` / `cosh` / `tanh` — **landed** (2026-04-19)

Composed via `ctl_stdlib_exp` + small-|x| Taylor polynomials, avoiding
the FreeBSD `expm1f` port entirely. All three reach 1 ULP per the
section above. Moved out of this deferred log.

### `atan2` — **measured, no improvement from composition** (2026-04-19)

Tried the standard libm-style composition on top of `metal::precise::
atan` — `|arg| <= 1` range reduction via swap plus quadrant fix-ups:

```msl
if (ax >= ay) r = precise::atan(y / x); // + π/−π when x<0
else          r = ±π/2 − precise::atan(x / y);
```

Measured on the same 1024-sample `[-16, 16]²` sweep as the baseline:
- Baseline (`metal::precise::atan2`):   max **2 ULP**, **353** diverged.
- Composed (`precise::atan` + quadrants): max **2 ULP**, **370** diverged.

The composition is fractionally worse, not better, confirming that the
1-ULP gap beyond `atan` comes from the `y/x` (or `x/y`) FP32 rounding
being coarser than Apple libm's FP64 intermediate, not from range
conditioning. Both GPU paths operate in FP32 internally; no
composition on top of FP32 primitives can recover Apple libm's FP64
quotient precision.

Closing the remaining 1 ULP would require either (a) a
single-precision-tuned atan minimax plus a hand-rolled two-argument
atan2 that handles `(y_hi, y_lo)` / `(x_hi, x_lo)` pairs from an
error-free FP32 division (Dekker-style), or (b) an FP64 emulation
path on the GPU — both large investments for a 1-ULP recovery that
no known downstream consumer requires. Deferred until something
compounds this drift the way ACES v2's seven stacked `pow` calls
compounded `metal::precise::pow`'s 1-ULP floor into a 293-ULP tail.
Baseline at 2 ULP / 353 divergent stays as the CI threshold.

### `pow` — **landed** (2026-04-19)

FreeBSD `e_powf.c` port landed as `ctl_stdlib_pow`. Moved out of this
deferred log — see the "pow — hand-rolled FreeBSD `e_powf.c` port"
row above for measured results and the CI-enforced threshold. Trigger
was ACES v2 output-transform CAM16 drift compounding `metal::precise::
pow`'s 1-ULP floor across 7+ per-pixel pow calls into a 293-ULP
downstream tail — the "hard downstream blocker" condition this entry
had been parked behind.

### `a*b+c` → `metal::fma` peephole in MetalBinaryOpNode — **reverted** (2026-04-22)

Prototyped on the hypothesis that Apple Clang's default
`-ffp-contract=on` would contract CPU-side `a*b+c` into a single
`fmadd` and that mirroring the contraction on Metal would tighten
the ACES v2 matrix-multiply drift. Measured on marci-512 /
Rec.709 full pipeline:

- Without fusion: ch0 20670 / ch1 712 / ch2 431 max ULP
- With fusion:    ch0 20670 / ch1 622 / ch2 364 max ULP

ACES gain is marginal. More importantly the hypothesis turned
out wrong: CPU `SimdInterpreter` lowers each CTL binary op to a
separate `SimdInst` whose result is stored to an arena buffer
before the next op reads it, so Clang never fuses across them —
the CPU reference stays double-rounded on `a*b+c`. A Metal-side
fusion therefore makes Metal *tighter* than CPU and breaks gated
0-ULP tests: `testMetalArithmetic` sample 18 (`x = a * 2.0 + b`)
failed at 1 ULP on the `CTL_USE_ACCELERATE=ON` build.

Reverted. The right future direction is either (a) teach
`SimdInterpreter` to emit a fused `MulAdd` SimdInst so both
paths fuse symmetrically, or (b) accept the CPU double-rounded
floor as the reference and keep Metal matching it bit-for-bit
on simple arithmetic.

### `precise::pow` vs FreeBSD port on macOS 26.3 — **re-measured, no change** (2026-04-22)

Re-ran the A/B on macOS 26.3 / M4 Max against today's libm to check
whether Apple had tightened `precise::pow` since 2026-04-19.
Probe: `pow(fabs(x), 0.42)` on 0002.exr's 2.2M pixels (the
workload ACES v2 `post_adaptation_cone_response_compression_fwd`
exercises).

| pow impl | max ULP | ch0 div rate | ch1 div rate | ch2 div rate |
|----------|--------:|-------------:|-------------:|-------------:|
| FreeBSD `e_powf.c` (shipped) | 1 | 7.0% | 6.8% | 7.8% |
| `metal::precise::pow`        | 1 | 53% | 26% | 26% |

Both hit the 1-ULP correctly-rounded bound, but the FreeBSD port
lands on the *correct* side of the halfway point 3–7× more often
against scalar libm. The existing choice stands; no switch.

### `atan2f` port on macOS 26.3 — **NOT attempted** (2026-04-22)

Considered porting FreeBSD's `e_atan2f.c` to close the residual
1–2 ULP of `metal::precise::atan2` that compounds through
`Aab_to_JMh`'s hue path in ACES v2. Not attempted: `e_atan2f.c`
calls `atanf` internally, and the 2026-04-19 `atan` measurement
(above) showed the FreeBSD `s_atanf.c` port regresses from 1 ULP
to 25 ULP because its `aT[]` coefficients are an FP64-tuned
minimax that loses precision in single-precision evaluation.
`e_atan2f.c` would inherit the same regression. Re-evaluate only
after a single-precision-tuned atan minimax is derived; direct
port is **not the answer** for this function family.

### Colorspace forward transforms — inherited `pow` drift

`LuvtoXYZ` / `LabtoXYZ` (inverses) are **0 ULP** — no `pow` on the
inverse path; `fInverse` is a cubic on the positive branch and an
affine on the low branch, both fuse exactly under the recipe in
`memory/metal_fma_fusion_parity.md`.

The forward transforms `XYZtoLuv` / `XYZtoLab` call the scalar
`f(x) = pow(x, 1/3)` branch on each normalized channel. That single
`pow` call inherits `metal::precise::pow`'s 1-ULP drift (see the
`pow` row above), and the drift is then amplified by the subsequent
arithmetic:

| Function | Input domain                    | Max ULP | Diverged |
|----------|---------------------------------|--------:|---------:|
| `XYZtoLuv` | well-separated XYZ ∈ `[0.15, 1.0]` | 4 | 209 / 1024 |
| `XYZtoLab` | well-separated XYZ ∈ `[0.15, 1.0]` | 7 | 568 / 1024 |

**Root cause:** `XYZtoLab` computes `500 * (fX - fY)` and `200 * (fY - fZ)`
where `fX`, `fY`, `fZ` are each `pow(ratio, 1/3)` calls. When the three
ratios are close, `fX - fY` suffers catastrophic cancellation, turning
1 ULP of pow drift into several ULPs in `astar` / `bstar`. `XYZtoLuv`
sees a smaller amplification because its numerator `(13*L) * (u' - u'_n)`
spreads the drift across a longer chain rather than concentrating it
in a subtract.

**Fixture note:** the reduction in `testMetalArithmetic` sums channels
with `fabs(r[0]) + fabs(r[1]) + fabs(r[2])`, not a signed weighted sum
— signed weights let `+L` cancel against `-astar` for some inputs,
collapsing the measured output near zero and inflating relative ULP
into the hundreds. The fabs reduction reports honest per-channel drift.

**Threshold:** `XYZtoLuv` ≤ 4 ULP, `XYZtoLab` ≤ 8 ULP.

**Plan-to-close:** ports the 1-ULP `pow` drift to 0; both thresholds
collapse to 0 at the same time.

### ACES v2 OutputTransform (Rec.709) — full gamut mapper wired in

- **Measured** (macOS 15.x / M4 Max, 2026-04-19, 1024 samples,
  `testMetalAcesV2Parity` on `aces_combined.ctl`): max **55 ULP**,
  **1426 / 3072** channels diverge. 1024 samples × 3 output channels.
  Distribution of the 1426 diverged channels: 305 at ≤1 ULP, 516 at
  ≤2 ULP, 814 at ≤4 ULP, 1156 at ≤8 ULP (81%), 1384 at ≤16 ULP (97%).
  Only 42 channels (1.4% of all outputs, 3% of diverged) exceed 16 ULP;
  these concentrate on the saturated-primary corner of the sample set
  where CAM16 opponent-axis cancellation amplifies `pow`'s 1-ULP floor.
  The high total diverged fraction is expected: ~50 transcendentals per
  pixel × a 1-ULP FP64 floor per call means nearly every pixel lands
  off by at least one bit somewhere in the chain. Bit-exact rate on
  natural imagery (marci-512.exr, 200 704 pixels) is ~73%.
- **Pipeline** (end-to-end CTL → MSL composition per pixel):
  `aces → aces_to_JMh → tonemapAndCompress_fwd → gamutMap_fwd →
  JMh_to_output_XYZ → XYZ_to_display`. `gamutMap_fwd` internally calls
  `cuspFromTable`, `reachMFromTable`, `chromaCompression`, and the
  boundary-intersection solve (`findGamutBoundaryIntersection` +
  `evaluate_gamma_fit` via the `GAMUT_TOP_GAMMA` lookup). Gamut cusp /
  reach / upper-hull-gamma tables are computed on CPU by the SIMD
  sidecar and injected as MSL literal aggregates.
- **Root cause of 55-ULP drift:** arithmetic amplification of the 1-ULP
  `pow`, `log`, `log10`, `exp`, `asin`, `acos`, `sin`, `cos`, `pow10`,
  `sinh` / `cosh` / `tanh` FP64 floors across ~50 transcendentals per
  pixel, compounded at hue / chroma cusp lookup boundaries where small
  input drift flips a binary-search bucket. No new deviation classes
  observed — every ULP traces back to a documented per-function bound.
- **Threshold:** max ≤ 128 ULP, enforced by `testMetalAcesV2Parity`.
  ~2.3× headroom above measured absorbs hardware-family / driver
  variance on future macOS releases, and is tight enough that a
  regression reverting any `ctl_stdlib_*` port back to its
  `metal::precise::*` baseline (historically 4–7× worse on this chain)
  blows past it immediately.
- **Plan-to-close:** collapses to 0 when every individual `ctl_stdlib_*`
  port reaches 0-ULP. Each per-function row above documents the
  remaining gap; most are FP64-floor-limited and require a full
  double-precision path to close (Apple GPUs don't have FP64 hardware,
  so this is effectively the physical limit for the FP32 pipeline).

### ACES v2 OutputTransform (Rec.709) — fwd+inv round-trip

- **Measured** (macOS 15.x / M4 Max, 2026-04-19, 1024 samples,
  `testMetalAcesV2RoundTrip` running `aces → fwd pipeline → inverse
  pipeline → aces` via the new `gamutMap_inv` + `tonemapAndCompress_inv`
  wrappers): max **688 ULP**, **2073 / 3072** channels diverge.
  Distribution of the 2073 diverged channels: 932 ≤8 ULP (45%), 1381
  ≤16 ULP (67%), 1989 ≤64 ULP (96%), 2071 ≤256 ULP (99.9%). Two
  outliers sit between 256 and 688.
- **Pipeline** (per pixel):
  `aces → aces_to_JMh → tonemapAndCompress_fwd → gamutMap_fwd →
  gamutMap_inv → tonemapAndCompress_inv → JMh_to_aces`. Bypasses the
  JMh↔XYZ display wrappers so any deviation is attributable to the
  inverse primitives (`compressGamut(invert=true)`,
  `chromaCompression(invert=true)`, `tonescale_inv`).
- **Root cause of ~10× amplification over forward-only (55 → 688):**
  the round trip exercises every transcendental twice, and the
  `compressionFunction` inverse branch evaluates
  `thr + s * (-nd / (nd - 1))` which blows up near `nd → 1`
  (the compressed-to-boundary limit). Small forward drift in `nd` is
  amplified by the pole, producing the long tail of 2 samples above
  256 ULP.
- **Threshold:** max ≤ 2048 ULP, enforced by `testMetalAcesV2RoundTrip`.
  ~3× headroom above measured; tight enough that reverting an inverse
  primitive back to its `metal::precise::*` baseline (where the
  forward-only chain had already been running 4–7× worse, and the
  round trip would compound) blows past it.
- **Note on `tonescale_inv`:** despite its comment, the ACES v2 reference
  `tonescale_inv(Y, params)` takes `Y` in the dimensionless h-scale
  (`luminanceTS / n_r`), not cd/m² — `tonescale_fwd` returns `h * n_r`
  and the inverse's internal `Z` clamp is in h-scale. The
  `tonemapAndCompress_inv` wrapper divides `luminanceTS` by
  `TSPARAMS.n_r` before calling `tonescale_inv`; without this the
  clamp saturates every typical-magnitude input to the same value.

### ACES v2 Rec.709 output transform — multi-file import path (2026-04-22)

Context: the two entries above (full gamut mapper; fwd+inv round-trip)
drive `aces_combined.ctl`, a flat single-file concatenation of the
`Lib.Academy.*` helpers and the Rec.709 output transform. Up through
2026-04-21 it was the *only* ACES v2 workload the Metal backend could
compile, because each `MetalModule` owned its own `MetalCodegen` and a
kernel built from one module's codegen never saw the MSL bodies of
its imports. The 2026-04-22 shared-codegen change (codegen moved to
`MetalInterpreter`; user-function addrs module-qualified so imported
helpers don't collide) lets the backend compile the actual shipping
`aces-output/d65/rec709/Output.Academy.Rec709-D65_100nit_in_Rec709-D65_BT1886.ctl`,
which imports `Lib.Academy.Utilities`, `Lib.Academy.Tonescale`,
`Lib.Academy.OutputTransform`, and `Lib.Academy.DisplayEncoding`.

- **Measured** (macOS 26.3 / M4 Max, 2026-04-22, natural imagery;
  `ctlrender-metal --parity-check -format tiff16`, CPU reference
  built with `CTL_USE_ACCELERATE=OFF` so the CPU side also runs
  scalar libm). Per-channel stats aggregated over diverged samples
  only; "abs" is max absolute float32 difference in the output, "p99"
  is the 99th percentile ULP across diverged samples.

  | Input (pixels) | ch0 max ULP | ch0 p99 | ch0 abs max | ch2 max ULP | ch2 p99 | ch2 abs max |
  |---|---:|---:|---:|---:|---:|---:|
  | marci-512 (200 704) | 20670 | 464 | 2.2e-5 | 364 | 81 | 6.2e-5 |
  | 0001 (2.2M) | 62 | — | — | 136 389 | — | — |
  | 0002 (2.2M) | 321 694 | 464 | 2.2e-5 | 1 312 975 | 81 | 6.2e-5 |
  | 0006 (2.2M) | 762 | — | — | 518 335 | — | — |
  | 0013 (2.2M) | 14 380 | — | — | 882 131 | — | — |
  | 0019 (2.2M) | 111 | — | — | 3 072 | — | — |

  Typical-case drift is 3–9 ULP (p50) / 17–464 ULP (p99). Max-ULP
  outliers run into the millions on 0002 but correspond to **output
  values near zero** where `ULP(1e-6) ≈ 1e-13` magnifies a normal
  1-ULP per-op drift into a 10⁶-ULP "headline." The honest metric is
  the absolute-diff column: **max abs diff ≤ 8e-5** across every
  sample input and channel, which is ~5 levels at 16-bit and well
  under one level at 8-bit. 8-bit TIFF outputs differ by 0–5 bytes
  out of 805 KB (marci-512) with max ±1 per byte.

- **Drift entry point, bisected** (0002 input, Rec.709 output
  transform split into four `-ctl` probes feeding a bit-identical
  prefix of the full pipeline):

  | Stopping after | ch0 abs max | ch1 abs max | ch2 abs max |
  |---|---:|---:|---:|
  | `clamp_AP0_to_AP1` | **0** | **0** | **0** |
  | + `RGB_to_Aab` | 1.8e-7 | 1.2e-4 | 2.1e-5 |
  | + `Aab_to_JMh` | 2.3e-5 | 1.1e-4 | **2.2e-2** |
  | + `tonemap_and_compress_fwd` | 3.4e-5 | 9.5e-5 | 2.2e-2 |
  | + `gamut_compress_fwd` | 3.8e-5 | 7.1e-5 | 2.2e-2 |
  | full pipeline (+ `display_encoding`) | 2.2e-5 | 7.8e-5 | 6.2e-5 |

  Drift first appears in `RGB_to_Aab`, which runs
  `mult_f3_f33` + `post_adaptation_cone_response_compression_fwd` (=
  `pow(x, 0.42)` per channel) + `mult_f3_f33`. `Aab_to_JMh` then
  amplifies via `atan2` + `wrap_to_360` near the hue-0 boundary.
  `display_encoding` re-amplifies again via `pow(L, 1/2.4)` near
  L=0.

- **Worst-pixel analysis** (0002, pixel (1088, 122) at
  `RGB_to_Aab` output, input `(0.0124, 0.0118, 0.0120)` — nearly
  neutral gray):

  ```
  CPU Aab = (0.160698,  0.606914, -4.84e-7)
  GPU Aab = (0.160698,  0.606916, -3.69e-6)
  ```

  `A` and `a` agree to ~1e-6. The `b` coordinate — the input to
  `atan2` for hue — has cancelled down near zero because the input
  is nearly on the achromatic axis, so normal ~1-ULP drift in the
  matrix multiply and the `pow(x, 0.42)` calls becomes dominant in
  the residual. `atan2` itself is faithful; it just gets a ~3e-6
  disagreement on its `y` argument. **Catastrophic cancellation in
  user-level CTL math, not a backend deviation.**

- **Root cause (no new deviation class):** every ULP traces back to
  the per-function entries above — `pow`'s 1-ULP floor (7% div rate
  on this workload; see measurement in the ROI log below),
  `atan2`'s 2-ULP floor at the `precise::atan2` baseline, and
  arithmetic amplification at ACES-v2-specific cusps (hue wrap,
  gamma-toe, CAM16 cancellation at neutrals). None of these can be
  tightened without either (a) deriving a single-precision-tuned
  minimax for the transcendental involved — unexplored — or (b) an
  FP64 path on the GPU, which Apple Silicon doesn't provide.

- **Threshold:** not asserted. This is an end-user rendering
  workload, not a gated regression fixture. Parity is guarded via
  `testMetalAcesV2Parity` on the flat single-file fixture (above);
  the multi-file path shares the same codegen and stdlib, so a
  regression there will flag first in the gated test.

## CPU-side backend affects measured parity (Accelerate / sleef)

Every parity fixture in `unittest/IlmCtlMetal/` compares Metal GPU
output against `SimdInterpreter` CPU output. The CPU interpreter's
transcendental path is chosen at CMake-configure time:

| CPU backend           | Platform default | Per-function contract |
|-----------------------|:---------------:|:----------------------|
| scalar libm           | (opt-in only)   | reference                              |
| `CTL_USE_ACCELERATE`  | APPLE           | ≤1 ULP vs Apple libm (`vvexpf` etc.)   |
| `CTL_USE_SLEEF`       | non-APPLE       | ≤1 ULP vs libm (`_u10` tier)           |

Accelerate and sleef vectorize the 15 one-arg transcendentals
(`exp`, `log`, `log10`, `sin`, `cos`, `tan`, `asin`, `acos`,
`atan`, `sinh`, `cosh`, `tanh`, `sqrt`, `pow`, `atan2`). `fabs`,
`floor`, `ceil`, `fmod`, `round`, `copysign`, `sign`, and the
classification ops (`isfinite` etc.) stay on scalar libm either way
and remain bit-exact across backends.

**Implication for parity thresholds.** Under stock defaults the
comparison is *GPU-precise:: vs CPU-Accelerate* (or *CPU-sleef* on
Linux/Windows), not GPU-precise:: vs scalar libm. A 1 ULP per-function
CPU drift against libm can be amplified through downstream arithmetic
— the `stdlib fabs/floor/sqrt/fmod` fixture is a worked example: 1 ULP
of `sqrt(s)` with `s ≈ 3.6` becomes ~32 ULP of output at magnitude
~0.07 after the `out = s + floor(..) - fabs(..) + m` sum. Fixture
thresholds accommodate this with a 2× margin over observed.

**To reproduce zero-drift parity** (GPU-precise:: vs scalar libm):

```
cmake -B build -DCTL_BUILD_METAL_BACKEND=ON \
      -DCTL_USE_ACCELERATE=OFF -DCTL_USE_SLEEF=OFF
```

This rebuild is the correct way to isolate **GPU-only** drift. The
default-ON builds measure the **end-to-end** drift a user of the
stock binaries will see — that's what CI gates on, because that's the
comparison a real ACES pipeline performs.

## Half exp/log tables: MTLBuffer hoisting (landed)

The four `half` stdlib transcendentals (`exp_h`, `log_h`, `log10_h`,
`pow10_h`) and `pow_h(half, float)` share three precomputed tables
totaling ~739 KB that give 0-ULP parity with the CPU SIMD backend's
`halfExpLog.h` path. Those tables are no longer emitted inline into
each module's MSL. Instead, three `device const` `MTLBuffer`s are
allocated on the pipeline, uploaded once, and reused across every
dispatch against that pipeline. The tables' bytes cross the CPU→GPU
boundary exactly once per pipeline lifetime, regardless of dispatch
count or sample count.

### Plumbing shape (unconditional, mirrors `__ctl_err_flag`)

Every user helper's signature unconditionally takes three trailing
`device const` pointer args:

```
device const uint*   __ctl_half_log10_tbl
device const uint*   __ctl_half_log_tbl
device const ushort* __ctl_half_exp_tbl
```

Every kernel wrapper declares the three buffer bindings after
`__ctl_err_flag`, and the three pointer variables are forwarded on
every user-to-user and kernel-to-user call site. Stdlib half-exp/log
calls append the three pointers inside `generateCall` on the
`exp_h` / `log_h` / `log10_h` / `pow10_h` / `pow_h` branch.

Unconditional plumbing is the deliberate choice: the earliest
candidate for "conditional" was to walk the call graph and plumb
only when a helper's transitive body reaches a half-exp/log helper,
but that analysis would have to run ahead of `generateStatementList`
(which is where `_halfExpLogUsed` flips). Unconditional plumbing
costs three extra arg tokens per helper and three extra kernel
buffer slots; both are in the noise next to the ~739 KB the hoist
itself saves out of the MSL.

### Host-side binding

`MetalFunctionCall::callFunction` appends three `MetalKernelBinding`
entries after the `__ctl_err_flag` binding. Each binding sets
`persistent = true`, which tells the `MetalPipeline::dispatch`
overload (see `CtlMetalDispatch.mm`) to:

* Look up the binding's `hostData` pointer in the pipeline's
  `std::unordered_map<const void *, id<MTLBuffer>>`.
* On first sight: allocate an `MTLResourceStorageModeShared`
  `MTLBuffer` sized to `binding.bytes`, `memcpy` the table bytes
  once, cache by pointer identity.
* On subsequent dispatches: return the cached buffer as-is. No
  re-upload, no readback.

The table accessors (`halfLog10Table()`, `halfLogTable()`,
`halfExpTable()`) return the same address process-wide, so the
cache key is stable across interpreters that share a process.
Each pipeline owns its own cache, so two interpreters with two
pipelines each pay ~739 KB of GPU memory per pipeline; that is the
worst case and matches what the inline-`constant`-array emission
would have burned inside the compiled pipeline binary anyway.

### Gains vs cost

Measured on the `testMetalHalfExpLog` fixture (macOS 15.x / M4 Max,
2026-04-21, `CTL_METAL_CACHE_DEBUG=1` logging the `newLibraryWithSource`
compile time and MSL byte count for the `top_kernel` pipeline).
Pre-hoist numbers are from commit `740cab6` with the testMetalHalfExpLog
sources pulled forward; post-hoist are from `4102d7d` as landed. Cold
runs wipe `/var/folders/$USER/C/com.apple.metal/` (Apple's persistent
MSL-compile cache) before each invocation.

| Metric                                   | Pre-hoist (740cab6) | Post-hoist (4102d7d) | Ratio       |
|------------------------------------------|--------------------:|---------------------:|------------:|
| MSL source bytes (one kernel)            |           2,565,046 |               64,826 | **39.6× smaller** |
| Cold `newLibraryWithSource` (cache wiped)|           190–198 ms |             40–58 ms | **~4× faster**    |
| Warm `newLibraryWithSource` (cache hit)  |              ~5 ms  |              ~0.6 ms | **~8× faster**    |
| End-to-end cold wall-clock               |              ~280 ms |              ~100 ms | 2.8× faster |
| End-to-end warm wall-clock               |              ~100 ms |               ~50 ms | 2.0× faster |

In-process repeat compilations of the same kernel land in Apple's
in-process cache at ~0.15 ms regardless of source size, so the "three
dispatches against one pipeline + two dispatches against a second
pipeline" shape in the fixture is really 1 cold compile + 1 in-process
cache hit; the numbers above describe the cold/warm compile events.

Cost side:

* Three extra arg tokens on every user helper signature (negligible
  compile-time tax for non-half-exp/log programs; the emitted MSL
  text still shrinks because the tables themselves are gone).
* Three extra `MTLBuffer` bindings per dispatch. All persistent, so
  the runtime cost is a hash-map lookup on the hot path.

### Test coverage

`testMetalHalfExpLog` exercises the plumbing end-to-end: a
three-level call graph (`top -> mid -> leaf`) where only `leaf`
reads the tables, so `mid` is pure forwarding. All five stdlib
helpers are reached through `leaf`. The program is dispatched three
times against the same pipeline to validate cache reuse, then a
second `MetalInterpreter` is constructed to validate that each
pipeline owns its own cache. Parity is 0 ULP vs the SIMD backend on
every dispatch.

