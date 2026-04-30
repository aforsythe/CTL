///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

//
// Direct-construction test for MetalModule + MetalLContext. Bypasses the
// Parser so we can exercise the data-addressing factories before the
// syntax-tree-node factories are implemented.
//

#include "testMetalLContext.h"

#include <CtlMetalAddr.h>
#include <CtlMetalCodegen.h>
#include <CtlMetalDevice.h>
#include <CtlMetalInterpreter.h>
#include <CtlMetalLContext.h>
#include <CtlMetalModule.h>
#include <CtlSymbolTable.h>
#include <CtlType.h>

#include "testRequire.h"
#include <iostream>
#include <sstream>

namespace {

Ctl::MetalDataAddr *
asMetalAddr(const Ctl::AddrPtr &addr)
{
    return dynamic_cast<Ctl::MetalDataAddr *>(addr.pointer());
}

} // anonymous namespace

void
testMetalLContext()
{
    std::cout << "Testing MetalModule + MetalLContext scaffolding"
              << std::endl;

    if (!Ctl::metalBackendAvailable()) {
        std::cout << "  backend unavailable on this host — skipping."
                  << std::endl;
        return;
    }

    Ctl::MetalInterpreter interp;

    Ctl::MetalModule module(interp, "testModule", "testModule.ctl");
    REQUIRE(module.codegen().source().find("#include <metal_stdlib>")
           != std::string::npos);

    Ctl::SymbolTable symtab;
    std::istringstream file("");
    Ctl::MetalLContext lcontext(file, &module, symtab);

    REQUIRE(lcontext.metalModule() == &module);

    Ctl::DataTypePtr floatT = lcontext.newFloatType();
    REQUIRE(floatT);

    lcontext.newStackFrame();

    Ctl::AddrPtr p0 = lcontext.parameterAddr(floatT);
    Ctl::AddrPtr p1 = lcontext.parameterAddr(floatT);
    Ctl::AddrPtr r0 = lcontext.returnValueAddr(floatT);
    Ctl::AddrPtr v0 = lcontext.autoVariableAddr(floatT);
    Ctl::AddrPtr v1 = lcontext.autoVariableAddr(floatT);

    Ctl::MetalDataAddr *mp0 = asMetalAddr(p0);
    Ctl::MetalDataAddr *mp1 = asMetalAddr(p1);
    Ctl::MetalDataAddr *mr0 = asMetalAddr(r0);
    Ctl::MetalDataAddr *mv0 = asMetalAddr(v0);
    Ctl::MetalDataAddr *mv1 = asMetalAddr(v1);
    REQUIRE(mp0 && mp1 && mr0 && mv0 && mv1);

    REQUIRE(mp0->mslName() == "param0");
    REQUIRE(mp1->mslName() == "param1");
    REQUIRE(mr0->mslName() == "ret0");
    REQUIRE(mv0->mslName() == "var0");
    REQUIRE(mv1->mslName() == "var1");

    lcontext.newStackFrame();

    Ctl::AddrPtr p0b = lcontext.parameterAddr(floatT);
    Ctl::MetalDataAddr *mp0b = asMetalAddr(p0b);
    REQUIRE(mp0b && mp0b->mslName() == "param0");

    std::cout << "ok" << std::endl;
}
