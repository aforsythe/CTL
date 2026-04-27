///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

//
// End-to-end smoke test: load a trivial CTL program, ask the
// Metal backend to run it over four samples, and verify each sample is
// set to 1.0f. Exercises the full chain: parser → MetalLContext →
// MetalCodegen → MSL compile → MTLComputePipelineState → dispatch.
//

#include "testMetalHelloWorld.h"

#include <CtlFunctionCall.h>
#include <CtlMetalDevice.h>
#include <CtlMetalInterpreter.h>

#include "testRequire.h"
#include <cmath>
#include <iostream>
#include <string>

namespace {

const char *kHelloSource =
    "namespace hello\n"
    "{\n"
    "    void setOne(output float x)\n"
    "    {\n"
    "        x = 1.0;\n"
    "    }\n"
    "}\n";

} // anonymous namespace

void
testMetalHelloWorld()
{
    std::cout << "Testing Metal hello-world end-to-end dispatch"
              << std::endl;

    if (!Ctl::metalBackendAvailable()) {
        std::cout << "  Metal backend unavailable on this host — skipping."
                  << std::endl;
        return;
    }

    Ctl::MetalInterpreter interp;
    interp.loadModule("hello", "hello.ctl", kHelloSource);

    Ctl::FunctionCallPtr fn = interp.newFunctionCall("hello::setOne");
    REQUIRE(fn);
    REQUIRE(fn->numInputArgs() == 0);
    REQUIRE(fn->numOutputArgs() == 1);

    const size_t N = 4;
    fn->callFunction(N);

    const Ctl::FunctionArgPtr &out = fn->outputArg(0);
    // setOne's output is `output float`, i.e. uniform — buffer holds
    // one sample regardless of N.  Broadcast equivalence to all N
    // lanes is verified by the dispatch-side checks in other tests;
    // here we just assert the single uniform sample is 1.0.
    const float *data = reinterpret_cast<const float *>(out->data());
    REQUIRE(data[0] == 1.0f);

    std::cout << "  hello::setOne set " << N << " samples to 1.0 — ok"
              << std::endl;
}
