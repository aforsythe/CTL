///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

//
// Verify that the Metal backend rejects CTL constructs it cannot emit as
// MSL with a clean, surfaced NoImplExc rather than producing invalid
// shader source or falling back to an opaque LogicExc deep in codegen.
//
// CTL's parser wraps module loading in a try/catch that captures the
// exception's message but then continues with a null syntax tree — see
// `Parser::parseInput`. A backend that throws mid-parse therefore does
// NOT propagate to `loadModule`'s caller; the module just comes back
// empty. We expose the failure the same way `testMetalIlmCtlFixtures`
// does: probe the fixture's entry point with `newFunctionCall` after
// load. For rejection tests specifically, `newFunctionCall` must throw
// because the symbol never materialized.
//

#include "testMetalLanguageRejections.h"

#include <CtlMetalDevice.h>
#include <CtlMetalInterpreter.h>

#include "testRequire.h"
#include <iostream>
#include <string>

namespace {

bool
loadAndProbeFails(Ctl::MetalInterpreter &interp,
                  const char *moduleName,
                  const char *source,
                  const char *functionName)
{
    try {
        interp.loadModule(moduleName,
                          std::string(moduleName) + ".ctl",
                          source);
    } catch (const std::exception &) {
        return true;
    }

    try {
        Ctl::FunctionCallPtr fn = interp.newFunctionCall(functionName);
        return !fn;
    } catch (const std::exception &) {
        return true;
    }
}

const char *kDirectRecursionSource =
    "namespace rec\n"
    "{\n"
    "    int fact(int n)\n"
    "    {\n"
    "        if (n <= 1)\n"
    "            return 1;\n"
    "        return n * fact(n - 1);\n"
    "    }\n"
    "\n"
    "    void compute(input varying int n,\n"
    "                 output varying int out)\n"
    "    {\n"
    "        out = fact(n);\n"
    "    }\n"
    "}\n";

//
// Probe for file-based fixtures that must fail codegen. Mirrors
// `loadAndProbeFails` but takes a .ctl path and uses `loadFile`, so
// we exercise the same parse/codegen path that `ctlrender-metal`
// runs for user-supplied sources on disk.
//
bool
loadFileAndProbeFails(Ctl::MetalInterpreter &interp,
                      const char *filePath,
                      const char *moduleName,
                      const char *functionName)
{
    try {
        interp.loadFile(filePath, moduleName);
    } catch (const std::exception &) {
        return true;
    }

    try {
        Ctl::FunctionCallPtr fn = interp.newFunctionCall(functionName);
        return !fn;
    } catch (const std::exception &) {
        return true;
    }
}

} // anonymous namespace

void
testMetalLanguageRejections()
{
    std::cout << "Testing Metal language-level rejections" << std::endl;

    if (!Ctl::metalBackendAvailable()) {
        std::cout << "  Metal backend unavailable on this host — skipping."
                  << std::endl;
        return;
    }

    {
        Ctl::MetalInterpreter interp;
        const bool rejected = loadAndProbeFails(
            interp, "rec", kDirectRecursionSource, "rec::compute");
        REQUIRE(rejected && "direct recursion must be rejected");
        std::cout << "  direct self-recursion rejected" << std::endl;
    }

    //
    // File-based companion to the inline direct-recursion case.
    // `unittest/IlmCtl/example.ctl` defines `ilm::factorial2`, a
    // textbook self-recursive factorial; the CMake COPY block for
    // `testMetalIlmCtlFixtures` already stages it into the test CWD.
    // Loading via `loadFile` exercises the same path `ctlrender-metal`
    // takes for user-supplied sources on disk, which is distinct from
    // the string-input `loadModule` path used by the inline case.
    //
    {
        Ctl::MetalInterpreter interp;
        const bool rejected = loadFileAndProbeFails(
            interp, "example.ctl", "example", "ilm::factorial2");
        REQUIRE(rejected &&
               "example.ctl::factorial2 must be rejected (recursive)");
        std::cout << "  example.ctl recursion rejected" << std::endl;
    }
}
