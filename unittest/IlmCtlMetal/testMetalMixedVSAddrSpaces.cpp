///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

//
// Regression for the templated VSArray pointer formal: the same helper
// called once with a module-`const` actual and once with a function-
// local actual in a single kernel must produce both instantiations and
// match CPU SIMD bit-for-bit.
//

#include "testMetalMixedVSAddrSpaces.h"

#include <CtlFunctionCall.h>
#include <CtlMetalDevice.h>
#include <CtlMetalInterpreter.h>
#include <CtlSimdInterpreter.h>

#include "testRequire.h"
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>

namespace {

struct Rgb { float r, g, b; };

std::vector<Rgb>
makeSamples(size_t n, uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-2.0f, 2.0f);
    std::vector<Rgb> out(n);
    for (size_t i = 0; i < n; ++i)
        out[i] = { dist(rng), dist(rng), dist(rng) };
    return out;
}

template <class Interp>
std::vector<float>
runMixed(Interp &interp, const char *moduleName, const std::vector<Rgb> &samples)
{
    interp.loadFile("mixedVSAddrSpaces.ctl", moduleName);
    Ctl::FunctionCallPtr fn = interp.newFunctionCall("mixedVSA::testMixed");
    REQUIRE(fn);
    REQUIRE(fn->numInputArgs() == 3);
    REQUIRE(fn->numOutputArgs() == 1);

    Ctl::FunctionArgPtr rIn  = fn->inputArg(0);
    Ctl::FunctionArgPtr gIn  = fn->inputArg(1);
    Ctl::FunctionArgPtr bIn  = fn->inputArg(2);
    Ctl::FunctionArgPtr sOut = fn->outputArg(0);

    const size_t n = samples.size();
    for (size_t i = 0; i < n; ++i) {
        std::memcpy(rIn->data() + i * sizeof(float), &samples[i].r, sizeof(float));
        std::memcpy(gIn->data() + i * sizeof(float), &samples[i].g, sizeof(float));
        std::memcpy(bIn->data() + i * sizeof(float), &samples[i].b, sizeof(float));
    }

    fn->callFunction(n);

    std::vector<float> result(n);
    std::memcpy(result.data(), sOut->data(), n * sizeof(float));
    return result;
}

bool
bitsEqual(float a, float b)
{
    uint32_t ua, ub;
    std::memcpy(&ua, &a, sizeof(ua));
    std::memcpy(&ub, &b, sizeof(ub));
    return ua == ub;
}

} // anonymous namespace

void
testMetalMixedVSAddrSpaces()
{
    std::cout << "Testing mixed address-space VSArray template instantiation"
              << std::endl;

    if (!Ctl::metalBackendAvailable()) {
        std::cout << "  Metal backend unavailable on this host — skipping."
                  << std::endl;
        return;
    }

    const size_t numSamples = 256;
    const std::vector<Rgb> samples = makeSamples(numSamples, 0xC0FFEEu);

    std::vector<float> cpuOut, gpuOut;
    {
        Ctl::SimdInterpreter cpu;
        cpuOut = runMixed(cpu, "mixedVSA_cpu", samples);
    }
    {
        Ctl::MetalInterpreter gpu;
        gpuOut = runMixed(gpu, "mixedVSA_gpu", samples);
    }

    REQUIRE(cpuOut.size() == gpuOut.size());

    size_t diverged = 0;
    size_t firstDiff = SIZE_MAX;
    for (size_t i = 0; i < numSamples; ++i) {
        if (!bitsEqual(cpuOut[i], gpuOut[i])) {
            ++diverged;
            if (firstDiff == SIZE_MAX) firstDiff = i;
        }
    }
    if (diverged != 0) {
        std::cerr << "testMetalMixedVSAddrSpaces: diverged on " << diverged
                  << " / " << numSamples << " samples; first diff at ["
                  << firstDiff << "]: cpu=" << cpuOut[firstDiff]
                  << " gpu=" << gpuOut[firstDiff] << std::endl;
        std::abort();
    }

    //
    // Anchor the constant-actual half against its closed-form sum. A
    // template instantiation that read from the wrong address space
    // would leave this half at ~0 — many orders of magnitude off,
    // not float-rounding off.
    //
    const double kExpectedFromConst = 10101010101.0;
    const double kRelTolerance      = 1.0e-5;
    for (size_t i = 0; i < numSamples; ++i) {
        const Rgb &s = samples[i];
        double expectedLocal =
            static_cast<double>(s.r) *      1.0 +
            static_cast<double>(s.g) *     10.0 +
            static_cast<double>(s.b) *    100.0 +
            static_cast<double>(s.r) * 2.0 *   1000.0 +
            static_cast<double>(s.g) * 3.0 *  10000.0 +
            static_cast<double>(s.b) * 4.0 * 100000.0;
        double residualConstSide =
            static_cast<double>(gpuOut[i]) - expectedLocal;
        double relDrift = std::abs(residualConstSide - kExpectedFromConst)
                        / kExpectedFromConst;
        if (relDrift > kRelTolerance) {
            std::cerr << "testMetalMixedVSAddrSpaces: constant-actual half "
                      << "drifted at sample " << i << ": expected "
                      << kExpectedFromConst << " got " << residualConstSide
                      << " (relDrift=" << relDrift << ")" << std::endl;
            std::abort();
        }
    }

    std::cout << "  " << numSamples
              << " samples bit-exact CPU↔GPU; constant-actual half anchored at "
              << static_cast<long long>(kExpectedFromConst) << std::endl;
}
