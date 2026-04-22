///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

//
// Metal halfExpLog MTLBuffer hoisting — in-tree workload.
//
// The three halfExpLog tables (halfLog10Table 65536 uint, halfLogTable
// 65536 uint, halfExpTable ~113 k ushort; ~739 KB combined) used to be
// emitted inline as `constant` arrays in every module's MSL source. We
// hoisted them to persistent device-const MTLBuffers bound on three
// dedicated kernel-buffer slots. Every user helper unconditionally
// takes three trailing `device const` pointer args so the three
// buffers can be forwarded through arbitrary call graphs.
//
// This test exercises the full plumbing:
//
//   1. A three-level call graph (`top` -> `mid` -> `leaf`) where the
//      tables are only read in the leaf, so the forwarding through
//      two user-to-user call sites must work or the MSL won't link.
//   2. All five stdlib entry points (`exp_h`, `log_h`, `log10_h`,
//      `pow10_h`, `pow_h`) are reached through `leaf`.
//   3. The program is dispatched **three times** against the same
//      pipeline. On first dispatch the three MTLBuffers are allocated
//      and uploaded; on the second and third they must be reused from
//      the pipeline's per-pointer cache. We re-verify parity against
//      the SIMD backend on every dispatch so a broken cache (stale
//      data, zeroed buffer, etc.) would show up as a mismatch.
//   4. We also instantiate a second MetalInterpreter with the same
//      module source to confirm each pipeline owns its own cache —
//      the buffers are keyed on hostData pointer identity, which is
//      stable across interpreters because `halfLog10Table()` /
//      `halfLogTable()` / `halfExpTable()` return the same addresses
//      process-wide.
//
// Target is 0 ULP. All helpers are table-lookup + bit-cast only; the
// CPU and GPU read the same bytes through identical index arithmetic.
//

#include "testMetalHalfExpLog.h"

#include <CtlFunctionCall.h>
#include <CtlMetalDevice.h>
#include <CtlMetalInterpreter.h>
#include <CtlSimdInterpreter.h>

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

//
// Load the halfExpLog.ctl fixture from the binary dir. CMake copies it
// next to the test binary via `file(COPY ... DESTINATION)` so we can
// open it relative to the CWD ctest uses.
//
std::string
readFixture()
{
    std::ifstream in("halfExpLog.ctl");
    if (!in.is_open()) {
        std::cerr << "testMetalHalfExpLog: cannot open halfExpLog.ctl"
                  << std::endl;
        std::abort();
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool
bitEqual(float a, float b)
{
    std::uint32_t ua, ub;
    std::memcpy(&ua, &a, sizeof(ua));
    std::memcpy(&ub, &b, sizeof(ub));
    return ua == ub;
}

template <class Interp>
std::vector<float>
runProgram(Interp &interp,
           const std::string &source,
           const std::vector<float> &aIn,
           const std::vector<float> &bIn,
           bool alreadyLoaded = false)
{
    if (!alreadyLoaded)
        interp.loadModule("halfExpLogTest", "halfExpLog.ctl", source);
    Ctl::FunctionCallPtr fn = interp.newFunctionCall("halfExpLogTest::top");
    assert(fn);
    assert(fn->numInputArgs() == 2);
    assert(fn->numOutputArgs() == 1);

    const size_t n = aIn.size();

    Ctl::FunctionArgPtr aArg = fn->inputArg(0);
    Ctl::FunctionArgPtr bArg = fn->inputArg(1);
    Ctl::FunctionArgPtr out  = fn->outputArg(0);

    for (size_t s = 0; s < n; ++s) {
        std::memcpy(aArg->data() + s * sizeof(float),
                    &aIn[s], sizeof(float));
        std::memcpy(bArg->data() + s * sizeof(float),
                    &bIn[s], sizeof(float));
    }

    fn->callFunction(n);

    std::vector<float> result(n);
    std::memcpy(result.data(), out->data(), n * sizeof(float));
    return result;
}

//
// Dispatch the top-level kernel `nDispatches` times against the same
// interpreter. First dispatch populates the per-pipeline MTLBuffer
// cache; subsequent dispatches must reuse it (no re-upload) and the
// output must still match the CPU byte-for-byte.
//
void
runCachedDispatches(Ctl::MetalInterpreter &gpu,
                    Ctl::SimdInterpreter  &cpu,
                    const std::string     &source,
                    const std::vector<float> &a,
                    const std::vector<float> &b,
                    size_t nDispatches,
                    bool   moduleAlreadyLoaded)
{
    auto cpuOut = runProgram(cpu, source, a, b, moduleAlreadyLoaded);

    for (size_t d = 0; d < nDispatches; ++d) {
        auto gpuOut = runProgram(gpu, source, a, b,
                                 /*alreadyLoaded*/ d > 0 ||
                                     moduleAlreadyLoaded);
        assert(gpuOut.size() == cpuOut.size());
        for (size_t i = 0; i < gpuOut.size(); ++i) {
            if (!bitEqual(gpuOut[i], cpuOut[i])) {
                std::uint32_t ug, uc;
                std::memcpy(&ug, &gpuOut[i], sizeof(ug));
                std::memcpy(&uc, &cpuOut[i], sizeof(uc));
                std::cerr << "testMetalHalfExpLog: parity failure on "
                             "dispatch " << d << " at sample " << i
                          << ": gpu=" << gpuOut[i]
                          << " (0x" << std::hex << ug << ")"
                          << " cpu=" << cpuOut[i]
                          << " (0x" << uc << ")"
                          << std::dec
                          << " (a=" << a[i] << " b=" << b[i] << ")"
                          << std::endl;
                std::abort();
            }
        }
        std::cout << "  dispatch " << d << " — " << gpuOut.size()
                  << " samples parity ok" << std::endl;
    }
}

} // anonymous namespace

void
testMetalHalfExpLog()
{
    std::cout << "Testing Metal halfExpLog MTLBuffer hoisting (5 helpers, "
                 "3 dispatches, 2 pipelines)"
              << std::endl;

    if (!Ctl::metalBackendAvailable()) {
        std::cout << "  Metal backend unavailable on this host — skipping."
                  << std::endl;
        return;
    }

    const std::string source = readFixture();

    //
    // Inputs span the four table domains:
    //   * exp_h:   a  in [-16, 11]
    //   * pow10_h: a  in [-4.8, 4.8] (fed as a * 0.25 -> [-1.2, 1.2])
    //   * log_h / log10_h: half(fabs(x)+1) -> [1, 17+1]
    //   * pow_h:   exp_h(y * log_h(x)), y = b * 0.125 -> [-2, 2]
    //
    std::mt19937 rng(0xCAC4E);
    std::uniform_real_distribution<float> aDist(-16.0f, 11.0f);
    std::uniform_real_distribution<float> bDist(-16.0f, 16.0f);

    const size_t n = 2048;
    std::vector<float> a(n);
    std::vector<float> b(n);
    for (size_t i = 0; i < n; ++i) {
        a[i] = aDist(rng);
        b[i] = bDist(rng);
    }

    //
    // Pipeline #1: 3 dispatches against the same MetalInterpreter so the
    // per-pipeline persistent buffer cache gets a hit on dispatches 2+3.
    //
    {
        Ctl::MetalInterpreter gpu;
        Ctl::SimdInterpreter  cpu;
        runCachedDispatches(gpu, cpu, source, a, b,
                            /*nDispatches*/ 3,
                            /*moduleAlreadyLoaded*/ false);
    }

    //
    // Pipeline #2: fresh MetalInterpreter -> fresh pipeline -> fresh
    // cache. Buffers are keyed on hostData pointer identity, which is
    // stable across interpreters (`halfLog10Table()` et al. return the
    // same address process-wide), so this still reuses the tables'
    // byte contents but via a freshly-allocated MTLBuffer. Output must
    // still match the SIMD backend byte-for-byte.
    //
    {
        Ctl::MetalInterpreter gpu;
        Ctl::SimdInterpreter  cpu;
        runCachedDispatches(gpu, cpu, source, a, b,
                            /*nDispatches*/ 2,
                            /*moduleAlreadyLoaded*/ false);
    }

    std::cout << "  testMetalHalfExpLog ok" << std::endl;
}
