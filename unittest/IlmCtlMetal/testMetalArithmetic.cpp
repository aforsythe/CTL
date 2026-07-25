///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

//
// Parity test: exercise literals, unary and binary arithmetic,
// implicit int→float conversion, comparison operators, and native MSL
// control flow (if/else, while). For each CTL program we emit, we also
// run the SIMD backend on the same input and assert bit-for-bit output
// agreement across a deterministic 1024-sample sweep.
//

#include "testMetalArithmetic.h"

#include <CtlFunctionCall.h>
#include <CtlMetalDevice.h>
#include <CtlMetalInterpreter.h>
#include <CtlSimdInterpreter.h>

#include "testRequire.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

struct Sample
{
    float a;
    float b;
    int   i;
};

std::vector<Sample>
makeSamples(size_t n)
{
    std::mt19937 rng(0xC71F00D);
    std::uniform_real_distribution<float> floats(-16.0f, 16.0f);
    std::uniform_int_distribution<int>    ints(-32, 32);
    std::vector<Sample> out(n);
    for (size_t i = 0; i < n; ++i) {
        out[i].a = floats(rng);
        out[i].b = floats(rng);
        out[i].i = ints(rng);
    }
    return out;
}

// Run `functionName` from the given CTL source on `interp`, copying the
// same input samples into the three input args (a, b, i) and recording
// the single float output. Generic over the interpreter subclass.
template <class Interp>
std::vector<float>
runProgram(Interp &interp,
           const char *moduleName,
           const char *source,
           const char *functionName,
           const std::vector<Sample> &samples)
{
    interp.loadModule(moduleName, std::string(moduleName) + ".ctl", source);
    Ctl::FunctionCallPtr fn = interp.newFunctionCall(functionName);
    REQUIRE(fn);
    REQUIRE(fn->numInputArgs() == 3);
    REQUIRE(fn->numOutputArgs() == 1);

    const size_t n = samples.size();

    Ctl::FunctionArgPtr aArg = fn->inputArg(0);
    Ctl::FunctionArgPtr bArg = fn->inputArg(1);
    Ctl::FunctionArgPtr iArg = fn->inputArg(2);
    Ctl::FunctionArgPtr out  = fn->outputArg(0);

    REQUIRE(aArg->isVarying());
    REQUIRE(bArg->isVarying());
    REQUIRE(iArg->isVarying());
    REQUIRE(out->isVarying());

    for (size_t s = 0; s < n; ++s) {
        std::memcpy(aArg->data() + s * sizeof(float),
                    &samples[s].a, sizeof(float));
        std::memcpy(bArg->data() + s * sizeof(float),
                    &samples[s].b, sizeof(float));
        std::memcpy(iArg->data() + s * sizeof(int),
                    &samples[s].i, sizeof(int));
    }

    fn->callFunction(n);

    std::vector<float> result(n);
    std::memcpy(result.data(), out->data(), n * sizeof(float));
    return result;
}

bool
bitEqual(float a, float b)
{
    std::uint32_t ua, ub;
    std::memcpy(&ua, &a, sizeof(ua));
    std::memcpy(&ub, &b, sizeof(ub));
    return ua == ub;
}

//
// Signed ULP distance between two floats sharing the same sign and both
// finite. For NaN / Inf mixes we fall back to a sentinel so the caller
// can treat them as "not ULP-comparable" rather than lying.
//
int64_t
ulpDistance(float a, float b)
{
    if (std::isnan(a) || std::isnan(b))
        return (a == b) ? 0 : INT64_MAX;
    if (std::isinf(a) || std::isinf(b))
        return (a == b) ? 0 : INT64_MAX;
    std::int32_t ia, ib;
    std::memcpy(&ia, &a, sizeof(ia));
    std::memcpy(&ib, &b, sizeof(ib));
    if (ia < 0) ia = INT32_MIN - ia;
    if (ib < 0) ib = INT32_MIN - ib;
    return std::llabs(static_cast<int64_t>(ia) - static_cast<int64_t>(ib));
}

void
checkParity(const char *label,
            const std::vector<float> &gpu,
            const std::vector<float> &cpu,
            const std::vector<Sample> &samples)
{
    REQUIRE(gpu.size() == cpu.size());
    for (size_t i = 0; i < gpu.size(); ++i) {
        if (!bitEqual(gpu[i], cpu[i])) {
            std::uint32_t ug, uc;
            std::memcpy(&ug, &gpu[i], sizeof(ug));
            std::memcpy(&uc, &cpu[i], sizeof(uc));
            std::cerr << "testMetalArithmetic: " << label
                      << " parity failure at sample " << i
                      << ": gpu=" << gpu[i] << " (0x" << std::hex << ug
                      << ") cpu=" << cpu[i] << " (0x" << uc
                      << std::dec << ") ulp=" << ulpDistance(gpu[i], cpu[i])
                      << " (input: a=" << samples[i].a
                      << " b=" << samples[i].b
                      << " i=" << samples[i].i << ")" << std::endl;
            std::abort();
        }
    }
}

void
runFixture(const char *label,
           const char *moduleName,
           const char *source,
           const char *functionName,
           const std::vector<Sample> &samples)
{
    Ctl::MetalInterpreter gpu;
    Ctl::SimdInterpreter  cpu;
    auto gpuOut = runProgram(gpu, moduleName, source, functionName, samples);
    auto cpuOut = runProgram(cpu, moduleName, source, functionName, samples);
    checkParity(label, gpuOut, cpuOut, samples);
    std::cout << "  " << label << " — " << samples.size()
              << " samples parity ok" << std::endl;
}

//
// Measurement-mode fixture: reports per-sample ULP divergence without
// aborting. Used when first baselining a function to determine the
// threshold that will then be locked in with `ulpBoundedFixture`.
//
void
measureFixture(const char *label,
               const char *moduleName,
               const char *source,
               const char *functionName,
               const std::vector<Sample> &samples)
{
    Ctl::MetalInterpreter gpu;
    Ctl::SimdInterpreter  cpu;
    auto gpuOut = runProgram(gpu, moduleName, source, functionName, samples);
    auto cpuOut = runProgram(cpu, moduleName, source, functionName, samples);

    int64_t maxUlp = 0;
    size_t  diverged = 0;
    size_t  unrepresentable = 0;
    for (size_t i = 0; i < gpuOut.size(); ++i) {
        int64_t d = ulpDistance(gpuOut[i], cpuOut[i]);
        if (d == INT64_MAX) {
            ++unrepresentable;
            continue;
        }
        if (d != 0) ++diverged;
        if (d > maxUlp) maxUlp = d;
    }
    std::cout << "  MEASURE " << label << " — " << samples.size()
              << " samples, max ULP=" << maxUlp
              << ", diverged=" << diverged
              << "/" << samples.size();
    if (unrepresentable)
        std::cout << ", unrepresentable=" << unrepresentable;
    std::cout << std::endl;
}

//
// Threshold-gated ULP parity fixture: runs both backends, computes
// per-sample ULP distance, asserts max(ULP) <= `maxUlpAllowed`. Used
// for stdlib functions that cannot reach 0-ULP under
// `metal::precise::` (see `lib/IlmCtlMetal/PRECISION.md`). The
// threshold is an upper bound — tightening it is always allowed,
// exceeding it is a CI failure.
//
void
ulpBoundedFixture(const char *label,
                  const char *moduleName,
                  const char *source,
                  const char *functionName,
                  const std::vector<Sample> &samples,
                  int64_t maxUlpAllowed)
{
    Ctl::MetalInterpreter gpu;
    Ctl::SimdInterpreter  cpu;
    auto gpuOut = runProgram(gpu, moduleName, source, functionName, samples);
    auto cpuOut = runProgram(cpu, moduleName, source, functionName, samples);

    int64_t maxUlp = 0;
    size_t  diverged = 0;
    size_t  unrepresentable = 0;
    for (size_t i = 0; i < gpuOut.size(); ++i) {
        int64_t d = ulpDistance(gpuOut[i], cpuOut[i]);
        if (d == INT64_MAX) {
            ++unrepresentable;
            continue;
        }
        if (d != 0) ++diverged;
        if (d > maxUlp) maxUlp = d;
    }
    std::cout << "  " << label << " — " << samples.size()
              << " samples, max ULP=" << maxUlp
              << " (threshold " << maxUlpAllowed << ")"
              << ", diverged=" << diverged
              << "/" << samples.size();
    if (unrepresentable)
        std::cout << ", unrepresentable=" << unrepresentable;
    std::cout << std::endl;
    if (maxUlp > maxUlpAllowed) {
        std::cerr << "testMetalArithmetic: " << label
                  << " exceeded ULP threshold " << maxUlpAllowed
                  << " (measured " << maxUlp << "). See PRECISION.md."
                  << std::endl;
        std::abort();
    }
    if (unrepresentable > 0) {
        std::cerr << "testMetalArithmetic: " << label
                  << " produced " << unrepresentable
                  << " samples that are not ULP-comparable "
                  << "(NaN/Inf disagreement); failing." << std::endl;
        std::abort();
    }
}

const char *kArithSource =
    "namespace arith\n"
    "{\n"
    "    void scalar(input varying float a,\n"
    "                input varying float b,\n"
    "                input varying int i,\n"
    "                output varying float out)\n"
    "    {\n"
    "        float x = a * 2.0 + b;\n"
    "        x = x - b / 4.0;\n"
    "        x = -x + 1.5 * a;\n"
    "        float fi = i;\n"
    "        x = x + fi * 0.25;\n"
    "        out = x;\n"
    "    }\n"
    "}\n";

const char *kControlSource =
    "namespace control\n"
    "{\n"
    "    void branch(input varying float a,\n"
    "                input varying float b,\n"
    "                input varying int i,\n"
    "                output varying float out)\n"
    "    {\n"
    "        float r;\n"
    "        if (a > b)\n"
    "            r = a - b;\n"
    "        else\n"
    "            r = b - a;\n"
    "\n"
    "        int k = 0;\n"
    "        while (k < 4)\n"
    "        {\n"
    "            float fk = k;\n"
    "            r = r + fk;\n"
    "            k = k + 1;\n"
    "        }\n"
    "\n"
    "        if (i < 0)\n"
    "            r = -r;\n"
    "\n"
    "        out = r;\n"
    "    }\n"
    "}\n";

//
// `caller` exercises user-function calls: a void helper with an output
// parameter, a non-void helper used as an expression operand, and a
// cross-helper invocation so the call-graph walk + temporary-slot logic
// are all exercised in the same program.
//
const char *kCallSource =
    "namespace call\n"
    "{\n"
    "    float square(float x)\n"
    "    {\n"
    "        return x * x;\n"
    "    }\n"
    "\n"
    "    void bias(float x, output float y)\n"
    "    {\n"
    "        y = x + 0.5;\n"
    "    }\n"
    "\n"
    "    void caller(input varying float a,\n"
    "                input varying float b,\n"
    "                input varying int i,\n"
    "                output varying float out)\n"
    "    {\n"
    "        float biased;\n"
    "        bias(a, biased);\n"
    "        float s = square(biased) + square(b);\n"
    "        float fi = i;\n"
    "        out = s - fi * 0.125;\n"
    "    }\n"
    "}\n";

//
// `aggregate` exercises arrays + structs + member access + .size + value
// literals without any stdlib dependency. The top-level parameters stay
// scalar varying, but the function body declares a local fixed-size
// array and a struct-typed local, indexes into the array through a
// while loop driven by `.size`, and reads struct members.
//
const char *kAggregateSource =
    "namespace aggr\n"
    "{\n"
    "    struct Pair\n"
    "    {\n"
    "        float x;\n"
    "        float y;\n"
    "    };\n"
    "\n"
    "    Pair makePair(float a, float b)\n"
    "    {\n"
    "        Pair p = {a, b};\n"
    "        return p;\n"
    "    }\n"
    "\n"
    "    float sumArray(float arr[4], float scale)\n"
    "    {\n"
    "        float total = 0.0;\n"
    "        int k = 0;\n"
    "        while (k < arr.size)\n"
    "        {\n"
    "            total = total + arr[k] * scale;\n"
    "            k = k + 1;\n"
    "        }\n"
    "        return total;\n"
    "    }\n"
    "\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    "        Pair p = makePair(a, b);\n"
    "        float arr[4] = {p.x, p.y, a * 2.0, b * 0.5};\n"
    "        float fi = i;\n"
    "        out = sumArray(arr, fi * 0.25) + p.x - p.y;\n"
    "    }\n"
    "}\n";

//
// Variable-size array (VSArray) parameter passing. The helper takes a
// `float arr[]` — no compile-time size — and reads both its `.size` and
// the elements. The kernel-entry `compute` declares a fixed-size local
// and passes it to the helper, which is the exact shape real-world CTL
// stdlib calls use (e.g. `lookup1D(table, ...)` where `table` is a
// user-declared fixed-size local with a `float table[]` formal param).
//
const char *kVSArraySource =
    "namespace vsa\n"
    "{\n"
    "    float sumScaled(float arr[], float scale)\n"
    "    {\n"
    "        float total = 0.0;\n"
    "        int k = 0;\n"
    "        while (k < arr.size)\n"
    "        {\n"
    "            total = total + arr[k] * scale;\n"
    "            k = k + 1;\n"
    "        }\n"
    "        return total;\n"
    "    }\n"
    "\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    "        float arr[5] = {a, b, a * 2.0, b * 0.5, a + b};\n"
    "        float fi = i;\n"
    "        out = sumScaled(arr, fi * 0.125);\n"
    "    }\n"
    "}\n";

//
// `stdlkup1d` exercises `lookup1D(float[], float, float, float)` from
// the CTL stdlib. The kernel-entry `compute` declares a fixed-size
// local `float table[8] = {...}` and calls `lookup1D(table, pMin,
// pMax, p)`. This verifies both the MSL `ctl_stdlib_lookup1D` helper
// and VSArray decay (`&table[0]`, `8u`) at the stdlib call site.
//
const char *kStdLookup1DSource =
    "namespace stdlkup1d\n"
    "{\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    "        float table[8] = {0.1, 0.5, 1.0, 0.2,\n"
    "                          -0.3, 2.0, 1.5, 0.8};\n"
    "        float fi = i;\n"
    "        float p = a * 0.1 + b * 0.05 + fi * 0.01;\n"
    "        out = lookup1D(table, -1.0, 1.0, p);\n"
    "    }\n"
    "}\n";

//
// `stdcubic1d` exercises `lookupCubic1D(float[], float, float, float)`.
// The same VSArray decay convention as `lookup1D`, but the Hermite
// blend has four terms instead of two — any re-ordering drifts by 1
// ULP at sub-interval boundaries.
//
const char *kStdLookupCubic1DSource =
    "namespace stdcubic1d\n"
    "{\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    "        float table[8] = {0.1, 0.5, 1.0, 0.2,\n"
    "                          -0.3, 2.0, 1.5, 0.8};\n"
    "        float fi = i;\n"
    "        float p = a * 0.1 + b * 0.05 + fi * 0.01;\n"
    "        out = lookupCubic1D(table, -1.0, 1.0, p);\n"
    "    }\n"
    "}\n";

//
// `stdinterp1d` exercises `interpolate1D(float[][2], float)`. The
// knot table is a fixed-size `float[5][2]` local declared inside the
// kernel-entry; at the call site it decays to a VSArray of
// `metal::array<float, 2>` pairs.
//
const char *kStdInterp1DSource =
    "namespace stdinterp1d\n"
    "{\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    "        float table[5][2] = {\n"
    "            {-2.0,  1.0},\n"
    "            {-0.5,  0.3},\n"
    "            { 0.0,  0.0},\n"
    "            { 0.75, 0.9},\n"
    "            { 2.5,  1.5}\n"
    "        };\n"
    "        float fi = i;\n"
    "        float p = a * 0.2 + b * 0.05 + fi * 0.1;\n"
    "        out = interpolate1D(table, p);\n"
    "    }\n"
    "}\n";

//
// `stdinterpc1d` — analogous test for `interpolateCubic1D`.
//
const char *kStdInterpCubic1DSource =
    "namespace stdinterpc1d\n"
    "{\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    "        float table[5][2] = {\n"
    "            {-2.0,  1.0},\n"
    "            {-0.5,  0.3},\n"
    "            { 0.0,  0.0},\n"
    "            { 0.75, 0.9},\n"
    "            { 2.5,  1.5}\n"
    "        };\n"
    "        float fi = i;\n"
    "        float p = a * 0.2 + b * 0.05 + fi * 0.1;\n"
    "        out = interpolateCubic1D(table, p);\n"
    "    }\n"
    "}\n";

//
// `stdlkup3df3` exercises `lookup3D_f3(float[][][][3], float[3],
// float[3], float[3])` from the CTL stdlib. The kernel-entry
// `compute` declares a fixed-size local `float f[2][2][2][3]` and
// calls `lookup3D_f3(f, pMin, pMax, p)`. This verifies multi-dim
// VSArray decay (four leading variable-size levels → pointer + three
// length uniforms) and the `ctl_stdlib_lookup3D_f3` trilinear helper.
// Target 0 ULP — pure fma-matched trilinear blend with no
// transcendentals on either side.
//
const char *kStdLookup3DF3Source =
    "namespace stdlkup3df3\n"
    "{\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    "        float f[2][2][2][3] = {\n"
    "            {{{0.0, 0.0, 0.0}, {1.0, 0.1, 0.2}},\n"
    "             {{0.1, 1.0, 0.3}, {1.0, 1.0, 0.4}}},\n"
    "            {{{0.2, 0.3, 1.0}, {1.0, 0.4, 1.0}},\n"
    "             {{0.5, 1.0, 1.0}, {1.0, 1.0, 1.0}}}\n"
    "        };\n"
    "        float pMin[3] = {-1.0, -1.0, -1.0};\n"
    "        float pMax[3] = { 1.0,  1.0,  1.0};\n"
    "        float fi = i;\n"
    "        float p[3] = {a * 0.5 + fi * 0.01,\n"
    "                      b * 0.5 + fi * 0.02,\n"
    "                      (a + b) * 0.25 + fi * 0.03};\n"
    "        float r[3] = lookup3D_f3(f, pMin, pMax, p);\n"
    "        out = r[0] * 0.37 + r[1] * 0.41 + r[2] * 0.22;\n"
    "    }\n"
    "}\n";

//
// `stdlkup3df` exercises `lookup3D_f(float[][][][3], float[3],
// float[3], float, float, float, output float, output float, output
// float)` — the scalar-in, scalar-out variant of `lookup3D_f3`. Same
// trilinear body, different ABI: three scalar coords in, three
// `output float` refs out. Target 0 ULP because the MSL helper
// delegates to `ctl_stdlib_lookup3D_f3` unchanged.
//
const char *kStdLookup3DFSource =
    "namespace stdlkup3df\n"
    "{\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    "        float f[2][2][2][3] = {\n"
    "            {{{0.0, 0.0, 0.0}, {1.0, 0.1, 0.2}},\n"
    "             {{0.1, 1.0, 0.3}, {1.0, 1.0, 0.4}}},\n"
    "            {{{0.2, 0.3, 1.0}, {1.0, 0.4, 1.0}},\n"
    "             {{0.5, 1.0, 1.0}, {1.0, 1.0, 1.0}}}\n"
    "        };\n"
    "        float pMin[3] = {-1.0, -1.0, -1.0};\n"
    "        float pMax[3] = { 1.0,  1.0,  1.0};\n"
    "        float fi = i;\n"
    "        float p0 = a * 0.5 + fi * 0.01;\n"
    "        float p1 = b * 0.5 + fi * 0.02;\n"
    "        float p2 = (a + b) * 0.25 + fi * 0.03;\n"
    "        float q0; float q1; float q2;\n"
    "        lookup3D_f(f, pMin, pMax, p0, p1, p2, q0, q1, q2);\n"
    "        out = q0 * 0.37 + q1 * 0.41 + q2 * 0.22;\n"
    "    }\n"
    "}\n";

//
// `stdlkup3dh` exercises `lookup3D_h` — same shape as `lookup3D_f`
// but with half scalars for the input coords and output refs. The
// trilinear body runs at float precision (same table, same core
// helper), so the only CPU↔GPU divergence risks come from the
// half↔float round-trip at the ABI boundary. The CPU path uses
// Imath's `half` operator= which rounds-to-nearest-even; Apple's
// MSL `half(float)` cast is the same IEEE-754 operation, so target
// 0 ULP.
//
const char *kStdLookup3DHSource =
    "namespace stdlkup3dh\n"
    "{\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    "        float f[2][2][2][3] = {\n"
    "            {{{0.0, 0.0, 0.0}, {1.0, 0.1, 0.2}},\n"
    "             {{0.1, 1.0, 0.3}, {1.0, 1.0, 0.4}}},\n"
    "            {{{0.2, 0.3, 1.0}, {1.0, 0.4, 1.0}},\n"
    "             {{0.5, 1.0, 1.0}, {1.0, 1.0, 1.0}}}\n"
    "        };\n"
    "        float pMin[3] = {-1.0, -1.0, -1.0};\n"
    "        float pMax[3] = { 1.0,  1.0,  1.0};\n"
    "        float fi = i;\n"
    "        half p0 = a * 0.5 + fi * 0.01;\n"
    "        half p1 = b * 0.5 + fi * 0.02;\n"
    "        half p2 = (a + b) * 0.25 + fi * 0.03;\n"
    "        half q0; half q1; half q2;\n"
    "        lookup3D_h(f, pMin, pMax, p0, p1, p2, q0, q1, q2);\n"
    "        out = q0 * 0.37 + q1 * 0.41 + q2 * 0.22;\n"
    "    }\n"
    "}\n";

//
// `stdlkup3dtf3` exercises `lookup3DTetra_f3` — the tetrahedral
// variant of `lookup3D_f3`. Same table and probe pattern as the
// trilinear fixture; the table is non-planar inside each cell, so
// tetrahedral and trilinear results genuinely differ and this fixture
// validates the tetrahedral blend itself, not a shared code path.
// The probe coordinates are decorrelated and scaled so the 1024
// makeSamples entries hit every one of the six tetrahedra (85+
// samples each) plus the boundary-clamp/tie paths — the trilinear
// fixtures' `(a+b)*0.25` third coordinate correlates with the first
// two and leaves one tetrahedron completely unsampled. Target 0 ULP
// — the tetrahedron selection is exact comparisons and the four-term
// blend mirrors the CPU's discrete fsub/fmul/fadd sequence.
//
const char *kStdLookup3DTetraF3Source =
    "namespace stdlkup3dtf3\n"
    "{\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    "        float f[2][2][2][3] = {\n"
    "            {{{0.0, 0.0, 0.0}, {1.0, 0.1, 0.2}},\n"
    "             {{0.1, 1.0, 0.3}, {1.0, 1.0, 0.4}}},\n"
    "            {{{0.2, 0.3, 1.0}, {1.0, 0.4, 1.0}},\n"
    "             {{0.5, 1.0, 1.0}, {1.0, 1.0, 1.0}}}\n"
    "        };\n"
    "        float pMin[3] = {-1.0, -1.0, -1.0};\n"
    "        float pMax[3] = { 1.0,  1.0,  1.0};\n"
    "        float fi = i;\n"
    "        float p[3] = {a * 0.09 + fi * 0.011,\n"
    "                      b * 0.07 - fi * 0.013,\n"
    "                      a * 0.05 - b * 0.06 + fi * 0.017};\n"
    "        float r[3] = lookup3DTetra_f3(f, pMin, pMax, p);\n"
    "        out = r[0] * 0.37 + r[1] * 0.41 + r[2] * 0.22;\n"
    "    }\n"
    "}\n";

//
// `stdlkup3dtf` exercises `lookup3DTetra_f` — scalar-in, scalar-out
// ABI over the same tetrahedral core. Target 0 ULP because the MSL
// helper delegates to `ctl_stdlib_lookup3DTetra_f3` unchanged.
//
const char *kStdLookup3DTetraFSource =
    "namespace stdlkup3dtf\n"
    "{\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    "        float f[2][2][2][3] = {\n"
    "            {{{0.0, 0.0, 0.0}, {1.0, 0.1, 0.2}},\n"
    "             {{0.1, 1.0, 0.3}, {1.0, 1.0, 0.4}}},\n"
    "            {{{0.2, 0.3, 1.0}, {1.0, 0.4, 1.0}},\n"
    "             {{0.5, 1.0, 1.0}, {1.0, 1.0, 1.0}}}\n"
    "        };\n"
    "        float pMin[3] = {-1.0, -1.0, -1.0};\n"
    "        float pMax[3] = { 1.0,  1.0,  1.0};\n"
    "        float fi = i;\n"
    "        float p0 = a * 0.09 + fi * 0.011;\n"
    "        float p1 = b * 0.07 - fi * 0.013;\n"
    "        float p2 = a * 0.05 - b * 0.06 + fi * 0.017;\n"
    "        float q0; float q1; float q2;\n"
    "        lookup3DTetra_f(f, pMin, pMax, p0, p1, p2, q0, q1, q2);\n"
    "        out = q0 * 0.37 + q1 * 0.41 + q2 * 0.22;\n"
    "    }\n"
    "}\n";

//
// `stdlkup3dth` exercises `lookup3DTetra_h` — half scalar coords and
// output refs around the float-precision tetrahedral core. Same
// half↔float boundary analysis as `lookup3D_h`; target 0 ULP.
//
const char *kStdLookup3DTetraHSource =
    "namespace stdlkup3dth\n"
    "{\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    "        float f[2][2][2][3] = {\n"
    "            {{{0.0, 0.0, 0.0}, {1.0, 0.1, 0.2}},\n"
    "             {{0.1, 1.0, 0.3}, {1.0, 1.0, 0.4}}},\n"
    "            {{{0.2, 0.3, 1.0}, {1.0, 0.4, 1.0}},\n"
    "             {{0.5, 1.0, 1.0}, {1.0, 1.0, 1.0}}}\n"
    "        };\n"
    "        float pMin[3] = {-1.0, -1.0, -1.0};\n"
    "        float pMax[3] = { 1.0,  1.0,  1.0};\n"
    "        float fi = i;\n"
    "        half p0 = a * 0.09 + fi * 0.011;\n"
    "        half p1 = b * 0.07 - fi * 0.013;\n"
    "        half p2 = a * 0.05 - b * 0.06 + fi * 0.017;\n"
    "        half q0; half q1; half q2;\n"
    "        lookup3DTetra_h(f, pMin, pMax, p0, p1, p2, q0, q1, q2);\n"
    "        out = q0 * 0.37 + q1 * 0.41 + q2 * 0.22;\n"
    "    }\n"
    "}\n";

//
// `stdluv` exercises `LuvtoXYZ(f3, f3)`. Pure rational + t*t*t,
// no transcendentals — target 0 ULP.
//
const char *kStdLuvToXyzSource =
    "namespace stdluv\n"
    "{\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    "        float Luv[3] = {a, b, a + b};\n"
    "        float XYZn[3] = {0.95, 1.0, 1.09};\n"
    "        float r[3] = LuvtoXYZ(Luv, XYZn);\n"
    "        float fi = i;\n"
    "        out = r[0] * 0.37 + r[1] * 0.41 + r[2] * 0.22 + fi * 0.0;\n"
    "    }\n"
    "}\n";

//
// `stdlab` exercises `LabtoXYZ(f3, f3)`. Same no-pow property.
//
const char *kStdLabToXyzSource =
    "namespace stdlab\n"
    "{\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    "        float Lab[3] = {a, b, a - b};\n"
    "        float XYZn[3] = {0.95, 1.0, 1.09};\n"
    "        float r[3] = LabtoXYZ(Lab, XYZn);\n"
    "        float fi = i;\n"
    "        out = r[0] * 0.37 + r[1] * 0.41 + r[2] * 0.22 + fi * 0.0;\n"
    "    }\n"
    "}\n";

//
// `stdxyzluv` / `stdxyzlab` exercise the pow-using colorspace
// forward transforms. Inputs biased into (0, 1] so the `pow(x, 1/3)`
// branch of `f()` dominates; the composed drift inherits `pow`'s
// 1-ULP bound and sometimes widens by the subsequent multiply/add
// chain.
//
const char *kStdXyzToLuvSource =
    "namespace stdxyzluv\n"
    "{\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    "        float base = fabs(a) * 0.01 + 0.1;\n"
    "        float XYZ[3]  = {base, base + 0.02, base + 0.05};\n"
    "        float XYZn[3] = {0.95, 1.0, 1.09};\n"
    "        float r[3] = XYZtoLuv(XYZ, XYZn);\n"
    "        float fi = i;\n"
    "        out = r[0] * 0.37 + r[1] * 0.41 + r[2] * 0.22 +\n"
    "              fi * 0.0 + b * 0.0;\n"
    "    }\n"
    "}\n";

//
// `stdinvert_f33` / `stdinvert_f44` — exercise matrix inversion
// through both Imath branches. For `invert_f33` we build a non-affine
// 3x3 so the adjugate/determinant branch fires; we also do a companion
// affine test via an upper-left 2x2 matrix with translation. For
// `invert_f44` we build a non-affine 4x4 so `gjInverse` fires, plus an
// affine one for the 3x3 adjugate path.
//
const char *kStdInvertF33Source =
    "namespace stdinvf33\n"
    "{\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    //
    // Non-affine 3x3 — x[i][2] all nonzero and x[2][2] != 1, forcing
    // the general adjugate path. Seed the matrix deterministically
    // off `a`, `b`, `i` so the parity test sees per-sample variation.
    //
    "        float s = fabs(a) * 0.3 + 0.7;\n"
    "        float t = fabs(b) * 0.3 + 0.5;\n"
    "        float M[3][3] = {{s + 0.1, 0.2, 0.3},\n"
    "                         {0.1, s + 0.4, 0.2},\n"
    "                         {t,   0.05, s + 0.8}};\n"
    "        float Mi[3][3] = invert_f33(M);\n"
    "        float fi = i;\n"
    "        out = fabs(Mi[0][0]) + fabs(Mi[0][1]) + fabs(Mi[0][2]) +\n"
    "              fabs(Mi[1][0]) + fabs(Mi[1][1]) + fabs(Mi[1][2]) +\n"
    "              fabs(Mi[2][0]) + fabs(Mi[2][1]) + fabs(Mi[2][2]) +\n"
    "              fi * 0.0;\n"
    "    }\n"
    "}\n";

const char *kStdInvertF44Source =
    "namespace stdinvf44\n"
    "{\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    //
    // Affine 4x4 — x[i][3] all zero, x[3][3] == 1. Hits the fast
    // adjugate path (same algebra as the XYZtoRGB helper reuses).
    // Translation row x[3][0..2] is nonzero so the bottom-row
    // reconstruction formula is exercised.
    //
    "        float s = fabs(a) * 0.3 + 0.7;\n"
    "        float t = fabs(b) * 0.3 + 0.5;\n"
    "        float M[4][4] = {{s + 0.1, 0.2, 0.3, 0.0},\n"
    "                         {0.1, s + 0.4, 0.2, 0.0},\n"
    "                         {t,   0.05, s + 0.8, 0.0},\n"
    "                         {0.5, 0.25, 0.125, 1.0}};\n"
    "        float Mi[4][4] = invert_f44(M);\n"
    "        float fi = i;\n"
    "        out = fabs(Mi[0][0]) + fabs(Mi[0][1]) + fabs(Mi[0][2]) +\n"
    "              fabs(Mi[1][0]) + fabs(Mi[1][1]) + fabs(Mi[1][2]) +\n"
    "              fabs(Mi[2][0]) + fabs(Mi[2][1]) + fabs(Mi[2][2]) +\n"
    "              fabs(Mi[3][0]) + fabs(Mi[3][1]) + fabs(Mi[3][2]) +\n"
    "              fi * 0.0;\n"
    "    }\n"
    "}\n";

const char *kStdInvertF44GjSource =
    "namespace stdinvf44gj\n"
    "{\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    //
    // Non-affine 4x4 — x[0][3] is nonzero, which trips Imath's
    // `gjInverse` branch. This exercises the full Gauss-Jordan
    // elimination path with partial pivoting.
    //
    "        float s = fabs(a) * 0.3 + 0.7;\n"
    "        float t = fabs(b) * 0.3 + 0.5;\n"
    "        float M[4][4] = {{s + 0.1, 0.2, 0.3, 0.15},\n"
    "                         {0.1, s + 0.4, 0.2, 0.25},\n"
    "                         {t,   0.05, s + 0.8, 0.35},\n"
    "                         {0.5, 0.25, 0.125, s + 1.0}};\n"
    "        float Mi[4][4] = invert_f44(M);\n"
    "        float fi = i;\n"
    "        out = fabs(Mi[0][0]) + fabs(Mi[0][1]) + fabs(Mi[0][2]) +\n"
    "              fabs(Mi[0][3]) +\n"
    "              fabs(Mi[1][0]) + fabs(Mi[1][1]) + fabs(Mi[1][2]) +\n"
    "              fabs(Mi[1][3]) +\n"
    "              fabs(Mi[2][0]) + fabs(Mi[2][1]) + fabs(Mi[2][2]) +\n"
    "              fabs(Mi[2][3]) +\n"
    "              fabs(Mi[3][0]) + fabs(Mi[3][1]) + fabs(Mi[3][2]) +\n"
    "              fabs(Mi[3][3]) + fi * 0.0;\n"
    "    }\n"
    "}\n";

//
// `stdrgbxyz` / `stdxyzrgb` exercise the Chromaticities-taking matrix
// builders. The chromaticities struct itself is uniform per dispatch
// (no way to vary a struct per-sample through the CTL stdlib
// signature), so to give the test real per-sample variation we vary
// `Y` (the white-luminance scalar) off the sample's `a` input.
// Reduction sums the matrix's nine non-zero entries with fabs() to
// avoid cross-entry cancellation in the ULP measurement.
//
const char *kStdRgbToXyzSource =
    "namespace stdrgbxyz\n"
    "{\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    //
    // Rec.709 primaries — the reference set the CTL standard
    // library was designed against.
    //
    "        Chromaticities chr = {{0.64, 0.33}, {0.3, 0.6},\n"
    "                              {0.15, 0.06}, {0.3127, 0.329}};\n"
    "        float Y = fabs(a) * 0.5 + 0.5;\n"
    "        float M[4][4] = RGBtoXYZ(chr, Y);\n"
    "        float fi = i;\n"
    "        out = fabs(M[0][0]) + fabs(M[0][1]) + fabs(M[0][2]) +\n"
    "              fabs(M[1][0]) + fabs(M[1][1]) + fabs(M[1][2]) +\n"
    "              fabs(M[2][0]) + fabs(M[2][1]) + fabs(M[2][2]) +\n"
    "              fi * 0.0 + b * 0.0;\n"
    "    }\n"
    "}\n";

const char *kStdXyzToRgbSource =
    "namespace stdxyzrgb\n"
    "{\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    "        Chromaticities chr = {{0.64, 0.33}, {0.3, 0.6},\n"
    "                              {0.15, 0.06}, {0.3127, 0.329}};\n"
    "        float Y = fabs(a) * 0.5 + 0.5;\n"
    "        float M[4][4] = XYZtoRGB(chr, Y);\n"
    "        float fi = i;\n"
    "        out = fabs(M[0][0]) + fabs(M[0][1]) + fabs(M[0][2]) +\n"
    "              fabs(M[1][0]) + fabs(M[1][1]) + fabs(M[1][2]) +\n"
    "              fabs(M[2][0]) + fabs(M[2][1]) + fabs(M[2][2]) +\n"
    "              fi * 0.0 + b * 0.0;\n"
    "    }\n"
    "}\n";

const char *kStdXyzToLabSource =
    "namespace stdxyzlab\n"
    "{\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    //
    // Use well-separated X/Y/Z so `fX - fY` and `fY - fZ` don't
    // hit catastrophic cancellation on close-to-identical pow
    // results — the 1-ULP pow drift would otherwise amplify into
    // 20+ ULPs in `astar`/`bstar`. Real colorimetric triplets
    // look like this; pathological equal-channel inputs don't.
    //
    "        float XYZ[3] = {fabs(a) * 0.3 + 0.15,\n"
    "                        fabs(a) * 0.1 + 0.45,\n"
    "                        fabs(a) * 0.2 + 0.75};\n"
    "        float XYZn[3] = {0.95, 1.0, 1.09};\n"
    "        float r[3] = XYZtoLab(XYZ, XYZn);\n"
    "        float fi = i;\n"
    //
    // `astar`/`bstar` can have opposite sign from `Lstar`, so
    // a signed weighted sum of the three channels can cancel to
    // near-zero and inflate relative ULP. Reduce via fabs() so the
    // measurement reflects per-channel drift rather than a lucky
    // cancellation in the reduction.
    //
    "        out = fabs(r[0]) + fabs(r[1]) + fabs(r[2]) +\n"
    "              fi * 0.0 + b * 0.0;\n"
    "    }\n"
    "}\n";

//
// `early` exercises the `return;` statement from inside nested control
// flow. The top-level CTL function is void, so `return;` short-circuits
// the rest of the body without going through a CTL call.
//
const char *kReturnSource =
    "namespace control\n"
    "{\n"
    "    void early(input varying float a,\n"
    "               input varying float b,\n"
    "               input varying int i,\n"
    "               output varying float out)\n"
    "    {\n"
    "        if (i < 0)\n"
    "        {\n"
    "            out = a - b;\n"
    "            return;\n"
    "        }\n"
    "        int k = 0;\n"
    "        while (k < 3)\n"
    "        {\n"
    "            float fk = k;\n"
    "            if (a + fk > b)\n"
    "            {\n"
    "                out = a + fk;\n"
    "                return;\n"
    "            }\n"
    "            k = k + 1;\n"
    "        }\n"
    "        out = a + b;\n"
    "    }\n"
    "}\n";

//
// `statics` exercises module-level `const` declarations. Each one
// becomes a `constant T staticN = ...;` global in the MSL header
// (via MetalVariableNode's MetalStaticAddr branch). The compute
// function reads all three globals and combines them with varying
// per-sample input so SIMD/Metal parity covers both int + float
// constant storage in one run.
//
const char *kStaticsSource =
    "namespace statics\n"
    "{\n"
    "    const int   kRepeat = 3;\n"
    "    const float kScale  = 0.25;\n"
    "    const float kBias   = -1.5;\n"
    "\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    "        float r = 0.0;\n"
    "        int k = 0;\n"
    "        while (k < kRepeat)\n"
    "        {\n"
    "            float fk = k;\n"
    "            r = r + a * kScale + b * fk;\n"
    "            k = k + 1;\n"
    "        }\n"
    "        float fi = i;\n"
    "        out = r + kBias + fi * kScale;\n"
    "    }\n"
    "}\n";

//
// `matrix` exercises module-level const *aggregates*: a 3-vector, a
// 3x3 matrix, and a struct with a mix of scalar + array members. This
// is the shape ACES color-correction CTLs lean on heavily (colorspace
// conversion matrices declared once at module scope and re-read per
// pixel). Each lowers to `constant metal::array<...>` or
// `constant struct_Name` in the MSL header.
//
const char *kMatrixSource =
    "namespace matx\n"
    "{\n"
    "    const float kWeights[3]    = {0.2126, 0.7152, 0.0722};\n"
    "    const float kMatrix[3][3]  = {{1.0, 0.5, 0.25},\n"
    "                                  {0.125, 1.0, 0.0625},\n"
    "                                  {0.03125, 0.015625, 1.0}};\n"
    "\n"
    "    struct Tuning\n"
    "    {\n"
    "        float scale;\n"
    "        float bias;\n"
    "        float offsets[3];\n"
    "    };\n"
    "\n"
    "    const Tuning kTune = {0.75, -0.25, {0.1, 0.2, 0.3}};\n"
    "\n"
    "    float weightedSum(float v[3])\n"
    "    {\n"
    "        float s = 0.0;\n"
    "        int k = 0;\n"
    "        while (k < 3)\n"
    "        {\n"
    "            s = s + v[k] * kWeights[k];\n"
    "            k = k + 1;\n"
    "        }\n"
    "        return s;\n"
    "    }\n"
    "\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    "        float fi = i;\n"
    "        float v[3] = {a, b, fi * 0.1};\n"
    "\n"
    "        float r[3];\n"
    "        int row = 0;\n"
    "        while (row < 3)\n"
    "        {\n"
    "            r[row] = kMatrix[row][0] * v[0]\n"
    "                   + kMatrix[row][1] * v[1]\n"
    "                   + kMatrix[row][2] * v[2]\n"
    "                   + kTune.offsets[row];\n"
    "            row = row + 1;\n"
    "        }\n"
    "\n"
    "        out = weightedSum(r) * kTune.scale + kTune.bias;\n"
    "    }\n"
    "}\n";

//
// `stdmath` exercises the Phase-3 IEEE-754-exact stdlib functions
// reachable through the MetalStdLibFuncAddr path: `fabs`, `floor`,
// `sqrt`, and `fmod`. MSL's metal:: counterparts are all specified
// as correctly-rounded IEEE-754 operations under MTLMathModeSafe, so
// parity must hold at 0 ULP. The combined expression mixes 1-arg and
// 2-arg stdlib calls so a regression in either the call-site codegen
// or the argument-marshalling path surfaces immediately.
//
const char *kStdMathSource =
    "namespace stdmath\n"
    "{\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    "        float fi = i;\n"
    "        float s = sqrt(fabs(a) + 1.0);\n"
    "        float m = fmod(fabs(b) * 3.0 + 1.0, 2.5);\n"
    "        out = s + floor(b * 4.0) - fabs(fi * 0.25) + m;\n"
    "    }\n"
    "}\n";

//
// `stdlimits` exercises the four bool-returning classification stdlib
// functions: `isfinite_f`, `isnormal_f`, `isnan_f`, `isinf_f`. Our sample
// inputs span [-16, 16] (all finite, all normal) so the isfinite/isnormal
// calls take the `true` branch and the isnan/isinf calls take the `false`
// branch — covering both outcomes of the bool-returning call-site codegen
// path in one fixture. Edge cases (actual NaN / Inf / denormal inputs)
// belong to a dedicated per-stdlib-function test file; here we just
// prove the plumbing between Ctl bool and MSL bool is bit-exact against
// the CPU reference.
//
//
// `stdtrans_exp` / `stdtrans_log` exercise the Phase-3 transcendental
// baseline: CTL stdlib names wired to `metal::precise::exp` / `::log`.
// Per `metal_precise_namespace.md`, these are expected to diverge from
// Apple libm's FreeBSD-derived implementations by a small number of ULP
// on some fraction of inputs — the measurement fixture reports the gap
// rather than aborting. 0-ULP is the merge gate; a non-zero result
// motivates a hand-rolled port of the libm algorithm as a follow-up.
//
// Input domains are constrained to each function's well-defined range:
//   exp: a is unconstrained; the [-16, 16] sample range is safe (e^16
//        ~= 8.9e6, well inside float32).
//   log: must be positive, so we use `fabs(a) + 1.0` which is in
//        [1.0, 17.0] — dense enough to catch most rounding disagreements.
//
const char *kStdExpSource =
    "namespace stdtrans_exp\n"
    "{\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    "        out = exp(a);\n"
    "    }\n"
    "}\n";

const char *kStdLogSource =
    "namespace stdtrans_log\n"
    "{\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    "        out = log(fabs(a) + 1.0);\n"
    "    }\n"
    "}\n";

//
// Remaining scalar transcendentals. Each body confines the input to the
// function's well-defined domain so the measurement is about ULP drift
// in the implementation, not edge cases / NaN propagation. Where the
// input variable needs scaling we apply it in CTL (not via a separate
// sample-generation path) so CPU and GPU see identical source code.
//
const char *kStdLog10Source =
    "namespace stdtrans_log10\n"
    "{\n"
    "    void compute(input varying float a, input varying float b,\n"
    "                 input varying int i, output varying float out)\n"
    "    {\n"
    "        out = log10(fabs(a) + 1.0);\n"
    "    }\n"
    "}\n";

const char *kStdPowSource =
    "namespace stdtrans_pow\n"
    "{\n"
    "    void compute(input varying float a, input varying float b,\n"
    "                 input varying int i, output varying float out)\n"
    "    {\n"
    "        out = pow(fabs(a) + 1.0, b * 0.25);\n"
    "    }\n"
    "}\n";

const char *kStdSinSource =
    "namespace stdtrans_sin\n"
    "{\n"
    "    void compute(input varying float a, input varying float b,\n"
    "                 input varying int i, output varying float out)\n"
    "    { out = sin(a); }\n"
    "}\n";

const char *kStdCosSource =
    "namespace stdtrans_cos\n"
    "{\n"
    "    void compute(input varying float a, input varying float b,\n"
    "                 input varying int i, output varying float out)\n"
    "    { out = cos(a); }\n"
    "}\n";

const char *kStdTanSource =
    "namespace stdtrans_tan\n"
    "{\n"
    "    void compute(input varying float a, input varying float b,\n"
    "                 input varying int i, output varying float out)\n"
    "    { out = tan(a * 0.1); }\n"
    "}\n";

const char *kStdAsinSource =
    "namespace stdtrans_asin\n"
    "{\n"
    "    void compute(input varying float a, input varying float b,\n"
    "                 input varying int i, output varying float out)\n"
    "    { out = asin(a * 0.03125); }\n"
    "}\n";

const char *kStdAcosSource =
    "namespace stdtrans_acos\n"
    "{\n"
    "    void compute(input varying float a, input varying float b,\n"
    "                 input varying int i, output varying float out)\n"
    "    { out = acos(a * 0.03125); }\n"
    "}\n";

const char *kStdAtanSource =
    "namespace stdtrans_atan\n"
    "{\n"
    "    void compute(input varying float a, input varying float b,\n"
    "                 input varying int i, output varying float out)\n"
    "    { out = atan(a); }\n"
    "}\n";

const char *kStdAtan2Source =
    "namespace stdtrans_atan2\n"
    "{\n"
    "    void compute(input varying float a, input varying float b,\n"
    "                 input varying int i, output varying float out)\n"
    "    { out = atan2(a, b); }\n"
    "}\n";

const char *kStdSinhSource =
    "namespace stdtrans_sinh\n"
    "{\n"
    "    void compute(input varying float a, input varying float b,\n"
    "                 input varying int i, output varying float out)\n"
    "    { out = sinh(a * 0.1); }\n"
    "}\n";

const char *kStdCoshSource =
    "namespace stdtrans_cosh\n"
    "{\n"
    "    void compute(input varying float a, input varying float b,\n"
    "                 input varying int i, output varying float out)\n"
    "    { out = cosh(a * 0.1); }\n"
    "}\n";

const char *kStdTanhSource =
    "namespace stdtrans_tanh\n"
    "{\n"
    "    void compute(input varying float a, input varying float b,\n"
    "                 input varying int i, output varying float out)\n"
    "    { out = tanh(a); }\n"
    "}\n";

const char *kStdHypotSource =
    "namespace stdtrans_hypot\n"
    "{\n"
    "    void compute(input varying float a, input varying float b,\n"
    "                 input varying int i, output varying float out)\n"
    "    { out = hypot(a, b); }\n"
    "}\n";

//
// ACES v2 user-level helpers. These names (round, log2, sign, copysign,
// ceil) are NOT CTL stdlib entries; ACES v2 defines them in
// `Lib.Academy.Utilities.ctl` on top of the primitives the backend
// already supports (floor, log, fabs, comparison, int/float mix). The
// audit's goal for these five is confirming that Metal codegen
// reproduces SIMD bit-for-bit for the ACES definitions — every op
// reduces to either a stdlib primitive already covered elsewhere or a
// language-level construct covered earlier. `trunc` is intentionally
// omitted: it is not in either stdlib and is not referenced in the
// ACES v2 library, so there is no parity target to test.
//
const char *kAudRoundSource =
    "namespace aud_round\n"
    "{\n"
    "    float aces_round(float x)\n"
    "    {\n"
    "        int x1;\n"
    "        if (x < 0.0) x1 = x - 0.5;\n"
    "        else         x1 = x + 0.5;\n"
    "        return x1;\n"
    "    }\n"
    "    void compute(input varying float a, input varying float b,\n"
    "                 input varying int i, output varying float out)\n"
    "    { out = aces_round(a); }\n"
    "}\n";

const char *kAudLog2Source =
    "namespace aud_log2\n"
    "{\n"
    "    float aces_log2(float x) { return log(x) / log(2.0); }\n"
    "    void compute(input varying float a, input varying float b,\n"
    "                 input varying int i, output varying float out)\n"
    "    { out = aces_log2(fabs(a) + 1.0); }\n"
    "}\n";

const char *kAudSignSource =
    "namespace aud_sign\n"
    "{\n"
    "    int aces_sign(float x)\n"
    "    {\n"
    "        int y;\n"
    "        if (x < 0)      y = -1;\n"
    "        else if (x > 0) y =  1;\n"
    "        else            y =  0;\n"
    "        return y;\n"
    "    }\n"
    "    void compute(input varying float a, input varying float b,\n"
    "                 input varying int i, output varying float out)\n"
    "    { int s = aces_sign(a); float fs = s; out = fs; }\n"
    "}\n";

const char *kAudCopysignSource =
    "namespace aud_copysign\n"
    "{\n"
    "    int aces_sign(float x)\n"
    "    {\n"
    "        int y;\n"
    "        if (x < 0)      y = -1;\n"
    "        else if (x > 0) y =  1;\n"
    "        else            y =  0;\n"
    "        return y;\n"
    "    }\n"
    "    float aces_copysign(float x, float y)\n"
    "    { return aces_sign(y) * fabs(x); }\n"
    "    void compute(input varying float a, input varying float b,\n"
    "                 input varying int i, output varying float out)\n"
    "    { out = aces_copysign(a, b); }\n"
    "}\n";

const char *kAudCeilSource =
    "namespace aud_ceil\n"
    "{\n"
    "    float aces_ceil(float x) { return floor(x + 1.0); }\n"
    "    void compute(input varying float a, input varying float b,\n"
    "                 input varying int i, output varying float out)\n"
    "    { out = aces_ceil(a); }\n"
    "}\n";

// pow10: constrain exponent to [-4, 4] so 10^a stays well inside float32
// range and avoids any overflow-region divergence polluting the ULP
// measurement.
const char *kStdPow10Source =
    "namespace stdtrans_pow10\n"
    "{\n"
    "    void compute(input varying float a, input varying float b,\n"
    "                 input varying int i, output varying float out)\n"
    "    { out = pow10(a * 0.25); }\n"
    "}\n";

//
// `stdvecmat` exercises the vec3/matrix stdlib primitives: mult_f3_f33,
// mult_f_f3, add_f3_f3, sub_f3_f3, cross_f3_f3, dot_f3_f3, length_f3.
// The MSL helpers hand-code the identical mul/add sequence that Imath's
// V3f / M33f operators use on the CPU, so 0-ULP parity is the target.
// The compute function chains every primitive into one scalar output so
// a regression in any helper surfaces in this single fixture.
//
const char *kStdVecMatSource =
    "namespace stdvecmat\n"
    "{\n"
    "    const float kMatrix[3][3] = {{1.0, 0.5, 0.25},\n"
    "                                 {0.125, 1.0, 0.0625},\n"
    "                                 {0.03125, 0.015625, 1.0}};\n"
    "\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    "        float fi = i;\n"
    "        float v[3] = {a, b, fi * 0.1};\n"
    "        float w[3] = {b, fi * 0.05, a * 0.5};\n"
    "\n"
    "        float vm[3]    = mult_f3_f33(v, kMatrix);\n"
    "        float scaled[3] = mult_f_f3(0.75, vm);\n"
    "        float sum[3]   = add_f3_f3(scaled, w);\n"
    "        float diff[3]  = sub_f3_f3(sum, v);\n"
    "        float xp[3]    = cross_f3_f3(diff, v);\n"
    "        float d        = dot_f3_f3(xp, w);\n"
    "        float L        = length_f3(xp);\n"
    "\n"
    "        float mScaled[3][3] = mult_f_f33(fi * 0.0625, kMatrix);\n"
    "        float mT[3][3]      = transpose_f33(kMatrix);\n"
    "        float mSum[3][3]    = add_f33_f33(mScaled, mT);\n"
    "        float mProd[3][3]   = mult_f33_f33(mSum, kMatrix);\n"
    "        float mv[3]         = mult_f3_f33(xp, mProd);\n"
    "        out = d + L + mv[0] + mv[1] + mv[2];\n"
    "    }\n"
    "}\n";

//
// Matrix 4x4 + affine-vector helpers. Same shape as the
// 3x3 fixture above.
//
const char *kStdMat44Source =
    "namespace stdmat44\n"
    "{\n"
    "    const float kM[4][4] = {{1.0,    0.5,   0.25,  0.125},\n"
    "                            {0.0625, 1.0,   0.03125, 0.015625},\n"
    "                            {0.5,    0.25,  1.0,   0.5},\n"
    "                            {0.125,  0.0625, 0.03125, 1.0}};\n"
    "\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    "        float fi = i;\n"
    "        float v[3] = {a, b, fi * 0.1};\n"
    "        float mScaled[4][4] = mult_f_f44(fi * 0.015625, kM);\n"
    "        float mT[4][4]      = transpose_f44(kM);\n"
    "        float mSum[4][4]    = add_f44_f44(mScaled, mT);\n"
    "        float mProd[4][4]   = mult_f44_f44(mSum, kM);\n"
    "        float mv[3]         = mult_f3_f44(v, mProd);\n"
    "        out = mv[0] + mv[1] + mv[2];\n"
    "    }\n"
    "}\n";

//
// Print / assert fixtures. Print is a declared no-op on the Metal
// backend; this fixture just verifies that a program containing one
// call of each `print_*` overload (including `print_string`) still
// parses, compiles, and produces bit-exact output vs. the CPU backend
// for the non-print arithmetic in the rest of the program. The stderr
// noise from the CPU's real print output is harmless here — ctest
// captures and shows it only on failure.
//
// Assert-true fixture mirrors the same structure but routes through
// `REQUIRE(cond)` with a condition that is always true. The error flag
// buffer stays zero and the dispatch completes normally. A separate
// dedicated test exercises the false-condition path end-to-end and
// asserts that `MetalFunctionCall::callFunction` throws.
//
const char *kStdPrintSource =
    "namespace stdprint\n"
    "{\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    "        print_float(a);\n"
    "        print_int(i);\n"
    "        print_bool(a > b);\n"
    "        print_string(\"hello\");\n"
    "        float fi = i;\n"
    "        out = a + b + fi * 0.25;\n"
    "    }\n"
    "}\n";

const char *kStdAssertTrueSource =
    "namespace stdasserttrue\n"
    "{\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    "        assert(a == a);\n"
    "        float fi = i;\n"
    "        out = a * 2.0 + b - fi * 0.125;\n"
    "    }\n"
    "}\n";

const char *kStdLimitsSource =
    "namespace stdlimits\n"
    "{\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    "        float r = 0.0;\n"
    "        if (isfinite_f(a))\n"
    "            r = r + 1.0;\n"
    "        if (isnormal_f(b))\n"
    "            r = r + 2.0;\n"
    "        if (isnan_f(a))\n"
    "            r = r + 4.0;\n"
    "        if (isinf_f(b))\n"
    "            r = r + 8.0;\n"
    "        float fi = i;\n"
    "        out = r + fi * 0.1;\n"
    "    }\n"
    "}\n";

//
// Half-precision exp/log fixtures. Each exercises one of the four
// `exp_h` / `log_h` / `log10_h` / `pow10_h` stdlib helpers the Metal
// backend ports bit-for-bit from `halfExpLog.h` via three precomputed
// `constant` tables. Target is 0 ULP because the GPU path reads the
// same table bits the CPU reads and applies the identical bit-casts;
// any drift would indicate the tables or the index arithmetic (for
// `exp_h`/`pow10_h`) diverged. Input ranges match the table domains:
//
//   * `exp_h`   — float in [-16, 11] (between log(HALF_MIN) ≈ -16.635
//                 and log(HALF_MAX) ≈ 11.090)
//   * `pow10_h` — float in [-4.8, 4.8] (same domain scaled by log10(e))
//   * `log_h` / `log10_h` — input is `half(a + 33.0)` so the table
//                 index always covers a finite positive half (avoiding
//                 the NaN/neg-inf table slots that would propagate as
//                 qNaN bits and stop `bitEqual` from matching on the
//                 non-NaN sample paths). The `+33` offset keeps the
//                 cast from rounding to zero on the small-magnitude
//                 random samples in `makeSamples`.
//
const char *kStdExpHSource =
    "namespace stdexph\n"
    "{\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    "        float fi = i;\n"
    "        half h = exp_h(a);\n"
    "        float hf = h;\n"
    "        out = hf + b * 0.0 + fi * 0.0;\n"
    "    }\n"
    "}\n";

const char *kStdPow10HSource =
    "namespace stdpow10h\n"
    "{\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    "        float fi = i;\n"
    "        half h = pow10_h(a * 0.3);\n"
    "        float hf = h;\n"
    "        out = hf + b * 0.0 + fi * 0.0;\n"
    "    }\n"
    "}\n";

const char *kStdLogHSource =
    "namespace stdlogh\n"
    "{\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    "        float fi = i;\n"
    "        half hx = a + 33.0;\n"
    "        out = log_h(hx) + b * 0.0 + fi * 0.0;\n"
    "    }\n"
    "}\n";

const char *kStdLog10HSource =
    "namespace stdlog10h\n"
    "{\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    "        float fi = i;\n"
    "        half hx = a + 33.0;\n"
    "        out = log10_h(hx) + b * 0.0 + fi * 0.0;\n"
    "    }\n"
    "}\n";

//
// pow_h(half, float) is composed on both backends as
// `exp_h(y * log_h(x))`. Input is `half(fabs(a)+1)` so log_h sees a
// finite positive half, and y is scaled to stay inside exp_h's domain.
//
const char *kStdPowHSource =
    "namespace stdpowh\n"
    "{\n"
    "    void compute(input varying float a,\n"
    "                 input varying float b,\n"
    "                 input varying int i,\n"
    "                 output varying float out)\n"
    "    {\n"
    "        float fi = i;\n"
    "        half hx = fabs(a) + 1.0;\n"
    "        out = pow_h(hx, b * 0.25) + fi * 0.0;\n"
    "    }\n"
    "}\n";

} // anonymous namespace

void
testMetalArithmetic()
{
    std::cout << "Testing Metal arithmetic / control-flow parity with SIMD"
              << std::endl;

    if (!Ctl::metalBackendAvailable()) {
        std::cout << "  Metal backend unavailable on this host — skipping."
                  << std::endl;
        return;
    }

    auto samples = makeSamples(1024);

    runFixture("arithmetic",
               "arith", kArithSource, "arith::scalar", samples);
    runFixture("branch + while",
               "control", kControlSource, "control::branch", samples);
    runFixture("early return",
               "control_early", kReturnSource, "control::early", samples);
    runFixture("function calls",
               "call", kCallSource, "call::caller", samples);
    runFixture("arrays + structs",
               "aggr", kAggregateSource, "aggr::compute", samples);
    runFixture("VSArray parameters",
               "vsa", kVSArraySource, "vsa::compute", samples);
    runFixture("stdlib lookup1D",
               "stdlkup1d", kStdLookup1DSource, "stdlkup1d::compute",
               samples);
    runFixture("stdlib lookupCubic1D",
               "stdcubic1d", kStdLookupCubic1DSource,
               "stdcubic1d::compute", samples);
    runFixture("stdlib interpolate1D",
               "stdinterp1d", kStdInterp1DSource,
               "stdinterp1d::compute", samples);
    runFixture("stdlib interpolateCubic1D",
               "stdinterpc1d", kStdInterpCubic1DSource,
               "stdinterpc1d::compute", samples);
    runFixture("stdlib lookup3D_f3",
               "stdlkup3df3", kStdLookup3DF3Source,
               "stdlkup3df3::compute", samples);
    runFixture("stdlib lookup3D_f",
               "stdlkup3df", kStdLookup3DFSource,
               "stdlkup3df::compute", samples);
    runFixture("stdlib lookup3D_h",
               "stdlkup3dh", kStdLookup3DHSource,
               "stdlkup3dh::compute", samples);
    runFixture("stdlib lookup3DTetra_f3",
               "stdlkup3dtf3", kStdLookup3DTetraF3Source,
               "stdlkup3dtf3::compute", samples);
    runFixture("stdlib lookup3DTetra_f",
               "stdlkup3dtf", kStdLookup3DTetraFSource,
               "stdlkup3dtf::compute", samples);
    runFixture("stdlib lookup3DTetra_h",
               "stdlkup3dth", kStdLookup3DTetraHSource,
               "stdlkup3dth::compute", samples);
    runFixture("stdlib LuvtoXYZ",
               "stdluv", kStdLuvToXyzSource,
               "stdluv::compute", samples);
    runFixture("stdlib LabtoXYZ",
               "stdlab", kStdLabToXyzSource,
               "stdlab::compute", samples);
    runFixture("module-level statics",
               "statics", kStaticsSource, "statics::compute", samples);
    runFixture("matrix constants",
               "matx", kMatrixSource, "matx::compute", samples);
    // `sqrt` is the only Accelerate-vectorized call in this fixture
    // (fabs/floor/fmod stay on scalar libm either way). Under the
    // default Apple build (`CTL_USE_ACCELERATE=ON`) the CPU path
    // runs `vvsqrtf`, whose per-function contract is <=1 ULP vs
    // Apple libm's sqrtf. Here `s = sqrt(|a| + 1)` is on the order
    // of 3, and the output expression folds `s` into a sum at
    // magnitude ~0.07 -- the relative-ULP-rescaling from s to out
    // amplifies 1 ULP on s into ~32 ULP on out. Threshold set to
    // 64 to leave 2x headroom over observed. See PRECISION.md for
    // the Accelerate/sleef coupling; to reproduce 0 ULP, build with
    // `-DCTL_USE_ACCELERATE=OFF -DCTL_USE_SLEEF=OFF`.
    ulpBoundedFixture("stdlib fabs/floor/sqrt/fmod",
                      "stdmath", kStdMathSource,
                      "stdmath::compute", samples, /*ULP*/ 64);
    runFixture("stdlib isfinite/isnormal/isnan/isinf",
               "stdlimits", kStdLimitsSource, "stdlimits::compute", samples);

    //
    // Half-precision exp/log table-lookup helpers. Target 0 ULP: GPU
    // reads the same precomputed tables the CPU reads, with identical
    // bit-casts and identical index arithmetic. A regression here
    // almost certainly means the emitted MSL `constant` tables drifted
    // from the C++ source, or the FP_CONTRACT OFF pragma stopped
    // preventing the compiler from contracting `x * 4094.98169f +
    // 68122.7031f` into a fused multiply-add that rounds differently
    // from the CPU's discrete mul+add.
    //
    runFixture("stdlib exp_h",
               "stdexph", kStdExpHSource,
               "stdexph::compute", samples);
    runFixture("stdlib pow10_h",
               "stdpow10h", kStdPow10HSource,
               "stdpow10h::compute", samples);
    runFixture("stdlib log_h",
               "stdlogh", kStdLogHSource,
               "stdlogh::compute", samples);
    runFixture("stdlib log10_h",
               "stdlog10h", kStdLog10HSource,
               "stdlog10h::compute", samples);
    runFixture("stdlib pow_h",
               "stdpowh", kStdPowHSource,
               "stdpowh::compute", samples);
    runFixture("stdlib print_* (no-op on GPU)",
               "stdprint", kStdPrintSource, "stdprint::compute", samples);
    runFixture("stdlib assert(true) passthrough",
               "stdasserttrue", kStdAssertTrueSource,
               "stdasserttrue::compute", samples);

    //
    // Threshold-bounded transcendentals. ULP bounds below are the sum
    // of two per-function <=1 ULP sources: the Metal precise::/FreeBSD
    // port (vs libm reference) and the CPU vector backend (Accelerate
    // `vvexpf` etc. under the default Apple build, sleef `_u10` under
    // the default non-Apple build — both contractually <=1 ULP vs
    // libm). Worst-case GPU-vs-CPU drift for a single call is ~2 ULP;
    // chains grow proportionally with depth. See PRECISION.md
    // "CPU-side backend affects measured parity" for the rebuild
    // recipe that isolates GPU-only drift (<=1 ULP).
    //
    std::cout << "Checking Metal transcendental divergence vs CPU backend"
              << std::endl;
    ulpBoundedFixture("stdlib exp (FreeBSD s_expf.c port)",
                      "stdtrans_exp", kStdExpSource,
                      "stdtrans_exp::compute", samples,
                      /*maxUlpAllowed=*/2);
    ulpBoundedFixture("stdlib log (FreeBSD s_logf.c port)",
                      "stdtrans_log", kStdLogSource,
                      "stdtrans_log::compute", samples,
                      /*maxUlpAllowed=*/2);

    //
    // Additional scalar transcendentals. Thresholds widened to 2x the
    // original Metal-vs-libm baseline to accommodate the 1 ULP/function
    // CPU backend drift contributed by Accelerate (vForce) or sleef
    // `_u10`. Tightening after a measured run on every platform is
    // always acceptable; rebuilding with both backends OFF recovers
    // the original <=1 ULP bound.
    //
    ulpBoundedFixture("log10 (FreeBSD s_log10f.c port)",
                      "stdtrans_log10", kStdLog10Source,
                      "stdtrans_log10::compute", samples, /*ULP*/ 2);
    ulpBoundedFixture("pow",   "stdtrans_pow",   kStdPowSource,
                      "stdtrans_pow::compute",   samples, /*ULP*/ 2);
    ulpBoundedFixture("sin (FreeBSD k_sinf.c FP32 kernel + Cody-Waite)",
                      "stdtrans_sin",   kStdSinSource,
                      "stdtrans_sin::compute",   samples, /*ULP*/ 2);
    ulpBoundedFixture("cos (FreeBSD k_cosf.c FP32 kernel + Cody-Waite)",
                      "stdtrans_cos",   kStdCosSource,
                      "stdtrans_cos::compute",   samples, /*ULP*/ 2);
    ulpBoundedFixture("tan (sin/cos composition on reduced argument)",
                      "stdtrans_tan",   kStdTanSource,
                      "stdtrans_tan::compute",   samples, /*ULP*/ 4);
    ulpBoundedFixture("asin",  "stdtrans_asin",  kStdAsinSource,
                      "stdtrans_asin::compute",  samples, /*ULP*/ 2);
    ulpBoundedFixture("acos",  "stdtrans_acos",  kStdAcosSource,
                      "stdtrans_acos::compute",  samples, /*ULP*/ 2);
    ulpBoundedFixture("atan",  "stdtrans_atan",  kStdAtanSource,
                      "stdtrans_atan::compute",  samples, /*ULP*/ 2);
    ulpBoundedFixture("atan2", "stdtrans_atan2", kStdAtan2Source,
                      "stdtrans_atan2::compute", samples, /*ULP*/ 4);
    ulpBoundedFixture("sinh (Taylor + ctl_stdlib_exp composition)",
                      "stdtrans_sinh",  kStdSinhSource,
                      "stdtrans_sinh::compute",  samples, /*ULP*/ 2);
    ulpBoundedFixture("cosh (Taylor + ctl_stdlib_exp composition)",
                      "stdtrans_cosh",  kStdCoshSource,
                      "stdtrans_cosh::compute",  samples, /*ULP*/ 2);
    ulpBoundedFixture("tanh (Taylor + ctl_stdlib_exp composition)",
                      "stdtrans_tanh",  kStdTanhSource,
                      "stdtrans_tanh::compute",  samples, /*ULP*/ 2);

    ulpBoundedFixture("hypot", "stdtrans_hypot", kStdHypotSource,
                      "stdtrans_hypot::compute", samples, /*ULP*/ 2);
    ulpBoundedFixture("pow10", "stdtrans_pow10", kStdPow10Source,
                      "stdtrans_pow10::compute", samples, /*ULP*/ 2);

    //
    // ACES v2 Utilities helpers (round, log2, sign, copysign, ceil).
    // Not stdlib entries — ACES v2 defines them on top of primitives
    // the Metal backend already supports. 0-ULP bit-exact is the
    // target for each (log2 inherits `log`'s 1-ULP floor).
    //
    runFixture("aces round",    "aud_round",    kAudRoundSource,
               "aud_round::compute",    samples);
    //
    // `log2` is computed as `log(x) / log(2.0)`; both calls inherit the
    // 1-ULP FreeBSD-log port floor, and the subsequent division can
    // round up to one more ULP. Measured 2 ULP / 60 / 1024.
    //
    ulpBoundedFixture("aces log2",
                      "aud_log2",     kAudLog2Source,
                      "aud_log2::compute",     samples, /*ULP*/ 4);
    runFixture("aces sign",     "aud_sign",     kAudSignSource,
               "aud_sign::compute",     samples);
    runFixture("aces copysign", "aud_copysign", kAudCopysignSource,
               "aud_copysign::compute", samples);
    runFixture("aces ceil",     "aud_ceil",     kAudCeilSource,
               "aud_ceil::compute",     samples);

    runFixture("stdlib vec3/matrix",
               "stdvecmat", kStdVecMatSource, "stdvecmat::compute",
               samples);

    runFixture("stdlib matrix 4x4",
               "stdmat44", kStdMat44Source, "stdmat44::compute",
               samples);

    //
    // Colorspace forward transforms that call `pow(x, 1/3)`: drift
    // inherited from `metal::precise::pow` (1 ULP floor), widened up
    // to a small multiple by subsequent multiply/add chains. Threshold
    // tuned empirically from the 1024-sample run on M4 Max.
    //
    ulpBoundedFixture("stdlib XYZtoLuv",
                      "stdxyzluv", kStdXyzToLuvSource,
                      "stdxyzluv::compute", samples, /*ULP*/ 8);
    ulpBoundedFixture("stdlib XYZtoLab",
                      "stdxyzlab", kStdXyzToLabSource,
                      "stdxyzlab::compute", samples, /*ULP*/ 16);

    //
    // Chromaticities-taking matrix builders. Both MSL helpers mirror
    // the Clang-emitted ARM64 fmul/fmadd/fnmadd/fmsub sequence for
    // `Ctl::RGBtoXYZ` / `Imath::Matrix44::inverse` (see
    // `/tmp/rgbxyz.s` / `/tmp/xyzrgb.s`) via explicit `metal::fma`
    // calls, restoring the single-rounded-fused shape under MSL
    // FP_CONTRACT OFF. Bit-exact parity is required — any regression
    // in the fused-op placement should fail CI immediately.
    //
    runFixture("stdlib RGBtoXYZ",
               "stdrgbxyz", kStdRgbToXyzSource,
               "stdrgbxyz::compute", samples);
    runFixture("stdlib XYZtoRGB",
               "stdxyzrgb", kStdXyzToRgbSource,
               "stdxyzrgb::compute", samples);

    //
    // Matrix inversion. Imath's affine fast path is the same 3x3
    // adjugate algebra XYZtoRGB reuses; `invert_f44` non-affine
    // exercises the full Gauss-Jordan elimination. Thresholds
    // start generous (naive line-for-line MSL port without
    // fma-fusion matching) and are tightened below after measuring.
    //
    ulpBoundedFixture("stdlib invert_f33",
                      "stdinvf33", kStdInvertF33Source,
                      "stdinvf33::compute", samples, /*ULP*/ 0);
    ulpBoundedFixture("stdlib invert_f44 (affine)",
                      "stdinvf44", kStdInvertF44Source,
                      "stdinvf44::compute", samples, /*ULP*/ 0);
    ulpBoundedFixture("stdlib invert_f44 (gjInverse)",
                      "stdinvf44gj", kStdInvertF44GjSource,
                      "stdinvf44gj::compute", samples, /*ULP*/ 0);
}
