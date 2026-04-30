///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

//
// ACES v2 OutputTransform parity: run aces_combined.ctl (the flattened
// Rec.709 ODT: aces → CAM16 Hellwig2022 JMh → display XYZ) on both
// SimdInterpreter and MetalInterpreter and assert max-ULP drift stays
// below the documented threshold. This is the CI regression guard for
// the whole ACES v2 compute chain — every per-pixel `pow`, `sin`,
// `cos`, `atan2`, division, and matrix multiply compounds into these
// three output channels, so a regression anywhere in the stdlib port or
// codegen shows up here.
//
// Threshold rationale: as of 2026-04-19 with the FreeBSD `ctl_stdlib_pow`
// port and the `k_sinf.c` / `k_cosf.c` FP32 trig kernels landed, the
// CAM16 chain measures within a small ULP window that mostly comes from
// the 1-ULP FP64-floor deviations on `exp`/`log`/`pow`/`sin`/`cos`
// compounding through the tonescale + sigmoid + hue-angle arithmetic.
// If a precision regression drops us back to `metal::precise::pow` (the
// pre-port baseline), the ACES v2 chain drifted >290 ULPs — so a
// threshold set in the low-hundreds is both tight enough to catch
// realistic regressions and loose enough to absorb expected FP32
// accumulation across the ~50 transcendentals in the pipeline.
//

#include "testMetalAcesV2Parity.h"

#include <CtlFunctionCall.h>
#include <CtlMetalDevice.h>
#include <CtlMetalInterpreter.h>
#include <CtlSimdInterpreter.h>

#include "testRequire.h"
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

//
// ULP distance between two finite, same-sign floats. NaN/Inf pairs that
// don't bit-match return a sentinel so the caller can separate
// "numeric drift" from "regime flip".
//
int64_t
ulpDistance(float a, float b)
{
    if (std::isnan(a) || std::isnan(b))
        return (a == b) ? 0 : INT64_MAX;
    if (std::isinf(a) || std::isinf(b))
        return (a == b) ? 0 : INT64_MAX;
    int32_t ia, ib;
    std::memcpy(&ia, &a, sizeof(ia));
    std::memcpy(&ib, &b, sizeof(ib));
    if (ia < 0) ia = INT32_MIN - ia;
    if (ib < 0) ib = INT32_MIN - ib;
    return std::llabs(int64_t(ia) - int64_t(ib));
}

//
// Deterministic input set covering the ACES working range:
//   * a handful of pinned values: black, mid-gray, unity, super-white,
//     and a saturated-per-channel primary sweep
//   * the bulk drawn uniformly from [0.02, 8.0] so each channel hits
//     the tonescale's interesting regimes (shadow roll-off through
//     super-white highlight compression), seeded for reproducibility
//
struct Rgb { float r, g, b; };

std::vector<Rgb>
makeAcesSamples(size_t n, uint32_t seed)
{
    std::vector<Rgb> out;
    out.reserve(n);

    const Rgb pinned[] = {
        { 0.0f, 0.0f, 0.0f },       // black
        { 0.18f, 0.18f, 0.18f },    // mid-gray
        { 1.0f, 1.0f, 1.0f },       // diffuse white
        { 4.0f, 4.0f, 4.0f },       // super-white
        { 1.0f, 0.0f, 0.0f },       // saturated red
        { 0.0f, 1.0f, 0.0f },       // saturated green
        { 0.0f, 0.0f, 1.0f },       // saturated blue
        { 0.75f, 0.5f, 0.25f },     // generic warm mid-tone
    };
    for (const Rgb &p : pinned)
        out.push_back(p);

    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(0.02f, 8.0f);
    while (out.size() < n)
        out.push_back({ dist(rng), dist(rng), dist(rng) });

    return out;
}

template <class Interp>
std::vector<float>
runAcesCombined(Interp &interp, const std::vector<Rgb> &samples)
{
    interp.loadFile("aces_combined.ctl", "aces_combined");
    Ctl::FunctionCallPtr fn = interp.newFunctionCall("::main");
    REQUIRE(fn);
    REQUIRE(fn->numInputArgs() == 3);
    REQUIRE(fn->numOutputArgs() == 3);

    const size_t n = samples.size();

    Ctl::FunctionArgPtr rIn = fn->inputArg(0);
    Ctl::FunctionArgPtr gIn = fn->inputArg(1);
    Ctl::FunctionArgPtr bIn = fn->inputArg(2);
    Ctl::FunctionArgPtr rOut = fn->outputArg(0);
    Ctl::FunctionArgPtr gOut = fn->outputArg(1);
    Ctl::FunctionArgPtr bOut = fn->outputArg(2);

    REQUIRE(rIn->isVarying() && gIn->isVarying() && bIn->isVarying());
    REQUIRE(rOut->isVarying() && gOut->isVarying() && bOut->isVarying());

    for (size_t i = 0; i < n; ++i) {
        std::memcpy(rIn->data() + i * sizeof(float),
                    &samples[i].r, sizeof(float));
        std::memcpy(gIn->data() + i * sizeof(float),
                    &samples[i].g, sizeof(float));
        std::memcpy(bIn->data() + i * sizeof(float),
                    &samples[i].b, sizeof(float));
    }

    fn->callFunction(n);

    std::vector<float> result(3 * n);
    std::memcpy(result.data() + 0 * n, rOut->data(), n * sizeof(float));
    std::memcpy(result.data() + 1 * n, gOut->data(), n * sizeof(float));
    std::memcpy(result.data() + 2 * n, bOut->data(), n * sizeof(float));
    return result;
}

} // anonymous namespace

void
testMetalAcesV2Parity()
{
    std::cout << "Testing ACES v2 OutputTransform CPU vs GPU parity"
              << std::endl;

    if (!Ctl::metalBackendAvailable()) {
        std::cout << "  Metal backend unavailable on this host — skipping."
                  << std::endl;
        return;
    }

    //
    // CI regression threshold. Tight enough that any serious precision
    // backslide (e.g. reverting a stdlib port) blows past it; loose
    // enough that expected FP32 accumulation across ~50 transcendentals
    // per pixel fits under the bound.
    //
    // Measured 2026-04-19 on M4 Max with the full ACES v2 gamut mapper
    // wired in (gamutMap_fwd + chromaCompression + evaluate_gamma_fit):
    // max drift is 55 ULPs, ~46% of 1024 samples diverged. 128 ULP gives
    // ~2.3× headroom above current to absorb hardware-family / driver
    // variance on future macOS releases, while still catching a
    // regression that reverts any of the ported stdlib helpers back to
    // their `metal::precise::*` baselines (measured 4×–7× higher on
    // this chain during the port-in-progress iterations).
    //
    const int64_t kMaxUlpThreshold = 128;

    const size_t numSamples = 1024;
    const uint32_t seed     = 0xACE2C71F;

    std::vector<Rgb> samples = makeAcesSamples(numSamples, seed);

    Ctl::SimdInterpreter  cpu;
    Ctl::MetalInterpreter gpu;

    std::vector<float> cpuOut, gpuOut;
    try {
        cpuOut = runAcesCombined(cpu, samples);
    } catch (const std::exception &e) {
        std::cerr << "testMetalAcesV2Parity: CPU run failed: " << e.what()
                  << std::endl;
        throw;
    }
    try {
        gpuOut = runAcesCombined(gpu, samples);
    } catch (const std::exception &e) {
        std::cerr << "testMetalAcesV2Parity: GPU run failed: " << e.what()
                  << std::endl;
        throw;
    }

    REQUIRE(cpuOut.size() == gpuOut.size());
    REQUIRE(cpuOut.size() == 3 * numSamples);

    int64_t maxUlp   = 0;
    size_t  diverged = 0;
    size_t  firstDiff = SIZE_MAX;
    size_t  firstDiffChan = 0;
    const char *chanName[3] = { "rOut", "gOut", "bOut" };

    size_t le1 = 0, le2 = 0, le4 = 0, le8 = 0, le16 = 0, le64 = 0;
    for (size_t c = 0; c < 3; ++c) {
        for (size_t s = 0; s < numSamples; ++s) {
            size_t idx = c * numSamples + s;
            uint32_t ug, uc;
            std::memcpy(&ug, &gpuOut[idx], sizeof(ug));
            std::memcpy(&uc, &cpuOut[idx], sizeof(uc));
            if (ug == uc) continue;
            ++diverged;
            int64_t d = ulpDistance(gpuOut[idx], cpuOut[idx]);
            if (d > maxUlp) maxUlp = d;
            if (d <= 1)  ++le1;
            if (d <= 2)  ++le2;
            if (d <= 4)  ++le4;
            if (d <= 8)  ++le8;
            if (d <= 16) ++le16;
            if (d <= 64) ++le64;
            if (firstDiff == SIZE_MAX) {
                firstDiff = s;
                firstDiffChan = c;
            }
        }
    }

    std::cout << "  aces_combined.ctl (Rec.709 ODT): "
              << "3 ch x " << numSamples << " samples, "
              << "max ULP=" << maxUlp << " (threshold "
              << kMaxUlpThreshold << "), diverged=" << diverged
              << " / " << (3 * numSamples)
              << " [<=1:" << le1 << " <=2:" << le2
              << " <=4:" << le4 << " <=8:" << le8
              << " <=16:" << le16 << " <=64:" << le64 << "]"
              << std::endl;

    if (maxUlp > kMaxUlpThreshold) {
        std::cerr << "testMetalAcesV2Parity: max ULP " << maxUlp
                  << " exceeds threshold " << kMaxUlpThreshold
                  << ". First diff at " << chanName[firstDiffChan]
                  << "[" << firstDiff << "]: rgb_in=("
                  << samples[firstDiff].r << ", " << samples[firstDiff].g
                  << ", " << samples[firstDiff].b << "), gpu="
                  << gpuOut[firstDiffChan * numSamples + firstDiff]
                  << " cpu="
                  << cpuOut[firstDiffChan * numSamples + firstDiff]
                  << std::endl
                  << "See lib/IlmCtlMetal/PRECISION.md for the stdlib"
                  << " precision floors this chain inherits." << std::endl;
        std::abort();
    }
}
