///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

//
// End-to-end verification that the Metal backend's assert(cond) path
// surfaces a failing condition as a host-side `Iex::BaseExc`-derived
// exception whose message contains "CTL assertion failed.", matching
// the CPU backend's exception surface (see
// `lib/IlmCtlSimd/CtlSimdStdLibAssert.cpp::throwAssertFailed`).
//
// Caveat on the exact message: the CPU backend's instruction loop in
// `CtlSimdInst.cpp` catches the raw `LogicExc("CTL assertion failed.")`
// in its `catch (Iex::BaseExc &e)` handler, rewraps it with a
// `"\n<file>.ctl:<line>: "` prefix via `REPLACE_EXC`, then re-throws
// the sliced `BaseExc`. The GPU path has no file/line context at the
// point the error flag is observed (the OR-atomic loses thread
// identity), so it throws `LogicExc("CTL assertion failed.")` with the
// raw message. This test therefore verifies substring containment, not
// byte-equality, and catches `BaseExc` to cover both paths.
//
// The mechanism exercised on the GPU: the kernel's `ctl_stdlib_assert`
// helper observes `!cond` and atomically ORs bit 0 into the trailing
// `device atomic_uint* __ctl_err_flag` buffer. After `dispatch`, the
// host reads back a nonzero flag and `MetalFunctionCall::callFunction`
// throws a `LogicExc` that propagates through the common `BaseExc`
// surface.
//

#include "testMetalAssertFailure.h"

#include <CtlMetalDevice.h>
#include <CtlMetalInterpreter.h>
#include <CtlSimdInterpreter.h>

#include <Iex.h>

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

namespace {

//
// Assert an always-false literal. Exercises the path where every kernel
// thread sets the error bit; the host must see a nonzero flag and throw.
//
const char *kAssertFalseLiteralSource =
    "namespace stdassertlit\n"
    "{\n"
    "    void compute(input varying float a,\n"
    "                 output varying float out)\n"
    "    {\n"
    "        assert(false);\n"
    "        out = a;\n"
    "    }\n"
    "}\n";

//
// Assert a data-dependent condition that is false for every sample.
// Exercises the varying-input path: the condition is computed per-thread
// and the OR-atomic must see a failing thread regardless of which one
// reaches the helper first.
//
const char *kAssertFalseVaryingSource =
    "namespace stdassertvar\n"
    "{\n"
    "    void compute(input varying float a,\n"
    "                 output varying float out)\n"
    "    {\n"
    "        assert(a != a);\n"
    "        out = a;\n"
    "    }\n"
    "}\n";

template <class Interp>
Ctl::FunctionCallPtr
loadAndProbe(Interp &interp,
             const char *moduleName,
             const char *source,
             const char *functionName)
{
    interp.loadModule(moduleName,
                      std::string(moduleName) + ".ctl",
                      source);
    Ctl::FunctionCallPtr fn = interp.newFunctionCall(functionName);
    assert(fn);
    return fn;
}

void
fillFloatInput(Ctl::FunctionArgPtr arg, size_t n, float start, float step)
{
    for (size_t s = 0; s < n; ++s) {
        float v = start + static_cast<float>(s) * step;
        std::memcpy(arg->data() + s * sizeof(float), &v, sizeof(v));
    }
}

//
// Run `functionName` from `source` on the Metal backend with `n` samples
// and verify that `callFunction` throws `Iex::LogicExc` whose message
// matches the CPU backend's exact string ("CTL assertion failed.").
//
void
expectAssertThrow(const char *label,
                  const char *moduleName,
                  const char *source,
                  const char *functionName,
                  size_t n)
{
    Ctl::MetalInterpreter interp;
    Ctl::FunctionCallPtr fn =
        loadAndProbe(interp, moduleName, source, functionName);

    Ctl::FunctionArgPtr aArg = fn->inputArg(0);
    Ctl::FunctionArgPtr out  = fn->outputArg(0);
    assert(aArg->isVarying());
    assert(out->isVarying());

    fillFloatInput(aArg, n, 0.5f, 0.125f);

    bool threw = false;
    std::string msg;
    try {
        fn->callFunction(n);
    } catch (const IEX_NAMESPACE::BaseExc &e) {
        threw = true;
        msg = e.what();
    } catch (const std::exception &e) {
        std::cerr << "testMetalAssertFailure: " << label
                  << " threw unexpected exception type: "
                  << e.what() << std::endl;
        std::abort();
    }

    if (!threw) {
        std::cerr << "testMetalAssertFailure: " << label
                  << " did not throw — error flag path is not wired up."
                  << std::endl;
        std::abort();
    }
    if (msg.find("CTL assertion failed.") == std::string::npos) {
        std::cerr << "testMetalAssertFailure: " << label
                  << " threw BaseExc with unexpected message: \""
                  << msg << "\" (expected substring "
                     "\"CTL assertion failed.\")" << std::endl;
        std::abort();
    }

    std::cout << "  " << label
              << " — " << n << " samples, threw BaseExc containing "
                 "\"CTL assertion failed.\" as expected" << std::endl;
}

//
// Sanity cross-check: the CPU backend raises the same exception type
// and message for the same program. Any divergence here would indicate
// the Metal backend's exception surface has drifted.
//
void
expectCpuAssertThrow(const char *label,
                     const char *moduleName,
                     const char *source,
                     const char *functionName,
                     size_t n)
{
    Ctl::SimdInterpreter interp;
    Ctl::FunctionCallPtr fn =
        loadAndProbe(interp, moduleName, source, functionName);

    Ctl::FunctionArgPtr aArg = fn->inputArg(0);
    Ctl::FunctionArgPtr out  = fn->outputArg(0);

    fillFloatInput(aArg, n, 0.5f, 0.125f);

    bool threw = false;
    std::string msg;
    try {
        fn->callFunction(n);
    } catch (const IEX_NAMESPACE::BaseExc &e) {
        threw = true;
        msg = e.what();
    }

    if (!threw ||
        msg.find("CTL assertion failed.") == std::string::npos) {
        std::cerr << "testMetalAssertFailure: CPU backend control case "
                  << label << " did not behave as expected (threw="
                  << threw << ", msg=\"" << msg << "\")" << std::endl;
        std::abort();
    }
    std::cout << "  " << label
              << " (CPU control) — threw BaseExc containing "
                 "\"CTL assertion failed.\" as expected" << std::endl;
}

} // anonymous namespace

void
testMetalAssertFailure()
{
    std::cout << "Testing Metal assert(false) error-flag plumbing"
              << std::endl;

    if (!Ctl::metalBackendAvailable()) {
        std::cout << "  Metal backend unavailable on this host — skipping."
                  << std::endl;
        return;
    }

    const size_t n = 64;

    expectCpuAssertThrow("assert(false) literal",
                         "stdassertlit_cpu",
                         kAssertFalseLiteralSource,
                         "stdassertlit::compute", n);
    expectAssertThrow("assert(false) literal",
                      "stdassertlit",
                      kAssertFalseLiteralSource,
                      "stdassertlit::compute", n);

    expectCpuAssertThrow("assert(a != a) varying",
                         "stdassertvar_cpu",
                         kAssertFalseVaryingSource,
                         "stdassertvar::compute", n);
    expectAssertThrow("assert(a != a) varying",
                      "stdassertvar",
                      kAssertFalseVaryingSource,
                      "stdassertvar::compute", n);
}
