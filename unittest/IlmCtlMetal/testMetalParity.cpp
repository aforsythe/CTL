///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

//
// Per-program parity: run real .ctl fixtures from disk through
// both SimdInterpreter and MetalInterpreter on identical deterministic
// half-precision pixel input, and assert byte-for-byte output match.
// This covers the full front-end → Metal codegen → dispatch → readback
// pipeline against hand-authored fixture files that were not written
// with the Metal backend in mind.
//
// For every .ctl fixture in unittest/IlmCtl/, unittest/ctlrender/,
// unittest/IlmImfCtl/, and resources/test/ctl/, run both
// SimdInterpreter and MetalInterpreter on identical deterministic
// pixel input and assert bit-exact output match.
//
// Scope: the identity-shaped fixtures from unittest/ctlrender/
// (unity, unity_with_alpha) — these exercise the
// varying-half I/O path that real ctlrender transforms use, with no
// stdlib dependency so 0-ULP is achievable on all pixels. Math-heavy
// fixtures are gated behind stdlib landings and are added to this
// harness as each function lands with documented precision.
//

#include "testMetalParity.h"

#include <CtlFunctionCall.h>
#include <CtlMetalDevice.h>
#include <CtlMetalInterpreter.h>
#include <CtlSimdInterpreter.h>

#include <half.h>

#include "testRequire.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

//
// Describes a fixture on disk. All input/output args are assumed varying
// half; this matches the unittest/ctlrender/*.ctl shape that ctlrender
// itself feeds. Fixtures that need uniform parameters or non-half types
// are added as separate entries once their stdlib support lands.
//
struct VaryingHalfFixture
{
    const char *file;          // relative to CMAKE_CURRENT_BINARY_DIR
    const char *moduleName;
    const char *functionName;
    std::vector<const char *> inputs;   // varying half input names
    std::vector<const char *> outputs;  // varying half output names
};

//
// Deterministic half sample set. mt19937 seeded with a fixed constant,
// values drawn uniformly from [-4, 4] — a range wide enough to include
// normal positives, normal negatives, and subnormals near zero once
// rounded to half.
//
std::vector<half>
makeHalfSamples(size_t n, uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> floats(-4.0f, 4.0f);
    std::vector<half> out(n);
    for (size_t i = 0; i < n; ++i)
        out[i] = half(floats(rng));
    return out;
}

//
// Run a single fixture on a given interpreter, returning the output
// channels laid out as [output0_samples..., output1_samples..., ...].
// Each input channel gets its own deterministic seed so every channel
// carries a different value at every sample.
//
template <class Interp>
std::vector<half>
runFixture(Interp &interp,
           const VaryingHalfFixture &fx,
           size_t numSamples,
           uint32_t baseSeed)
{
    interp.loadFile(fx.file, fx.moduleName);
    Ctl::FunctionCallPtr fn = interp.newFunctionCall(fx.functionName);
    REQUIRE(fn);
    REQUIRE(fn->numInputArgs() == fx.inputs.size());
    REQUIRE(fn->numOutputArgs() == fx.outputs.size());

    // Resolve input args by name so the fixture declaration order need
    // not match the function's parameter order.
    for (size_t i = 0; i < fx.inputs.size(); ++i) {
        Ctl::FunctionArgPtr arg;
        for (size_t j = 0; j < fn->numInputArgs(); ++j) {
            if (fn->inputArg(j)->name() == fx.inputs[i]) {
                arg = fn->inputArg(j);
                break;
            }
        }
        if (!arg) {
            std::cerr << "testMetalParity: input arg '" << fx.inputs[i]
                      << "' not found in fixture " << fx.file << std::endl;
            std::abort();
        }
        REQUIRE(arg->isVarying());

        std::vector<half> samples =
            makeHalfSamples(numSamples, baseSeed + uint32_t(i));
        std::memcpy(arg->data(), samples.data(),
                    numSamples * sizeof(half));
    }

    fn->callFunction(numSamples);

    std::vector<half> result(fx.outputs.size() * numSamples);
    for (size_t i = 0; i < fx.outputs.size(); ++i) {
        Ctl::FunctionArgPtr arg;
        for (size_t j = 0; j < fn->numOutputArgs(); ++j) {
            if (fn->outputArg(j)->name() == fx.outputs[i]) {
                arg = fn->outputArg(j);
                break;
            }
        }
        if (!arg) {
            std::cerr << "testMetalParity: output arg '" << fx.outputs[i]
                      << "' not found in fixture " << fx.file << std::endl;
            std::abort();
        }
        REQUIRE(arg->isVarying());
        std::memcpy(result.data() + i * numSamples,
                    arg->data(),
                    numSamples * sizeof(half));
    }
    return result;
}

bool
halfBitsEqual(half a, half b)
{
    uint16_t ua, ub;
    std::memcpy(&ua, &a, sizeof(ua));
    std::memcpy(&ub, &b, sizeof(ub));
    return ua == ub;
}

//
// ULP distance for half values. NaN/Inf mixes return a sentinel so
// callers can report them separately from a clean ULP count.
//
int32_t
halfUlpDistance(half a, half b)
{
    if (a.isNan() || b.isNan())
        return (a == b) ? 0 : INT32_MAX;
    if (a.isInfinity() || b.isInfinity())
        return (a == b) ? 0 : INT32_MAX;
    int16_t ia, ib;
    std::memcpy(&ia, &a, sizeof(ia));
    std::memcpy(&ib, &b, sizeof(ib));
    if (ia < 0) ia = int16_t(-0x8000) - ia;
    if (ib < 0) ib = int16_t(-0x8000) - ib;
    return std::abs(int32_t(ia) - int32_t(ib));
}

void
checkFixtureParity(const VaryingHalfFixture &fx,
                   const std::vector<half> &gpu,
                   const std::vector<half> &cpu,
                   size_t numSamples)
{
    REQUIRE(gpu.size() == cpu.size());
    REQUIRE(gpu.size() == fx.outputs.size() * numSamples);

    int32_t maxUlp = 0;
    size_t  diverged = 0;
    size_t  firstDiff = SIZE_MAX;
    size_t  firstDiffChan = 0;

    for (size_t c = 0; c < fx.outputs.size(); ++c) {
        for (size_t s = 0; s < numSamples; ++s) {
            size_t idx = c * numSamples + s;
            if (!halfBitsEqual(gpu[idx], cpu[idx])) {
                ++diverged;
                int32_t d = halfUlpDistance(gpu[idx], cpu[idx]);
                if (d > maxUlp) maxUlp = d;
                if (firstDiff == SIZE_MAX) {
                    firstDiff = s;
                    firstDiffChan = c;
                }
            }
        }
    }

    if (diverged != 0) {
        std::cerr << "testMetalParity: " << fx.file
                  << " parity FAIL: " << diverged << " / "
                  << (fx.outputs.size() * numSamples)
                  << " samples diverged, max ULP=" << maxUlp
                  << ", first diff at " << fx.outputs[firstDiffChan]
                  << "[" << firstDiff << "]: gpu="
                  << float(gpu[firstDiffChan * numSamples + firstDiff])
                  << " cpu="
                  << float(cpu[firstDiffChan * numSamples + firstDiff])
                  << std::endl;
        std::abort();
    }

    std::cout << "  " << fx.file << " — "
              << fx.outputs.size() << " ch x " << numSamples
              << " samples: bit-exact parity ok" << std::endl;
}

const VaryingHalfFixture kFixtures[] =
{
    //
    // unity.ctl: pure identity on 3 half channels. Guaranteed 0-ULP on
    // any backend because the emitted kernel does nothing but copy.
    //
    {
        "unity.ctl", "unity", "::unity",
        { "rIn", "gIn", "bIn" },
        { "rOut", "gOut", "bOut" },
    },

    //
    // unity_with_alpha.ctl: identity on 4 half channels. Exercises the
    // 4-arg varying path that the RGBA ctlrender pipeline uses.
    //
    {
        "unity_with_alpha.ctl", "unity_with_alpha", "::unity_with_alpha",
        { "rIn", "gIn", "bIn", "aIn" },
        { "rOut", "gOut", "bOut", "aOut" },
    },
};

} // anonymous namespace

void
testMetalParity()
{
    std::cout << "Testing CPU vs GPU parity on real .ctl fixtures"
              << std::endl;

    if (!Ctl::metalBackendAvailable()) {
        std::cout << "  Metal backend unavailable on this host — skipping."
                  << std::endl;
        return;
    }

    const size_t numSamples = 1024;
    const uint32_t baseSeed = 0xA11CE01;

    const size_t numFixtures = sizeof(kFixtures) / sizeof(kFixtures[0]);
    for (size_t i = 0; i < numFixtures; ++i) {
        const VaryingHalfFixture &fx = kFixtures[i];

        Ctl::SimdInterpreter  cpu;
        Ctl::MetalInterpreter gpu;

        std::vector<half> cpuOut, gpuOut;
        try {
            cpuOut = runFixture(cpu, fx, numSamples, baseSeed);
        } catch (const std::exception &e) {
            std::cerr << "testMetalParity: CPU run failed for " << fx.file
                      << ": " << e.what() << std::endl;
            throw;
        }
        try {
            gpuOut = runFixture(gpu, fx, numSamples, baseSeed);
        } catch (const std::exception &e) {
            std::cerr << "testMetalParity: GPU run failed for " << fx.file
                      << ": " << e.what() << std::endl;
            throw;
        }

        checkFixtureParity(fx, gpuOut, cpuOut, numSamples);
    }
}
