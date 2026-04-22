///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#include "testMetalCodegen.h"

#include <CtlMetalCodegen.h>

#include <cassert>
#include <iostream>
#include <string>

namespace {

void
assertEqual(const std::string &actual,
            const std::string &expected,
            const char *label)
{
    if (actual != expected) {
        std::cerr << "testMetalCodegen: " << label
                  << " mismatch.\n--- expected ---\n"
                  << expected
                  << "\n--- actual ---\n"
                  << actual
                  << "\n--- end ---\n";
        assert(false);
    }
}

void
testEmptyPreamble()
{
    Ctl::MetalCodegen gen;
    const std::string expected =
        "#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "#pragma STDC FP_CONTRACT OFF\n"
        "\n";
    assertEqual(gen.source(), expected, "empty preamble");
}

void
testWritelnIndent()
{
    Ctl::MetalCodegen gen;
    gen.writeln("void f()");
    gen.writeln("{");
    gen.indent();
    gen.writeln("int x = 1;");
    gen.outdent();
    gen.writeln("}");

    const std::string expectedBody =
        "void f()\n"
        "{\n"
        "    int x = 1;\n"
        "}\n";
    assertEqual(gen.bodyBuffer(), expectedBody, "writeln + indent");
}

void
testHeaderAndBody()
{
    Ctl::MetalCodegen gen;
    gen.setSection(Ctl::MetalCodegen::Header);
    gen.writeln("struct Pixel { float r; float g; float b; };");
    gen.setSection(Ctl::MetalCodegen::Body);
    gen.writeln("void f() {}");

    const std::string expected =
        "#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "#pragma STDC FP_CONTRACT OFF\n"
        "\n"
        "struct Pixel { float r; float g; float b; };\n"
        "void f() {}\n";
    assertEqual(gen.source(), expected, "header + body");
}

void
testKernelScaffold()
{
    Ctl::MetalCodegen gen;
    gen.beginKernel("hello");
    gen.declareKernelBuffer("float", "out_x");
    gen.endKernel();
    gen.indent();
    gen.writeln("out_x[tid] = 1.0f;");
    gen.outdent();
    gen.writeln("}");

    const std::string expectedBody =
        "kernel void hello(uint tid [[thread_position_in_grid]],\n"
        "                      device float *out_x [[buffer(0)]])\n"
        "{\n"
        "    out_x[tid] = 1.0f;\n"
        "}\n";
    assertEqual(gen.bodyBuffer(), expectedBody, "kernel scaffold");
}

} // anonymous namespace

void
testMetalCodegen()
{
    std::cout << "testMetalCodegen ..." << std::endl;
    testEmptyPreamble();
    testWritelnIndent();
    testHeaderAndBody();
    testKernelScaffold();
    std::cout << "testMetalCodegen ok" << std::endl;
}
