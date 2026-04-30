///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

//
// CPU-vs-GPU benchmark + regression guard. Runs a matrix of
// (program complexity × sample count) on both `SimdInterpreter` and
// `MetalInterpreter`, times each, emits a JSON summary to
// `benchmark_results.json` in the current working directory (so CI can
// upload it as an artifact), and fails the test if the warm-dispatch
// GPU/CPU ratio at the largest compute-heavy config drops below a
// configurable floor.
//
// This catches two classes of regression automatically:
//   1. Codegen changes that silently slow the GPU path down.
//   2. Stdlib implementations that the compiler fails to vectorize
//      efficiently on either backend.
//
// Skip gracefully when `Ctl::metalBackendAvailable()` is false — CI
// macOS runners without an accessible GPU still build and sanity-check
// the link without running the benchmark.
//

#include "testMetalBenchmark.h"

#include <CtlFunctionCall.h>
#include <CtlMetalDevice.h>
#include <CtlMetalInterpreter.h>
#include <CtlSimdInterpreter.h>

#include <algorithm>
#include "testRequire.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#import <Foundation/Foundation.h>

namespace {

using clock_t_ = std::chrono::steady_clock;

double
msBetween(clock_t_::time_point a, clock_t_::time_point b)
{
    using ms_t = std::chrono::duration<double, std::milli>;
    return std::chrono::duration_cast<ms_t>(b - a).count();
}

//
// A benchmark config: a CTL program plus the input/output arg names the
// harness should bind, plus a name for the JSON and a sample count.
//
struct BenchmarkProgram
{
    const char *name;        // JSON key / log label
    const char *moduleName;  // CTL module name
    const char *source;      // inline CTL source (NULL if loaded from sourcePath)
    const char *sourcePath;  // on-disk .ctl path relative to CMAKE_CURRENT_BINARY_DIR
                             // (NULL if `source` is inline). For fixtures too
                             // large to inline as C strings (e.g. full ACES v2
                             // pipeline, ~1800 lines).
    const char *function;    // fully-qualified name passed to newFunctionCall
    size_t      numInputs;   // 1, 2, or 3
    size_t      numOutputs;  // 1 or 3
};

//
// Unity: pure dispatch overhead. Output = input. Measures the fixed cost
// of getting one sample through the backend, not the math.
//
static const char kUnitySource[] =
    "namespace bench_unity\n"
    "{\n"
    "    void compute(output varying float o,\n"
    "                 input  varying float a,\n"
    "                 input  varying float b)\n"
    "    { o = a; }\n"
    "}\n";

//
// Matrix-3x3 transform: moderate compute, exercises the V3f/M33f
// helpers in the stdlib but nothing transcendental. Three ins, three
// outs — representative of a color-space matrix stage.
//
static const char kMatrix33Source[] =
    "namespace bench_mat33\n"
    "{\n"
    "    void compute(output varying float oR,\n"
    "                 output varying float oG,\n"
    "                 output varying float oB,\n"
    "                 input  varying float iR,\n"
    "                 input  varying float iG,\n"
    "                 input  varying float iB)\n"
    "    {\n"
    "        oR = 0.4124564 * iR + 0.3575761 * iG + 0.1804375 * iB;\n"
    "        oG = 0.2126729 * iR + 0.7151522 * iG + 0.0721750 * iB;\n"
    "        oB = 0.0193339 * iR + 0.1191920 * iG + 0.9503041 * iB;\n"
    "    }\n"
    "}\n";

//
// Transcendental: exercises the FreeBSD-ported exp/log kernels plus a
// pow10 and a trig call — the heaviest compute pattern CTL programs
// typically see, and the regime where GPU parallelism should win
// decisively over CPU SIMD.
//
static const char kTranscendentalSource[] =
    "namespace bench_xcend\n"
    "{\n"
    "    void compute(output varying float o,\n"
    "                 input  varying float a,\n"
    "                 input  varying float b)\n"
    "    {\n"
    "        float x = fabs(a) + 1.0;\n"
    "        float y = log(x) * b;\n"
    "        o = exp(y) + sin(a) * cos(b);\n"
    "    }\n"
    "}\n";

//
// ACES v2 Hellwig2022 JMh forward — a self-contained port of the CAM
// pass from `Lib.Academy.OutputTransform.ctl`
// (ampas/aces-dev @ v2-dev-release-2). Captures the compute-heaviest
// real-world per-pixel stage of the ACES v2 ODT: one 3x3 matrix
// multiply, three `spow` with exponent 0.42, three divisions, an
// `atan2`, a `sqrt`, and a dynamic-exponent `pow`.
//
// `MATRIX_16` is precomputed from CAM16_PRI via the CPU reference
// `XYZtoRGB_f33(CAM16_PRI, 1.0)` and inlined as a literal — the
// Metal backend does not yet support host-side evaluation of
// module-init code that calls same-module user functions (as
// `generate_panlrcm` does in the full ACES v2 module). Everything
// else matches the reference byte-for-byte at algorithm level.
//
static const char kAcesV2JmhSource[] =
    "namespace bench_aces_v2_jmh\n"
    "{\n"
    "const float MATRIX_16[3][3] =\n"
    "{\n"
    "    { 0.364074498,  0.594700873,  0.0411012731 },\n"
    "    {-0.222245112,  1.0738554,    0.147945315  },\n"
    "    {-0.0020676204, 0.0488259755, 0.950387657  }\n"
    "};\n"
    "const float L_A = 100.0;\n"
    "const float Y_b = 20.0;\n"
    "const float ra  = 2.0;\n"
    "const float ba  = 0.05;\n"
    "\n"
    "float spow(float base, float exponent)\n"
    "{\n"
    "    if (base < 0.0 && exponent != floor(exponent)) return 0.0;\n"
    "    float s = 1.0; if (base < 0.0) s = -1.0;\n"
    "    return pow(fabs(base), exponent) * s;\n"
    "}\n"
    "\n"
    "float[3] padapt_fwd(float RGB[3], float F_L)\n"
    "{\n"
    "    float F_L_RGB[3];\n"
    "    F_L_RGB[0] = spow(F_L * fabs(RGB[0]) / 100.0, 0.42);\n"
    "    F_L_RGB[1] = spow(F_L * fabs(RGB[1]) / 100.0, 0.42);\n"
    "    F_L_RGB[2] = spow(F_L * fabs(RGB[2]) / 100.0, 0.42);\n"
    "    float s0 = 1.0; if (RGB[0] < 0.0) s0 = -1.0;\n"
    "    float s1 = 1.0; if (RGB[1] < 0.0) s1 = -1.0;\n"
    "    float s2 = 1.0; if (RGB[2] < 0.0) s2 = -1.0;\n"
    "    float RGB_c[3];\n"
    "    RGB_c[0] = (400.0 * s0 * F_L_RGB[0]) / (27.13 + F_L_RGB[0]);\n"
    "    RGB_c[1] = (400.0 * s1 * F_L_RGB[1]) / (27.13 + F_L_RGB[1]);\n"
    "    RGB_c[2] = (400.0 * s2 * F_L_RGB[2]) / (27.13 + F_L_RGB[2]);\n"
    "    return RGB_c;\n"
    "}\n"
    "\n"
    "float wrap360(float h)\n"
    "{\n"
    "    float y = fmod(h, 360.0);\n"
    "    if (y < 0.0) y = y + 360.0;\n"
    "    return y;\n"
    "}\n"
    "\n"
    "void compute(output varying float oR,\n"
    "             output varying float oG,\n"
    "             output varying float oB,\n"
    "             input  varying float iR,\n"
    "             input  varying float iG,\n"
    "             input  varying float iB)\n"
    "{\n"
    "    float XYZ_w[3] = { 95.045593, 100.0, 108.905776 };\n"
    "    float XYZ[3]   = { iR, iG, iB };\n"
    "    float c = 0.59; float N_c = 0.9;\n"
    "    float Y_w = XYZ_w[1];\n"
    "    float RGB_w[3] = mult_f3_f33(XYZ_w, MATRIX_16);\n"
    "    float k  = 1.0 / (5.0 * L_A + 1.0);\n"
    "    float k4 = pow(k, 4.0);\n"
    "    float F_L = 0.2 * k4 * (5.0 * L_A)\n"
    "              + 0.1 * pow(1.0 - k4, 2.0) * spow(5.0 * L_A, 1.0/3.0);\n"
    "    float n = Y_b / Y_w;\n"
    "    float z = 1.48 + sqrt(n);\n"
    "    float D_RGB[3];\n"
    "    D_RGB[0] = Y_w / RGB_w[0];\n"
    "    D_RGB[1] = Y_w / RGB_w[1];\n"
    "    D_RGB[2] = Y_w / RGB_w[2];\n"
    "    float RGB_wc[3];\n"
    "    RGB_wc[0] = D_RGB[0] * RGB_w[0];\n"
    "    RGB_wc[1] = D_RGB[1] * RGB_w[1];\n"
    "    RGB_wc[2] = D_RGB[2] * RGB_w[2];\n"
    "    float RGB_aw[3] = padapt_fwd(RGB_wc, F_L);\n"
    "    float A_w = ra * RGB_aw[0] + RGB_aw[1] + ba * RGB_aw[2];\n"
    "    float RGB[3] = mult_f3_f33(XYZ, MATRIX_16);\n"
    "    float RGB_c[3];\n"
    "    RGB_c[0] = D_RGB[0] * RGB[0];\n"
    "    RGB_c[1] = D_RGB[1] * RGB[1];\n"
    "    RGB_c[2] = D_RGB[2] * RGB[2];\n"
    "    float RGB_a[3] = padapt_fwd(RGB_c, F_L);\n"
    "    float a = RGB_a[0] - 12.0 * RGB_a[1] / 11.0 + RGB_a[2] / 11.0;\n"
    "    float b = (RGB_a[0] + RGB_a[1] - 2.0 * RGB_a[2]) / 9.0;\n"
    "    float hr = atan2(b, a);\n"
    "    float h = wrap360(hr * 180.0 / M_PI);\n"
    "    float A = ra * RGB_a[0] + RGB_a[1] + ba * RGB_a[2];\n"
    "    float J = 100.0 * pow(A / A_w, c * z);\n"
    "    float M = 43.0 * N_c * sqrt(a * a + b * b);\n"
    "    if (J == 0.0) M = 0.0;\n"
    "    oR = J; oG = M; oB = h;\n"
    "}\n"
    "}\n";

static const BenchmarkProgram kPrograms[] =
{
    { "unity",          "bench_unity",         kUnitySource,         nullptr,
      "bench_unity::compute",                  2, 1 },
    { "matrix33",       "bench_mat33",         kMatrix33Source,      nullptr,
      "bench_mat33::compute",                  3, 3 },
    { "transcendental", "bench_xcend",         kTranscendentalSource, nullptr,
      "bench_xcend::compute",                  2, 1 },
    { "aces_v2_jmh",    "bench_aces_v2_jmh",   kAcesV2JmhSource,     nullptr,
      "bench_aces_v2_jmh::compute",            3, 3 },
    //
    // Full ACES v2 OutputTransform (Rec.709): aces_to_JMh →
    // tonemapAndCompress_fwd → gamutMap_fwd → JMh_to_output_XYZ.
    // Loaded from aces_combined.ctl (CMake `file(COPY)` drops it in the
    // test binary dir) — roughly 1800 lines, too large to inline as a
    // C string literal. This is the whole pipeline the real CI parity
    // test drives; timing it here catches any dispatch-path regression
    // introduced by the gamut-mapper landing (Apr 2026).
    //
    { "aces_v2_full",   "aces_combined",       nullptr,              "aces_combined.ctl",
      "::main",                                3, 3 },
};

//
// Set up all input args with deterministic random floats.  The harness
// reuses the same input buffers across timing iterations so each
// iteration measures only the kernel's own work.
//
template <class Interp>
void
primeInterpreter(Interp &interp,
                 const BenchmarkProgram &prog,
                 Ctl::FunctionCallPtr &fn,
                 std::vector<std::vector<float>> &inputs,
                 size_t numSamples)
{
    if (prog.source) {
        interp.loadModule(prog.moduleName,
                          std::string(prog.moduleName) + ".ctl",
                          prog.source);
    } else {
        // File-backed fixture. The CMake `file(COPY ...)` rule places
        // it next to the test binary, so `loadFile` resolves via the
        // cwd that ctest launches from. Two-arg overload pins the
        // module name so `::main` resolves under that module regardless
        // of any autoloaded filename-based name.
        interp.loadFile(prog.sourcePath, prog.moduleName);
    }
    fn = interp.newFunctionCall(prog.function);
    REQUIRE(fn);
    REQUIRE(fn->numInputArgs()  == prog.numInputs);
    REQUIRE(fn->numOutputArgs() == prog.numOutputs);

    std::mt19937 rng(0xBE1CE17);
    std::uniform_real_distribution<float> d(-2.0f, 2.0f);

    inputs.assign(prog.numInputs, {});
    for (size_t i = 0; i < prog.numInputs; ++i) {
        inputs[i].resize(numSamples);
        for (size_t s = 0; s < numSamples; ++s)
            inputs[i][s] = d(rng);
    }
}

//
// Feed `numSamples` through the FunctionCall in batches of at most
// `interp.maxSamples()` each, matching how ctlrender / ctlrender-metal
// drive real transforms. `SimdInterpreter::maxSamples() == 4096` and
// `MetalInterpreter::maxSamples() == 65536`, so any benchmark size
// above those floors MUST batch — a single `callFunction(N)` with
// N > maxSamples stomps the interpreter's internal register arrays
// and corrupts the ObjC runtime on the Metal side.
//
template <class Interp>
void
runOneIteration(Interp &interp,
                Ctl::FunctionCallPtr &fn,
                const std::vector<std::vector<float>> &inputs,
                size_t numSamples)
{
    const size_t batch = interp.maxSamples();
    size_t offset = 0;
    while (offset < numSamples) {
        size_t n = std::min(batch, numSamples - offset);
        for (size_t i = 0; i < fn->numInputArgs(); ++i) {
            Ctl::FunctionArgPtr arg = fn->inputArg(i);
            REQUIRE(arg->isVarying());
            std::memcpy(arg->data(),
                        inputs[i].data() + offset,
                        n * sizeof(float));
        }
        fn->callFunction(n);
        offset += n;
    }
}

struct Timing
{
    double cold_ms;
    double warm_min_ms;
    double warm_mean_ms;
    double warm_median_ms;
};

template <class Interp>
Timing
timeBackend(const BenchmarkProgram &prog,
            size_t numSamples,
            int warmIterations)
{
    Timing t;

    // Single interpreter, reused for both cold and warm timing. Mirror
    // `ctlrender-metal/benchmark.cc`'s shape: cold = first dispatch
    // (includes interpreter construction + load + compile + first PSO);
    // warm = subsequent dispatches on the same interpreter after one
    // explicit warmup. Constructing a second interpreter back-to-back
    // for the warm phase was observed to destabilize the ObjC runtime
    // on macOS 15, even with @autoreleasepool around the ctor.
    Interp interp;
    Ctl::FunctionCallPtr fn;
    std::vector<std::vector<float>> inputs;

    auto t0 = clock_t_::now();
    primeInterpreter(interp, prog, fn, inputs, numSamples);
    runOneIteration(interp, fn, inputs, numSamples);
    auto t1 = clock_t_::now();
    t.cold_ms = msBetween(t0, t1);

    // A second warmup dispatch ensures the Metal PSO is fully created
    // before the timing loop (the first dispatch's cost is already
    // captured in `cold_ms`).
    runOneIteration(interp, fn, inputs, numSamples);

    std::vector<double> samples;
    samples.reserve(warmIterations);
    for (int i = 0; i < warmIterations; ++i) {
        auto ti0 = clock_t_::now();
        runOneIteration(interp, fn, inputs, numSamples);
        auto ti1 = clock_t_::now();
        samples.push_back(msBetween(ti0, ti1));
    }
    std::sort(samples.begin(), samples.end());
    t.warm_min_ms    = samples.front();
    t.warm_median_ms = samples[samples.size() / 2];
    t.warm_mean_ms   =
        std::accumulate(samples.begin(), samples.end(), 0.0)
        / double(samples.size());
    return t;
}

double
readFloorOverride(double defaultFloor)
{
    const char *env = std::getenv("CTL_METAL_BENCHMARK_FLOOR");
    if (!env || !*env) return defaultFloor;
    char *end = nullptr;
    double v = std::strtod(env, &end);
    if (end == env || v <= 0.0) return defaultFloor;
    return v;
}

} // namespace

void
testMetalBenchmark()
{
    std::cout << "Benchmarking CPU SIMD vs Metal GPU" << std::endl;

    if (!Ctl::metalBackendAvailable()) {
        std::cout << "  Metal backend unavailable on this host — skipping."
                  << std::endl;
        return;
    }

    // Sizes chosen to span the range from per-pixel-overhead-dominated
    // (64K ≈ 256² image) to memory-bandwidth-dominated (1M ≈ 1024²).
    const size_t kSizes[] = { 64u * 1024u, 256u * 1024u, 1024u * 1024u };
    const size_t kNumSizes = sizeof(kSizes) / sizeof(kSizes[0]);
    const int kWarmIters = 10;

    // `CTL_METAL_BENCHMARK_FLOOR` lets local tuning tighten the guard;
    // default 1.0 means "GPU warm dispatch at the largest compute-heavy
    // config must not be slower than CPU". Loose because GitHub Apple
    // Silicon runners have highly variable GPU sharing.
    const double floorRatio = readFloorOverride(1.0);

    std::ofstream json("benchmark_results.json");
    json.precision(3);
    json << std::fixed;
    json << "{\n  \"host\": \"metal-available\",\n";
    json << "  \"floor_ratio\": " << floorRatio << ",\n";
    json << "  \"warm_iterations\": " << kWarmIters << ",\n";
    json << "  \"programs\": [\n";

    double worstHeavyRatio = 1e9;
    size_t worstHeavySize  = 0;
    const size_t numProgs = sizeof(kPrograms) / sizeof(kPrograms[0]);

    for (size_t p = 0; p < numProgs; ++p) {
        const BenchmarkProgram &prog = kPrograms[p];
        json << "    {\n      \"name\": \"" << prog.name << "\",\n";
        json << "      \"sizes\": [\n";

        for (size_t i = 0; i < kNumSizes; ++i) {
            size_t n = kSizes[i];

            Timing cpu, gpu;

            // Drain autoreleased ObjC temporaries between configs. Both
            // the MetalInterpreter ctor and MetalPipeline::dispatch leak
            // autoreleased NSObjects at their boundary; without draining
            // between configs, the runtime's method cache gets corrupted
            // after ~20 interpreter construct/destroy cycles.
            @autoreleasepool {
                cpu = timeBackend<Ctl::SimdInterpreter>(prog, n, kWarmIters);
                gpu = timeBackend<Ctl::MetalInterpreter>(prog, n, kWarmIters);
            }

            double ratio = (gpu.warm_mean_ms > 0.0)
                ? cpu.warm_mean_ms / gpu.warm_mean_ms
                : 0.0;

            std::cout << "  " << prog.name << " n=" << n
                      << ": cpu_warm=" << cpu.warm_mean_ms << "ms"
                      << " gpu_warm=" << gpu.warm_mean_ms << "ms"
                      << " speedup=" << ratio << "x"
                      << std::endl;

            // The floor is checked only at the **largest** size of the
            // compute-heavy programs. Dispatch-overhead is amortized at
            // 1 Mpx; at 64 K every kernel is dispatch-bound and can
            // legitimately favor CPU. In particular, with the
            // Accelerate-ON CPU default, `vvexpf`/`vvlogf` make the
            // small-N CPU path ~2x faster, dropping the 64 K
            // transcendental crossover below 1x — not a Metal codegen
            // regression. Both `transcendental` and `aces_v2_full` are
            // arithmetic-bound at 1 Mpx; either one falling below the
            // floor at that size trips the guard.
            const bool isHeavy =
                std::string(prog.name) == "transcendental" ||
                std::string(prog.name) == "aces_v2_full";
            const bool isLargestSize = (n == kSizes[kNumSizes - 1]);
            if (isHeavy && isLargestSize && ratio < worstHeavyRatio) {
                worstHeavyRatio = ratio;
                worstHeavySize  = n;
            }

            json << "        {\n";
            json << "          \"samples\": " << n << ",\n";
            json << "          \"cpu_cold_ms\": " << cpu.cold_ms << ",\n";
            json << "          \"cpu_warm_min_ms\": " << cpu.warm_min_ms << ",\n";
            json << "          \"cpu_warm_mean_ms\": " << cpu.warm_mean_ms << ",\n";
            json << "          \"cpu_warm_median_ms\": " << cpu.warm_median_ms << ",\n";
            json << "          \"gpu_cold_ms\": " << gpu.cold_ms << ",\n";
            json << "          \"gpu_warm_min_ms\": " << gpu.warm_min_ms << ",\n";
            json << "          \"gpu_warm_mean_ms\": " << gpu.warm_mean_ms << ",\n";
            json << "          \"gpu_warm_median_ms\": " << gpu.warm_median_ms << ",\n";
            json << "          \"speedup_warm_mean\": " << ratio << "\n";
            json << "        }" << (i + 1 < kNumSizes ? "," : "") << "\n";
        }

        json << "      ]\n    }" << (p + 1 < numProgs ? "," : "") << "\n";
    }

    json << "  ],\n";
    json << "  \"guard\": {\n";
    json << "    \"programs\": [\"transcendental\", \"aces_v2_full\"],\n";
    json << "    \"worst_size\": " << worstHeavySize << ",\n";
    json << "    \"worst_speedup\": " << worstHeavyRatio << ",\n";
    json << "    \"floor_ratio\": " << floorRatio << "\n";
    json << "  }\n}\n";
    json.close();

    // The floor is the regression guard. If GPU codegen gets worse in a
    // way that makes compute-heavy programs dispatch-bound, this is the
    // signal. Keep the guard loose by default (see `floorRatio`
    // comment); tighten via env-var on a stable host.
    if (worstHeavyRatio < floorRatio) {
        std::cerr << "testMetalBenchmark: compute-heavy warm-dispatch "
                  << "speedup " << worstHeavyRatio << "x at n="
                  << worstHeavySize << " fell below floor "
                  << floorRatio << "x across {transcendental, aces_v2_full}. "
                  << "See PRECISION.md and benchmark_results.json."
                  << std::endl;
        std::abort();
    }
}
