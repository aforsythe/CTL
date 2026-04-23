///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

//
// Kernel-reachable `scatteredDataToGrid3D` parity test. Loads
// `scatterKernel.ctl`, calls `testMetalScatterKernel::eval` on both the
// Metal GPU backend and the CPU SIMD reference, compares per-channel
// output ULP.
//
// The fixture exercises the full Metal MSL port of the RBF solve +
// grid eval (see `CtlMetalCodegen.cpp`'s `ctl_stdlib_scatteredDataToGrid3D`).
// Because the call site is inside a varying function, the MSL helper
// runs once per lane at dispatch time instead of being collapsed into
// a module-init value on the CPU sidecar.
//

#include "testMetalScatterKernel.h"

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
#include <string>

namespace {

//
// ULP distance for same-sign finite floats. Diverging NaN / inf maps to
// UINT32_MAX so the caller can clamp to "unbounded" rather than silently
// count it as adjacent.
//
uint32_t
ulpDistance(float a, float b)
{
    if (a == b) return 0u;
    if (std::isnan(a) || std::isnan(b)) return UINT32_MAX;
    if (std::isinf(a) || std::isinf(b)) return UINT32_MAX;
    int32_t ai, bi;
    std::memcpy(&ai, &a, sizeof(ai));
    std::memcpy(&bi, &b, sizeof(bi));
    if ((ai < 0) != (bi < 0)) return UINT32_MAX;
    int32_t d = (ai > bi) ? (ai - bi) : (bi - ai);
    return static_cast<uint32_t>(d);
}

//
// Drive one interpreter through the eval. Returns three N-long output
// vectors packed into a single float[3*N] for convenience.
//
std::vector<float>
runEval(Ctl::Interpreter &interp,
        const std::string &moduleName,
        const std::string &sourcePath,
        const std::vector<float> &rIn,
        const std::vector<float> &gIn,
        const std::vector<float> &bIn)
{
    interp.loadFile(sourcePath, moduleName);
    Ctl::FunctionCallPtr fn =
        interp.newFunctionCall("testMetalScatterKernel::eval");
    assert(fn);
    assert(fn->numInputArgs() == 3);
    assert(fn->numOutputArgs() == 3);

    const size_t N = rIn.size();
    assert(gIn.size() == N && bIn.size() == N);

    //
    // Populate the three input args from the host-side vectors. Every
    // arg is varying float; the Metal backend's FunctionArg::data()
    // exposes a float* we can memcpy into.
    //
    const Ctl::FunctionArgPtr &aR = fn->inputArg(0);
    const Ctl::FunctionArgPtr &aG = fn->inputArg(1);
    const Ctl::FunctionArgPtr &aB = fn->inputArg(2);
    aR->setVarying(true);
    aG->setVarying(true);
    aB->setVarying(true);
    std::memcpy(aR->data(), rIn.data(), N * sizeof(float));
    std::memcpy(aG->data(), gIn.data(), N * sizeof(float));
    std::memcpy(aB->data(), bIn.data(), N * sizeof(float));

    fn->callFunction(N);

    std::vector<float> out(3 * N, 0.0f);
    const Ctl::FunctionArgPtr &oR = fn->outputArg(0);
    const Ctl::FunctionArgPtr &oG = fn->outputArg(1);
    const Ctl::FunctionArgPtr &oB = fn->outputArg(2);
    std::memcpy(out.data() + 0 * N, oR->data(), N * sizeof(float));
    std::memcpy(out.data() + 1 * N, oG->data(), N * sizeof(float));
    std::memcpy(out.data() + 2 * N, oB->data(), N * sizeof(float));
    return out;
}

} // anonymous namespace

void
testMetalScatterKernel()
{
    std::cout << "Testing kernel-reachable scatteredDataToGrid3D parity"
              << std::endl;

    if (!Ctl::metalBackendAvailable()) {
        std::cout << "  Metal backend unavailable on this host — skipping."
                  << std::endl;
        return;
    }

    //
    // Small batch so the 3-deep CG solve (30*n * n^2 FLOPs per channel,
    // three channels, n=5) runs in milliseconds across eight lanes.
    // Input values are irrelevant — `eval` multiplies them by 0 and
    // reads the RBF grid only.
    //
    const size_t N = 8;
    std::vector<float> rIn(N, 0.5f);
    std::vector<float> gIn(N, 0.25f);
    std::vector<float> bIn(N, 0.75f);

    Ctl::MetalInterpreter gpu;
    Ctl::SimdInterpreter  cpu;
    auto gpuOut = runEval(gpu, "testMetalScatterKernel",
                          "scatterKernel.ctl", rIn, gIn, bIn);
    auto cpuOut = runEval(cpu, "testMetalScatterKernel",
                          "scatterKernel.ctl", rIn, gIn, bIn);

    //
    // Report per-channel max ULP and max absolute diff. The RBF solve
    // is intrinsically FP32 here (CPU runs double internally but
    // returns float); expect non-zero but bounded drift.
    //
    static const char *labels[3] = { "rOut", "gOut", "bOut" };
    bool pass = true;
    for (int c = 0; c < 3; ++c) {
        uint32_t maxUlp = 0;
        float    maxAbs = 0.0f;
        for (size_t i = 0; i < N; ++i) {
            float g = gpuOut[c * N + i];
            float cv = cpuOut[c * N + i];
            uint32_t u = ulpDistance(g, cv);
            if (u > maxUlp) maxUlp = u;
            float a = std::fabs(g - cv);
            if (a > maxAbs) maxAbs = a;
        }
        std::cout << "  " << labels[c]
                  << ":  max ULP=" << maxUlp
                  << "  max abs=" << maxAbs
                  << "  GPU[0]=" << gpuOut[c * N + 0]
                  << "  CPU[0]=" << cpuOut[c * N + 0]
                  << std::endl;
        //
        // Threshold: FP32 CG vs FP64 CG on identity data should converge
        // to within a few tens of ULP; budget 1024 ULP as the regression
        // guard so we catch order-of-magnitude drift but absorb the
        // expected few-ULP precision gap.
        //
        if (maxUlp > 1024u && maxAbs > 1e-4f) pass = false;
    }
    if (!pass) {
        std::cerr << "  scatteredDataToGrid3D kernel-reachable parity FAILED"
                  << std::endl;
        std::abort();
    }
    std::cout << "  scatteredDataToGrid3D parity ok" << std::endl;
}
