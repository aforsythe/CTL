///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

//
// ACES v2 OutputTransform round-trip parity: run aces_combined.ctl's
// `roundtrip_main` (aces -> tonemapAndCompress_fwd -> gamutMap_fwd ->
// gamutMap_inv -> tonemapAndCompress_inv -> aces) on both the SIMD and
// Metal interpreters and assert the GPU tracks the CPU within a
// documented ULP bound.
//
// This is the regression guard for the ACES v2 *inverse* primitives
// (`compressGamut(invert=true)` + `chromaCompression(invert=true)` +
// `tonescale_inv`). The forward-only parity test (testMetalAcesV2Parity)
// exercises the forward chain through the Rec.709 display endpoint; this
// test exercises the full forward+inverse chain back in ACES space,
// skipping the JMh<->XYZ wrappers so any deviation is attributable to
// the inverse primitives rather than the display endpoint.
//
// Self-consistency of the CTL inverse (CPU output ≈ CPU input) is NOT
// asserted here — that's a language-level test of the aces_combined
// transform itself, not of the Metal backend. The compressGamut M < ε
// short-circuit and the tonescale near-zero/near-infinity clipping make
// a tight round-trip bound impossible anyway.
//

#include "testMetalAcesV2RoundTrip.h"

#include <CtlFunctionCall.h>
#include <CtlMetalDevice.h>
#include <CtlMetalInterpreter.h>
#include <CtlSimdInterpreter.h>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

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

struct Rgb { float r, g, b; };

//
// Deterministic input set biased toward the invertible interior of the
// forward transform: modest positive magnitudes where tonescale +
// chromaCompression are not saturating or short-circuiting. The pinned
// values include black, mid-gray, and diffuse white; the bulk is drawn
// from [0.05, 4.0] so we stay clear of the super-white regime where the
// tonescale flattens and the inverse becomes ill-conditioned.
//
std::vector<Rgb>
makeRoundTripSamples(size_t n, uint32_t seed)
{
    std::vector<Rgb> out;
    out.reserve(n);

    const Rgb pinned[] = {
        { 0.0f, 0.0f, 0.0f },
        { 0.18f, 0.18f, 0.18f },
        { 1.0f, 1.0f, 1.0f },
        { 0.5f, 0.3f, 0.1f },
        { 0.05f, 0.05f, 0.05f },
    };
    for (const Rgb &p : pinned)
        out.push_back(p);

    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(0.05f, 4.0f);
    while (out.size() < n)
        out.push_back({ dist(rng), dist(rng), dist(rng) });

    return out;
}

template <class Interp>
std::vector<float>
runRoundTrip(Interp &interp, const std::vector<Rgb> &samples)
{
    interp.loadFile("aces_combined.ctl", "aces_combined");
    Ctl::FunctionCallPtr fn = interp.newFunctionCall("::roundtrip_main");
    assert(fn);
    assert(fn->numInputArgs() == 3);
    assert(fn->numOutputArgs() == 3);

    const size_t n = samples.size();

    Ctl::FunctionArgPtr rIn = fn->inputArg(0);
    Ctl::FunctionArgPtr gIn = fn->inputArg(1);
    Ctl::FunctionArgPtr bIn = fn->inputArg(2);
    Ctl::FunctionArgPtr rOut = fn->outputArg(0);
    Ctl::FunctionArgPtr gOut = fn->outputArg(1);
    Ctl::FunctionArgPtr bOut = fn->outputArg(2);

    assert(rIn->isVarying() && gIn->isVarying() && bIn->isVarying());
    assert(rOut->isVarying() && gOut->isVarying() && bOut->isVarying());

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
testMetalAcesV2RoundTrip()
{
    std::cout << "Testing ACES v2 OutputTransform fwd+inv round-trip CPU vs GPU"
              << std::endl;

    if (!Ctl::metalBackendAvailable()) {
        std::cout << "  Metal backend unavailable on this host — skipping."
                  << std::endl;
        return;
    }

    //
    // CI regression threshold for the round-trip chain. The forward-only
    // chain measures 55 ULP on the same 1024-sample synthetic set; the
    // round-trip doubles the transcendental count (forward + inverse
    // share the same stdlib helpers) and amplifies cancellation in
    // compressionFunction's inverse branch (`-nd / (nd - 1)`, which
    // blows up as nd → 1). Measured 2026-04-19 on M4 Max: 688 ULP max
    // drift with 98.4% of diverged samples ≤64 ULP and only two outliers
    // above 256. 2048 ULP gives ~3× headroom while still catching a
    // regression that reverts any inverse primitive back to its
    // `metal::precise::*` baseline.
    //
    const int64_t kMaxUlpThreshold = 2048;

    const size_t numSamples = 1024;
    const uint32_t seed     = 0xACE21F02;

    std::vector<Rgb> samples = makeRoundTripSamples(numSamples, seed);

    Ctl::SimdInterpreter  cpu;
    Ctl::MetalInterpreter gpu;

    std::vector<float> cpuOut, gpuOut;
    try {
        cpuOut = runRoundTrip(cpu, samples);
    } catch (const std::exception &e) {
        std::cerr << "testMetalAcesV2RoundTrip: CPU run failed: " << e.what()
                  << std::endl;
        throw;
    }
    try {
        gpuOut = runRoundTrip(gpu, samples);
    } catch (const std::exception &e) {
        std::cerr << "testMetalAcesV2RoundTrip: GPU run failed: " << e.what()
                  << std::endl;
        throw;
    }

    assert(cpuOut.size() == gpuOut.size());
    assert(cpuOut.size() == 3 * numSamples);

    int64_t maxUlp   = 0;
    size_t  diverged = 0;
    size_t  firstDiff = SIZE_MAX;
    size_t  firstDiffChan = 0;
    const char *chanName[3] = { "rOut", "gOut", "bOut" };

    size_t le1 = 0, le2 = 0, le4 = 0, le8 = 0, le16 = 0,
           le64 = 0, le256 = 0;
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
            if (d <= 1)   ++le1;
            if (d <= 2)   ++le2;
            if (d <= 4)   ++le4;
            if (d <= 8)   ++le8;
            if (d <= 16)  ++le16;
            if (d <= 64)  ++le64;
            if (d <= 256) ++le256;
            if (firstDiff == SIZE_MAX) {
                firstDiff = s;
                firstDiffChan = c;
            }
        }
    }

    std::cout << "  aces_combined.ctl (fwd+inv): "
              << "3 ch x " << numSamples << " samples, "
              << "max ULP=" << maxUlp << " (threshold "
              << kMaxUlpThreshold << "), diverged=" << diverged
              << " / " << (3 * numSamples)
              << " [<=1:" << le1 << " <=2:" << le2
              << " <=4:" << le4 << " <=8:" << le8
              << " <=16:" << le16 << " <=64:" << le64
              << " <=256:" << le256 << "]"
              << std::endl;

    if (maxUlp > kMaxUlpThreshold) {
        std::cerr << "testMetalAcesV2RoundTrip: max ULP " << maxUlp
                  << " exceeds threshold " << kMaxUlpThreshold
                  << ". First diff at " << chanName[firstDiffChan]
                  << "[" << firstDiff << "]: rgb_in=("
                  << samples[firstDiff].r << ", " << samples[firstDiff].g
                  << ", " << samples[firstDiff].b << "), gpu="
                  << gpuOut[firstDiffChan * numSamples + firstDiff]
                  << " cpu="
                  << cpuOut[firstDiffChan * numSamples + firstDiff]
                  << std::endl;
        std::abort();
    }
}
