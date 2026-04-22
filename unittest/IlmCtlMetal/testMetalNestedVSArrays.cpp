///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

//
// Nested-VSArray codegen regression test. CTL lets a callee receive a
// caller's multi-dim fixed-size array as a variable-size parameter
// (e.g. `float[][3]`) and then index it as `arr[i][j]`. The Metal
// backend lowers such a parameter to a flat `thread const float*`
// plus length uniforms, so the CTL index chain needs explicit stride
// arithmetic rather than MSL's `[]` operator chain — without it,
// `arr[i][j]` reads the wrong scalar (or fails to compile altogether).
//
// This test exercises three shapes against `nestedVSArrays.ctl`:
//   * read-only `float[][3]`              — `mat23_weighted`
//   * writable  `float[][3]`              — `mat33_writeback`
//   * fully variable 3D `float[][][]`     — `grid_sample`
//
// Each is called via a varying scalar wrapper. Metal and SimdInterpreter
// must agree bit-for-bit — these are integer-ish coefficient sums so no
// transcendentals are involved and 0-ULP parity is achievable.
//

#include "testMetalNestedVSArrays.h"

#include <CtlFunctionCall.h>
#include <CtlMetalDevice.h>
#include <CtlMetalInterpreter.h>
#include <CtlSimdInterpreter.h>

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

struct Rgb { float r, g, b; };

std::vector<Rgb>
makeSamples(size_t n, uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-4.0f, 4.0f);
    std::vector<Rgb> out(n);
    for (size_t i = 0; i < n; ++i)
        out[i] = { dist(rng), dist(rng), dist(rng) };
    return out;
}

template <class Interp>
std::vector<float>
runScalarWrapper(Interp &interp,
                 const char *moduleName,
                 const char *functionName,
                 const std::vector<Rgb> &samples)
{
    interp.loadFile("nestedVSArrays.ctl", moduleName);
    Ctl::FunctionCallPtr fn = interp.newFunctionCall(functionName);
    assert(fn);
    assert(fn->numInputArgs() == 3);
    assert(fn->numOutputArgs() == 1);

    Ctl::FunctionArgPtr rIn = fn->inputArg(0);
    Ctl::FunctionArgPtr gIn = fn->inputArg(1);
    Ctl::FunctionArgPtr bIn = fn->inputArg(2);
    Ctl::FunctionArgPtr sOut = fn->outputArg(0);
    assert(rIn->isVarying() && gIn->isVarying() && bIn->isVarying());
    assert(sOut->isVarying());

    const size_t n = samples.size();
    for (size_t i = 0; i < n; ++i) {
        std::memcpy(rIn->data() + i * sizeof(float),
                    &samples[i].r, sizeof(float));
        std::memcpy(gIn->data() + i * sizeof(float),
                    &samples[i].g, sizeof(float));
        std::memcpy(bIn->data() + i * sizeof(float),
                    &samples[i].b, sizeof(float));
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

void
checkParity(const char *label,
            const std::vector<float> &cpuOut,
            const std::vector<float> &gpuOut)
{
    assert(cpuOut.size() == gpuOut.size());
    size_t diverged = 0;
    size_t firstDiff = SIZE_MAX;
    for (size_t i = 0; i < cpuOut.size(); ++i) {
        if (!bitsEqual(cpuOut[i], gpuOut[i])) {
            ++diverged;
            if (firstDiff == SIZE_MAX) firstDiff = i;
        }
    }
    if (diverged != 0) {
        std::cerr << "testMetalNestedVSArrays: " << label
                  << " diverged on " << diverged << " / "
                  << cpuOut.size() << " samples; first diff at ["
                  << firstDiff << "]: cpu=" << cpuOut[firstDiff]
                  << " gpu=" << gpuOut[firstDiff] << std::endl;
        std::abort();
    }
    std::cout << "  " << label << ": " << cpuOut.size()
              << " samples bit-exact ok" << std::endl;
}

} // anonymous namespace

void
testMetalNestedVSArrays()
{
    std::cout << "Testing nested-VSArray codegen parity (CPU vs GPU)"
              << std::endl;

    if (!Ctl::metalBackendAvailable()) {
        std::cout << "  Metal backend unavailable on this host — skipping."
                  << std::endl;
        return;
    }

    const size_t numSamples = 256;
    const uint32_t seed     = 0xBEEFCAFE;
    const std::vector<Rgb> samples = makeSamples(numSamples, seed);

    struct Case {
        const char *label;
        const char *moduleName;
        const char *fnName;
    };

    const Case cases[] = {
        { "float[][3] read   (mat23_weighted)",
          "nestedVSA_r", "nestedVSA::testMat23Weighted" },
        { "float[][3] write  (mat33_writeback)",
          "nestedVSA_w", "nestedVSA::testMat33Writeback" },
        { "float[][][]       (grid_sample)",
          "nestedVSA_g", "nestedVSA::testGridSample" },
    };

    for (const Case &c : cases) {
        Ctl::SimdInterpreter  cpu;
        Ctl::MetalInterpreter gpu;

        std::vector<float> cpuOut, gpuOut;
        try {
            cpuOut = runScalarWrapper(cpu, c.moduleName, c.fnName, samples);
        } catch (const std::exception &e) {
            std::cerr << "testMetalNestedVSArrays: CPU run failed for "
                      << c.label << ": " << e.what() << std::endl;
            throw;
        }
        try {
            gpuOut = runScalarWrapper(gpu, c.moduleName, c.fnName, samples);
        } catch (const std::exception &e) {
            std::cerr << "testMetalNestedVSArrays: GPU run failed for "
                      << c.label << ": " << e.what() << std::endl;
            throw;
        }
        checkParity(c.label, cpuOut, gpuOut);
    }
}
