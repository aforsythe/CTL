///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#include "testMetalCodegen.h"

#include <CtlMetalCodegen.h>

#include "testRequire.h"
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
        REQUIRE(false);
    }
}

void
assertStartsWith(const std::string &actual,
                 const std::string &prefix,
                 const char *label)
{
    if (actual.compare(0, prefix.size(), prefix) != 0) {
        std::cerr << "testMetalCodegen: " << label
                  << " prefix mismatch.\n--- expected prefix ---\n"
                  << prefix
                  << "\n--- actual head ---\n"
                  << actual.substr(0, prefix.size() + 64)
                  << "\n--- end ---\n";
        REQUIRE(false);
    }
}

void
testEmptyPreamble()
{
    Ctl::MetalCodegen gen;
    // Preamble grows over time as stdlib helpers are added; pin only
    // the leading invariants (include, namespace, FP_CONTRACT).
    const std::string prefix =
        "#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "#pragma STDC FP_CONTRACT OFF\n";
    assertStartsWith(gen.source(), prefix, "empty preamble");
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

    const std::string source = gen.source();
    REQUIRE(source.find("struct Pixel { float r; float g; float b; };\n")
            != std::string::npos);
    REQUIRE(source.find("void f() {}\n") != std::string::npos);
    // Header section content must appear before the body section.
    REQUIRE(source.find("struct Pixel") < source.find("void f()"));
}

void
testKernelScaffold()
{
    // beginKernel/.../finishKernel writes into a per-kernel buffer that
    // sourceForKernel() concatenates with the shared preamble.  The
    // assembled source must contain the kernel signature, declared
    // buffer binding, body line, and closing brace, in that order.
    Ctl::MetalCodegen gen;
    gen.beginKernel("hello");
    gen.declareKernelBuffer("float", "out_x");
    gen.endKernel();
    gen.indent();
    gen.writeln("out_x[tid] = 1.0f;");
    gen.outdent();
    gen.writeln("}");
    gen.finishKernel();

    const std::string source = gen.sourceForKernel("hello");
    REQUIRE(source.find("kernel void hello(uint tid [[thread_position_in_grid]],")
            != std::string::npos);
    REQUIRE(source.find("device float *out_x [[buffer(0)]]")
            != std::string::npos);
    REQUIRE(source.find("    out_x[tid] = 1.0f;\n") != std::string::npos);
    REQUIRE(source.rfind("}\n") != std::string::npos);
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
