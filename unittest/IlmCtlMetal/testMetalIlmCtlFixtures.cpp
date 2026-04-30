///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

//
// Load real `.ctl` fixture files from unittest/IlmCtl/ through the Metal
// interpreter and assert that each loads, parses, and compiles without
// error. The bar is "loads + probes a named function" — parity is
// covered separately by testMetalArithmetic / testMetalParity /
// testMetalAcesV2*. Fixtures that exercise CTL stdlib helpers the
// Metal backend has registered (math, lookup/interp, colorspace,
// assert, varying I/O) are in-scope. Fixtures that hit still-deferred
// codegen paths (multi-dim VSArray `.size`, int VSArray params,
// anonymous structs, recursion, the IlmCtl `@error` parser-directive
// harness) are documented inline in `kFixtures` and intentionally
// excluded — each tracks a specific backend gap. This test complements the per-fixture bit-exact parity
// checks by exercising the front-end ↔ Metal codegen path against
// hand-authored CTL source that was never written with the Metal
// backend in mind.
//

#include "testMetalIlmCtlFixtures.h"

#include <CtlMetalDevice.h>
#include <CtlMetalInterpreter.h>

#include <cstddef>
#include <iostream>
#include <stdexcept>

namespace {

struct Fixture
{
    const char *file;
    const char *moduleName;
    // A fully-qualified CTL function name that the fixture must define.
    // After loadFile we call Interpreter::newFunctionCall on this name
    // to verify that parsing actually produced a usable module (rather
    // than silently swallowing a NoImplExc inside the CTL parser).
    // nullptr means "fixture has no functions — just check loadFile".
    const char *probeFunction;
};

const Fixture kFixtures[] =
{
    // Absolute minimum: an empty namespace. Exercises just the module
    // scaffolding (no functions, no statements).
    { "testEmpty.ctl",  "testEmpty",  nullptr            },

    // Placeholder module with a single comment inside a namespace.
    { "test.ctl",       "test",       nullptr            },

    // Two zero-argument functions at module scope (no namespace). Makes
    // sure our FunctionNode kernel wrapper handles zero-parameter
    // helpers cleanly.
    { "testNoName.ctl", "testNoName", "::returnInt"         },

    // Namespace-scoped `const int` plus two zero-arg functions.
    // Exercises module-level static-variable codegen: the `const`
    // lowers to `constant int staticN = 20;` in the MSL header.
    { "testName.ctl",   "testName",   "testName::returnInt" },

    // Small free-standing helpers with control flow (if/else, unary
    // minus, relational ops, call through a local function). No stdlib
    // dependency.
    { "common.ctl",     "common",     "::absolute"          },

    // Remaining IlmCtl fixtures. Bar is still "loads + probes a named
    // function" — parity is covered elsewhere. Scoping: every entry
    // below uses only stdlib helpers the Metal backend has registered
    // (math, lookup/interp, colorspace, assert, varying I/O).
    // `example.ctl` is omitted (defines a recursive `factorial2` which
    // the Metal backend intentionally rejects — MSL forbids recursion;
    // recursion-rejection is covered by the language-rejections test).
    // `testInterpolator.ctl` calls `scatteredDataToGrid3D` — the Metal
    // backend registers it as an MSL no-op stub; the actual RBF solve
    // and its grid-value assertions run on the SIMD sidecar during
    // module-init, which is how the fixture's correctness is enforced.
    { "testArray.ctl",              "testArray",          "testArray::returnIntArray1"      },
    { "testCast.ctl",               "testCast",           "arrayTest::i2b"                  },
    { "testComments.ctl",           "testComments",       "::testComments"                  },
    { "testCppCall.ctl",            "testCppCall",        "cppCall::funcRETi"               },
    { "testCtlVersion.ctl",         "testCtlVersion",     "testCtlVersion::test"            },
    { "testDefaults.ctl",           "testDefaults",       "testDefaults::defaults1"         },
    { "testExamples.ctl",           "testExamples",       "::testExamples"                  },
    { "testExamplesNamespace.ctl",  "testExamplesNamespace", "MyLib::myFunc"                },
    { "testExpr.ctl",               "testExpr",           "testExpr::zero_f"                },
    { "testFunc.ctl",               "testFunc",           "testFunc::returnIntNoArgs"       },
    // The two `scatteredDataToGrid3D` call sites in `test1` / `test2`
    // go through the Metal stdlib no-op stub; all of `runTest`
    // executes on the SIMD sidecar at module-init, including the
    // `equalWithAbsErr` assertions that validate the computed grid.
    { "testInterpolator.ctl",       "testInterpolator",   "testInterpolator::runTest"       },
    // testHugeInit.ctl omitted — requires a companion .ctl generated
    // at test time by the IlmCtl testHugeInit harness; the raw source
    // on its own has unbound `vla` / `vlaSize` / `vla2D` references
    // and cannot parse standalone.

    { "testLiterals.ctl",           "testLiterals",       "::testLiterals"                  },
    { "testLookupTables.ctl",       "testLookupTables",   "::f3"                            },
    { "testLoops.ctl",              "testLoops",          "testLoops::whileLoopWithReturn"  },
    { "testName2.ctl",              "testName2",          "testName::testName"              },
    { "testNameSpace.ctl",          "testNameSpace",      "::test"                          },
    { "testNameSpace2.ctl",         "testNameSpace2",     "testNameSpace2::returnInt2"      },
    { "testParse.ctl",              "testParse",          "testParse::undefinedType"        },
    { "testScope.ctl",              "testScope",          "::testScope2"                    },
    { "testScope2.ctl",             "testScope2",         "testScope2::testScope"           },
    { "testStruct.ctl",             "testStruct",         "testStruct::returnMember"        },
    { "testTypes.ctl",              "testTypes",          "::testBoolRunTimeEvaluation"     },
    { "testVarying.ctl",            "testVarying",        "testVarying::f"                  },
    { "testVaryingLookup.ctl",      "testVaryingLookup",  "::varyingLookup1D"               },
    { "testVaryingReturn.ctl",      "testVaryingReturn",  "testVaryingReturn::testIf"       },
    { "testVSArrays.ctl",           "testVSArrays",       "testVSArrays::testVSA"           },
};

} // anonymous namespace

void
testMetalIlmCtlFixtures()
{
    std::cout << "Testing Metal loader against stdlib-free IlmCtl fixtures"
              << std::endl;

    if (!Ctl::metalBackendAvailable()) {
        std::cout << "  Metal backend unavailable on this host — skipping."
                  << std::endl;
        return;
    }

    const size_t n = sizeof(kFixtures) / sizeof(kFixtures[0]);
    for (size_t i = 0; i < n; ++i) {
        const Fixture &f = kFixtures[i];
        Ctl::MetalInterpreter interp;
        try {
            interp.loadFile(f.file, f.moduleName);
        } catch (const std::exception &e) {
            std::cerr << "testMetalIlmCtlFixtures: load failed for "
                      << f.file << ": " << e.what() << std::endl;
            throw;
        }

        //
        // Verify the fixture actually parsed into a usable module by
        // resolving a known function. This guards against CTL's parser
        // silently turning a backend NoImplExc into "module loaded but
        // empty" — without this probe, a regression in any Phase-2 node
        // would quietly pass as long as the exception came from inside
        // the parser's try/catch.
        //
        if (f.probeFunction) {
            try {
                Ctl::FunctionCallPtr fn =
                    interp.newFunctionCall(f.probeFunction);
                if (!fn) {
                    throw std::runtime_error(
                        std::string("newFunctionCall returned null for ") +
                        f.probeFunction);
                }
            } catch (const std::exception &e) {
                std::cerr << "testMetalIlmCtlFixtures: probe failed for "
                          << f.file << " / " << f.probeFunction << ": "
                          << e.what() << std::endl;
                throw;
            }
        }

        std::cout << "  " << f.file << " — loaded" << std::endl;
    }
}
