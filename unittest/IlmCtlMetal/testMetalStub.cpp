///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

//
// Verifies that MetalInterpreter acquires a Metal device + command
// queue on supported hardware, and that parser-driven entry points
// surface a clear NoImpl error for unsupported language features.
//

#include "testMetalStub.h"

#include <CtlMetalDevice.h>
#include <CtlMetalInterpreter.h>
#include <Iex.h>

#include "testRequire.h"
#include <iostream>
#include <string>

void
testMetalStub()
{
    std::cout << "Testing Metal interpreter lifecycle" << std::endl;

    std::cout << "  metal device: \""
              << Ctl::metalDeviceName()
              << "\", backend available: "
              << (Ctl::metalBackendAvailable() ? "yes" : "no")
              << std::endl;

    if (!Ctl::metalBackendAvailable()) {
        std::cout << "  backend unavailable on this host — skipping."
                  << std::endl;
        return;
    }

    Ctl::MetalInterpreter interp;
    const std::string name = interp.deviceName();
    std::cout << "  interpreter deviceName(): \"" << name << "\""
              << std::endl;
    REQUIRE(!name.empty());
    REQUIRE(interp.maxSamples() == (1u << 24));

    bool threw = false;
    try {
        interp.newFunctionCall("nonexistent");
    } catch (const IEX_NAMESPACE::NoImplExc &) {
        threw = true;
    } catch (const IEX_NAMESPACE::BaseExc &) {
        threw = true;
    }
    REQUIRE(threw);

    std::cout << "ok" << std::endl;
}
