///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------------
//
//  End-to-end Metal dispatch test using the hello-world program:
//
//      void f(output float x) { x = 1.0; }
//
//  We emit the equivalent MSL via MetalCodegen, compile + dispatch it, and
//  verify the output buffer contents. The MSL emission path bypasses the
//  CTL parser for now; wiring the parser to drive MetalCodegen is a
//  later-phase task.
//
//-----------------------------------------------------------------------------

#include "testMetalDispatch.h"

#include <CtlMetalCodegen.h>
#include <CtlMetalDevice.h>
#include <CtlMetalDispatch.h>

#include <Iex.h>

#include "testRequire.h"
#include <iostream>
#include <vector>

namespace {

void
runConstantOutputKernel()
{
    // Emit: for each thread, write 1.0f to its output slot.
    Ctl::MetalCodegen gen;
    gen.beginKernel("hello");
    gen.declareKernelBuffer("float", "out_x");
    gen.endKernel();
    gen.indent();
    gen.writeln("out_x[tid] = 1.0f;");
    gen.outdent();
    gen.writeln("}");

    const std::string src = gen.source();

    Ctl::MetalPipeline pipe(src, "hello");
    const size_t numSamples = 4;
    std::vector<float> out(numSamples, -1.0f);
    pipe.dispatch(out.data(), sizeof(float), numSamples);

    for (size_t i = 0; i < numSamples; ++i) {
        if (out[i] != 1.0f) {
            std::cerr << "testMetalDispatch: out[" << i << "] = "
                      << out[i] << ", expected 1.0" << std::endl;
            REQUIRE(false);
        }
    }
}

} // anonymous namespace

void
testMetalDispatch()
{
    std::cout << "testMetalDispatch ..." << std::endl;
    if (!Ctl::metalBackendAvailable()) {
        std::cout << "  Metal backend unavailable on this host — skipping."
                  << std::endl;
        return;
    }
    std::cout << "  device: " << Ctl::metalDeviceName() << std::endl;
    runConstantOutputKernel();
    std::cout << "testMetalDispatch ok" << std::endl;
}
