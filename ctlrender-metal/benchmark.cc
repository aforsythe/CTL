///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#include "benchmark.hh"
#include "dpx_file.hh"
#include "tiff_file.hh"
#include "exr_file.hh"
#include <CtlMetalInterpreter.h>
#include <CtlStdType.h>
#include <Iex.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

namespace {

using clock_t_ = std::chrono::steady_clock;
using ms_t = std::chrono::duration<double, std::milli>;

static double ms_between(clock_t_::time_point a, clock_t_::time_point b)
{
    return std::chrono::duration_cast<ms_t>(b - a).count();
}

static void populate_inputs(CTLResults &results,
                            const ctl::dpx::fb<float> &fb)
{
    if (fb.depth() > 0)
        results.push_back(mkresult("rIn", "c00In", fb, 0));
    if (fb.depth() > 1)
        results.push_back(mkresult("gIn", "c01In", fb, 1));
    if (fb.depth() > 2)
        results.push_back(mkresult("bIn", "c02In", fb, 2));
    if (fb.depth() > 3)
        results.push_back(mkresult("aIn", "c03In", fb, 3));

    char name[16];
    for (uint32_t j = 4; j < fb.depth(); j++)
    {
        std::memset(name, 0, sizeof(name));
        std::snprintf(name, sizeof(name) - 1, "c%02dIn", j);
        results.push_back(mkresult(name, NULL, fb, j));
    }
}

static void apply_params(CTLResults &results,
                         const ctl_operation_t &op,
                         const CTLParameters &globals)
{
    for (CTLParameters::const_iterator p = globals.begin(); p != globals.end(); ++p)
        add_parameter_value_to_ctl_results(&results, *p);
    for (CTLParameters::const_iterator p = op.local.begin(); p != op.local.end(); ++p)
        add_parameter_value_to_ctl_results(&results, *p);
}

//
// Derive the CTL "module" name (basename with extension stripped) from a
// file path. CTL's convention is that the script's primary function is
// named either `main` or the module name — so the benchmark tries both.
//
static std::string derive_module_name(const char *filename)
{
    std::string path(filename);
    std::string::size_type slash = path.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    std::string::size_type dot = base.find_last_of('.');
    return (dot == std::string::npos) ? base : base.substr(0, dot);
}

//
// Runs the function-call batch loop over a single transform — the part
// that actually dispatches work on the GPU. Does NOT recompile: assumes
// the caller has already done loadFile() and newFunctionCall(), and
// holds the interpreter and FunctionCall alive across iterations. This
// is the "warm" path used inside the benchmark timing loop.
//
static void dispatch_batched(Ctl::Interpreter &interpreter,
                             Ctl::FunctionCallPtr fn,
                             CTLResults &ctl_results,
                             size_t count)
{
    CTLResults new_results;
    size_t offset = 0;
    while (offset < count)
    {
        size_t pass = interpreter.maxSamples();
        if (pass > (count - offset))
            pass = count - offset;

        for (size_t i = 0; i < fn->numInputArgs(); i++)
        {
            Ctl::FunctionArgPtr arg = fn->inputArg(i);
            set_ctl_function_argument_from_ctl_results(&arg, ctl_results, offset, pass);
        }

        fn->callFunction(pass);

        for (size_t i = 0; i < fn->numOutputArgs(); i++)
        {
            set_ctl_results_from_ctl_function_argument(&new_results,
                                                       fn->outputArg(i),
                                                       offset, pass, count);
        }

        offset += pass;
    }
    ctl_results = new_results;
}

struct stats_t
{
    double min;
    double mean;
    double median;
    double max;
    double p95;
};

static stats_t compute_stats(std::vector<double> v)
{
    stats_t s;
    std::sort(v.begin(), v.end());
    s.min = v.front();
    s.max = v.back();
    s.median = v[v.size() / 2];
    s.mean = std::accumulate(v.begin(), v.end(), 0.0) / v.size();
    size_t p95_idx = static_cast<size_t>(0.95 * (v.size() - 1));
    s.p95 = v[p95_idx];
    return s;
}

} // namespace

int run_benchmark(int iterations,
                  const char *inputFile,
                  float input_scale,
                  format_t *image_format,
                  const CTLOperations &ctl_operations,
                  const CTLParameters &global_parameters)
{
    if (iterations < 1)
        iterations = 1;

    if (ctl_operations.size() != 1)
    {
        fprintf(stderr,
                "--benchmark currently supports exactly one -ctl script "
                "(got %zu).\n", ctl_operations.size());
        return 1;
    }
    const ctl_operation_t &op = ctl_operations.front();

    ctl::dpx::fb<float> image_buffer;
    if (!dpx_read(inputFile, input_scale, &image_buffer, image_format) &&
        !exr_read(inputFile, input_scale, &image_buffer, image_format) &&
        !tiff_read(inputFile, input_scale, &image_buffer, image_format))
    {
        fprintf(stderr, "unable to read file %s (unknown format).\n", inputFile);
        return 1;
    }
    if (image_format->bps == 0)
        image_format->bps = image_format->src_bps;

    //
    // Cold iteration: measures the true first-invocation cost — interpreter
    // construction, MTLDevice init, CTL parse, MSL emission, MTLLibrary
    // compile, first PSO creation, plus the dispatch itself. Everything
    // the user sees when they launch ctlrender-metal from a cold shell.
    //
    double cold_ms;
    {
        CTLResults cold_results;
        populate_inputs(cold_results, image_buffer);
        apply_params(cold_results, op, global_parameters);

        auto t0 = clock_t_::now();
        Ctl::MetalInterpreter cold_interp;
        run_ctl_transform(cold_interp, op, &cold_results, image_buffer.pixels());
        auto t1 = clock_t_::now();
        cold_ms = ms_between(t0, t1);
    }

    //
    // Warm iterations: build the interpreter + FunctionCall once outside
    // the loop, then time only the batch dispatch. After the initial
    // warmup, the MTLLibrary is compiled and the PSO is cached, so each
    // iteration measures buffer setup + GPU dispatch + readback only.
    //
    Ctl::MetalInterpreter warm_interp;
    warm_interp.loadFile(op.filename);

    Ctl::FunctionCallPtr fn;
    std::string module = derive_module_name(op.filename);
    try { fn = warm_interp.newFunctionCall(std::string("main")); }
    catch (const Iex::ArgExc &) {}
    if (fn.refcount() == 0)
    {
        try { fn = warm_interp.newFunctionCall(module); }
        catch (...)
        {
            fprintf(stderr,
                    "--benchmark: no main() or %s() function in %s.\n",
                    module.c_str(), op.filename);
            return 1;
        }
    }

    // One warmup dispatch to ensure PSO creation has happened before the
    // timing loop starts.
    {
        CTLResults warmup_results;
        populate_inputs(warmup_results, image_buffer);
        apply_params(warmup_results, op, global_parameters);
        dispatch_batched(warm_interp, fn, warmup_results, image_buffer.pixels());
    }

    std::vector<double> warm_times;
    warm_times.reserve(iterations);
    for (int i = 0; i < iterations; i++)
    {
        CTLResults iter_results;
        populate_inputs(iter_results, image_buffer);
        apply_params(iter_results, op, global_parameters);

        auto t0 = clock_t_::now();
        dispatch_batched(warm_interp, fn, iter_results, image_buffer.pixels());
        auto t1 = clock_t_::now();
        warm_times.push_back(ms_between(t0, t1));
    }

    stats_t s = compute_stats(warm_times);

    const size_t pixels = image_buffer.pixels();
    double warm_mean = s.mean;
    double mpix_per_sec = (warm_mean > 0.0)
                             ? (static_cast<double>(pixels) / (warm_mean * 1e3))
                             : 0.0;

    //
    // JSON on stdout so scripts can jq over it. Field names mirror the
    // benchmark-aggregator schema; keep them in sync so downstream
    // tooling doesn't have to re-map.
    //
    fprintf(stdout, "{\n");
    fprintf(stdout, "  \"input\": \"%s\",\n", inputFile);
    fprintf(stdout, "  \"ctl\": \"%s\",\n", op.filename);
    fprintf(stdout, "  \"width\": %u,\n", image_buffer.width());
    fprintf(stdout, "  \"height\": %u,\n", image_buffer.height());
    fprintf(stdout, "  \"channels\": %u,\n", image_buffer.depth());
    fprintf(stdout, "  \"pixels\": %zu,\n", pixels);
    fprintf(stdout, "  \"iterations\": %d,\n", iterations);
    fprintf(stdout, "  \"cold_first_ms\": %.3f,\n", cold_ms);
    fprintf(stdout, "  \"warm_min_ms\": %.3f,\n", s.min);
    fprintf(stdout, "  \"warm_mean_ms\": %.3f,\n", s.mean);
    fprintf(stdout, "  \"warm_median_ms\": %.3f,\n", s.median);
    fprintf(stdout, "  \"warm_p95_ms\": %.3f,\n", s.p95);
    fprintf(stdout, "  \"warm_max_ms\": %.3f,\n", s.max);
    fprintf(stdout, "  \"warm_mean_Mpix_per_sec\": %.2f\n", mpix_per_sec);
    fprintf(stdout, "}\n");

    return 0;
}
