///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------------
//
//  Tests host-side defaulting of CTL function parameters at the Metal
//  kernel entry point. The SIMD backend resolves a missing-argument
//  call-site by copying the parser-evaluated default static into the
//  arg's register on `setDefaultValue()`. The Metal backend mirrors
//  that via its host-side SIMD sidecar — the same sidecar that
//  evaluates module-scope const initializers whose RHS isn't
//  MSL-constexpr.
//
//-----------------------------------------------------------------------------

#include "testMetalDefaultParams.h"

#include <CtlFunctionCall.h>
#include <CtlMetalDevice.h>
#include <CtlMetalInterpreter.h>

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

//
// Non-varying default: `float x = 2.0`. The host-side `data()` buffer
// for a uniform arg holds exactly one element, so `setDefaultValue()`
// must drop the default into that one slot.
//
void
runUniformDefault()
{
    const char *src =
        "namespace mdp\n"
        "{\n"
        "    void observe(int i, output float out, float x = 2.0)\n"
        "    {\n"
        "        out = x;\n"
        "    }\n"
        "}\n";

    Ctl::MetalInterpreter metal;
    metal.loadModule("mdp", "mdp.ctl", src);

    Ctl::FunctionCallPtr fn = metal.newFunctionCall("mdp::observe");
    assert(fn);
    assert(fn->numInputArgs() == 2);
    assert(fn->numOutputArgs() == 1);

    Ctl::FunctionArgPtr argI = fn->inputArg(0);
    Ctl::FunctionArgPtr argX = fn->inputArg(1);
    Ctl::FunctionArgPtr out  = fn->outputArg(0);

    assert(!argX->isVarying());
    assert(argI->hasDefaultValue() == false);
    assert(argX->hasDefaultValue() == true);

    std::memset(argI->data(), 0, sizeof(int));

    // Explicit value first — makes sure the uniform-input path itself
    // isn't broken in a way that would mask a defaulting bug.
    const float xSet = 1.2345f;
    std::memcpy(argX->data(), &xSet, sizeof(float));
    fn->callFunction(1);
    const float gotSet = *reinterpret_cast<const float *>(out->data());
    assert(gotSet == xSet);

    // Now request the default and confirm the emitted kernel sees 2.0.
    argX->setDefaultValue();
    fn->callFunction(1);
    const float gotDefault =
        *reinterpret_cast<const float *>(out->data());
    if (gotDefault != 2.0f) {
        std::cerr << "testMetalDefaultParams: uniform default observed "
                  << gotDefault << " (expected 2.0)" << std::endl;
        std::abort();
    }
}

//
// Varying default: `varying float x = 2.5`. SIMD fans the uniform
// default across every lane of the register; Metal must do the same so
// every sample index reads the same value from the kernel buffer.
//
void
runVaryingDefault()
{
    const char *src =
        "namespace mdp2\n"
        "{\n"
        "    void observe(output varying float out,\n"
        "                 varying float x = 2.5)\n"
        "    {\n"
        "        out = x;\n"
        "    }\n"
        "}\n";

    Ctl::MetalInterpreter metal;
    metal.loadModule("mdp2", "mdp2.ctl", src);

    Ctl::FunctionCallPtr fn = metal.newFunctionCall("mdp2::observe");
    assert(fn);

    Ctl::FunctionArgPtr argX = fn->inputArg(0);
    Ctl::FunctionArgPtr out  = fn->outputArg(0);
    assert(argX->isVarying());
    assert(argX->hasDefaultValue() == true);

    argX->setDefaultValue();

    const size_t N = 64;
    fn->callFunction(N);

    const float *outData = reinterpret_cast<const float *>(out->data());
    for (size_t i = 0; i < N; ++i) {
        if (outData[i] != 2.5f) {
            std::cerr << "testMetalDefaultParams: varying default lane "
                      << i << " saw " << outData[i] << " (expected 2.5)"
                      << std::endl;
            std::abort();
        }
    }
}

//
// Parameter without a declared default: `hasDefaultValue()` must
// return false, and `setDefaultValue()` must be a no-op (matching the
// SIMD backend). This is the common case — if we returned true here
// the caller would be led to believe the arg is initialized when it
// actually holds whatever bytes the host last wrote.
//
void
runNoDefault()
{
    const char *src =
        "namespace mdp3\n"
        "{\n"
        "    void observe(output float out, float x) { out = x; }\n"
        "}\n";

    Ctl::MetalInterpreter metal;
    metal.loadModule("mdp3", "mdp3.ctl", src);

    Ctl::FunctionCallPtr fn = metal.newFunctionCall("mdp3::observe");
    assert(fn);

    Ctl::FunctionArgPtr argX = fn->inputArg(0);
    assert(argX->hasDefaultValue() == false);

    // setDefaultValue() on a no-default arg is a harmless no-op.
    argX->setDefaultValue();
}

} // anonymous namespace

void
testMetalDefaultParams()
{
    std::cout << "testMetalDefaultParams ..." << std::endl;
    if (!Ctl::metalBackendAvailable()) {
        std::cout << "  Metal backend unavailable on this host — skipping."
                  << std::endl;
        return;
    }

    runUniformDefault();
    runVaryingDefault();
    runNoDefault();

    std::cout << "testMetalDefaultParams ok" << std::endl;
}
