///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#include <CtlMetalCodegen.h>
#include <CtlMetalHalfExpLogTable.h>
#include <CtlMetalType.h>
#include <CtlType.h>
#include <Iex.h>

#include <cstdio>
#include <map>
#include <queue>
#include <set>
#include <sstream>

namespace Ctl {

namespace {

//
// `#pragma STDC FP_CONTRACT OFF` turns off opportunistic fma fusion at
// the MSL source level. `fastMathEnabled=NO` / `MTLMathModeSafe` already
// bans reassociation and finite-math assumptions, but contraction is
// controlled separately (Clang's `-ffp-contract`), so any `a*b + c`
// expression can still collapse to a single fma and round differently
// than the CPU SIMD backend's discrete mul + add. We need bit-exact
// parity, so we disable contraction globally.
//
const char * const kPreamble =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "#pragma STDC FP_CONTRACT OFF\n"
    "\n"
    //
    // CTL stdlib named constants. Mirrors the CPU SIMD backend's
    // `defineConstants` table in `CtlSimdStdLibLimits.cpp`. CTL source
    // references (`M_PI`, `FLT_MIN`, etc.) resolve to these MSL
    // identifiers via each symbol's `MetalStaticAddr::mslName()`. The
    // `ctl_const_` prefix keeps the MSL spellings distinct from
    // `<climits>` / `<float.h>` macros shipped by the Metal standard
    // library so there's no preprocessor collision.
    //
    "constant float ctl_const_M_E         = 2.71828182845904523536f;\n"
    "constant float ctl_const_M_PI        = 3.14159265358979323846f;\n"
    "constant float ctl_const_FLT_MAX     = 3.40282346638528859812e+38f;\n"
    "constant float ctl_const_FLT_MIN     = 1.17549435082228750797e-38f;\n"
    "constant float ctl_const_FLT_EPSILON = 1.19209289550781250000e-7f;\n"
    "constant float ctl_const_FLT_POS_INF = as_type<float>(0x7f800000u);\n"
    "constant float ctl_const_FLT_NEG_INF = as_type<float>(0xff800000u);\n"
    "constant float ctl_const_FLT_NAN     = as_type<float>(0x7fc00000u);\n"
    "constant half  ctl_const_HALF_MAX     = as_type<half>((ushort)0x7bffu);\n"
    "constant half  ctl_const_HALF_MIN     = as_type<half>((ushort)0x0400u);\n"
    "constant half  ctl_const_HALF_EPSILON = as_type<half>((ushort)0x1400u);\n"
    "constant half  ctl_const_HALF_POS_INF = as_type<half>((ushort)0x7c00u);\n"
    "constant half  ctl_const_HALF_NEG_INF = as_type<half>((ushort)0xfc00u);\n"
    "constant half  ctl_const_HALF_NAN     = as_type<half>((ushort)0x7e00u);\n"
    "constant int   ctl_const_INT_MAX      = 0x7fffffff;\n"
    "constant int   ctl_const_INT_MIN      = (int)0x80000000;\n"
    "constant uint  ctl_const_UINT_MAX     = 0xffffffffu;\n"
    "\n"
    //
    // Stdlib helpers. One `static inline` per registered CTL stdlib
    // name — see `CtlMetalStdLibrary.cpp`.
    //
    //   fabs  — `metal::fabs` just clears the sign bit; bit-exact.
    //   floor — `metal::floor` is IEEE-754 roundToNegativeInfinity;
    //           bit-exact.
    //   sqrt  — `metal::precise::sqrt` is the correctly-rounded
    //           single-precision sqrt variant. The plain `metal::sqrt`
    //           lands in the default math-mode pool and was observed
    //           drifting 1 ULP from Apple libm on ~1% of samples even
    //           under MTLMathModeSafe (see the `stdmath` parity test).
    //           `precise::sqrt` forces the correctly-rounded path and
    //           matches libm bit-for-bit.
    //   fmod  — `metal::fmod` follows C99 semantics (x - trunc(x/y)*y);
    //           bit-exact.
    //
    // Each helper must match CPU libm bit-for-bit; the `stdmath`
    // parity fixture enforces this on every push. Unused helpers are
    // discarded by the MSL compiler.
    //
    "static inline float ctl_stdlib_fabs(float x)  { return metal::fabs(x); }\n"
    "static inline float ctl_stdlib_floor(float x) { return metal::floor(x); }\n"
    "static inline float ctl_stdlib_sqrt(float x)  { return metal::precise::sqrt(x); }\n"
    "static inline float ctl_stdlib_fmod(float x, float y) { return metal::fmod(x, y); }\n"
    "static inline bool  ctl_stdlib_isfinite_f(float x) { return metal::isfinite(x); }\n"
    "static inline bool  ctl_stdlib_isnormal_f(float x) { return metal::isnormal(x); }\n"
    "static inline bool  ctl_stdlib_isnan_f(float x)    { return metal::isnan(x); }\n"
    "static inline bool  ctl_stdlib_isinf_f(float x)    { return metal::isinf(x); }\n"
    //
    // Transcendentals — BASELINE pass. These are `metal::precise::`
    // variants, which select the correctly-rounded-per-Metal-spec path
    // but are NOT guaranteed to match Apple libm's FreeBSD-derived
    // polynomial approximations bit-for-bit. The precision memory
    // (`metal_precise_namespace.md`) predicts these will diverge from
    // CPU libm by 1–several ULP on a non-trivial fraction of inputs.
    //
    // The dedicated ULP-measurement parity test in `testMetalArithmetic`
    // reports the exact gap per-function; any function that fails the
    // 0-ULP gate must either (a) be replaced with a hand-rolled port of
    // the FreeBSD libm algorithm before shipping, or (b) gain a
    // documented deviation entry in `lib/IlmCtlMetal/PRECISION.md` with
    // a CI-enforced upper bound. No silent precision loss.
    //
    //
    // `expf` — hand-rolled single-precision port of FreeBSD msun
    // `s_expf.c`. Argument reduction
    //     x = k*ln2 + r,   |r| <= 0.5*ln2
    // uses a two-term representation of ln2 (`ln2HI` exact, `ln2LO`
    // correction) to keep `k*ln2` accurate. The primary-range
    // polynomial is degree 2:
    //     exp(r) ≈ 1 + r + r*c/(2 - c)   where c = r - r²*(P1 + r²*P2)
    // then scaled by 2^k via bit-reinterpret. The constants below are
    // byte-identical to `s_expf.c`. `FP_CONTRACT OFF` at file scope
    // keeps every `a*b + c` discrete; explicit `metal::fma` is added
    // at the specific points Apple Clang fuses on ARM64 (so the
    // rounding count on GPU matches CPU).
    //
    // Does NOT hit 0 ULP vs Apple libm `expf`. Cuts the diverged-
    // sample count ~4× (382→99 per 1024 inputs on `[-16, 16]`) but a
    // residual 1-ULP gap remains. Root cause: Apple libm `expf`
    // promotes to FP64 internally (disassembly 2026-04-19: `fcvt d0,
    // s0` at entry, 128-entry FP64 table + FP64 polynomial, `fcvt s0,
    // d0` at exit). Apple Silicon GPU MSL has no `double`, so
    // single-precision GPU math cannot reproduce that rounding.
    // Accepted under `PRECISION.md`'s `exp` entry at ≤ 1-ULP bound.
    //
    "static inline float ctl_stdlib_exp(float x)\n"
    "{\n"
    "    const float ln2HI_p =  6.9314575195e-01f;\n"
    "    const float ln2HI_n = -6.9314575195e-01f;\n"
    "    const float ln2LO_p =  1.4286067653e-06f;\n"
    "    const float ln2LO_n = -1.4286067653e-06f;\n"
    "    const float invln2  =  1.4426950216e+00f;\n"
    "    const float P1      =  1.6666625440e-01f;\n"
    "    const float P2      = -2.7667332906e-03f;\n"
    "    const float halF_p  =  0.5f;\n"
    "    const float halF_n  = -0.5f;\n"
    "    const float o_threshold =  8.8721679688e+01f;\n"
    "    const float u_threshold = -1.0397208405e+02f;\n"
    "    const float huge_val    =  1.0e+30f;\n"
    "    const float twom100     =  7.8886090522e-31f;\n"
    "    float hi = 0.0f, lo = 0.0f;\n"
    "    int k = 0;\n"
    "    uint32_t hx = as_type<uint32_t>(x);\n"
    "    int xsb = int((hx >> 31) & 1u);\n"
    "    hx &= 0x7fffffffu;\n"
    "    // Filter non-finite / overflow / underflow argument.\n"
    "    if (hx >= 0x42b17218u) {\n"
    "        if (hx > 0x7f800000u) return x + x;        // NaN propagation\n"
    "        if (hx == 0x7f800000u) return (xsb == 0) ? x : 0.0f; // +-inf\n"
    "        if (x >  o_threshold) return huge_val * huge_val;    // +inf\n"
    "        if (x <  u_threshold) return twom100 * twom100;      // 0\n"
    "    }\n"
    // Argument reduction: x = k*ln2 + (hi - lo). The explicit
    // `metal::fma` calls below mirror what Apple Clang emits for the
    // FreeBSD source at `-O2` on ARM64 (disassembled: `fmadd` for
    // `invln2*x + halF`, and `fmsub` / `fnmsub` for `x - t*ln2HI_p`).
    // With `FP_CONTRACT OFF` the Metal compiler would otherwise emit
    // discrete mul+add, adding rounding steps CPU doesn't pay.
    "    if (hx > 0x3eb17218u) {\n"
    "        if (hx < 0x3f851592u) {\n"
    "            hi = x - (xsb == 0 ? ln2HI_p : ln2HI_n);\n"
    "            lo = (xsb == 0 ? ln2LO_p : ln2LO_n);\n"
    "            k  = 1 - xsb - xsb;\n"
    "        } else {\n"
    "            float fk = metal::fma(invln2, x, (xsb == 0 ? halF_p : halF_n));\n"
    "            k  = int(fk);\n"
    "            float t = float(k);\n"
    "            hi = metal::fma(-t, ln2HI_p, x);   // t*ln2HI is exact here\n"
    "            lo = t * ln2LO_p;\n"
    "        }\n"
    "        x = hi - lo;\n"
    "    } else if (hx < 0x39000000u) {\n"
    "        // |x| < 2^-14 — fast path. `1 + x` is the correctly rounded\n"
    "        // result to single precision in this regime.\n"
    "        return 1.0f + x;\n"
    "    }\n"
    "    // Primary-range polynomial and scale. Clang fuses\n"
    "    //   c = x - t*(P1 + t*P2)\n"
    "    // into exactly two ARM64 instructions: `fmadd` for\n"
    "    // `(P1 + t*P2)`, then `fmsub` for `x - t * inner`. Mirror with\n"
    "    // two explicit `metal::fma` calls so the rounding count is 2,\n"
    "    // not 4 (mul, add, mul, sub).\n"
    "    float t  = x * x;\n"
    "    float twopk;\n"
    "    if (k >= -125)\n"
    "        twopk = as_type<float>(uint32_t(0x3f800000 + (k << 23)));\n"
    "    else\n"
    "        twopk = as_type<float>(uint32_t(0x3f800000 + ((k + 100) << 23)));\n"
    "    float c = metal::fma(-t, metal::fma(t, P2, P1), x);\n"
    "    if (k == 0)\n"
    "        return 1.0f - ((x * c) / (c - 2.0f) - x);\n"
    "    float y = 1.0f - ((lo - (x * c) / (2.0f - c)) - hi);\n"
    "    if (k >= -125) return y * twopk;\n"
    "    return y * twopk * twom100;\n"
    "}\n"
    //
    // `logf` — hand-rolled single-precision port of FreeBSD msun
    // `s_logf.c`. Range reduction: `x = 2^k * (1 + f)` with
    // `f ∈ [-1/3, 1/3]`, then compute `log(1+f)` via the substitution
    // `s = f / (2+f)` (so `log(1+f) = 2*atanh(s)`), approximated by a
    // 4-term polynomial `R(s²)` whose coefficients Lg1..Lg4 are tuned
    // for a single-precision error bound of 2^-34 (FreeBSD source
    // comment). Pure single-precision throughout — no double-promote —
    // which keeps single-precision rounding error below the FP64 floor
    // Apple libm's `logf` sees.
    //
    "static inline float ctl_stdlib_log(float x)\n"
    "{\n"
    "    const float ln2_hi = 6.9313812256e-01f;\n"
    "    const float ln2_lo = 9.0580006145e-06f;\n"
    "    const float two25  = 3.3554432e+07f;\n"
    "    const float Lg1    = 0.66666662693023682f;\n"
    "    const float Lg2    = 0.40000972151756287f;\n"
    "    const float Lg3    = 0.28498786687850952f;\n"
    "    const float Lg4    = 0.24279078841209412f;\n"
    "    uint32_t ix = as_type<uint32_t>(x);\n"
    "    int k = 0;\n"
    "    if (ix < 0x00800000u) {\n"
    "        if ((ix & 0x7fffffffu) == 0u) return -1.0f / 0.0f;\n"
    "        if (int(ix) < 0) return (x - x) / 0.0f;\n"
    "        k -= 25;\n"
    "        x *= two25;\n"
    "        ix = as_type<uint32_t>(x);\n"
    "    }\n"
    "    if (ix >= 0x7f800000u) return x + x;\n"
    "    k += int(ix >> 23) - 127;\n"
    "    ix &= 0x007fffffu;\n"
    "    uint32_t i = (ix + (uint32_t(0x95f64u) << 3)) & 0x00800000u;\n"
    "    x = as_type<float>(ix | (i ^ 0x3f800000u));\n"
    "    k += int(i >> 23);\n"
    "    float f = x - 1.0f;\n"
    "    if ((0x007fffffu & (15u + ix)) < 16u) {\n"
    "        if (f == 0.0f) {\n"
    "            if (k == 0) return 0.0f;\n"
    "            float dk = float(k);\n"
    "            return metal::fma(dk, ln2_lo, dk * ln2_hi);\n"
    "        }\n"
    "        float R = f * f * metal::fma(-0.33333333333333333f, f, 0.5f);\n"
    "        if (k == 0) return f - R;\n"
    "        float dk = float(k);\n"
    "        return dk * ln2_hi - ((R - dk * ln2_lo) - f);\n"
    "    }\n"
    "    float s  = f / (2.0f + f);\n"
    "    float dk = float(k);\n"
    "    float z  = s * s;\n"
    "    uint32_t iu = ix - (uint32_t(0x6147au) << 3);\n"
    "    float w  = z * z;\n"
    "    uint32_t ju = (uint32_t(0x6b851u) << 3) - ix;\n"
    "    float t1 = w * metal::fma(w, Lg4, Lg2);\n"
    "    float t2 = z * metal::fma(w, Lg3, Lg1);\n"
    "    iu |= ju;\n"
    "    float R  = t2 + t1;\n"
    "    if (int(iu) > 0) {\n"
    "        float hfsq = 0.5f * f * f;\n"
    "        if (k == 0) return f - (hfsq - s * (hfsq + R));\n"
    "        return dk * ln2_hi - ((hfsq - (s * (hfsq + R) + dk * ln2_lo)) - f);\n"
    "    }\n"
    "    if (k == 0) return f - s * (f - R);\n"
    "    return dk * ln2_hi - ((s * (f - R) - dk * ln2_lo) - f);\n"
    "}\n"
    //
    // `log10f` — hand-rolled single-precision port of FreeBSD msun
    // `s_log10f.c`. Same range reduction as `ctl_stdlib_log`, then the
    // key trick: split `log(1+f) = hi + lo` where `hi = (f - hfsq)` has
    // its low 12 mantissa bits zeroed so that `hi * ivln10hi` is exact
    // (ivln10hi = 0x3ede6000 also has 12 trailing zeros). The final
    // accumulator
    //   y*log10_2lo + (hi+lo)*ivln10lo + lo*ivln10hi + hi*ivln10hi
    //   + y*log10_2hi
    // sums from smallest to largest to keep single-precision rounding
    // error below the FP64 floor Apple's `log10f` hits internally.
    //
    "static inline float ctl_stdlib_log10(float x)\n"
    "{\n"
    "    const float two25     =  3.3554432e+07f;\n"
    "    const float ivln10hi  =  4.3432617188e-01f;\n"
    "    const float ivln10lo  = -3.1689971365e-05f;\n"
    "    const float log10_2hi =  3.0102920532e-01f;\n"
    "    const float log10_2lo =  7.9034151668e-07f;\n"
    "    const float Lg1 = 0.66666662693023682f;\n"
    "    const float Lg2 = 0.40000972151756287f;\n"
    "    const float Lg3 = 0.28498786687850952f;\n"
    "    const float Lg4 = 0.24279078841209412f;\n"
    "    uint32_t ix = as_type<uint32_t>(x);\n"
    "    int k = 0;\n"
    "    if (ix < 0x00800000u) {\n"
    "        if ((ix & 0x7fffffffu) == 0u) return -1.0f / 0.0f;\n"
    "        if (int(ix) < 0) return (x - x) / 0.0f;\n"
    "        k -= 25;\n"
    "        x *= two25;\n"
    "        ix = as_type<uint32_t>(x);\n"
    "    }\n"
    "    if (ix >= 0x7f800000u) return x + x;\n"
    "    if (ix == 0x3f800000u) return 0.0f;\n"
    "    k += int(ix >> 23) - 127;\n"
    "    ix &= 0x007fffffu;\n"
    "    uint32_t i = (ix + 0x004afb0du) & 0x00800000u;\n"
    "    x = as_type<float>(ix | (i ^ 0x3f800000u));\n"
    "    k += int(i >> 23);\n"
    "    float y = float(k);\n"
    "    float f = x - 1.0f;\n"
    "    float hfsq = 0.5f * f * f;\n"
    "    float s = f / (2.0f + f);\n"
    "    float z = s * s;\n"
    "    float w = z * z;\n"
    "    float t1 = w * metal::fma(w, Lg4, Lg2);\n"
    "    float t2 = z * metal::fma(w, Lg3, Lg1);\n"
    "    float R = t2 + t1;\n"
    "    float r = s * (hfsq + R);\n"
    "    float hi = f - hfsq;\n"
    "    uint32_t hib = as_type<uint32_t>(hi) & 0xfffff000u;\n"
    "    hi = as_type<float>(hib);\n"
    "    float lo = (f - hi) - hfsq + r;\n"
    "    return y * log10_2lo + (lo + hi) * ivln10lo + lo * ivln10hi\n"
    "         + hi * ivln10hi + y * log10_2hi;\n"
    "}\n"
    //
    // `powf` — hand-rolled single-precision port of FreeBSD msun
    // `e_powf.c`. Algorithm:
    //   1. Compute `log2(|x|) = n + (p_h + p_l)` with Dekker-style hi/lo
    //      splits of intermediate terms; the 6-term polynomial in `s =
    //      (ax-bp)/(ax+bp)` approximates `(3/2)*(log(ax) - 2s - 2/3 s³)`
    //      with bp ∈ {1.0, 1.5} chosen per-interval.
    //   2. Multiply by y, split y as `y1 + (y - y1)` to preserve the
    //      full product in two floats.
    //   3. Recover `2^(p_h+p_l)` via argument reduction + a degree-5
    //      polynomial in z; the final bit-fiddle on the integer exponent
    //      assembles the result directly.
    // Baseline (`metal::precise::pow`) measured 1 ULP / 382 diverged per
    // 1024-sample sweep. The ACES CAM16 output transform compounds that
    // ~1 ULP across 7+ per-pixel pow calls, so downstream drift lands in
    // the 100-300 ULP range (see the `ACES v2 CAM16 ODT` row in
    // `PRECISION.md`). This port is the `e_powf.c` follow-through the
    // ROI log parked behind a "hard downstream blocker" trigger.
    // Explicit `metal::fma` at every Horner polynomial step plus at the
    // fused `cp_l*p_h + p_l*cp` / `(y-y1)*t1 + y*t2` patterns where
    // Apple Clang emits `fmadd` on ARM64 at `-O2`.
    //
    "static inline float ctl_stdlib_pow(float x, float y)\n"
    "{\n"
    "    const float bp0 = 1.0f;\n"
    "    const float bp1 = 1.5f;\n"
    "    const float dp_h1 = 5.84960938e-01f;\n"
    "    const float dp_l1 = 1.56322085e-06f;\n"
    "    const float two24  = 1.6777216e+07f;\n"
    "    const float huge_v = 1.0e+30f;\n"
    "    const float tiny_v = 1.0e-30f;\n"
    "    const float halfc  = 0.5f;\n"
    "    const float qrtrc  = 0.25f;\n"
    "    const float thrdc  = 3.33333343e-01f;\n"
    "    const float L1 = 6.0000002384e-01f;\n"
    "    const float L2 = 4.2857143283e-01f;\n"
    "    const float L3 = 3.3333334327e-01f;\n"
    "    const float L4 = 2.7272811532e-01f;\n"
    "    const float L5 = 2.3066075146e-01f;\n"
    "    const float L6 = 2.0697501302e-01f;\n"
    "    const float P1 =  1.6666667163e-01f;\n"
    "    const float P2 = -2.7777778450e-03f;\n"
    "    const float P3 =  6.6137559770e-05f;\n"
    "    const float P4 = -1.6533901999e-06f;\n"
    "    const float P5 =  4.1381369442e-08f;\n"
    "    const float lg2    = 6.9314718246e-01f;\n"
    "    const float lg2_h  = 6.93145752e-01f;\n"
    "    const float lg2_l  = 1.42860654e-06f;\n"
    "    const float ovt    = 4.2995665694e-08f;\n"
    "    const float cpc    = 9.6179670095e-01f;\n"
    "    const float cp_h   = 9.6191406250e-01f;\n"
    "    const float cp_l   = -1.1736857402e-04f;\n"
    "    const float ivln2    = 1.4426950216e+00f;\n"
    "    const float ivln2_h  = 1.4426879883e+00f;\n"
    "    const float ivln2_l  = 7.0526075433e-06f;\n"
    "    int hx = as_type<int>(x);\n"
    "    int hy = as_type<int>(y);\n"
    "    int ix = hx & 0x7fffffff;\n"
    "    int iy = hy & 0x7fffffff;\n"
    "    // y == 0\n"
    "    if (iy == 0) return 1.0f;\n"
    "    // x == 1\n"
    "    if (hx == 0x3f800000) return 1.0f;\n"
    "    // NaN propagation\n"
    "    if (ix > 0x7f800000 || iy > 0x7f800000) return x + y;\n"
    "    // Determine if y is an odd/even int when x < 0.\n"
    "    int yisint = 0;\n"
    "    if (hx < 0) {\n"
    "        if (iy >= 0x4b800000) yisint = 2;\n"
    "        else if (iy >= 0x3f800000) {\n"
    "            int k = (iy >> 23) - 0x7f;\n"
    "            int j = iy >> (23 - k);\n"
    "            if ((j << (23 - k)) == iy) yisint = 2 - (j & 1);\n"
    "        }\n"
    "    }\n"
    "    // y is +-inf\n"
    "    if (iy == 0x7f800000) {\n"
    "        if (ix == 0x3f800000) return 1.0f;\n"
    "        else if (ix > 0x3f800000) return (hy >= 0) ? y : 0.0f;\n"
    "        else return (hy < 0) ? -y : 0.0f;\n"
    "    }\n"
    "    // y is +-1\n"
    "    if (iy == 0x3f800000) return (hy < 0) ? (1.0f / x) : x;\n"
    "    if (hy == 0x40000000) return x * x;\n"
    "    if (hy == 0x3f000000 && hx >= 0) return metal::precise::sqrt(x);\n"
    "    float ax = metal::abs(x);\n"
    "    // |x| is 0, inf, or 1 -- short-circuit via reciprocal + sign.\n"
    "    if (ix == 0x7f800000 || ix == 0 || ix == 0x3f800000) {\n"
    "        float z = ax;\n"
    "        if (hy < 0) z = 1.0f / z;\n"
    "        if (hx < 0) {\n"
    "            if (((ix - 0x3f800000) | yisint) == 0) z = (z - z) / (z - z);\n"
    "            else if (yisint == 1) z = -z;\n"
    "        }\n"
    "        return z;\n"
    "    }\n"
    "    int n = (int)((uint)hx >> 31) - 1;\n"
    "    // (x<0)**(non-int) is NaN\n"
    "    if ((n | yisint) == 0) return (x - x) / (x - x);\n"
    "    float sn = 1.0f;\n"
    "    if ((n | (yisint - 1)) == 0) sn = -1.0f;\n"
    "    float t1, t2;\n"
    "    int is;\n"
    "    // |y| is huge: compute log(x) via x - x**2/2 + x**3/3 - ...\n"
    "    if (iy > 0x4d000000) {\n"
    "        if (ix < 0x3f7ffff6)\n"
    "            return (hy < 0) ? sn * huge_v * huge_v : sn * tiny_v * tiny_v;\n"
    "        if (ix > 0x3f800007)\n"
    "            return (hy > 0) ? sn * huge_v * huge_v : sn * tiny_v * tiny_v;\n"
    "        float t = ax - 1.0f;\n"
    "        float w = (t * t) * (halfc - t * (thrdc - t * qrtrc));\n"
    "        float u = ivln2_h * t;\n"
    "        float v = t * ivln2_l - w * ivln2;\n"
    "        t1 = u + v;\n"
    "        is = as_type<int>(t1);\n"
    "        t1 = as_type<float>(is & (int)0xfffff000);\n"
    "        t2 = v - (t1 - u);\n"
    "    } else {\n"
    "        float s_h, s_l, t_h, t_l, p_h_inner, p_l_inner;\n"
    "        n = 0;\n"
    "        // Subnormal ax: scale up by 2^24, compensate n.\n"
    "        if (ix < 0x00800000) {\n"
    "            ax *= two24;\n"
    "            n -= 24;\n"
    "            ix = as_type<int>(ax);\n"
    "        }\n"
    "        n += (ix >> 23) - 0x7f;\n"
    "        int j = ix & 0x007fffff;\n"
    "        ix = j | 0x3f800000;\n"
    "        int k;\n"
    "        if (j <= 0x1cc471) k = 0;\n"
    "        else if (j < 0x5db3d7) k = 1;\n"
    "        else { k = 0; n += 1; ix -= 0x00800000; }\n"
    "        ax = as_type<float>(ix);\n"
    "        float bpk = (k == 0) ? bp0 : bp1;\n"
    "        float u = ax - bpk;\n"
    "        float v = 1.0f / (ax + bpk);\n"
    "        float s = u * v;\n"
    "        s_h = s;\n"
    "        is = as_type<int>(s_h);\n"
    "        s_h = as_type<float>(is & (int)0xfffff000);\n"
    "        is = ((ix >> 1) & (int)0xfffff000) | 0x20000000;\n"
    "        t_h = as_type<float>(is + 0x00400000 + (k << 21));\n"
    "        t_l = ax - (t_h - bpk);\n"
    "        s_l = v * ((u - s_h * t_h) - s_h * t_l);\n"
    "        float s2 = s * s;\n"
    "        // 6-term Horner polynomial: L1 + s2*(L2 + s2*(L3 + ...)) — Clang\n"
    "        // fuses each (Li + s2*inner) into fmadd on ARM64; mirror with\n"
    "        // explicit metal::fma so the 6 rounding steps line up.\n"
    "        float r = metal::fma(s2, L6, L5);\n"
    "        r = metal::fma(s2, r, L4);\n"
    "        r = metal::fma(s2, r, L3);\n"
    "        r = metal::fma(s2, r, L2);\n"
    "        r = metal::fma(s2, r, L1);\n"
    "        r = s2 * s2 * r;\n"
    "        r = metal::fma(s_l, s_h + s, r);\n"
    "        s2 = s_h * s_h;\n"
    "        t_h = 3.0f + s2 + r;\n"
    "        is = as_type<int>(t_h);\n"
    "        t_h = as_type<float>(is & (int)0xfffff000);\n"
    "        t_l = r - ((t_h - 3.0f) - s2);\n"
    "        u = s_h * t_h;\n"
    "        v = s_l * t_h + t_l * s;\n"
    "        p_h_inner = u + v;\n"
    "        is = as_type<int>(p_h_inner);\n"
    "        p_h_inner = as_type<float>(is & (int)0xfffff000);\n"
    "        p_l_inner = v - (p_h_inner - u);\n"
    "        float z_h = cp_h * p_h_inner;\n"
    "        float dp_l_k = (k == 0) ? 0.0f : dp_l1;\n"
    "        float z_l = metal::fma(cp_l, p_h_inner, metal::fma(p_l_inner, cpc, dp_l_k));\n"
    "        float tf = (float)n;\n"
    "        float dp_h_k = (k == 0) ? 0.0f : dp_h1;\n"
    "        t1 = ((z_h + z_l) + dp_h_k) + tf;\n"
    "        is = as_type<int>(t1);\n"
    "        t1 = as_type<float>(is & (int)0xfffff000);\n"
    "        t2 = z_l - (((t1 - tf) - dp_h_k) - z_h);\n"
    "    }\n"
    "    // Split y into y1 + (y - y1); y1 keeps top 12 mantissa bits.\n"
    "    is = as_type<int>(y);\n"
    "    float y1 = as_type<float>(is & (int)0xfffff000);\n"
    "    float p_l_out = metal::fma(y - y1, t1, y * t2);\n"
    "    float p_h_out = y1 * t1;\n"
    "    float z = p_l_out + p_h_out;\n"
    "    int jz = as_type<int>(z);\n"
    "    if (jz > 0x43000000) return sn * huge_v * huge_v;\n"
    "    else if (jz == 0x43000000) {\n"
    "        if (p_l_out + ovt > z - p_h_out) return sn * huge_v * huge_v;\n"
    "    } else if ((jz & 0x7fffffff) > 0x43160000) return sn * tiny_v * tiny_v;\n"
    "    else if (jz == (int)0xc3160000) {\n"
    "        if (p_l_out <= z - p_h_out) return sn * tiny_v * tiny_v;\n"
    "    }\n"
    "    int ii = jz & 0x7fffffff;\n"
    "    int kk = (ii >> 23) - 0x7f;\n"
    "    n = 0;\n"
    "    if (ii > 0x3f000000) {\n"
    "        n = jz + (0x00800000 >> (kk + 1));\n"
    "        kk = ((n & 0x7fffffff) >> 23) - 0x7f;\n"
    "        float tf2 = as_type<float>(n & ~(0x007fffff >> kk));\n"
    "        n = ((n & 0x007fffff) | 0x00800000) >> (23 - kk);\n"
    "        if (jz < 0) n = -n;\n"
    "        p_h_out -= tf2;\n"
    "    }\n"
    "    float tfc = p_l_out + p_h_out;\n"
    "    is = as_type<int>(tfc);\n"
    "    tfc = as_type<float>(is & (int)0xffff8000);\n"
    "    float u2 = tfc * lg2_h;\n"
    "    float v2 = (p_l_out - (tfc - p_h_out)) * lg2 + tfc * lg2_l;\n"
    "    z = u2 + v2;\n"
    "    float w2 = v2 - (z - u2);\n"
    "    float tt = z * z;\n"
    "    // 5-term Horner: P1 + tt*(P2 + tt*(P3 + tt*(P4 + tt*P5)))\n"
    "    float tp = metal::fma(tt, P5, P4);\n"
    "    tp = metal::fma(tt, tp, P3);\n"
    "    tp = metal::fma(tt, tp, P2);\n"
    "    tp = metal::fma(tt, tp, P1);\n"
    "    float t1p = z - tt * tp;\n"
    "    float r2 = (z * t1p) / (t1p - 2.0f) - (w2 + z * w2);\n"
    "    z = 1.0f - (r2 - z);\n"
    "    int j2 = as_type<int>(z);\n"
    "    j2 += n << 23;\n"
    "    if ((j2 >> 23) <= 0) z = metal::ldexp(z, n);\n"
    "    else z = as_type<float>(j2);\n"
    "    return sn * z;\n"
    "}\n"
    //
    // sin / cos — hand-rolled FP32 port mirroring FreeBSD msun pre-double
    // `k_sinf.c` / `k_cosf.c` kernels with a 2-piece Cody-Waite reduction
    // of π/2 (PIO2_HI has 8 trailing mantissa zeros, so n*PIO2_HI is
    // exact for |n| <= 256 — covers the [-16, 16] test domain where
    // |n| <= 11 with large margin). The polynomial kernels use Horner
    // evaluation via `metal::fma` so the rounding count matches the ARM64
    // Clang emission for the CPU form.
    //
    // Baseline to beat: `metal::precise::sin/cos` at 2 ULP / ~441–455
    // diverged on [-16, 16]. Apple libm's `sinf` internally promotes to
    // FP64 (disassembly shows `fcvt d0, s0` + FP64 polynomial) — Metal
    // has no FP64, so 0-ULP is physically impossible and 1 ULP is the
    // achievable target here.
    //
    "static inline float ctl_stdlib_cos_kernel(float y) {\n"
    "    // Polynomial for (cos(y) - 1 + y²/2) / y⁴, |y| <= π/4.\n"
    "    const float C1 =  4.1666667908e-02f;  //  1/24\n"
    "    const float C2 = -1.3888889225e-03f;  // -1/720\n"
    "    const float C3 =  2.4801587642e-05f;  //  1/40320\n"
    "    const float C4 = -2.7557314297e-07f;  // -1/3628800\n"
    "    const float C5 =  2.0875723372e-09f;  //  1/479001600\n"
    "    float z = y * y;\n"
    "    float w = z * z;\n"
    "    float r = metal::fma(z, C5, C4);\n"
    "    r = metal::fma(z, r, C3);\n"
    "    r = metal::fma(z, r, C2);\n"
    "    r = metal::fma(z, r, C1);\n"
    "    // cos(y) = 1 - 0.5*z + w*r, rearranged as 1 - (0.5*z - w*r)\n"
    "    // to preserve precision when the result is close to 1.\n"
    "    float hz = 0.5f * z;\n"
    "    return 1.0f - (hz - w * r);\n"
    "}\n"
    "static inline float ctl_stdlib_sin_kernel(float y) {\n"
    "    // Polynomial for (sin(y) - y) / y³, |y| <= π/4. FreeBSD S1..S5.\n"
    "    const float S1 = -1.6666667163e-01f;  // -1/6\n"
    "    const float S2 =  8.3333337680e-03f;  //  1/120\n"
    "    const float S3 = -1.9841270114e-04f;  // -1/5040\n"
    "    const float S4 =  2.7557314297e-06f;  //  1/362880\n"
    "    const float S5 = -2.5050759689e-08f;  // -1/39916800\n"
    "    float z = y * y;\n"
    "    float r = metal::fma(z, S5, S4);\n"
    "    r = metal::fma(z, r, S3);\n"
    "    r = metal::fma(z, r, S2);\n"
    "    r = metal::fma(z, r, S1);\n"
    "    float v = z * y;\n"
    "    return metal::fma(v, r, y);\n"
    "}\n"
    "static inline float ctl_stdlib_sin(float x) {\n"
    "    // Cody-Waite 2/π range reduction. PIO2_HI = 0x3FC90F00 — mantissa\n"
    "    // ends in 8 zero bits, so n*PIO2_HI is exact for |n| <= 256.\n"
    "    const float INV_PIO2 = 6.3661975861e-01f;  // ~ 2/π\n"
    "    const float PIO2_HI  = 1.5707855225e+00f;  // 0x3fc90f00\n"
    "    const float PIO2_LO  = 1.0804334125e-05f;  // π/2 - PIO2_HI\n"
    "    if (metal::fabs(x) < 0.7853981633e+00f) return ctl_stdlib_sin_kernel(x);\n"
    "    float n_f = metal::round(x * INV_PIO2);\n"
    "    int   n   = int(n_f);\n"
    "    float y   = metal::fma(-n_f, PIO2_HI, x);\n"
    "    y = metal::fma(-n_f, PIO2_LO, y);\n"
    "    int q = n & 3;\n"
    "    if (q == 0) return  ctl_stdlib_sin_kernel(y);\n"
    "    if (q == 1) return  ctl_stdlib_cos_kernel(y);\n"
    "    if (q == 2) return -ctl_stdlib_sin_kernel(y);\n"
    "    return             -ctl_stdlib_cos_kernel(y);\n"
    "}\n"
    "static inline float ctl_stdlib_cos(float x) {\n"
    "    const float INV_PIO2 = 6.3661975861e-01f;\n"
    "    const float PIO2_HI  = 1.5707855225e+00f;\n"
    "    const float PIO2_LO  = 1.0804334125e-05f;\n"
    "    if (metal::fabs(x) < 0.7853981633e+00f) return ctl_stdlib_cos_kernel(x);\n"
    "    float n_f = metal::round(x * INV_PIO2);\n"
    "    int   n   = int(n_f);\n"
    "    float y   = metal::fma(-n_f, PIO2_HI, x);\n"
    "    y = metal::fma(-n_f, PIO2_LO, y);\n"
    "    int q = n & 3;\n"
    "    if (q == 0) return  ctl_stdlib_cos_kernel(y);\n"
    "    if (q == 1) return -ctl_stdlib_sin_kernel(y);\n"
    "    if (q == 2) return -ctl_stdlib_cos_kernel(y);\n"
    "    return              ctl_stdlib_sin_kernel(y);\n"
    "}\n"
    //
    // tan — composed as sin/cos on the reduced argument. Direct port of
    // FreeBSD `k_tanf.c` rational (N/D) minimax would need separate
    // coefficients; sin/cos composition is ~1 extra division but reuses
    // the validated sin/cos kernels. Baseline to beat: precise::tan at
    // 3 ULP / 581 diverged on [-1.6, 1.6].
    //
    "static inline float ctl_stdlib_tan(float x) {\n"
    "    const float INV_PIO2 = 6.3661975861e-01f;\n"
    "    const float PIO2_HI  = 1.5707855225e+00f;\n"
    "    const float PIO2_LO  = 1.0804334125e-05f;\n"
    "    float y = x;\n"
    "    int   q = 0;\n"
    "    if (metal::fabs(x) >= 0.7853981633e+00f) {\n"
    "        float n_f = metal::round(x * INV_PIO2);\n"
    "        q = int(n_f) & 1;\n"
    "        y = metal::fma(-n_f, PIO2_HI, x);\n"
    "        y = metal::fma(-n_f, PIO2_LO, y);\n"
    "    }\n"
    "    float s = ctl_stdlib_sin_kernel(y);\n"
    "    float c = ctl_stdlib_cos_kernel(y);\n"
    "    return (q == 0) ? (s / c) : -(c / s);\n"
    "}\n"
    //
    // `asinf` — hand-rolled single-precision port of FreeBSD msun
    // `s_asinf.c`. Shares the (3,1) rational approximation with `acosf`
    // (pS0..pS2, qS1). Two regimes:
    //   |x| < 0.5:  asin(x) = x + x * (x²*R(x²))
    //   |x| >=0.5:  asin(x) = pi/2 - 2*(s + s*R(t)) where t=(1-|x|)/2,
    //                                               s = sqrt(t)
    //
    "static inline float ctl_stdlib_asin(float x)\n"
    "{\n"
    "    const float pio2_hi = 1.5707962513e+00f;\n"
    "    const float pio2_lo = 7.5497894159e-08f;\n"
    "    const float pS0     =  1.6666586697e-01f;\n"
    "    const float pS1     = -4.2743422091e-02f;\n"
    "    const float pS2     = -8.6563630030e-03f;\n"
    "    const float qS1     = -7.0662963390e-01f;\n"
    "    uint32_t hx = as_type<uint32_t>(x);\n"
    "    uint32_t ix = hx & 0x7fffffffu;\n"
    "    if (ix >= 0x3f800000u) {\n"
    "        if (ix == 0x3f800000u)\n"
    "            return metal::fma(x, pio2_lo, x * pio2_hi);\n"
    "        return (x - x) / (x - x);\n"
    "    }\n"
    "    if (ix < 0x3f000000u) {\n"
    "        if (ix < 0x39800000u) return x;\n"
    "        float t = x * x;\n"
    "        float p = t * metal::fma(t, metal::fma(t, pS2, pS1), pS0);\n"
    "        float q = metal::fma(t, qS1, 1.0f);\n"
    "        float w = p / q;\n"
    "        return metal::fma(x, w, x);\n"
    "    }\n"
    "    float w = 1.0f - metal::abs(x);\n"
    "    float t = w * 0.5f;\n"
    "    float p = t * metal::fma(t, metal::fma(t, pS2, pS1), pS0);\n"
    "    float q = metal::fma(t, qS1, 1.0f);\n"
    "    float s = metal::precise::sqrt(t);\n"
    "    float wr = p / q;\n"
    "    float tr = pio2_hi - (2.0f * metal::fma(s, wr, s) - pio2_lo);\n"
    "    return (int(hx) >= 0) ? tr : -tr;\n"
    "}\n"
    //
    // `acosf` — hand-rolled single-precision port of FreeBSD msun
    // `e_acosf.c`. Same FP64 floor applies as for `expf` (Apple libm
    // promotes to `double`); this port closes the gap vs
    // `metal::precise::acos` by matching FreeBSD's polynomial shape
    // exactly. Three regimes:
    //   |x| < 0.5:    acos(x) = pi/2 - (x + x*(x²*R(x²)))
    //   x < -0.5:     acos(x) = pi - 2*(s + (r*s - pio2_lo))
    //   x >  0.5:     acos(x) = 2*(df + (r*s + (z - df²)/(s + df)))
    // R(z) = z*(pS0 + z*(pS1 + z*pS2)) / (1 + z*qS1) is a (3,1) minimax
    // rational from FreeBSD msun (not Remez-generated here, byte-copied).
    //
    "static inline float ctl_stdlib_acos(float x)\n"
    "{\n"
    "    const float pio2_hi = 1.5707962513e+00f;\n"
    "    const float pio2_lo = 7.5497894159e-08f;\n"
    "    const float pS0     =  1.6666586697e-01f;\n"
    "    const float pS1     = -4.2743422091e-02f;\n"
    "    const float pS2     = -8.6563630030e-03f;\n"
    "    const float qS1     = -7.0662963390e-01f;\n"
    "    const float pi      =  3.1415925026e+00f;\n"
    "    uint32_t hx = as_type<uint32_t>(x);\n"
    "    uint32_t ix = hx & 0x7fffffffu;\n"
    "    if (ix >= 0x3f800000u) {\n"
    "        if (ix == 0x3f800000u)\n"
    "            return (hx == 0x3f800000u) ? 0.0f : (pi + 2.0f * pio2_lo);\n"
    "        return (x - x) / (x - x);\n"
    "    }\n"
    "    if (ix < 0x3f000000u) {\n"
    "        if (ix <= 0x32800000u) return pio2_hi + pio2_lo;\n"
    "        float z = x * x;\n"
    "        float p = z * metal::fma(z, metal::fma(z, pS2, pS1), pS0);\n"
    "        float q = metal::fma(z, qS1, 1.0f);\n"
    "        float r = p / q;\n"
    "        return pio2_hi - (x - metal::fma(-x, r, pio2_lo));\n"
    "    } else if (int(hx) < 0) {\n"
    "        float z = (1.0f + x) * 0.5f;\n"
    "        float p = z * metal::fma(z, metal::fma(z, pS2, pS1), pS0);\n"
    "        float q = metal::fma(z, qS1, 1.0f);\n"
    "        float s = metal::precise::sqrt(z);\n"
    "        float r = p / q;\n"
    "        float w = metal::fma(r, s, -pio2_lo);\n"
    "        return pi - 2.0f * (s + w);\n"
    "    } else {\n"
    "        float z = (1.0f - x) * 0.5f;\n"
    "        float s = metal::precise::sqrt(z);\n"
    "        float df = as_type<float>(as_type<uint32_t>(s) & 0xfffff000u);\n"
    "        float c = metal::fma(-df, df, z) / (s + df);\n"
    "        float p = z * metal::fma(z, metal::fma(z, pS2, pS1), pS0);\n"
    "        float q = metal::fma(z, qS1, 1.0f);\n"
    "        float r = p / q;\n"
    "        float w = metal::fma(r, s, c);\n"
    "        return 2.0f * (df + w);\n"
    "    }\n"
    "}\n"
    // `atan` stays on `metal::precise::atan`. FreeBSD `s_atanf.c` was
    // measured (2026-04-19) at max 25 ULP vs Apple libm on the same
    // 1024-sample sweep — its `aT[]` coefficients are a double-precision
    // minimax, and evaluating the 9-term polynomial in single precision
    // accumulates more rounding error than Apple's FP64 path avoids.
    // `metal::precise::atan` gives max 1 ULP / 376 diverged; the FreeBSD
    // port would be a regression. If a tighter bound is ever needed,
    // re-derive a single-precision-tuned minimax instead of porting the
    // FreeBSD source.
    "static inline float ctl_stdlib_atan(float x) { return metal::precise::atan(x); }\n"
    "static inline float ctl_stdlib_atan2(float y, float x) { return metal::precise::atan2(y, x); }\n"
    //
    // sinh / cosh / tanh — composed from the 1-ULP ctl_stdlib_exp port,
    // with Taylor expansions below |x| ~ 0.5 where direct exp composition
    // loses bits to cancellation (sinh, tanh) or to the 1.0-dominated sum
    // (cosh). Taylor coefficients are the exact rationals; the Horner
    // chain uses metal::fma throughout to preserve single-rounded
    // semantics matching Apple Clang's CPU codegen.
    //
    // Domain splits:
    //   |x| < 0.5:              Taylor (no cancellation)
    //   0.5 <= |x| < 22:        (exp(|x|) +/- 1/exp(|x|)) / 2
    //   22 <= |x| < 88.72:      exp(|x|)/2 (the 1/exp term underflows)
    //   |x| >= 88.72:           overflow to +/- infinity
    //
    // tanh saturates to +/- 1 above |x| ~ 9, so the 22-and-up branches
    // don't apply there.
    //
    "static inline float ctl_stdlib_sinh(float x)\n"
    "{\n"
    "    float ax = metal::fabs(x);\n"
    "    if (ax < 1.0f) {\n"
    "        // Taylor through x^11 — below |x|=1 this matches FreeBSD's\n"
    "        // expm1f-based formulation to well within 1 ULP, and avoids\n"
    "        // the 1-2 ULP drift that `(exp(x) - 1/exp(x))/2` accumulates\n"
    "        // near x=0.5 from two independent 1-ULP exp roundings.\n"
    "        // Truncation error at |x|=1: x^13/13! ~ 1.6e-10, negligible.\n"
    "        float x2 = x * x;\n"
    "        float p = 1.0f / 39916800.0f;\n"
    "        p = metal::fma(x2, p, 1.0f / 362880.0f);\n"
    "        p = metal::fma(x2, p, 1.0f / 5040.0f);\n"
    "        p = metal::fma(x2, p, 1.0f / 120.0f);\n"
    "        p = metal::fma(x2, p, 1.0f / 6.0f);\n"
    "        return metal::fma(x * x2, p, x);\n"
    "    }\n"
    "    if (ax < 22.0f) {\n"
    "        float ex = ctl_stdlib_exp(ax);\n"
    "        float r  = 0.5f * (ex - 1.0f / ex);\n"
    "        return (x < 0.0f) ? -r : r;\n"
    "    }\n"
    "    if (ax < 88.72283905f) {\n"
    "        float r = 0.5f * ctl_stdlib_exp(ax);\n"
    "        return (x < 0.0f) ? -r : r;\n"
    "    }\n"
    "    // Overflow — return signed infinity. `x * 1e38f * 1e38f` forces\n"
    "    // the canonical +/-inf bit pattern without a literal INFINITY.\n"
    "    return (x < 0.0f) ? -1.0f / 0.0f : 1.0f / 0.0f;\n"
    "}\n"
    "static inline float ctl_stdlib_cosh(float x)\n"
    "{\n"
    "    float ax = metal::fabs(x);\n"
    "    if (ax < 0.5f) {\n"
    "        // cosh(x) = 1 + x^2/2 + x^4/24 + x^6/720 + x^8/40320 + x^10/3628800\n"
    "        // At |x|=0.5: x^12/12! ~ 5e-13, negligible.\n"
    "        float x2 = ax * ax;\n"
    "        float p = 1.0f / 3628800.0f;\n"
    "        p = metal::fma(x2, p, 1.0f / 40320.0f);\n"
    "        p = metal::fma(x2, p, 1.0f / 720.0f);\n"
    "        p = metal::fma(x2, p, 1.0f / 24.0f);\n"
    "        p = metal::fma(x2, p, 0.5f);\n"
    "        return metal::fma(x2, p, 1.0f);\n"
    "    }\n"
    "    if (ax < 22.0f) {\n"
    "        float ex = ctl_stdlib_exp(ax);\n"
    "        return 0.5f * (ex + 1.0f / ex);\n"
    "    }\n"
    "    if (ax < 88.72283905f) {\n"
    "        return 0.5f * ctl_stdlib_exp(ax);\n"
    "    }\n"
    "    // For extreme inputs in [88.72, 89.42], compute as\n"
    "    // 0.5 * w * w where w = exp(ax/2), avoiding mid-computation\n"
    "    // overflow of exp(ax). Beyond 89.42, genuine overflow.\n"
    "    if (ax < 89.41598629f) {\n"
    "        float w = ctl_stdlib_exp(0.5f * ax);\n"
    "        return 0.5f * w * w;\n"
    "    }\n"
    "    return 1.0f / 0.0f;\n"
    "}\n"
    "static inline float ctl_stdlib_tanh(float x)\n"
    "{\n"
    "    float ax = metal::fabs(x);\n"
    "    if (ax < 0.5f) {\n"
    "        // Taylor: tanh(x) = x - x^3/3 + 2x^5/15 - 17x^7/315 + 62x^9/2835\n"
    "        //                   - 1382x^11/155925 + 21844x^13/6081075\n"
    "        // Truncation at |x|=0.5: next term ~8e-7 * 0.5^15 ~ 2e-11.\n"
    "        float x2 = x * x;\n"
    "        float p = 21844.0f / 6081075.0f;\n"
    "        p = metal::fma(x2, p, -1382.0f / 155925.0f);\n"
    "        p = metal::fma(x2, p,    62.0f /   2835.0f);\n"
    "        p = metal::fma(x2, p,   -17.0f /    315.0f);\n"
    "        p = metal::fma(x2, p,     2.0f /     15.0f);\n"
    "        p = metal::fma(x2, p,    -1.0f /      3.0f);\n"
    "        return metal::fma(x * x2, p, x);\n"
    "    }\n"
    "    if (ax < 9.0103f) {\n"
    "        // tanh(|x|) = 1 - 2/(exp(2|x|) + 1). Matches FreeBSD\n"
    "        // `s_tanhf.c`'s formulation exactly; no cancellation because\n"
    "        // `exp(2|x|) + 1` is strictly > 2 in this branch.\n"
    "        float t = ctl_stdlib_exp(2.0f * ax) + 1.0f;\n"
    "        float r = 1.0f - 2.0f / t;\n"
    "        return (x < 0.0f) ? -r : r;\n"
    "    }\n"
    "    // tanh saturates to +/-1 for |x| above ~9. FreeBSD's s_tanhf.c\n"
    "    // uses 9.0103, tested to be the smallest |x| where\n"
    "    // round-to-nearest of tanh(x) yields 1.0 exactly.\n"
    "    return (x < 0.0f) ? -1.0f : 1.0f;\n"
    "}\n"
    //
    // pow10 — attempted a table-based 2^(k + m/32 + r) scheme on top of
    // Cody-Waite reduction (mirroring Apple __pow10f) at 2026-04-19;
    // regressed from 1 ULP (precise::) to 2 ULP on the [-4, 4] test
    // range, so the precise:: wrapper stands. The previously-attempted
    // `ctl_stdlib_exp(x * ln10)` composition (8 ULP) is also off the
    // table. Revisit with a double-precision emulated reduction or a
    // port of FreeBSD `e_exp10f.c` if ACES workloads start using pow10.
    //
    "static inline float ctl_stdlib_pow10(float x) {\n"
    "    return metal::precise::pow(10.0f, x);\n"
    "}\n"
    //
    // hypot: MSL does not expose `metal::hypot` (neither default nor
    // precise::), so we hand-roll as `sqrt(x*x + y*y)` via the
    // correctly-rounded precise::sqrt. Caveat: this loses CPU libm's
    // overflow-avoiding scaling trick, so for extreme-magnitude inputs
    // ( |x| or |y| > ~sqrt(FLT_MAX) ≈ 1.8e19 ) the result can overflow
    // to +inf where CPU libm would still produce a finite number.
    // Inside ACES-relevant ranges ( < 10^6 ) this is a non-issue; if a
    // CTL program ever needs the full libm contract, port FreeBSD's
    // `e_hypotf.c` instead.
    //
    "static inline float ctl_stdlib_hypot(float x, float y) {\n"
    "    return metal::precise::sqrt(x * x + y * y);\n"
    "}\n"
    //
    // Vector / matrix primitives. Each mirrors Imath's V3f / M33f
    // sequence of scalar mul+add operations exactly so that CPU and GPU
    // agree at 0 ULP:
    //   - mult_f3_f33 is row-vector × matrix (CTL convention):
    //     r[j] = v[0]*m[0][j] + v[1]*m[1][j] + v[2]*m[2][j]
    //   - length_f3 is sqrt(dot(v, v)) using precise::sqrt; the
    //     intermediate mul/add sequence matches `Imath::Vec3::length()`.
    //
    "// Row-vector × matrix. Each output component is a 3-term sum\n"
    "// that Apple Clang fuses on ARM64 exactly the same way as\n"
    "// `dot_f3_f3`: `fmul s1, v[1], m[1][j]` + two `fmadd`s for\n"
    "// components 0 and 2. The constant-matrix case may fold on\n"
    "// the CPU and happen to match a naive MSL version, but the\n"
    "// dynamic-matrix case (e.g. result of mult_f33_f33) diverges\n"
    "// by 1 ULP without the explicit fma chain.\n"
    "static inline metal::array<float, 3> ctl_stdlib_mult_f3_f33(\n"
    "    metal::array<float, 3> v,\n"
    "    metal::array<metal::array<float, 3>, 3> m)\n"
    "{\n"
    "    metal::array<float, 3> r;\n"
    "    for (int j = 0; j < 3; ++j)\n"
    "    {\n"
    "        float t = v[1] * m[1][j];\n"
    "        t = metal::fma(v[0], m[0][j], t);\n"
    "        t = metal::fma(v[2], m[2][j], t);\n"
    "        r[j] = t;\n"
    "    }\n"
    "    return r;\n"
    "}\n"
    "static inline metal::array<float, 3> ctl_stdlib_mult_f_f3(\n"
    "    float s, metal::array<float, 3> v)\n"
    "{\n"
    "    metal::array<float, 3> r;\n"
    "    r[0] = s * v[0]; r[1] = s * v[1]; r[2] = s * v[2];\n"
    "    return r;\n"
    "}\n"
    "static inline metal::array<float, 3> ctl_stdlib_add_f3_f3(\n"
    "    metal::array<float, 3> a, metal::array<float, 3> b)\n"
    "{\n"
    "    metal::array<float, 3> r;\n"
    "    r[0] = a[0] + b[0]; r[1] = a[1] + b[1]; r[2] = a[2] + b[2];\n"
    "    return r;\n"
    "}\n"
    "static inline metal::array<float, 3> ctl_stdlib_sub_f3_f3(\n"
    "    metal::array<float, 3> a, metal::array<float, 3> b)\n"
    "{\n"
    "    metal::array<float, 3> r;\n"
    "    r[0] = a[0] - b[0]; r[1] = a[1] - b[1]; r[2] = a[2] - b[2];\n"
    "    return r;\n"
    "}\n"
    "// Cross product. Apple Clang with default `-ffp-contract=on`\n"
    "// fuses `y * v.z - z * v.y` into a single ARM64 `fnmsub`\n"
    "// instruction for the CPU SIMD backend (two roundings total:\n"
    "// one for `z * v.y`, one for the fused `a*b - c`). With our\n"
    "// `#pragma STDC FP_CONTRACT OFF` preamble Metal would split\n"
    "// this into two independent fmuls + one fsub (three roundings),\n"
    "// producing 1-ULP drift on sub-percent of inputs. Explicitly\n"
    "// calling `metal::fma` here restores the CPU's single-rounded\n"
    "// `a*b - c` shape while keeping every *other* codegen-site op\n"
    "// strict. The negation `-(a[2]*b[1])` is bit-exact.\n"
    "static inline metal::array<float, 3> ctl_stdlib_cross_f3_f3(\n"
    "    metal::array<float, 3> a, metal::array<float, 3> b)\n"
    "{\n"
    "    metal::array<float, 3> r;\n"
    "    r[0] = metal::fma(a[1], b[2], -(a[2] * b[1]));\n"
    "    r[1] = metal::fma(a[2], b[0], -(a[0] * b[2]));\n"
    "    r[2] = metal::fma(a[0], b[1], -(a[1] * b[0]));\n"
    "    return r;\n"
    "}\n"
    "// Dot product. Apple Clang compiles Imath's\n"
    "// `x*v.x + y*v.y + z*v.z` on ARM64 as `fmul s1, a[1], b[1]` +\n"
    "// `fmadd a[0], b[0], s1` + `fmadd a[2], b[2], s0` — i.e. the\n"
    "// middle multiply is kept strict and the two outer ones get\n"
    "// fused. Replicating that exact sequence here is what 0-ULP\n"
    "// parity requires; a naive `a[0]*b[0] + a[1]*b[1] + a[2]*b[2]`\n"
    "// with FP_CONTRACT OFF splits into 5 roundings and drifts\n"
    "// 1 ULP from the CPU's 3 roundings.\n"
    "static inline float ctl_stdlib_dot_f3_f3(\n"
    "    metal::array<float, 3> a, metal::array<float, 3> b)\n"
    "{\n"
    "    float t = a[1] * b[1];\n"
    "    t = metal::fma(a[0], b[0], t);\n"
    "    t = metal::fma(a[2], b[2], t);\n"
    "    return t;\n"
    "}\n"
    "// Length. Imath implements this as `sqrt(dot(v, v))` with an\n"
    "// underflow-safe `lengthTiny` fallback when `length^2 < 2*FLT_MIN`\n"
    "// (i.e. the square would denormalize). Replicate both branches\n"
    "// exactly: the `length^2` expression must share `dot_f3_f3`'s\n"
    "// fma chain, and the tiny fallback must match `lengthTiny`'s\n"
    "// abs/max/normalize-then-sqrt algorithm byte-for-byte.\n"
    "static inline float ctl_stdlib_length_f3(metal::array<float, 3> v)\n"
    "{\n"
    "    float l2 = v[1] * v[1];\n"
    "    l2 = metal::fma(v[0], v[0], l2);\n"
    "    l2 = metal::fma(v[2], v[2], l2);\n"
    "    if (l2 < 2.0f * FLT_MIN)\n"
    "    {\n"
    "        float ax = metal::fabs(v[0]);\n"
    "        float ay = metal::fabs(v[1]);\n"
    "        float az = metal::fabs(v[2]);\n"
    "        float m = ax;\n"
    "        if (m < ay) m = ay;\n"
    "        if (m < az) m = az;\n"
    "        if (m == 0.0f) return 0.0f;\n"
    "        ax /= m; ay /= m; az /= m;\n"
    "        float s2 = ay * ay;\n"
    "        s2 = metal::fma(ax, ax, s2);\n"
    "        s2 = metal::fma(az, az, s2);\n"
    "        return m * metal::precise::sqrt(s2);\n"
    "    }\n"
    "    return metal::precise::sqrt(l2);\n"
    "}\n"
    "\n"
    "// Matrix primitives. Scalar-matrix, matrix-add, and transpose\n"
    "// contain no mul+add patterns that the CPU would fuse, so a\n"
    "// direct element-wise port matches at 0 ULP.\n"
    "static inline metal::array<metal::array<float, 3>, 3>\n"
    "ctl_stdlib_mult_f_f33(\n"
    "    float s,\n"
    "    metal::array<metal::array<float, 3>, 3> m)\n"
    "{\n"
    "    metal::array<metal::array<float, 3>, 3> r;\n"
    "    for (int i = 0; i < 3; ++i)\n"
    "        for (int j = 0; j < 3; ++j)\n"
    "            r[i][j] = s * m[i][j];\n"
    "    return r;\n"
    "}\n"
    "static inline metal::array<metal::array<float, 3>, 3>\n"
    "ctl_stdlib_add_f33_f33(\n"
    "    metal::array<metal::array<float, 3>, 3> a,\n"
    "    metal::array<metal::array<float, 3>, 3> b)\n"
    "{\n"
    "    metal::array<metal::array<float, 3>, 3> r;\n"
    "    for (int i = 0; i < 3; ++i)\n"
    "        for (int j = 0; j < 3; ++j)\n"
    "            r[i][j] = a[i][j] + b[i][j];\n"
    "    return r;\n"
    "}\n"
    "static inline metal::array<metal::array<float, 3>, 3>\n"
    "ctl_stdlib_transpose_f33(\n"
    "    metal::array<metal::array<float, 3>, 3> m)\n"
    "{\n"
    "    metal::array<metal::array<float, 3>, 3> r;\n"
    "    for (int i = 0; i < 3; ++i)\n"
    "        for (int j = 0; j < 3; ++j)\n"
    "            r[i][j] = m[j][i];\n"
    "    return r;\n"
    "}\n"
    "// 3x3 matrix multiply. Each output element `r[i][j]` is a\n"
    "// 3-term sum `a[i][0]*b[0][j] + a[i][1]*b[1][j] + a[i][2]*b[2][j]`\n"
    "// which Apple Clang fuses into the same `fmul s1,a[i][1],b[1][j]`\n"
    "// + two `fmadd` pattern we verified for `dot_f3_f3`. Mirror that\n"
    "// ordering exactly so the emitted fma chain matches the CPU.\n"
    "static inline metal::array<metal::array<float, 3>, 3>\n"
    "ctl_stdlib_mult_f33_f33(\n"
    "    metal::array<metal::array<float, 3>, 3> a,\n"
    "    metal::array<metal::array<float, 3>, 3> b)\n"
    "{\n"
    "    metal::array<metal::array<float, 3>, 3> r;\n"
    "    for (int i = 0; i < 3; ++i)\n"
    "    {\n"
    "        for (int j = 0; j < 3; ++j)\n"
    "        {\n"
    "            float t = a[i][1] * b[1][j];\n"
    "            t = metal::fma(a[i][0], b[0][j], t);\n"
    "            t = metal::fma(a[i][2], b[2][j], t);\n"
    "            r[i][j] = t;\n"
    "        }\n"
    "    }\n"
    "    return r;\n"
    "}\n"
    "\n"
    "// 4x4 matrix primitives. Same shape as the 3x3 helpers above.\n"
    "// For the sum-of-products form Clang picks the *middle-lower*\n"
    "// multiply as the strict fmul on ARM64: for a 4-term expression\n"
    "// `a*b + c*d + e*f + g*h` the emitted sequence is `fmul s1,c,d;\n"
    "// fmadd a,b,s1; fmadd e,f,s1; fmadd g,h,s1`. (For 3-term it was\n"
    "// index 1.) Verified via `clang++ -O2 -S` on this toolchain.\n"
    "static inline metal::array<metal::array<float, 4>, 4>\n"
    "ctl_stdlib_mult_f_f44(\n"
    "    float s,\n"
    "    metal::array<metal::array<float, 4>, 4> m)\n"
    "{\n"
    "    metal::array<metal::array<float, 4>, 4> r;\n"
    "    for (int i = 0; i < 4; ++i)\n"
    "        for (int j = 0; j < 4; ++j)\n"
    "            r[i][j] = s * m[i][j];\n"
    "    return r;\n"
    "}\n"
    "static inline metal::array<metal::array<float, 4>, 4>\n"
    "ctl_stdlib_add_f44_f44(\n"
    "    metal::array<metal::array<float, 4>, 4> a,\n"
    "    metal::array<metal::array<float, 4>, 4> b)\n"
    "{\n"
    "    metal::array<metal::array<float, 4>, 4> r;\n"
    "    for (int i = 0; i < 4; ++i)\n"
    "        for (int j = 0; j < 4; ++j)\n"
    "            r[i][j] = a[i][j] + b[i][j];\n"
    "    return r;\n"
    "}\n"
    "static inline metal::array<metal::array<float, 4>, 4>\n"
    "ctl_stdlib_transpose_f44(\n"
    "    metal::array<metal::array<float, 4>, 4> m)\n"
    "{\n"
    "    metal::array<metal::array<float, 4>, 4> r;\n"
    "    for (int i = 0; i < 4; ++i)\n"
    "        for (int j = 0; j < 4; ++j)\n"
    "            r[i][j] = m[j][i];\n"
    "    return r;\n"
    "}\n"
    "static inline metal::array<metal::array<float, 4>, 4>\n"
    "ctl_stdlib_mult_f44_f44(\n"
    "    metal::array<metal::array<float, 4>, 4> a,\n"
    "    metal::array<metal::array<float, 4>, 4> b)\n"
    "{\n"
    "    metal::array<metal::array<float, 4>, 4> r;\n"
    "    for (int i = 0; i < 4; ++i)\n"
    "    {\n"
    "        for (int j = 0; j < 4; ++j)\n"
    "        {\n"
    "            float t = a[i][1] * b[1][j];\n"
    "            t = metal::fma(a[i][0], b[0][j], t);\n"
    "            t = metal::fma(a[i][2], b[2][j], t);\n"
    "            t = metal::fma(a[i][3], b[3][j], t);\n"
    "            r[i][j] = t;\n"
    "        }\n"
    "    }\n"
    "    return r;\n"
    "}\n"
    "// V3 × M44 in CTL is the homogeneous projective transform\n"
    "// Imath::Vec3::operator*(Matrix44) performs:\n"
    "//   x = v[0]*m[0][0] + v[1]*m[1][0] + v[2]*m[2][0] + m[3][0]\n"
    "//   y = v[0]*m[0][1] + v[1]*m[1][1] + v[2]*m[2][1] + m[3][1]\n"
    "//   z = v[0]*m[0][2] + v[1]*m[1][2] + v[2]*m[2][2] + m[3][2]\n"
    "//   w = v[0]*m[0][3] + v[1]*m[1][3] + v[2]*m[2][3] + m[3][3]\n"
    "//   return Vec3(x/w, y/w, z/w)\n"
    "// The CPU expression for each 4-term sum is fused by Clang\n"
    "// into `fmul s1, v[1], m[1][j]` + three `fmadd`s (the middle\n"
    "// multiply gets kept strict, matching the 3-term pattern we\n"
    "// verified for `dot_f3_f3`). The `m[3][j]` translation term\n"
    "// enters via the last `fmadd(1.0f, m[3][j], t)`.\n"
    "static inline metal::array<float, 3> ctl_stdlib_mult_f3_f44(\n"
    "    metal::array<float, 3> v,\n"
    "    metal::array<metal::array<float, 4>, 4> m)\n"
    "{\n"
    "    float xyzw[4];\n"
    "    for (int j = 0; j < 4; ++j)\n"
    "    {\n"
    "        float t = v[1] * m[1][j];\n"
    "        t = metal::fma(v[0], m[0][j], t);\n"
    "        t = metal::fma(v[2], m[2][j], t);\n"
    "        t = metal::fma(1.0f, m[3][j], t);\n"
    "        xyzw[j] = t;\n"
    "    }\n"
    "    metal::array<float, 3> r;\n"
    "    r[0] = xyzw[0] / xyzw[3];\n"
    "    r[1] = xyzw[1] / xyzw[3];\n"
    "    r[2] = xyzw[2] / xyzw[3];\n"
    "    return r;\n"
    "}\n"
    "\n"
    //
    // 1D interpolated table lookup. Reproduces `Ctl::lookup1D`'s
    // IEEE-754 path from `lib/IlmCtlMath/CtlLookupTable.cpp` byte for
    // byte:
    //   1. clamp `p` into `[pMin, pMax]` using explicit comparisons,
    //      NOT `metal::clamp` — the latter resolves to `fmin(fmax(p,
    //      pMin), pMax)` and turns NaN into `pMax` rather than
    //      propagating NaN as the CPU path does.
    //   2. map to `r = (clamped - pMin) / (pMax - pMin) * iMax`.
    //   3. `indicesAndWeights` branches exactly as Imath does: three
    //      cases based on `r >= 0 && r < iMax`. The NaN / negative
    //      and above-range cases short to single-sample reads.
    //   4. The final `table[i]*u1 + table[i1]*u` blend is what Apple
    //      Clang fuses into one `fmul` + one `fmadd` on ARM64. Mirror
    //      that with `metal::fma` — a naive two-mul-one-add under
    //      FP_CONTRACT OFF would add a third rounding and drift 1 ULP.
    //
    "static inline float ctl_stdlib_lookup1D(\n"
    "    thread const float *table, uint size,\n"
    "    float pMin, float pMax, float p)\n"
    "{\n"
    "    int iMax = int(size) - 1;\n"
    "    float clamped = (p < pMin) ? pMin : ((p > pMax) ? pMax : p);\n"
    "    float r = (clamped - pMin) / (pMax - pMin) * float(iMax);\n"
    "    int i, i1;\n"
    "    float u;\n"
    "    if (r >= 0.0f)\n"
    "    {\n"
    "        if (r < float(iMax))\n"
    "        {\n"
    "            i  = int(r);\n"
    "            i1 = i + 1;\n"
    "            u  = r - float(i);\n"
    "        }\n"
    "        else\n"
    "        {\n"
    "            i  = iMax;\n"
    "            i1 = iMax;\n"
    "            u  = 1.0f;\n"
    "        }\n"
    "    }\n"
    "    else\n"
    "    {\n"
    "        i  = 0;\n"
    "        i1 = 0;\n"
    "        u  = 1.0f;\n"
    "    }\n"
    "    float u1 = 1.0f - u;\n"
    "    float t = table[i1] * u;\n"
    "    return metal::fma(table[i], u1, t);\n"
    "}\n"
    "\n"
    //
    // `lookupCubic1D` — cubic Hermite interpolation across a regular 1D
    // table (VSArray of floats). Port of `Ctl::lookupCubic1D` in
    // `lib/IlmCtlMath/CtlLookupTable.cpp`. Key invariants:
    //   1. `size < 3` degrades to `lookup1D` — the CPU does the same and
    //      we must match exactly to avoid an ULP diff on tiny tables.
    //   2. The three-way branch on `r` (in-range / >= iMax / NaN-or-neg)
    //      uses the exact same short-circuit returns as the CPU so that
    //      boundary samples read a single table entry with no blending.
    //   3. m0 / m1 tangents are computed by a 4-branch sequence that
    //      depends on position: edge cases derive one tangent from the
    //      other. Replicate the four-if-cascade literally — reordering
    //      changes which rounding errors cascade.
    //   4. The final 4-term Hermite sum is written as two `metal::fma`
    //      calls matching the `fmul` + 3× `fmadd` shape Clang emits on
    //      ARM64. Pairing the multiplies left-to-right keeps the same
    //      dependency chain.
    //
    "static inline float ctl_stdlib_lookupCubic1D(\n"
    "    thread const float *table, uint size,\n"
    "    float pMin, float pMax, float p)\n"
    "{\n"
    "    if (int(size) < 3)\n"
    "        return ctl_stdlib_lookup1D(table, size, pMin, pMax, p);\n"
    "    int iMax = int(size) - 1;\n"
    "    float clamped = (p < pMin) ? pMin : ((p > pMax) ? pMax : p);\n"
    "    float r = (clamped - pMin) / (pMax - pMin) * float(iMax);\n"
    "    int i;\n"
    "    if (r >= 0.0f && r < float(iMax))\n"
    "        i = int(r);\n"
    "    else if (r >= float(iMax))\n"
    "        return table[iMax];\n"
    "    else\n"
    "        return table[0];\n"
    "    float dy = table[i + 1] - table[i];\n"
    "    float m0 = 0.0f;\n"
    "    float m1 = 0.0f;\n"
    "    if (i > 0)\n"
    "        m0 = (dy + (table[i] - table[i - 1])) * 0.5f;\n"
    "    if (i < int(size) - 2)\n"
    "        m1 = (dy + (table[i + 2] - table[i + 1])) * 0.5f;\n"
    "    if (i <= 0)\n"
    "        m0 = metal::fma(3.0f, dy, -m1) * 0.5f;\n"
    "    if (i >= int(size) - 2)\n"
    "        m1 = metal::fma(3.0f, dy, -m0) * 0.5f;\n"
    "    float t = r - float(i);\n"
    "    float t2 = t * t;\n"
    "    float t3 = t2 * t;\n"
    "    float three_t2 = t2 * 3.0f;\n"
    "    float h00 = metal::fma(t3, 2.0f, -three_t2) + 1.0f;\n"
    "    float h10 = metal::fma(t2, -2.0f, t3) + t;\n"
    "    float h01 = metal::fma(t3, -2.0f, three_t2);\n"
    "    float h11 = t3 - t2;\n"
    "    float acc = m0 * h10;\n"
    "    acc = metal::fma(table[i], h00, acc);\n"
    "    acc = metal::fma(table[i + 1], h01, acc);\n"
    "    acc = metal::fma(m1, h11, acc);\n"
    "    return acc;\n"
    "}\n"
    "\n"
    //
    // `interpolate1D` — linear interpolation across an irregular 1D
    // (x,y) table (VSArray of float2 pairs). Port of
    // `Ctl::interpolate1D` in `lib/IlmCtlMath/CtlLookupTable.cpp`.
    //
    // Invariants:
    //   1. The binary search is an identity port (while + (i+j)/2 +
    //      three-way branch); `size < 1` and boundary-p short-circuits
    //      are literal-identical.
    //   2. Final blend `s*table[i][1] + t*table[i+1][1]` uses one
    //      explicit `metal::fma` — Clang emits `fmul` then `fmadd` on
    //      ARM64; mirroring that shape gives 0-ULP. Reversing operands
    //      changes the rounding path.
    //
    "__attribute__((noinline))\n"
    "static float ctl_stdlib_interpolate1D(\n"
    "    thread const float *table, uint size,\n"
    "    float p)\n"
    "{\n"
    "    if (int(size) < 1) return 0.0f;\n"
    "    if (p < table[0]) return table[1];\n"
    "    int last = int(size) - 1;\n"
    "    if (p >= table[last * 2]) return table[last * 2 + 1];\n"
    "    int i = 0;\n"
    "    int j = int(size);\n"
    "    while (i < j - 1) {\n"
    "        int k = (i + j) / 2;\n"
    "        float xk = table[k * 2];\n"
    "        if (xk == p) return table[k * 2 + 1];\n"
    "        else if (xk < p) i = k;\n"
    "        else j = k;\n"
    "    }\n"
    "    float xi  = table[i * 2];\n"
    "    float xi1 = table[(i + 1) * 2];\n"
    "    float yi  = table[i * 2 + 1];\n"
    "    float yi1 = table[(i + 1) * 2 + 1];\n"
    "    float t = (p - xi) / (xi1 - xi);\n"
    "    float s = 1.0f - t;\n"
    "    float blend = t * yi1;\n"
    "    return metal::fma(s, yi, blend);\n"
    "}\n"
    "\n"
    //
    // `interpolateCubic1D` — cubic Hermite interpolation on the same
    // irregular `(x,y)` knot table. Port of
    // `Ctl::interpolateCubic1D`. Falls back to `interpolate1D` when
    // `size < 3`; otherwise runs the binary search then computes the
    // tangents m0/m1 by the same four-branch cascade as
    // `lookupCubic1D`, using `metal::fma` for the `3*dy - m` fallback
    // and the 4-term Hermite sum to match Clang's fnmsub / fmadd
    // sequence.
    //
    "__attribute__((noinline))\n"
    "static float ctl_stdlib_interpolateCubic1D(\n"
    "    thread const float *table, uint size,\n"
    "    float p)\n"
    "{\n"
    "    if (int(size) < 3)\n"
    "        return ctl_stdlib_interpolate1D(table, size, p);\n"
    "    if (p < table[0]) return table[1];\n"
    "    int last = int(size) - 1;\n"
    "    if (p >= table[last * 2]) return table[last * 2 + 1];\n"
    "    int i = 0;\n"
    "    int j = int(size);\n"
    "    while (i < j - 1)\n"
    "    {\n"
    "        int k = (i + j) / 2;\n"
    "        if (table[k * 2] == p) return table[k * 2 + 1];\n"
    "        else if (table[k * 2] < p) i = k;\n"
    "        else j = k;\n"
    "    }\n"
    "    float dx = table[(i + 1) * 2] - table[i * 2];\n"
    "    float dy = table[(i + 1) * 2 + 1] - table[i * 2 + 1];\n"
    "    float m0 = 0.0f;\n"
    "    float m1 = 0.0f;\n"
    "    if (i > 0)\n"
    "        m0 = 0.5f * (dy + dx * (table[i * 2 + 1] - table[(i - 1) * 2 + 1]) /\n"
    "                              (table[i * 2] - table[(i - 1) * 2]));\n"
    "    if (i < int(size) - 2)\n"
    "        m1 = 0.5f * (dy + dx * (table[(i + 2) * 2 + 1] - table[(i + 1) * 2 + 1]) /\n"
    "                              (table[(i + 2) * 2] - table[(i + 1) * 2]));\n"
    "    if (i <= 0)\n"
    "        m0 = metal::fma(3.0f, dy, -m1) * 0.5f;\n"
    "    if (i >= int(size) - 2)\n"
    "        m1 = metal::fma(3.0f, dy, -m0) * 0.5f;\n"
    "    float t = (p - table[i * 2]) / dx;\n"
    "    float t2 = t * t;\n"
    "    float t3 = t2 * t;\n"
    "    float three_t2 = t2 * 3.0f;\n"
    "    float h00 = metal::fma(t3, 2.0f, -three_t2) + 1.0f;\n"
    "    float h10 = metal::fma(t2, -2.0f, t3) + t;\n"
    "    float h01 = metal::fma(t3, -2.0f, three_t2);\n"
    "    float h11 = t3 - t2;\n"
    "    float acc = m0 * h10;\n"
    "    acc = metal::fma(table[i * 2 + 1], h00, acc);\n"
    "    acc = metal::fma(table[(i + 1) * 2 + 1], h01, acc);\n"
    "    acc = metal::fma(m1, h11, acc);\n"
    "    return acc;\n"
    "}\n"
    "\n"
    //
    // `lookup3D_f3` — trilinear 3D table lookup returning a float[3]
    // voxel. Port of `Ctl::lookup3D` in `lib/IlmCtlMath/CtlLookupTable.cpp`
    // with the same `indicesAndWeights` clamp/weight derivation applied
    // per axis, the same row-major `((i * sy + j) * sz + k)` indexing,
    // and the same two-level trilinear blend. Under `FP_CONTRACT OFF`
    // MSL will not reassociate `u1*a + u*b`; to hold 0-ULP parity
    // against Clang's CPU output, every `a*b + c*d` pair is expressed
    // as three discrete ops — `fmul`, `fmul`, `fadd` — matching the
    // ARM64 instruction sequence Clang emits on CPU for the V3f per-
    // channel scalar arithmetic. Verified against `objdump -d
    // CtlLookupTable.cpp.o` which shows fmul/fmul/fadd (no fmadd)
    // despite `-O2`. Explicit `metal::fma` here would drift 1 ULP
    // because fma performs only one rounding while CPU rounds twice
    // (the intermediate fmul rounds, then the fadd rounds the sum).
    //
    "__attribute__((noinline))\n"
    "static metal::array<float, 3> ctl_stdlib_lookup3D_f3(\n"
    "    thread const float* table,\n"
    "    uint size0, uint size1, uint size2,\n"
    "    metal::array<float, 3> pMin,\n"
    "    metal::array<float, 3> pMax,\n"
    "    metal::array<float, 3> p)\n"
    "{\n"
    "    int iMax = int(size0) - 1;\n"
    "    int jMax = int(size1) - 1;\n"
    "    int kMax = int(size2) - 1;\n"
    "    float cx = (p[0] < pMin[0]) ? pMin[0] : ((p[0] > pMax[0]) ? pMax[0] : p[0]);\n"
    "    float cy = (p[1] < pMin[1]) ? pMin[1] : ((p[1] > pMax[1]) ? pMax[1] : p[1]);\n"
    "    float cz = (p[2] < pMin[2]) ? pMin[2] : ((p[2] > pMax[2]) ? pMax[2] : p[2]);\n"
    "    float rx = (cx - pMin[0]) / (pMax[0] - pMin[0]) * float(iMax);\n"
    "    float ry = (cy - pMin[1]) / (pMax[1] - pMin[1]) * float(jMax);\n"
    "    float rz = (cz - pMin[2]) / (pMax[2] - pMin[2]) * float(kMax);\n"
    "    int i, i1; float u;\n"
    "    if (rx >= 0.0f) {\n"
    "        if (rx < float(iMax)) { i = int(rx); i1 = i + 1; u = rx - float(i); }\n"
    "        else                  { i = iMax; i1 = iMax; u = 1.0f; }\n"
    "    } else                    { i = 0; i1 = 0; u = 1.0f; }\n"
    "    int j, j1; float v;\n"
    "    if (ry >= 0.0f) {\n"
    "        if (ry < float(jMax)) { j = int(ry); j1 = j + 1; v = ry - float(j); }\n"
    "        else                  { j = jMax; j1 = jMax; v = 1.0f; }\n"
    "    } else                    { j = 0; j1 = 0; v = 1.0f; }\n"
    "    int k, k1; float w;\n"
    "    if (rz >= 0.0f) {\n"
    "        if (rz < float(kMax)) { k = int(rz); k1 = k + 1; w = rz - float(k); }\n"
    "        else                  { k = kMax; k1 = kMax; w = 1.0f; }\n"
    "    } else                    { k = 0; k1 = 0; w = 1.0f; }\n"
    "    float u1 = 1.0f - u;\n"
    "    float v1 = 1.0f - v;\n"
    "    float w1 = 1.0f - w;\n"
    "    int sy = int(size1);\n"
    "    int sz = int(size2);\n"
    "    int idxA = (i  * sy + j ) * sz + k;\n"
    "    int idxB = (i1 * sy + j ) * sz + k;\n"
    "    int idxC = (i  * sy + j1) * sz + k;\n"
    "    int idxD = (i1 * sy + j1) * sz + k;\n"
    "    int idxE = (i  * sy + j ) * sz + k1;\n"
    "    int idxF = (i1 * sy + j ) * sz + k1;\n"
    "    int idxG = (i  * sy + j1) * sz + k1;\n"
    "    int idxH = (i1 * sy + j1) * sz + k1;\n"
    "    metal::array<float, 3> out;\n"
    "    for (int c = 0; c < 3; ++c) {\n"
    "        float v000 = table[idxA * 3 + c];\n"
    "        float v100 = table[idxB * 3 + c];\n"
    "        float v010 = table[idxC * 3 + c];\n"
    "        float v110 = table[idxD * 3 + c];\n"
    "        float v001 = table[idxE * 3 + c];\n"
    "        float v101 = table[idxF * 3 + c];\n"
    "        float v011 = table[idxG * 3 + c];\n"
    "        float v111 = table[idxH * 3 + c];\n"
    "        float ab = u1 * v000 + u * v100;\n"
    "        float cd = u1 * v010 + u * v110;\n"
    "        float ef = u1 * v001 + u * v101;\n"
    "        float gh = u1 * v011 + u * v111;\n"
    "        float lo = v1 * ab + v * cd;\n"
    "        float hi = v1 * ef + v * gh;\n"
    "        out[c] = w1 * lo + w * hi;\n"
    "    }\n"
    "    return out;\n"
    "}\n"
    "\n"
    //
    // `lookup3D_f` — scalar-in, scalar-out trilinear lookup. The CTL
    // stdlib spec exposes this as `void(float[][][][3], float[3],
    // float[3], float, float, float, output float, output float,
    // output float)`; the helper unpacks the three scalar coords into
    // a `metal::array<float, 3>`, delegates to `ctl_stdlib_lookup3D_f3`
    // for the shared trilinear body, and writes the three channel
    // returns through the output refs. Bit-exact with the SIMD CPU
    // path: `simdLookup3D_f` packs a `V3f`, calls the same `lookup3D`
    // core the f3 variant uses, and copies the three channels out to
    // scalar storage in the same order.
    //
    "static inline void ctl_stdlib_lookup3D_f(\n"
    "    thread const float *table,\n"
    "    uint size0, uint size1, uint size2,\n"
    "    metal::array<float, 3> pMin,\n"
    "    metal::array<float, 3> pMax,\n"
    "    float p0, float p1, float p2,\n"
    "    thread float &q0, thread float &q1, thread float &q2)\n"
    "{\n"
    "    metal::array<float, 3> p;\n"
    "    p[0] = p0; p[1] = p1; p[2] = p2;\n"
    "    metal::array<float, 3> r = ctl_stdlib_lookup3D_f3(\n"
    "        table, size0, size1, size2, pMin, pMax, p);\n"
    "    q0 = r[0];\n"
    "    q1 = r[1];\n"
    "    q2 = r[2];\n"
    "}\n"
    "\n"
    //
    // `lookup3D_h` — half scalar inputs/outputs, float-precision
    // trilinear body. On the CPU side `simdLookup3D_h` promotes the
    // half coords to float implicitly through `V3f`'s half-constructor,
    // runs the float `lookup3D`, and writes the three channel results
    // back as half (round-to-nearest-even via the `half` assignment).
    // Mirror that exact sequence: half→float is a lossless promotion;
    // the trilinear blend happens at float precision against the same
    // float table; the final float→half assignment rounds in the same
    // direction the CPU's `half` operator= does. Target 0 ULP against
    // the SIMD reference.
    //
    "static inline void ctl_stdlib_lookup3D_h(\n"
    "    thread const float *table,\n"
    "    uint size0, uint size1, uint size2,\n"
    "    metal::array<float, 3> pMin,\n"
    "    metal::array<float, 3> pMax,\n"
    "    half p0, half p1, half p2,\n"
    "    thread half &q0, thread half &q1, thread half &q2)\n"
    "{\n"
    "    metal::array<float, 3> p;\n"
    "    p[0] = float(p0); p[1] = float(p1); p[2] = float(p2);\n"
    "    metal::array<float, 3> r = ctl_stdlib_lookup3D_f3(\n"
    "        table, size0, size1, size2, pMin, pMax, p);\n"
    "    q0 = half(r[0]);\n"
    "    q1 = half(r[1]);\n"
    "    q2 = half(r[2]);\n"
    "}\n"
    "\n"
    //
    // `scatteredDataToGrid3D` — full MSL port of CtlRbfInterpolator.
    //
    // Runs the CPU algorithm in its entirety on the GPU: a 12-coefficient
    // affine least-squares fit, per-sample sigma via brute-force 4-NN,
    // residual RBF solve via conjugate gradient (one solve per output
    // channel), and the 3-deep grid loop evaluating `value()` at each
    // lattice point.
    //
    // Compile-time cap `kMaxRbfSamples = 256`. Larger input `dataSize`
    // sets `__ctl_err_flag` bit 2; `MetalFunctionCall::callFunction`
    // turns that into a clean NoImplExc.
    //
    // Precision: FP32 throughout, using `metal::precise::sqrt` for the
    // inner distance norm. CPU reference uses FP64; small numerical
    // drift vs CPU is expected and documented in PRECISION.md.
    //
    // Neighbor search: brute-force linear scan over all samples. CPU
    // uses a k-d tree but for `n ≤ 256` the scan is ~65 K comparisons,
    // well below the cost of an FP32 kernel evaluation that dominates
    // the solve.
    //
    // CG matvec: computes the kernel matrix on the fly rather than
    // materializing it. Keeps thread-local scratch at ~13 KB for n=256
    // and matches the sparsity pattern (entries beyond 2·maxSigma are
    // skipped) without needing a separate index structure.
    //
    // Uniform inputs only: a varying `data`/`pMin`/`pMax` triple would
    // require a full per-lane RBF solve (thousands of FLOPs per pixel),
    // which is the wrong shape for GPU. The CTL parser supports
    // varying; MetalCallNode enforces uniform at the call site and
    // the helper sets `__ctl_err_flag` bit 1 if varying is detected
    // as a backstop.
    //
    "static inline float ctl_stdlib_rbf_kernel(float val, float sigma)\n"
    "{\n"
    "    if (val > 2.0f * sigma || sigma <= 0.0f) return 0.0f;\n"
    "    float r = val / sigma;\n"
    "    if (r > 1.0f) {\n"
    "        float rm = r - 2.0f;\n"
    "        return (-0.25f * rm * rm * rm) / (3.14159265358979323846f * sigma);\n"
    "    }\n"
    "    float r2 = r * r;\n"
    "    return (1.0f - 1.5f * r2 + 0.75f * r * r2) /\n"
    "           (3.14159265358979323846f * sigma);\n"
    "}\n"
    "\n"
    //
    // Solve a 4×4 symmetric positive-definite system via Cholesky.
    // Used for the 12-coefficient affine fit (decomposed as three
    // independent 4×4 systems, one per output channel). CPU uses
    // sparse CG for this; for a 4×4 problem a direct Cholesky is both
    // simpler and more numerically stable.
    //
    "static inline void ctl_stdlib_rbf_solve4x4(\n"
    "    thread float *M, thread float *b, thread float *x)\n"
    "{\n"
    "    // Cholesky in place: M = L L^T, L lower-triangular (row-major).\n"
    "    float L[16];\n"
    "    for (uint i = 0; i < 4; ++i) {\n"
    "        for (uint j = 0; j <= i; ++j) {\n"
    "            float s = M[i*4+j];\n"
    "            for (uint k = 0; k < j; ++k) s -= L[i*4+k] * L[j*4+k];\n"
    "            if (i == j) L[i*4+j] = metal::precise::sqrt(metal::max(s, 0.0f));\n"
    "            else        L[i*4+j] = (L[j*4+j] > 0.0f) ? (s / L[j*4+j]) : 0.0f;\n"
    "        }\n"
    "    }\n"
    "    // Forward solve L y = b (store y in x).\n"
    "    for (uint i = 0; i < 4; ++i) {\n"
    "        float s = b[i];\n"
    "        for (uint k = 0; k < i; ++k) s -= L[i*4+k] * x[k];\n"
    "        x[i] = (L[i*4+i] > 0.0f) ? (s / L[i*4+i]) : 0.0f;\n"
    "    }\n"
    "    // Back solve L^T x = y.\n"
    "    for (int i = 3; i >= 0; --i) {\n"
    "        float s = x[i];\n"
    "        for (int k = i + 1; k < 4; ++k) s -= L[k*4+i] * x[k];\n"
    "        x[i] = (L[i*4+i] > 0.0f) ? (s / L[i*4+i]) : 0.0f;\n"
    "    }\n"
    "}\n"
    "\n"
    "static inline void ctl_stdlib_scatteredDataToGrid3D(\n"
    "    thread const float *data, uint dataSize,\n"
    "    metal::array<float, 3> pMin,\n"
    "    metal::array<float, 3> pMax,\n"
    "    thread float *grid, uint gs0, uint gs1, uint gs2,\n"
    "    device atomic_uint *flag)\n"
    "{\n"
    "    constexpr uint kMaxRbfSamples = 256u;\n"
    "    if (dataSize == 0) return;\n"
    "    if (dataSize > kMaxRbfSamples) {\n"
    "        atomic_fetch_or_explicit(flag, 4u,\n"
    "                                 metal::memory_order_relaxed);\n"
    "        return;\n"
    "    }\n"
    "    uint n = dataSize;\n"
    "\n"
    "    // ---- Copy sample positions / values out of the packed input.\n"
    "    // Input layout: data[dataSize][2][3], row-major.\n"
    "    // data[6*s+0..2] = sample position, data[6*s+3..5] = target value.\n"
    "    float pts[kMaxRbfSamples * 3];\n"
    "    float vals[kMaxRbfSamples * 3];\n"
    "    for (uint s = 0; s < n; ++s) {\n"
    "        pts[3*s+0]  = data[6*s+0];\n"
    "        pts[3*s+1]  = data[6*s+1];\n"
    "        pts[3*s+2]  = data[6*s+2];\n"
    "        vals[3*s+0] = data[6*s+3];\n"
    "        vals[3*s+1] = data[6*s+4];\n"
    "        vals[3*s+2] = data[6*s+5];\n"
    "    }\n"
    "\n"
    "    // ---- Affine fit: y = A·x + b. Three decoupled 4-unknown systems\n"
    "    // (one per output channel), each solved via normal equations on\n"
    "    // the 4-column design matrix [px, py, pz, 1].\n"
    "    //\n"
    "    // AtA is the same 4×4 Gram matrix for all three channels; Atb\n"
    "    // is per channel.\n"
    "    float AtA[16]; for (uint i = 0; i < 16; ++i) AtA[i] = 0.0f;\n"
    "    float AtbX[4] = {0,0,0,0};\n"
    "    float AtbY[4] = {0,0,0,0};\n"
    "    float AtbZ[4] = {0,0,0,0};\n"
    "    for (uint s = 0; s < n; ++s) {\n"
    "        float row[4] = {pts[3*s+0], pts[3*s+1], pts[3*s+2], 1.0f};\n"
    "        for (uint i = 0; i < 4; ++i) {\n"
    "            for (uint j = 0; j < 4; ++j)\n"
    "                AtA[i*4+j] += row[i] * row[j];\n"
    "            AtbX[i] += row[i] * vals[3*s+0];\n"
    "            AtbY[i] += row[i] * vals[3*s+1];\n"
    "            AtbZ[i] += row[i] * vals[3*s+2];\n"
    "        }\n"
    "    }\n"
    "    float affine[12];\n"
    "    {\n"
    "        float tmpAtA[16];\n"
    "        float sol[4];\n"
    "        for (uint i = 0; i < 16; ++i) tmpAtA[i] = AtA[i];\n"
    "        ctl_stdlib_rbf_solve4x4(tmpAtA, AtbX, sol);\n"
    "        affine[0]=sol[0]; affine[1]=sol[1]; affine[2]=sol[2]; affine[3]=sol[3];\n"
    "        for (uint i = 0; i < 16; ++i) tmpAtA[i] = AtA[i];\n"
    "        ctl_stdlib_rbf_solve4x4(tmpAtA, AtbY, sol);\n"
    "        affine[4]=sol[0]; affine[5]=sol[1]; affine[6]=sol[2]; affine[7]=sol[3];\n"
    "        for (uint i = 0; i < 16; ++i) tmpAtA[i] = AtA[i];\n"
    "        ctl_stdlib_rbf_solve4x4(tmpAtA, AtbZ, sol);\n"
    "        affine[8]=sol[0]; affine[9]=sol[1]; affine[10]=sol[2]; affine[11]=sol[3];\n"
    "    }\n"
    "\n"
    "    // ---- Per-sample sigma via brute-force 4-NN.\n"
    "    //\n"
    "    // CPU uses a k-d tree; for n ≤ 256 a brute-force scan is ≤65K\n"
    "    // comparisons, which is negligible next to the CG solve below.\n"
    "    // Matches CPU's `nearestPoints(sample_i, 4, ...)` semantics by\n"
    "    // including self (distance 0) among the 4 neighbors; the CPU\n"
    "    // code's PointTree returns the 4 nearest to the query point,\n"
    "    // which always includes the query itself since it's in the tree.\n"
    "    //\n"
    "    float sigmas[kMaxRbfSamples];\n"
    "    float maxSigma = 0.0f;\n"
    "    for (uint i = 0; i < n; ++i) {\n"
    "        float d0 = as_type<float>(0x7f800000u);\n"
    "        float d1 = as_type<float>(0x7f800000u);\n"
    "        float d2 = as_type<float>(0x7f800000u);\n"
    "        float d3 = as_type<float>(0x7f800000u);\n"
    "        for (uint j = 0; j < n; ++j) {\n"
    "            float dx = pts[3*i+0] - pts[3*j+0];\n"
    "            float dy = pts[3*i+1] - pts[3*j+1];\n"
    "            float dz = pts[3*i+2] - pts[3*j+2];\n"
    "            float d2ij = dx*dx + dy*dy + dz*dz;\n"
    "            if (d2ij < d0) { d3=d2; d2=d1; d1=d0; d0=d2ij; }\n"
    "            else if (d2ij < d1) { d3=d2; d2=d1; d1=d2ij; }\n"
    "            else if (d2ij < d2) { d3=d2; d2=d2ij; }\n"
    "            else if (d2ij < d3) { d3=d2ij; }\n"
    "        }\n"
    "        float sum = d0 + d1 + d2 + d3;\n"
    "        sigmas[i] = 0.5f * metal::precise::sqrt(sum);\n"
    "        if (sigmas[i] > maxSigma) maxSigma = sigmas[i];\n"
    "    }\n"
    "    float twoMaxSigma = 2.0f * maxSigma;\n"
    "\n"
    "    // ---- Residual RHS: b_c = vals_c - affine * [x, y, z, 1]\n"
    "    float bX[kMaxRbfSamples];\n"
    "    float bY[kMaxRbfSamples];\n"
    "    float bZ[kMaxRbfSamples];\n"
    "    for (uint s = 0; s < n; ++s) {\n"
    "        float x = pts[3*s+0], y = pts[3*s+1], z = pts[3*s+2];\n"
    "        bX[s] = vals[3*s+0] - (affine[0]*x + affine[1]*y + affine[2]*z + affine[3]);\n"
    "        bY[s] = vals[3*s+1] - (affine[4]*x + affine[5]*y + affine[6]*z + affine[7]);\n"
    "        bZ[s] = vals[3*s+2] - (affine[8]*x + affine[9]*y + affine[10]*z + affine[11]);\n"
    "    }\n"
    "\n"
    "    // ---- CG solve A·lambda = b, three channels sharing matvec.\n"
    "    //\n"
    "    // Matrix A[i][j] = kernel(||pts[i] - pts[j]||, sigma[j]), symmetric\n"
    "    // for our kernel (because kernel(d, sigma) ignores sigma_i). Not\n"
    "    // materialized — each CG iteration recomputes on the fly.\n"
    "    //\n"
    "    // Tolerance matches CPU (1e-7); iteration cap matches CPU\n"
    "    // (30 * n). For well-conditioned inputs CG converges in\n"
    "    // O(sqrt(condition)) iterations; the 30n bound is a pathological\n"
    "    // upper limit.\n"
    "    //\n"
    "    float lambdaX[kMaxRbfSamples];\n"
    "    float lambdaY[kMaxRbfSamples];\n"
    "    float lambdaZ[kMaxRbfSamples];\n"
    "    for (uint s = 0; s < n; ++s) {\n"
    "        lambdaX[s] = 0.0f; lambdaY[s] = 0.0f; lambdaZ[s] = 0.0f;\n"
    "    }\n"
    "\n"
    "    // CG for one channel. Three calls inlined as macro-like loops.\n"
    "    // Would be cleaner as a helper, but MSL can't pass a per-call\n"
    "    // RHS/solution pointer pair as a thread-ptr tuple cheaply, and\n"
    "    // the three copies spill to device memory anyway.\n"
    "    //\n"
    "    // The three loops are identical modulo the (b, x) pair.\n"
    "    float r_buf[kMaxRbfSamples];\n"
    "    float p_buf[kMaxRbfSamples];\n"
    "    float Ap_buf[kMaxRbfSamples];\n"
    "    uint maxIter = 30u * n;\n"
    "    float tolSq = 1e-14f;   // tol^2, tol=1e-7\n"
    "\n"
    "    for (uint channel = 0; channel < 3; ++channel) {\n"
    "        thread float *b = (channel == 0) ? bX : ((channel == 1) ? bY : bZ);\n"
    "        thread float *x = (channel == 0) ? lambdaX : ((channel == 1) ? lambdaY : lambdaZ);\n"
    "\n"
    "        // r = b - A*x  (x starts at 0, so r = b)\n"
    "        for (uint s = 0; s < n; ++s) { r_buf[s] = b[s]; p_buf[s] = b[s]; }\n"
    "        //\n"
    "        // Kahan compensation on every inner reduction in the CG\n"
    "        // loop: `rr`, `pAp`, and `rr_new` are the three scalar\n"
    "        // accumulators whose FP32 rounding errors compound across\n"
    "        // up to 30*n iterations and drive the `alpha = rr/pAp`,\n"
    "        // `beta = rr_new/rr` step sizes that shape convergence.\n"
    "        // Vanilla FP32 drift on marci with n=9 measured p50=2 ULP;\n"
    "        // Kahan cuts the inner summation error from O(n*eps) to\n"
    "        // O(eps), at the cost of 3 extra FLOPs per term (negligible\n"
    "        // next to the n^2 matvec work).\n"
    "        //\n"
    "        float rr = 0.0f, rr_c = 0.0f;\n"
    "        for (uint s = 0; s < n; ++s) {\n"
    "            float y = r_buf[s] * r_buf[s] - rr_c;\n"
    "            float t = rr + y;\n"
    "            rr_c = (t - rr) - y;\n"
    "            rr = t;\n"
    "        }\n"
    "\n"
    "        for (uint it = 0; it < maxIter; ++it) {\n"
    "            if (rr < tolSq) break;\n"
    "            // Ap = A * p  (on-the-fly matvec, per-row Kahan)\n"
    "            for (uint i = 0; i < n; ++i) {\n"
    "                float sum = 0.0f, sum_c = 0.0f;\n"
    "                for (uint j = 0; j < n; ++j) {\n"
    "                    float dx = pts[3*i+0] - pts[3*j+0];\n"
    "                    float dy = pts[3*i+1] - pts[3*j+1];\n"
    "                    float dz = pts[3*i+2] - pts[3*j+2];\n"
    "                    float d2v = dx*dx + dy*dy + dz*dz;\n"
    "                    if (d2v > twoMaxSigma * twoMaxSigma) continue;\n"
    "                    float d = metal::precise::sqrt(d2v);\n"
    "                    float term = ctl_stdlib_rbf_kernel(d, sigmas[j])\n"
    "                                 * p_buf[j];\n"
    "                    float y = term - sum_c;\n"
    "                    float t = sum + y;\n"
    "                    sum_c = (t - sum) - y;\n"
    "                    sum = t;\n"
    "                }\n"
    "                Ap_buf[i] = sum;\n"
    "            }\n"
    "            float pAp = 0.0f, pAp_c = 0.0f;\n"
    "            for (uint i = 0; i < n; ++i) {\n"
    "                float y = p_buf[i] * Ap_buf[i] - pAp_c;\n"
    "                float t = pAp + y;\n"
    "                pAp_c = (t - pAp) - y;\n"
    "                pAp = t;\n"
    "            }\n"
    "            if (pAp <= 0.0f) break;\n"
    "            float alpha = rr / pAp;\n"
    "            float rr_new = 0.0f, rr_new_c = 0.0f;\n"
    "            for (uint i = 0; i < n; ++i) {\n"
    "                x[i]     += alpha * p_buf[i];\n"
    "                r_buf[i] -= alpha * Ap_buf[i];\n"
    "                float y = r_buf[i] * r_buf[i] - rr_new_c;\n"
    "                float t = rr_new + y;\n"
    "                rr_new_c = (t - rr_new) - y;\n"
    "                rr_new = t;\n"
    "            }\n"
    "            float beta = rr_new / rr;\n"
    "            rr = rr_new;\n"
    "            for (uint i = 0; i < n; ++i)\n"
    "                p_buf[i] = r_buf[i] + beta * p_buf[i];\n"
    "        }\n"
    "    }\n"
    "\n"
    "    // ---- Grid loop: evaluate RBF at each lattice point.\n"
    "    for (uint i = 0; i < gs0; ++i) {\n"
    "        float s_i = (gs0 > 1) ? (float(i) / float(gs0 - 1)) : 0.0f;\n"
    "        float t_i = 1.0f - s_i;\n"
    "        float px = pMin[0] * t_i + pMax[0] * s_i;\n"
    "        for (uint j = 0; j < gs1; ++j) {\n"
    "            float s_j = (gs1 > 1) ? (float(j) / float(gs1 - 1)) : 0.0f;\n"
    "            float t_j = 1.0f - s_j;\n"
    "            float py = pMin[1] * t_j + pMax[1] * s_j;\n"
    "            for (uint k = 0; k < gs2; ++k) {\n"
    "                float s_k = (gs2 > 1) ? (float(k) / float(gs2 - 1)) : 0.0f;\n"
    "                float t_k = 1.0f - s_k;\n"
    "                float pz = pMin[2] * t_k + pMax[2] * s_k;\n"
    "                //\n"
    "                // Kahan-compensated RBF-weight accumulation. The\n"
    "                // per-channel affine tail is a single FMA and does\n"
    "                // not benefit from compensation; add it after the\n"
    "                // loop closes.\n"
    "                //\n"
    "                float sumX = 0.0f, sumX_c = 0.0f;\n"
    "                float sumY = 0.0f, sumY_c = 0.0f;\n"
    "                float sumZ = 0.0f, sumZ_c = 0.0f;\n"
    "                for (uint q = 0; q < n; ++q) {\n"
    "                    float dx = pts[3*q+0] - px;\n"
    "                    float dy = pts[3*q+1] - py;\n"
    "                    float dz = pts[3*q+2] - pz;\n"
    "                    float d2v = dx*dx + dy*dy + dz*dz;\n"
    "                    if (d2v > twoMaxSigma * twoMaxSigma) continue;\n"
    "                    float d = metal::precise::sqrt(d2v);\n"
    "                    float w = ctl_stdlib_rbf_kernel(d, sigmas[q]);\n"
    "                    float termX = w * lambdaX[q];\n"
    "                    float yX = termX - sumX_c;\n"
    "                    float tX = sumX + yX;\n"
    "                    sumX_c = (tX - sumX) - yX;\n"
    "                    sumX = tX;\n"
    "                    float termY = w * lambdaY[q];\n"
    "                    float yY = termY - sumY_c;\n"
    "                    float tY = sumY + yY;\n"
    "                    sumY_c = (tY - sumY) - yY;\n"
    "                    sumY = tY;\n"
    "                    float termZ = w * lambdaZ[q];\n"
    "                    float yZ = termZ - sumZ_c;\n"
    "                    float tZ = sumZ + yZ;\n"
    "                    sumZ_c = (tZ - sumZ) - yZ;\n"
    "                    sumZ = tZ;\n"
    "                }\n"
    "                sumX += affine[0]*px + affine[1]*py + affine[2]*pz + affine[3];\n"
    "                sumY += affine[4]*px + affine[5]*py + affine[6]*pz + affine[7];\n"
    "                sumZ += affine[8]*px + affine[9]*py + affine[10]*pz + affine[11];\n"
    "                uint idx = (i * gs1 + j) * gs2 + k;\n"
    "                grid[idx*3 + 0] = sumX;\n"
    "                grid[idx*3 + 1] = sumY;\n"
    "                grid[idx*3 + 2] = sumZ;\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "}\n"
    "\n"
    //
    // CIE L*u*v* / L*a*b* helpers. Port of the branch in
    // `Ctl::CtlColorSpace.cpp` where `f()` = cube-root-ish nonlinearity
    // and `fInverse()` = t*t*t or a shifted linear fallback. The cube
    // inverse is pure multiply, so LuvtoXYZ and LabtoXYZ can land
    // 0-ULP with the right fused-multiply shape; XYZtoLuv/XYZtoLab
    // call `metal::precise::pow(x, 1/3)` internally and inherit its
    // ULP drift.
    //
    "static inline float ctl_stdlib_colorspace_fInverse(float t)\n"
    "{\n"
    "    if (t > 0.206893f)\n"
    "        return t * t * t;\n"
    "    return (1.0f / 7.787f) * (t - 16.0f / 116.0f);\n"
    "}\n"
    "\n"
    //
    // `f(x) = pow(x, 1/3)` for `x > 0.008856`, else linear. Used by
    // XYZtoLab and XYZtoLuv. Pow drift on M4 Max is 1 ULP / 37%
    // (see PRECISION.md), and since every channel threads its pow
    // result through multiply/add chains, the composed drift stays
    // near 1 ULP. Call pow once per channel — CPU does the same under
    // `-O2` CSE.
    //
    "static inline float ctl_stdlib_colorspace_f(float x)\n"
    "{\n"
    "    if (x > 0.008856f)\n"
    "        return ctl_stdlib_pow(x, 1.0f / 3.0f);\n"
    "    return metal::fma(7.787f, x, 16.0f / 116.0f);\n"
    "}\n"
    "\n"
    //
    // XYZtoLuv — `uprime(XYZ) - uprime(XYZn)` and `vprime`
    // counterpart are the main sources of rounding. Clang computes
    // each denom with two fused `fmadd`s, then divides. Mirror that
    // with `metal::fma` in MSL to hold the pow-inherited drift near
    // its floor.
    //
    "static inline metal::array<float, 3> ctl_stdlib_XYZtoLuv(\n"
    "    metal::array<float, 3> XYZ,\n"
    "    metal::array<float, 3> XYZn)\n"
    "{\n"
    "    float denom_XYZ  = metal::fma(15.0f, XYZ[1],  XYZ[0]);\n"
    "    denom_XYZ = metal::fma(3.0f, XYZ[2], denom_XYZ);\n"
    "    float denom_XYZn = metal::fma(15.0f, XYZn[1], XYZn[0]);\n"
    "    denom_XYZn = metal::fma(3.0f, XYZn[2], denom_XYZn);\n"
    "    float up_XYZ  = (4.0f * XYZ[0])  / denom_XYZ;\n"
    "    float up_XYZn = (4.0f * XYZn[0]) / denom_XYZn;\n"
    "    float vp_XYZ  = (9.0f * XYZ[1])  / denom_XYZ;\n"
    "    float vp_XYZn = (9.0f * XYZn[1]) / denom_XYZn;\n"
    "    float Lstar = metal::fma(\n"
    "        116.0f, ctl_stdlib_colorspace_f(XYZ[1] / XYZn[1]),\n"
    "        -16.0f);\n"
    "    float thirteenL = 13.0f * Lstar;\n"
    "    float ustar = thirteenL * (up_XYZ - up_XYZn);\n"
    "    float vstar = thirteenL * (vp_XYZ - vp_XYZn);\n"
    "    metal::array<float, 3> r;\n"
    "    r[0] = Lstar; r[1] = ustar; r[2] = vstar;\n"
    "    return r;\n"
    "}\n"
    "\n"
    //
    // XYZtoLab — three `f()` calls plus scaled differences. The CPU
    // source calls `f(XYZ.y / XYZn.y)` three times; Clang CSEs to one
    // pow invocation. Match that by computing `fY` once.
    //
    "static inline metal::array<float, 3> ctl_stdlib_XYZtoLab(\n"
    "    metal::array<float, 3> XYZ,\n"
    "    metal::array<float, 3> XYZn)\n"
    "{\n"
    "    float fX = ctl_stdlib_colorspace_f(XYZ[0] / XYZn[0]);\n"
    "    float fY = ctl_stdlib_colorspace_f(XYZ[1] / XYZn[1]);\n"
    "    float fZ = ctl_stdlib_colorspace_f(XYZ[2] / XYZn[2]);\n"
    "    float Lstar = metal::fma(116.0f, fY, -16.0f);\n"
    "    float astar = 500.0f * (fX - fY);\n"
    "    float bstar = 200.0f * (fY - fZ);\n"
    "    metal::array<float, 3> r;\n"
    "    r[0] = Lstar; r[1] = astar; r[2] = bstar;\n"
    "    return r;\n"
    "}\n"
    "\n"
    //
    // LuvtoXYZ — Clang emits a chain of `fmadd` ops for the denom
    // `X + 15*Y + 3*Z`, the `13*Lstar*vnprime + vstar`, the
    // `13*Lstar*unprime + ustar`, the `3*unprime - 12`, and the final
    // 3-term sum for the Z numerator. Each `metal::fma` below matches
    // one fused instruction verified from the Apple Clang ARM64 output.
    //
    "static inline metal::array<float, 3> ctl_stdlib_LuvtoXYZ(\n"
    "    metal::array<float, 3> Luv,\n"
    "    metal::array<float, 3> XYZn)\n"
    "{\n"
    "    float Lstar = Luv[0];\n"
    "    float ustar = Luv[1];\n"
    "    float vstar = Luv[2];\n"
    "    float four_X = 4.0f * XYZn[0];\n"
    "    float denom = metal::fma(15.0f, XYZn[1], XYZn[0]);\n"
    "    denom = metal::fma(3.0f, XYZn[2], denom);\n"
    "    float unprime = four_X / denom;\n"
    "    float nine_Y = 9.0f * XYZn[1];\n"
    "    float vnprime = nine_Y / denom;\n"
    "    float fY = (Lstar + 16.0f) / 116.0f;\n"
    "    float fInv = ctl_stdlib_colorspace_fInverse(fY);\n"
    "    float Y = XYZn[1] * fInv;\n"
    "    float thirteenL = 13.0f * Lstar;\n"
    "    float d = 4.0f * metal::fma(thirteenL, vnprime, vstar);\n"
    "    float Xnum = 9.0f * metal::fma(thirteenL, unprime, ustar);\n"
    "    Xnum = Y * Xnum;\n"
    "    float X = Xnum / d;\n"
    "    float tmp = metal::fma(unprime, 3.0f, -12.0f);\n"
    "    tmp = metal::fma(vnprime, 20.0f, tmp);\n"
    "    float Zacc = thirteenL * tmp;\n"
    "    Zacc = metal::fma(ustar, 3.0f, Zacc);\n"
    "    Zacc = metal::fma(vstar, 20.0f, Zacc);\n"
    "    float Z = -(Zacc * Y) / d;\n"
    "    metal::array<float, 3> r;\n"
    "    r[0] = X; r[1] = Y; r[2] = Z;\n"
    "    return r;\n"
    "}\n"
    "\n"
    //
    // LabtoXYZ — pure rational ops; no pow. `fX = astar/500 + fY` and
    // `fZ = fY - bstar/200` are straight arithmetic. `fInverse` is
    // t*t*t (2 muls) in the common path, so each channel reduces to
    // one div + 2 muls + 1 mul = 4 roundings, matching CPU exactly.
    //
    "static inline metal::array<float, 3> ctl_stdlib_LabtoXYZ(\n"
    "    metal::array<float, 3> Lab,\n"
    "    metal::array<float, 3> XYZn)\n"
    "{\n"
    "    float Lstar = Lab[0];\n"
    "    float astar = Lab[1];\n"
    "    float bstar = Lab[2];\n"
    "    float fY = (Lstar + 16.0f) / 116.0f;\n"
    "    float fX = astar / 500.0f + fY;\n"
    "    float fZ = fY - bstar / 200.0f;\n"
    "    metal::array<float, 3> r;\n"
    "    r[0] = XYZn[0] * ctl_stdlib_colorspace_fInverse(fX);\n"
    "    r[1] = XYZn[1] * ctl_stdlib_colorspace_fInverse(fY);\n"
    "    r[2] = XYZn[2] * ctl_stdlib_colorspace_fInverse(fZ);\n"
    "    return r;\n"
    "}\n"
    "\n"
    //
    // `Chromaticities` struct — CTL users spell this type name in source
    // and the parser resolves it via the symbol table entry installed by
    // `declareMetalChromaticitiesType`. The MSL side matches what
    // `MetalCodegen::ensureStructDeclared` would emit for that CTL
    // struct; by declaring it inline here and pre-populating
    // `_declaredStructs` (see the constructor), we sidestep the
    // preamble/header ordering problem: the RGBtoXYZ helper below needs
    // the struct visible, and the preamble emits before the header.
    //
    "struct __Chromaticities {\n"
    "    metal::array<float, 2> red;\n"
    "    metal::array<float, 2> green;\n"
    "    metal::array<float, 2> blue;\n"
    "    metal::array<float, 2> white;\n"
    "};\n"
    "\n"
    //
    // RGBtoXYZ — port of `Ctl::RGBtoXYZ` in
    // `lib/IlmCtlMath/CtlColorSpace.cpp`. Evaluation order and
    // multiply-add fusion are matched exactly to the ARM64 assembly
    // Apple Clang emits for the CPU helper (see `/tmp/rgbxyz.s` produced
    // via the recipe in `metal_fma_fusion_parity.md`). Each fmadd /
    // fnmadd / fmsub pair in the CPU trace is reproduced here by an
    // explicit `metal::fma` call so that MSL under
    // `#pragma STDC FP_CONTRACT OFF` delivers the same single-rounded
    // result the ARM64 fused instruction produces. The naming follows
    // the disassembly's register live-ranges: `XpZ = X + Z`,
    // `red_inner = Y*(ry-1) + ry*(X+Z)` etc.
    //
    "static inline metal::array<metal::array<float, 4>, 4>\n"
    "ctl_stdlib_RGBtoXYZ(__Chromaticities chroma, float Y)\n"
    "{\n"
    "    float rx = chroma.red[0],   ry = chroma.red[1];\n"
    "    float gx = chroma.green[0], gy = chroma.green[1];\n"
    "    float bx = chroma.blue[0],  by = chroma.blue[1];\n"
    "    float wx = chroma.white[0], wy = chroma.white[1];\n"
    "    // X and Z: plain fmul + fdiv (CPU does not fuse these).\n"
    "    float X   = (Y * wx) / wy;\n"
    "    float Zt1 = 1.0f - wx;\n"
    "    float Zt2 = Zt1 - wy;\n"
    "    float Z   = (Y * Zt2) / wy;\n"
    "    float XpZ = X + Z;\n"
    "    // red_inner = fmadd(Y, ry-1, (X+Z)*ry)\n"
    "    float ry_m1  = ry - 1.0f;\n"
    "    float XpZ_ry = XpZ * ry;\n"
    "    float red_inner = metal::fma(Y, ry_m1, XpZ_ry);\n"
    "    // (1 - rx - ry), stored early in the CPU register file and\n"
    "    // consumed after Sr is known for M[0][2].\n"
    "    float one_rx     = 1.0f - rx;\n"
    "    float one_rx_ry  = one_rx - ry;\n"
    "    // green_inner and its negation. The CPU emits an fmadd and a\n"
    "    // companion fnmadd reading the same operands; mirroring that\n"
    "    // with two separate metal::fma calls keeps each result\n"
    "    // single-rounded.\n"
    "    float gy_m1   = gy - 1.0f;\n"
    "    float XpZ_gy  = XpZ * gy;\n"
    "    float green_inner     =  metal::fma(Y, gy_m1, XpZ_gy);\n"
    "    float neg_green_inner = -metal::fma(Y, gy_m1, XpZ_gy);\n"
    "    float gy_m_ry         = gy - ry;\n"
    "    // Sb numerator:\n"
    "    //   acc  = -green_inner * rx\n"
    "    //   acc += X * (gy - ry)\n"
    "    //   acc += gx * red_inner\n"
    "    float sb_num = neg_green_inner * rx;\n"
    "    sb_num = metal::fma(X,  gy_m_ry,   sb_num);\n"
    "    sb_num = metal::fma(gx, red_inner, sb_num);\n"
    "    // (1 - gx - gy), CPU computes ahead of Sg division.\n"
    "    float one_gx    = 1.0f - gx;\n"
    "    float one_gx_gy = one_gx - gy;\n"
    "    // blue_inner and its negation.\n"
    "    float by_m1   = by - 1.0f;\n"
    "    float XpZ_by  = XpZ * by;\n"
    "    float blue_inner     =  metal::fma(Y, by_m1, XpZ_by);\n"
    "    float neg_blue_inner = -metal::fma(Y, by_m1, XpZ_by);\n"
    "    float ry_m_by        = ry - by;\n"
    "    // Sg numerator:\n"
    "    //   acc  = blue_inner * rx\n"
    "    //   acc += X * (ry - by)\n"
    "    //   acc -= red_inner * bx      (CPU: fmsub)\n"
    "    float sg_num = blue_inner * rx;\n"
    "    sg_num = metal::fma(X,   ry_m_by, sg_num);\n"
    "    sg_num = metal::fma(-red_inner, bx, sg_num);\n"
    "    float by_m_gy = by - gy;\n"
    "    // Sr numerator:\n"
    "    //   acc  = -blue_inner * gx\n"
    "    //   acc += X * (by - gy)\n"
    "    //   acc += bx * green_inner\n"
    "    float sr_num = neg_blue_inner * gx;\n"
    "    sr_num = metal::fma(X,  by_m_gy,     sr_num);\n"
    "    sr_num = metal::fma(bx, green_inner, sr_num);\n"
    "    // d = bx*(gy-ry) + rx*(by-gy) + gx*(ry-by)\n"
    "    float ry_m_by_d = ry - by;\n"
    "    float d_acc = bx * gy_m_ry;\n"
    "    d_acc = metal::fma(rx, by_m_gy,   d_acc);\n"
    "    float d = metal::fma(gx, ry_m_by_d, d_acc);\n"
    "    float Sr = sr_num / d;\n"
    "    float Sg = sg_num / d;\n"
    "    float Sb = sb_num / d;\n"
    "    // M-row stores: CPU fuses {Sr*rx, Sr*ry} via a 2-lane fmul\n"
    "    // and writes them as a pair. Per-lane fmul is bit-equivalent.\n"
    "    metal::array<metal::array<float, 4>, 4> M = {};\n"
    "    M[0][0] = Sr * rx;\n"
    "    M[0][1] = Sr * ry;\n"
    "    M[0][2] = one_rx_ry * Sr;\n"
    "    M[1][0] = Sg * gx;\n"
    "    M[1][1] = Sg * gy;\n"
    "    M[1][2] = one_gx_gy * Sg;\n"
    "    M[2][0] = Sb * bx;\n"
    "    M[2][1] = Sb * by;\n"
    "    // (1 - bx - by) computed fresh after Sb divide in the CPU\n"
    "    // trace.\n"
    "    float one_bx    = 1.0f - bx;\n"
    "    float one_bx_by = one_bx - by;\n"
    "    M[2][2] = one_bx_by * Sb;\n"
    "    M[3][3] = 1.0f;\n"
    "    return M;\n"
    "}\n"
    "\n"
    //
    // `invert_f33` — port of `Imath::Matrix33<float>::inverse()`.
    // Two branches:
    //   * Non-affine (`x[0][2] != 0 || x[1][2] != 0 || x[2][2] != 1`):
    //     full 3x3 adjugate/determinant — 9 cofactors, one dot-product
    //     for the determinant, then divide. Each cofactor is an
    //     `a*b - c*d` pattern Clang fuses to `fnmsub` on ARM64.
    //   * Affine (2D): 2x2 inverse of the upper-left plus the
    //     translation column reconstructed via `x[2][0] * s[0][j] + ...`.
    // The `|r| >= 1` fast branch matches Imath exactly; for `|r| < 1`
    // we follow Imath's per-entry `mr > |s[i][j]|` sanity check and
    // return an all-zero matrix on singular input.
    //
    "static inline metal::array<metal::array<float, 3>, 3>\n"
    "ctl_stdlib_invert_f33(metal::array<metal::array<float, 3>, 3> x)\n"
    "{\n"
    "    metal::array<metal::array<float, 3>, 3> s = {};\n"
    "    bool affine = (x[0][2] == 0.0f) && (x[1][2] == 0.0f) &&\n"
    "                  (x[2][2] == 1.0f);\n"
    "    if (!affine) {\n"
    // Cofactors and determinant: explicit fma to mirror the
    // `fnmul + fmadd` pair Clang emits for each `a*b - c*d` in
    // Matrix33::inverse() and the `fmul + fmadd + fmadd` chain for the
    // three-way determinant.  Required for 0-ULP CPU/GPU parity.
    "        s[0][0] = metal::fma(x[1][1], x[2][2], -(x[2][1] * x[1][2]));\n"
    "        s[0][1] = metal::fma(x[2][1], x[0][2], -(x[0][1] * x[2][2]));\n"
    "        s[0][2] = metal::fma(x[0][1], x[1][2], -(x[1][1] * x[0][2]));\n"
    "        s[1][0] = metal::fma(x[2][0], x[1][2], -(x[1][0] * x[2][2]));\n"
    "        s[1][1] = metal::fma(x[0][0], x[2][2], -(x[2][0] * x[0][2]));\n"
    "        s[1][2] = metal::fma(x[1][0], x[0][2], -(x[0][0] * x[1][2]));\n"
    "        s[2][0] = metal::fma(x[1][0], x[2][1], -(x[2][0] * x[1][1]));\n"
    "        s[2][1] = metal::fma(x[2][0], x[0][1], -(x[0][0] * x[2][1]));\n"
    "        s[2][2] = metal::fma(x[0][0], x[1][1], -(x[1][0] * x[0][1]));\n"
    "        float r_acc = x[0][1] * s[1][0];\n"
    "        r_acc = metal::fma(x[0][0], s[0][0], r_acc);\n"
    "        float r = metal::fma(x[0][2], s[2][0], r_acc);\n"
    "        float absr = metal::fabs(r);\n"
    "        if (absr >= 1.0f) {\n"
    "            for (int i = 0; i < 3; ++i)\n"
    "                for (int j = 0; j < 3; ++j)\n"
    "                    s[i][j] = s[i][j] / r;\n"
    "        } else {\n"
    "            float mr = absr / 1.17549435e-38f;\n"
    "            bool singular = false;\n"
    "            for (int i = 0; i < 3 && !singular; ++i)\n"
    "                for (int j = 0; j < 3 && !singular; ++j) {\n"
    "                    if (mr > metal::fabs(s[i][j]))\n"
    "                        s[i][j] = s[i][j] / r;\n"
    "                    else\n"
    "                        singular = true;\n"
    "                }\n"
    "            if (singular) {\n"
    "                metal::array<metal::array<float, 3>, 3> z = {};\n"
    "                return z;\n"
    "            }\n"
    "        }\n"
    "        return s;\n"
    "    }\n"
    //
    // Affine 2D: adjugate of the 2x2 upper-left gives s[0][0], s[0][1],
    // s[1][0], s[1][1]; the third row/column is identity plus the
    // translation-reconstruction formula.
    //
    "    s[0][0] =  x[1][1];\n"
    "    s[0][1] = -x[0][1];\n"
    "    s[0][2] = 0.0f;\n"
    "    s[1][0] = -x[1][0];\n"
    "    s[1][1] =  x[0][0];\n"
    "    s[1][2] = 0.0f;\n"
    "    s[2][0] = 0.0f; s[2][1] = 0.0f; s[2][2] = 1.0f;\n"
    // 2x2 determinant — same fma-fusion recipe as the 3x3 cofactors.
    "    float r = metal::fma(x[0][0], x[1][1], -(x[1][0] * x[0][1]));\n"
    "    float absr = metal::fabs(r);\n"
    "    if (absr >= 1.0f) {\n"
    "        for (int i = 0; i < 2; ++i)\n"
    "            for (int j = 0; j < 2; ++j)\n"
    "                s[i][j] = s[i][j] / r;\n"
    "    } else {\n"
    "        float mr = absr / 1.17549435e-38f;\n"
    "        bool singular = false;\n"
    "        for (int i = 0; i < 2 && !singular; ++i)\n"
    "            for (int j = 0; j < 2 && !singular; ++j) {\n"
    "                if (mr > metal::fabs(s[i][j]))\n"
    "                    s[i][j] = s[i][j] / r;\n"
    "                else\n"
    "                    singular = true;\n"
    "            }\n"
    "        if (singular) {\n"
    "            metal::array<metal::array<float, 3>, 3> z = {};\n"
    "            return z;\n"
    "        }\n"
    "    }\n"
    // Translation reconstruction: `-a*b - c*d` → `fnmul + fmsub` on
    // the CPU side (two roundings). Mirror with `metal::fma(-c, d,
    // -(a*b))` so MSL emits the same two-rounding sequence.
    "    s[2][0] = metal::fma(-x[2][1], s[1][0], -(x[2][0] * s[0][0]));\n"
    "    s[2][1] = metal::fma(-x[2][1], s[1][1], -(x[2][0] * s[0][1]));\n"
    "    return s;\n"
    "}\n"
    "\n"
    //
    // `invert_f44` — port of `Imath::Matrix44<float>::inverse()`.
    // Two branches:
    //   * Non-affine (`x[i][3] != 0` for any i<3 or `x[3][3] != 1`):
    //     full Gauss-Jordan with partial pivoting (see `gjInverse`
    //     in Imath). Forward elimination picks the largest |pivot|
    //     in each column, swaps rows in both `t` and `s`, then
    //     eliminates below; backward substitution normalizes the
    //     diagonal and eliminates above. Singular input returns the
    //     default (identity) matrix, matching `Matrix44()`.
    //   * Affine: reuse the 3x3 adjugate/det path from
    //     `ctl_stdlib_XYZtoRGB`, plus the bottom-row translation
    //     formula `s[3][j] = -x[3][0]*s[0][j] - x[3][1]*s[1][j] -
    //     x[3][2]*s[2][j]`.
    //
    "static inline metal::array<metal::array<float, 4>, 4>\n"
    "ctl_stdlib_invert_f44(metal::array<metal::array<float, 4>, 4> x)\n"
    "{\n"
    "    bool affine = (x[0][3] == 0.0f) && (x[1][3] == 0.0f) &&\n"
    "                  (x[2][3] == 0.0f) && (x[3][3] == 1.0f);\n"
    "    if (!affine) {\n"
    //
    // Full Gauss-Jordan. `t` starts as a copy of `x`; `s` starts as
    // the identity and accumulates the inverse as the row ops
    // applied to `t` reduce it to identity.
    //
    "        metal::array<metal::array<float, 4>, 4> s = {};\n"
    "        s[0][0] = 1.0f; s[1][1] = 1.0f;\n"
    "        s[2][2] = 1.0f; s[3][3] = 1.0f;\n"
    "        metal::array<metal::array<float, 4>, 4> t = x;\n"
    "        for (int i = 0; i < 3; ++i) {\n"
    "            int pivot = i;\n"
    "            float pivotsize = metal::fabs(t[i][i]);\n"
    "            for (int j = i + 1; j < 4; ++j) {\n"
    "                float tmp = metal::fabs(t[j][i]);\n"
    "                if (tmp > pivotsize) { pivot = j; pivotsize = tmp; }\n"
    "            }\n"
    "            if (pivotsize == 0.0f) {\n"
    "                metal::array<metal::array<float, 4>, 4> z = {};\n"
    "                return z;\n"
    "            }\n"
    "            if (pivot != i) {\n"
    "                for (int j = 0; j < 4; ++j) {\n"
    "                    float tmp = t[i][j]; t[i][j] = t[pivot][j]; t[pivot][j] = tmp;\n"
    "                    tmp = s[i][j]; s[i][j] = s[pivot][j]; s[pivot][j] = tmp;\n"
    "                }\n"
    "            }\n"
    "            for (int j = i + 1; j < 4; ++j) {\n"
    "                float f = t[j][i] / t[i][i];\n"
    "                for (int k = 0; k < 4; ++k) {\n"
    // `a - f*b` → Clang emits `fmsub` (single rounding). Mirror with
    // `metal::fma(-f, b, a)`.
    "                    t[j][k] = metal::fma(-f, t[i][k], t[j][k]);\n"
    "                    s[j][k] = metal::fma(-f, s[i][k], s[j][k]);\n"
    "                }\n"
    "            }\n"
    "        }\n"
    "        for (int i = 3; i >= 0; --i) {\n"
    "            float f = t[i][i];\n"
    "            if (f == 0.0f) {\n"
    "                metal::array<metal::array<float, 4>, 4> z = {};\n"
    "                return z;\n"
    "            }\n"
    "            for (int j = 0; j < 4; ++j) {\n"
    "                t[i][j] = t[i][j] / f;\n"
    "                s[i][j] = s[i][j] / f;\n"
    "            }\n"
    "            for (int j = 0; j < i; ++j) {\n"
    "                float ff = t[j][i];\n"
    "                for (int k = 0; k < 4; ++k) {\n"
    "                    t[j][k] = metal::fma(-ff, t[i][k], t[j][k]);\n"
    "                    s[j][k] = metal::fma(-ff, s[i][k], s[j][k]);\n"
    "                }\n"
    "            }\n"
    "        }\n"
    "        return s;\n"
    "    }\n"
    //
    // Affine path: 3x3 adjugate / determinant, plus the bottom-row
    // translation formula. Same structure as the XYZtoRGB helper's
    // body — only difference is the bottom row which XYZtoRGB
    // always produces as `[0 0 0 1]` (because RGBtoXYZ never writes
    // it) whereas the general inverse computes it from x[3][0..2].
    //
    "    metal::array<metal::array<float, 4>, 4> s = {};\n"
    // Same fma-fusion recipe as invert_f33 / ctl_stdlib_XYZtoRGB.
    "    s[0][0] = metal::fma(x[1][1], x[2][2], -(x[2][1] * x[1][2]));\n"
    "    s[0][1] = metal::fma(x[2][1], x[0][2], -(x[0][1] * x[2][2]));\n"
    "    s[0][2] = metal::fma(x[0][1], x[1][2], -(x[1][1] * x[0][2]));\n"
    "    s[1][0] = metal::fma(x[2][0], x[1][2], -(x[1][0] * x[2][2]));\n"
    "    s[1][1] = metal::fma(x[0][0], x[2][2], -(x[2][0] * x[0][2]));\n"
    "    s[1][2] = metal::fma(x[1][0], x[0][2], -(x[0][0] * x[1][2]));\n"
    "    s[2][0] = metal::fma(x[1][0], x[2][1], -(x[2][0] * x[1][1]));\n"
    "    s[2][1] = metal::fma(x[2][0], x[0][1], -(x[0][0] * x[2][1]));\n"
    "    s[2][2] = metal::fma(x[0][0], x[1][1], -(x[1][0] * x[0][1]));\n"
    "    float r_acc = x[0][1] * s[1][0];\n"
    "    r_acc = metal::fma(x[0][0], s[0][0], r_acc);\n"
    "    float r = metal::fma(x[0][2], s[2][0], r_acc);\n"
    "    float absr = metal::fabs(r);\n"
    "    if (absr >= 1.0f) {\n"
    "        for (int i = 0; i < 3; ++i)\n"
    "            for (int j = 0; j < 3; ++j)\n"
    "                s[i][j] = s[i][j] / r;\n"
    "    } else {\n"
    "        float mr = absr / 1.17549435e-38f;\n"
    "        bool singular = false;\n"
    "        for (int i = 0; i < 3 && !singular; ++i)\n"
    "            for (int j = 0; j < 3 && !singular; ++j) {\n"
    "                if (mr > metal::fabs(s[i][j]))\n"
    "                    s[i][j] = s[i][j] / r;\n"
    "                else\n"
    "                    singular = true;\n"
    "            }\n"
    "        if (singular) {\n"
    "            metal::array<metal::array<float, 4>, 4> z = {};\n"
    "            return z;\n"
    "        }\n"
    "    }\n"
    // Translation reconstruction: `-a*b - c*d - e*f` → CPU emits
    // `fnmul + fmsub + fmsub` (three roundings). Mirror with one
    // fmul for the seed, then two chained `fma(-e, f, acc)` steps.
    "    float t0 = -(x[3][0] * s[0][0]);\n"
    "    t0 = metal::fma(-x[3][1], s[1][0], t0);\n"
    "    s[3][0] = metal::fma(-x[3][2], s[2][0], t0);\n"
    "    float t1 = -(x[3][0] * s[0][1]);\n"
    "    t1 = metal::fma(-x[3][1], s[1][1], t1);\n"
    "    s[3][1] = metal::fma(-x[3][2], s[2][1], t1);\n"
    "    float t2 = -(x[3][0] * s[0][2]);\n"
    "    t2 = metal::fma(-x[3][1], s[1][2], t2);\n"
    "    s[3][2] = metal::fma(-x[3][2], s[2][2], t2);\n"
    "    s[3][3] = 1.0f;\n"
    "    return s;\n"
    "}\n"
    "\n"
    //
    // XYZtoRGB = (RGBtoXYZ)⁻¹. RGBtoXYZ produces a matrix whose right
    // column is [0 0 0 1] and whose bottom row is [0 0 0 1], so
    // Imath's `Matrix44::inverse()` takes the affine adjugate/
    // determinant fast path rather than `gjInverse`. The CPU-emitted
    // ARM64 code for that path (see `/tmp/xyzrgb.s`) computes each
    // cofactor as one `fnmul` + one `fmadd` (two roundings: the
    // negated multiply, then the fused multiply-add). Under MSL
    // `#pragma STDC FP_CONTRACT OFF` the same expression written
    // `a*b - c*d` compiles to three rounded ops (fmul + fmul + fsub)
    // — one extra rounding per cofactor, which is the root of the
    // previous 7-ULP drift. Mirroring Clang with explicit
    // `metal::fma(a, b, -(c*d))` restores the two-rounding shape and
    // brings the nine cofactor expressions to bit-exact parity. The
    // three-way determinant `M[0][0]*s[0][0] + M[0][1]*s[1][0] +
    // M[0][2]*s[2][0]` is similarly fused as fmul + fmadd + fmadd.
    //
    "static inline metal::array<metal::array<float, 4>, 4>\n"
    "ctl_stdlib_XYZtoRGB(__Chromaticities chroma, float Y)\n"
    "{\n"
    "    metal::array<metal::array<float, 4>, 4> M = "
           "ctl_stdlib_RGBtoXYZ(chroma, Y);\n"
    "    metal::array<metal::array<float, 4>, 4> s = {};\n"
    "    // Nine cofactors, each `a*b - c*d` → one fmul for c*d, one\n"
    "    // fma for `a*b + (-(c*d))`. Two roundings per cofactor,\n"
    "    // matching the `fnmul + fmadd` pair in `/tmp/xyzrgb.s`.\n"
    "    s[0][0] = metal::fma(M[1][1], M[2][2], -(M[2][1] * M[1][2]));\n"
    "    s[0][1] = metal::fma(M[2][1], M[0][2], -(M[0][1] * M[2][2]));\n"
    "    s[0][2] = metal::fma(M[0][1], M[1][2], -(M[1][1] * M[0][2]));\n"
    "    s[1][0] = metal::fma(M[2][0], M[1][2], -(M[1][0] * M[2][2]));\n"
    "    s[1][1] = metal::fma(M[0][0], M[2][2], -(M[2][0] * M[0][2]));\n"
    "    s[1][2] = metal::fma(M[1][0], M[0][2], -(M[0][0] * M[1][2]));\n"
    "    s[2][0] = metal::fma(M[1][0], M[2][1], -(M[2][0] * M[1][1]));\n"
    "    s[2][1] = metal::fma(M[2][0], M[0][1], -(M[0][0] * M[2][1]));\n"
    "    s[2][2] = metal::fma(M[0][0], M[1][1], -(M[1][0] * M[0][1]));\n"
    "    // Determinant: fmul + two fmadds, matching the disassembly's\n"
    "    // `fmul s1, s23, s6` / `fmadd s19, ...` / `fmadd s1, ...`\n"
    "    // three-step accumulation.\n"
    "    float r_acc = M[0][1] * s[1][0];\n"
    "    r_acc = metal::fma(M[0][0], s[0][0], r_acc);\n"
    "    float r = metal::fma(M[0][2], s[2][0], r_acc);\n"
    //
    // Chromaticities used in ACES / Rec.709 / DCI-P3 / etc. always
    // yield |det| < 1, so the `|r| < 1` branch in Imath is the one we
    // always hit. Chromatically-degenerate inputs (collinear primaries)
    // make `r` approach zero and Imath returns an all-zero matrix via
    // the `mr > abs(s[i][j])` guard. Replicate that — compute `mr`,
    // compare against each cofactor, and zero the result on singular
    // input. `FLT_MIN` matches `std::numeric_limits<float>::min()` on
    // the CPU side.
    //
    "    float absr = metal::fabs(r);\n"
    "    if (absr >= 1.0f) {\n"
    "        for (int i = 0; i < 3; ++i)\n"
    "            for (int j = 0; j < 3; ++j)\n"
    "                s[i][j] = s[i][j] / r;\n"
    "    } else {\n"
    "        float mr = absr / 1.17549435e-38f;\n"
    "        bool singular = false;\n"
    "        for (int i = 0; i < 3 && !singular; ++i)\n"
    "            for (int j = 0; j < 3 && !singular; ++j) {\n"
    "                if (mr > metal::fabs(s[i][j]))\n"
    "                    s[i][j] = s[i][j] / r;\n"
    "                else\n"
    "                    singular = true;\n"
    "            }\n"
    "        if (singular) {\n"
    "            metal::array<metal::array<float, 4>, 4> id = {};\n"
    "            id[0][0] = 1.0f; id[1][1] = 1.0f;\n"
    "            id[2][2] = 1.0f; id[3][3] = 1.0f;\n"
    "            return id;\n"
    "        }\n"
    "    }\n"
    //
    // M[3][0..2] are zero in our case (RGBtoXYZ never writes them),
    // so the Imath-s[3][j] formulas reduce to 0. The caller's Imath
    // constructor default-initializes M[3][3] = 1 via the identity
    // pattern — replicate that.
    //
    "    s[3][3] = 1.0f;\n"
    "    return s;\n"
    "}\n"
    "\n"
    //
    // `print_*` stdlib functions — GPU v1 is a no-op. The CPU backend
    // writes to stderr via CtlMessage; on Metal we have no device-side
    // I/O, so the helpers just discard their operand. A one-time
    // stderr warning is emitted from the host when the first print call
    // is encountered during codegen (see `MetalCodegen::markPrintUsed`).
    // `print_string` is short-circuited at the call site so the string
    // literal never needs to be codegen'd.
    //
    "static inline void ctl_stdlib_print_bool(bool x)        { (void)x; }\n"
    "static inline void ctl_stdlib_print_int(int x)          { (void)x; }\n"
    "static inline void ctl_stdlib_print_unsigned_int(uint x){ (void)x; }\n"
    "static inline void ctl_stdlib_print_half(half x)        { (void)x; }\n"
    "static inline void ctl_stdlib_print_float(float x)      { (void)x; }\n"
    "\n"
    //
    // `assert(bool)` — mirrors the CPU backend's behavior of throwing
    // `Iex::LogicExc("CTL assertion failed.")` when the condition is
    // false on any lane. Because MSL kernels cannot throw, we set an
    // atomic flag in a dedicated `device atomic_uint` buffer that every
    // dispatch binds as its last buffer. After `waitUntilCompleted` the
    // host reads the flag and, if nonzero, throws the matching LogicExc
    // so CPU and GPU user-visible semantics agree.
    //
    // The flag pointer is threaded through every user-defined helper as
    // its trailing `device atomic_uint* __ctl_err_flag` parameter so
    // helpers can pass it when they call assert or invoke other user
    // helpers. Stdlib helpers other than `assert` never fault, so they
    // do not take the flag.
    //
    "static inline void ctl_stdlib_assert(bool cond,\n"
    "                                     device atomic_uint* flag) {\n"
    "    if (!cond)\n"
    "        atomic_fetch_or_explicit(flag, 1u,\n"
    "                                 memory_order_relaxed);\n"
    "}\n"
    "\n";

} // anonymous namespace

MetalCodegen::MetalCodegen()
    : _section(Body),
      _indent(0),
      _atLineStart(true),
      _inKernelSignature(false),
      _kernelBufferIndex(0),
      _kernelActive(nullptr),
      _tempCounter(0),
      _staticCounter(0),
      _printUsed(false),
      _assertUsed(false),
      _halfExpLogUsed(false)
{
    //
    // The preamble already emits `struct __Chromaticities { ... };`
    // inline so the `ctl_stdlib_RGBtoXYZ` helper (also in the preamble)
    // can reference it. Pre-populate the declared-struct set so
    // `ensureStructDeclared` skips re-emitting the same struct into the
    // header when user code uses the CTL `Chromaticities` type. The
    // mangled MSL name matches `mslStructName("::Chromaticities")`.
    //
    _declaredStructs.insert("__Chromaticities");
}

MetalCodegen::~MetalCodegen()
{
}

std::string &
MetalCodegen::active()
{
    if (_kernelActive && _section == Body)
        return *_kernelActive;
    return _section == Header ? _header : _body;
}

void
MetalCodegen::applyIndent()
{
    if (!_atLineStart)
        return;
    std::string &buf = active();
    for (int i = 0; i < _indent; ++i)
        buf += "    ";
    _atLineStart = false;
}

void
MetalCodegen::write(const std::string &text)
{
    if (text.empty())
        return;
    applyIndent();
    active() += text;
    if (text.back() == '\n')
        _atLineStart = true;
}

void
MetalCodegen::writeln(const std::string &text)
{
    applyIndent();
    active() += text;
    active() += '\n';
    _atLineStart = true;
}

void
MetalCodegen::indent()
{
    ++_indent;
}

void
MetalCodegen::outdent()
{
    if (_indent > 0)
        --_indent;
}

void
MetalCodegen::setSection(Section s)
{
    _section = s;
    _atLineStart = true;
}

void
MetalCodegen::beginKernel(const std::string &name)
{
    //
    // Start a fresh per-kernel buffer and divert Body-section writes
    // into it for the remainder of the kernel wrapper. The buffer is
    // created empty on first insert; a second `beginKernel` with the
    // same name would clobber, but MetalFunctionNode only emits one
    // wrapper per CTL function name so that never happens in practice.
    //
    _kernelActive = &_kernelWrappers[name];
    _kernelActive->clear();
    Section previous = _section;
    setSection(Body);
    write("kernel void ");
    write(name);
    write("(uint tid [[thread_position_in_grid]]");
    _inKernelSignature = true;
    _kernelBufferIndex = 0;
    _section = previous;
}

void
MetalCodegen::declareKernelBuffer(const std::string &mslType,
                                  const std::string &name)
{
    Section previous = _section;
    setSection(Body);
    write(",\n                      device ");
    write(mslType);
    write(" *");
    write(name);
    write(" [[buffer(");
    write(std::to_string(_kernelBufferIndex));
    write(")]]");
    ++_kernelBufferIndex;
    _section = previous;
}

void
MetalCodegen::endKernel()
{
    Section previous = _section;
    setSection(Body);
    writeln(")");
    writeln("{");
    _inKernelSignature = false;
    _section = previous;
}

void
MetalCodegen::finishKernel()
{
    _kernelActive = nullptr;
}

void
MetalCodegen::pushExpr(const std::string &expr)
{
    _exprStack.push_back(expr);
}

std::string
MetalCodegen::popExpr()
{
    if (_exprStack.empty()) {
        throw IEX_NAMESPACE::LogicExc(
            "CTL Metal backend: expression stack underflow during codegen.");
    }
    std::string top = std::move(_exprStack.back());
    _exprStack.pop_back();
    return top;
}

std::string
MetalCodegen::nextTempName()
{
    return "__ctl_tmp_" + std::to_string(_tempCounter++);
}

std::string
MetalCodegen::nextStaticName()
{
    return "static" + std::to_string(_staticCounter++);
}

void
MetalCodegen::ensureStructDeclared(const StructType *type)
{
    if (!type)
        throw IEX_NAMESPACE::LogicExc(
            "CTL Metal backend: ensureStructDeclared called on null type.");
    if (type->name().empty()) {
        //
        // Error-recovery placeholder (empty-name, empty-members) — see
        // metalTypeName for the provenance. Emit an empty MSL struct
        // under the shared `__ctl_err_placeholder` name so any decl
        // that references it is at least syntactically valid.
        //
        if (type->members().size() == 0) {
            const std::string mslName = "__ctl_err_placeholder";
            if (_declaredStructs.count(mslName))
                return;
            _declaredStructs.insert(mslName);
            Section previous = _section;
            setSection(Header);
            writeln("struct " + mslName + " {};");
            writeln("");
            setSection(previous);
            return;
        }
        throw IEX_NAMESPACE::NoImplExc(
            "CTL Metal backend: anonymous struct types are not supported.");
    }

    const std::string mslName = mslStructName(type->name());
    if (_declaredStructs.count(mslName))
        return;
    _declaredStructs.insert(mslName);

    //
    // A struct's members may themselves reference other structs (either
    // directly or as the element type of an array). Recursively declare
    // those first so the emitted header is in topological order.
    //
    const MemberVector &members = type->members();
    for (const Member &m : members) {
        const DataType *mt = m.type.pointer();
        while (const ArrayType *at = dynamic_cast<const ArrayType *>(mt))
            mt = at->elementType().pointer();
        if (const StructType *st = dynamic_cast<const StructType *>(mt))
            ensureStructDeclared(st);
    }

    Section previous = _section;
    setSection(Header);
    writeln("struct " + mslName);
    writeln("{");
    indent();
    for (const Member &m : members) {
        writeln(metalTypeName(m.type.pointer()) + " " + m.name + ";");
    }
    outdent();
    writeln("};");
    writeln("");
    _section = previous;
    _atLineStart = true;
}

void
MetalCodegen::ensureVSArrayPlaceholderDeclared()
{
    const std::string mslName = "__ctl_err_placeholder_vsarray";
    if (_declaredStructs.count(mslName))
        return;
    _declaredStructs.insert(mslName);
    Section previous = _section;
    setSection(Header);
    writeln("typedef int " + mslName + ";");
    writeln("");
    setSection(previous);
}

void
MetalCodegen::pushEmittingFunction(const std::string &name)
{
    _functionStack.push_back(name);
}

void
MetalCodegen::popEmittingFunction()
{
    if (_functionStack.empty())
        throw IEX_NAMESPACE::LogicExc(
            "CTL Metal backend: popEmittingFunction called on empty stack.");
    _functionStack.pop_back();
}

bool
MetalCodegen::isEmittingFunction(const std::string &name) const
{
    for (const std::string &n : _functionStack)
        if (n == name)
            return true;
    return false;
}

bool
MetalCodegen::markPrintUsed()
{
    if (_printUsed)
        return false;
    _printUsed = true;
    return true;
}

//
// Build the MSL fragment that declares the four half-exp/log helper
// functions. Ported bit-for-bit from `halfExpLog.h` in the CPU SIMD
// backend. The magic constants (4094.98169f / 68122.7031f / the
// `-16.6355323f` lower bound / the `11.0898665f` upper bound /
// `2.30258509f` = log(10)) are taken verbatim from the generator; any
// drift would break parity on the first sample across the boundary.
//
// Table hoisting: the three precomputed tables (`halfLog10Table`,
// `halfLogTable`, `halfExpTable` — ~739 KB combined) are bound as
// trailing `device const` MTLBuffers at dispatch time instead of being
// emitted inline as `constant` module-scope decimal-literal arrays.
// The arguments are plumbed as mandatory trailing params on every user
// helper + kernel wrapper (see `CtlMetalSyntaxTree.cpp`), mirroring the
// `__ctl_err_flag` plumbing. This drops ~4 MB of MSL source text from
// every halfExpLog-using module, shrinks Apple's persistent MSL cache
// key to user-code-only, and lets the tables be shared once per
// MTLDevice instead of re-uploaded per kernel.
//
// `as_type<>` is MSL's bit-preserving reinterpret; it matches the CPU
// side's `memcpy(&f, &i, sizeof(i))` exactly.
//
static std::string
emitHalfExpLogSection()
{
    static const std::string cached = []() {
    std::ostringstream out;
    out << "\n";
    out << "// --- Half-precision exp/log helpers ---\n";
    out << "// Ported from lib/IlmCtlSimd/halfExpLog.h for 0-ULP parity.\n";
    out << "// Tables are bound as device-const MTLBuffers (see\n";
    out << "// MetalFunctionCall::callFunction) rather than baked into\n";
    out << "// MSL source.\n";
    out << "\n";

    //
    // `log_h(half x)` and `log10_h(half x)` — table lookup keyed by the
    // 16-bit half bit-pattern. CPU copies the 32-bit table entry into a
    // float via memcpy; MSL's `as_type<float>(uint)` does the same
    // bit-for-bit.
    //
    // `exp_h(float x)` — branch on range, index the exp table via the
    // same `int(x * 4094.98169f + 68122.7031f)` truncation the CPU
    // uses. For `x < -16.6355323f` return +0.0h; for `x > 11.0898665f`
    // return +inf.h; for NaN return a quiet NaN half. MSL's
    // `as_type<half>(ushort)` mirrors the CPU's
    // `*(half *)(&expTable[...])`.
    //
    // `pow10_h(float x) = exp_h(x * log(10))` — the constant
    // 2.30258509f matches the CPU generator literal. Multiply is a
    // single strict mul (FP_CONTRACT OFF), no fma.
    //
    out << "static inline float ctl_stdlib_log10_h(\n";
    out << "    half x,\n";
    out << "    device const uint *__ctl_half_log10_tbl,\n";
    out << "    device const uint *__ctl_half_log_tbl,\n";
    out << "    device const ushort *__ctl_half_exp_tbl)\n";
    out << "{\n";
    out << "    (void)__ctl_half_log_tbl; (void)__ctl_half_exp_tbl;\n";
    out << "    return as_type<float>(__ctl_half_log10_tbl["
           "as_type<ushort>(x)]);\n";
    out << "}\n";
    out << "static inline float ctl_stdlib_log_h(\n";
    out << "    half x,\n";
    out << "    device const uint *__ctl_half_log10_tbl,\n";
    out << "    device const uint *__ctl_half_log_tbl,\n";
    out << "    device const ushort *__ctl_half_exp_tbl)\n";
    out << "{\n";
    out << "    (void)__ctl_half_log10_tbl; (void)__ctl_half_exp_tbl;\n";
    out << "    return as_type<float>(__ctl_half_log_tbl["
           "as_type<ushort>(x)]);\n";
    out << "}\n";
    //
    // The CPU SIMD reference is `int(x * 4094.98169f + 68122.7031f)`
    // compiled with Apple Clang's default `-ffp-contract=on`, which
    // fuses the mul+add into a single-rounding `fma` per Clang's
    // within-expression contraction rule. Our MSL implementation must
    // match that fused rounding, so emit `metal::fma(x, c1, c2)`
    // explicitly. The file-scope `#pragma STDC FP_CONTRACT OFF` would
    // otherwise produce the discrete mul-then-add pair and drift by
    // one exp-table index on boundary cases (observed as a one-half-
    // ULP divergence in pow_h = `exp_h(y * log_h(x))`).
    //
    out << "static inline half ctl_stdlib_exp_h(\n";
    out << "    float x,\n";
    out << "    device const uint *__ctl_half_log10_tbl,\n";
    out << "    device const uint *__ctl_half_log_tbl,\n";
    out << "    device const ushort *__ctl_half_exp_tbl)\n";
    out << "{\n";
    out << "    (void)__ctl_half_log10_tbl; (void)__ctl_half_log_tbl;\n";
    out << "    if (x >= -16.6355323f) {\n";
    out << "        if (x <= 11.0898665f) {\n";
    out << "            int idx = int(metal::fma(x, 4094.98169f, "
           "68122.7031f));\n";
    out << "            return as_type<half>(__ctl_half_exp_tbl[idx]);\n";
    out << "        } else {\n";
    out << "            return as_type<half>((ushort)0x7c00);\n";
    out << "        }\n";
    out << "    } else if (x < -16.6355323f) {\n";
    out << "        return (half)0.0h;\n";
    out << "    } else {\n";
    out << "        return as_type<half>((ushort)0x7e00);\n";
    out << "    }\n";
    out << "}\n";
    out << "static inline half ctl_stdlib_pow10_h(\n";
    out << "    float x,\n";
    out << "    device const uint *__ctl_half_log10_tbl,\n";
    out << "    device const uint *__ctl_half_log_tbl,\n";
    out << "    device const ushort *__ctl_half_exp_tbl)\n";
    out << "{\n";
    out << "    return ctl_stdlib_exp_h(x * 2.30258509f,\n";
    out << "        __ctl_half_log10_tbl, __ctl_half_log_tbl, "
           "__ctl_half_exp_tbl);\n";
    out << "}\n";
    //
    // pow_h(x, y) = exp_h(y * log_h(x)). Mirrors the CPU SIMD helper
    // in `lib/IlmCtlSimd/halfExpLog.h`; both tables match the SIMD
    // backend bit-for-bit so the composition inherits 0-ULP parity.
    //
    out << "static inline half ctl_stdlib_pow_h(\n";
    out << "    half x, float y,\n";
    out << "    device const uint *__ctl_half_log10_tbl,\n";
    out << "    device const uint *__ctl_half_log_tbl,\n";
    out << "    device const ushort *__ctl_half_exp_tbl)\n";
    out << "{\n";
    out << "    return ctl_stdlib_exp_h(\n";
    out << "        y * ctl_stdlib_log_h(x, __ctl_half_log10_tbl, "
           "__ctl_half_log_tbl, __ctl_half_exp_tbl),\n";
    out << "        __ctl_half_log10_tbl, __ctl_half_log_tbl, "
           "__ctl_half_exp_tbl);\n";
    out << "}\n";
    out << "\n";

    return out.str();
    }();
    return cached;
}

namespace {

//
// Drop user helpers that are unreachable from any kernel wrapper. A
// loaded module typically contains helpers that are only used by the
// host-side sidecar interpreter to evaluate module-scope `const`
// initializers; emitting them into MSL inflates compile time and
// can introduce address-space conflicts with the runtime-reachable
// templated VSArray formals. Stdlib helpers live in the preamble and
// are not subject to pruning.
//

struct ParsedHelper
{
    std::string name;
    std::string text;
    std::set<std::string> callees;
};

void
collectCallees(const std::string &text, std::set<std::string> &out)
{
    const std::string prefix = "__ctl_";
    size_t pos = 0;
    while ((pos = text.find(prefix, pos)) != std::string::npos) {
        size_t nameStart = pos;
        size_t nameEnd = nameStart;
        while (nameEnd < text.size()) {
            char c = text[nameEnd];
            if (!(isalnum(static_cast<unsigned char>(c)) || c == '_'))
                break;
            ++nameEnd;
        }
        if (nameEnd < text.size() && text[nameEnd] == '(')
            out.insert(text.substr(nameStart, nameEnd - nameStart));
        pos = nameEnd;
    }
}

std::vector<ParsedHelper>
parseHelpers(const std::string &body)
{
    std::vector<ParsedHelper> helpers;
    const std::string sigPrefix = "static inline ";
    size_t pos = 0;
    while (pos < body.size()) {
        size_t sigStart = body.find(sigPrefix, pos);
        if (sigStart == std::string::npos) break;
        if (sigStart != 0 && body[sigStart - 1] != '\n') {
            pos = sigStart + sigPrefix.size();
            continue;
        }
        // If a `template<...>` line precedes the signature, include
        // it in the helper's text span so the prefix survives pruning.
        size_t helperStart = sigStart;
        if (sigStart >= 2) {
            size_t prevLineEnd = sigStart - 1;
            size_t prevLineStart = prevLineEnd;
            while (prevLineStart > 0 && body[prevLineStart - 1] != '\n')
                --prevLineStart;
            const std::string templatePrefix = "template<";
            if (prevLineEnd > prevLineStart &&
                body.compare(prevLineStart, templatePrefix.size(),
                             templatePrefix) == 0) {
                helperStart = prevLineStart;
            }
        }
        size_t parenOpen = body.find('(', sigStart);
        if (parenOpen == std::string::npos) break;
        size_t nameEnd = parenOpen;
        size_t nameStart = nameEnd;
        while (nameStart > sigStart) {
            char c = body[nameStart - 1];
            if (isalnum(static_cast<unsigned char>(c)) || c == '_')
                --nameStart;
            else
                break;
        }
        std::string name = body.substr(nameStart, nameEnd - nameStart);
        if (name.compare(0, 6, "__ctl_") != 0) {
            pos = parenOpen + 1;
            continue;
        }
        size_t braceOpen = body.find('{', parenOpen);
        if (braceOpen == std::string::npos) break;
        int depth = 1;
        size_t i = braceOpen + 1;
        while (i < body.size() && depth > 0) {
            char c = body[i];
            if (c == '{') ++depth;
            else if (c == '}') --depth;
            ++i;
        }
        if (depth != 0) break;
        size_t bodyEnd = i;
        if (bodyEnd < body.size() && body[bodyEnd] == '\n')
            ++bodyEnd;

        ParsedHelper h;
        h.name = std::move(name);
        h.text = body.substr(helperStart, bodyEnd - helperStart);
        std::set<std::string> callees;
        collectCallees(h.text, callees);
        callees.erase(h.name);
        h.callees = std::move(callees);
        helpers.push_back(std::move(h));

        pos = bodyEnd;
    }
    return helpers;
}

std::string
prunedBody(const std::string &body,
           const std::vector<const std::string *> &rootBodies)
{
    if (body.empty()) return body;

    std::vector<ParsedHelper> helpers = parseHelpers(body);
    if (helpers.empty()) return body;

    std::map<std::string, const ParsedHelper *> byName;
    for (const ParsedHelper &h : helpers)
        byName[h.name] = &h;

    std::set<std::string> reachable;
    std::queue<std::string> work;
    for (const std::string *rb : rootBodies) {
        std::set<std::string> rootCallees;
        collectCallees(*rb, rootCallees);
        for (const std::string &c : rootCallees) {
            if (byName.count(c) && reachable.insert(c).second)
                work.push(c);
        }
    }

    while (!work.empty()) {
        std::string n = work.front();
        work.pop();
        const ParsedHelper *h = byName[n];
        if (!h) continue;
        for (const std::string &c : h->callees) {
            if (byName.count(c) && reachable.insert(c).second)
                work.push(c);
        }
    }

    // Reassemble in original emission order so forward-declared type
    // dependencies between helpers are preserved.
    std::string out;
    out.reserve(body.size());
    for (const ParsedHelper &h : helpers) {
        if (reachable.count(h.name))
            out += h.text;
    }
    return out;
}

} // anonymous namespace

std::string
MetalCodegen::source() const
{
    std::string halfSection;
    if (_halfExpLogUsed)
        halfSection = emitHalfExpLogSection();

    std::vector<const std::string *> roots;
    for (const auto &kv : _kernelWrappers)
        roots.push_back(&kv.second);
    std::string filteredBody = roots.empty() ? _body
                                             : prunedBody(_body, roots);

    size_t kernelBytes = 0;
    for (const auto &kv : _kernelWrappers)
        kernelBytes += kv.second.size();

    std::string out;
    out.reserve(_header.size() + filteredBody.size() + halfSection.size()
                + kernelBytes + 64);
    out += kPreamble;
    out += halfSection;
    out += _header;
    if (!_header.empty() && _header.back() != '\n')
        out += '\n';
    out += filteredBody;
    for (const auto &kv : _kernelWrappers)
        out += kv.second;
    return out;
}

std::string
MetalCodegen::sourceForKernel(const std::string &kernelName) const
{
    auto it = _kernelWrappers.find(kernelName);
    if (it == _kernelWrappers.end())
        return source();

    std::string halfSection;
    if (_halfExpLogUsed)
        halfSection = emitHalfExpLogSection();

    std::vector<const std::string *> roots;
    roots.push_back(&it->second);
    std::string filteredBody = prunedBody(_body, roots);

    std::string out;
    out.reserve(_header.size() + filteredBody.size() + halfSection.size()
                + it->second.size() + 64);
    out += kPreamble;
    out += halfSection;
    out += _header;
    if (!_header.empty() && _header.back() != '\n')
        out += '\n';
    out += filteredBody;
    out += it->second;
    return out;
}

} // namespace Ctl
