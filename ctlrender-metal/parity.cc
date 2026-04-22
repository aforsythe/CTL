///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#include "parity.hh"
#include "dpx_file.hh"
#include "tiff_file.hh"
#include "exr_file.hh"
#include "aces_file.hh"
#include <CtlSimdInterpreter.h>
#include <CtlMetalInterpreter.h>
#include <Iex.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

//
// ULP distance between two float32s. Returns UINT32_MAX for
// NaN / opposite-sign / inf-mismatch pairs. Same-sign finite floats use
// the reinterpret-subtract convention from Bruce Dawson's "Comparing
// Floating Point Numbers, 2012 Edition" — adjacent floats differ by 1.
//
static uint32_t ulp_diff(float a, float b)
{
    if (a == b)
        return 0;
    if (std::isnan(a) || std::isnan(b))
        return UINT32_MAX;

    int32_t ai, bi;
    std::memcpy(&ai, &a, sizeof(ai));
    std::memcpy(&bi, &b, sizeof(bi));

    if ((ai < 0) != (bi < 0))
        return UINT32_MAX;

    int32_t d = (ai > bi) ? (ai - bi) : (bi - ai);
    return static_cast<uint32_t>(d);
}

static void write_fb(const char *outputFile, float output_scale,
                     ctl::dpx::fb<float> &fb, format_t *fmt,
                     Compression *compression)
{
    if (!strncmp(fmt->ext, "aces", 3))
    {
        aces_write(outputFile, output_scale,
                   fb.width(), fb.height(), fb.depth(),
                   fb.ptr(), fmt);
    }
    else if (!strncmp(fmt->ext, "exr", 3))
    {
        exr_write(outputFile, output_scale, fb, fmt, compression);
    }
    else if (!strncmp(fmt->ext, "adx", 3))
    {
        dpx_write(outputFile, output_scale, fb, fmt);
    }
    else if (!strncmp(fmt->ext, "dpx", 3))
    {
        dpx_write(outputFile, output_scale, fb, fmt);
    }
    else if (!strncmp(fmt->ext, "tiff", 3))
    {
        tiff_write(outputFile, output_scale, fb, fmt);
    }
    else
    {
        fprintf(stderr, "unable to write a %s file (unknown format).\n", fmt->ext);
        std::exit(1);
    }
}

//
// Splice "<stem>.<tag>.<ext>" from an outputFile path. If the path has
// no extension we just append ".<tag>". Preserves any directory prefix.
//
static std::string insert_tag(const std::string &outputFile, const char *tag)
{
    std::string::size_type slash = outputFile.find_last_of("/\\");
    std::string::size_type dotSearchStart =
        (slash == std::string::npos) ? 0 : slash + 1;
    std::string::size_type dot = outputFile.find_last_of('.');
    if (dot == std::string::npos || dot < dotSearchStart)
        return outputFile + "." + tag;
    return outputFile.substr(0, dot) + "." + tag + outputFile.substr(dot);
}

static void populate_inputs(CTLResults &results, const ctl::dpx::fb<float> &fb)
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

static void run_chain(Ctl::Interpreter &interpreter,
                      CTLResults &ctl_results,
                      const CTLOperations &ctl_operations,
                      const CTLParameters &global_parameters,
                      size_t pixel_count)
{
    for (CTLOperations::const_iterator op = ctl_operations.begin();
         op != ctl_operations.end(); ++op)
    {
        for (CTLParameters::const_iterator p = global_parameters.begin();
             p != global_parameters.end(); ++p)
        {
            add_parameter_value_to_ctl_results(&ctl_results, *p);
        }
        for (CTLParameters::const_iterator p = op->local.begin();
             p != op->local.end(); ++p)
        {
            add_parameter_value_to_ctl_results(&ctl_results, *p);
        }
        run_ctl_transform(interpreter, *op, &ctl_results, pixel_count);
    }
}

int run_parity_check(const char *inputFile, const char *outputFile,
                     float input_scale, float output_scale,
                     format_t *image_format,
                     Compression *compression,
                     const CTLOperations &ctl_operations,
                     const CTLParameters &global_parameters,
                     uint32_t maxUlpThreshold)
{
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
    // Build two independent input result lists. mkresult holds pointers
    // into image_buffer's storage, which both lists read from. Since
    // neither backend writes to the input DataArgs (they pull via copy
    // into FunctionArg buffers at dispatch time) this shared-read is
    // safe — the two backends never race because we run them sequentially.
    //
    CTLResults cpu_results, gpu_results;
    populate_inputs(cpu_results, image_buffer);
    populate_inputs(gpu_results, image_buffer);

    Ctl::SimdInterpreter cpu_interp;
    Ctl::MetalInterpreter gpu_interp;

    run_chain(cpu_interp, cpu_results, ctl_operations, global_parameters,
              image_buffer.pixels());
    run_chain(gpu_interp, gpu_results, ctl_operations, global_parameters,
              image_buffer.pixels());

    ctl::dpx::fb<float> cpu_fb, gpu_fb;
    cpu_fb.init(image_buffer.width(), image_buffer.height(), image_buffer.depth());
    gpu_fb.init(image_buffer.width(), image_buffer.height(), image_buffer.depth());

    format_t cpu_fmt = *image_format;
    format_t gpu_fmt = *image_format;
    mkimage(&cpu_fb, cpu_results, &cpu_fmt);
    mkimage(&gpu_fb, gpu_results, &gpu_fmt);

    if (cpu_fb.depth() != gpu_fb.depth() ||
        cpu_fb.pixels() != gpu_fb.pixels())
    {
        fprintf(stderr,
                "parity-check: backends produced incompatible image shapes "
                "(CPU %ux%ux%u, GPU %ux%ux%u). Cannot compare.\n",
                cpu_fb.width(), cpu_fb.height(), cpu_fb.depth(),
                gpu_fb.width(), gpu_fb.height(), gpu_fb.depth());
        return 1;
    }

    const uint32_t channels = cpu_fb.depth();
    const size_t pixels = cpu_fb.pixels();
    const float *cpu_ptr = cpu_fb.ptr();
    const float *gpu_ptr = gpu_fb.ptr();

    //
    // Per-channel max-ULP + diverged-pixel count. Channels correspond to
    // the output layout chosen by mkimage (typically rOut/gOut/bOut/aOut
    // or a colorspace quadruple). We report position-independent stats
    // because they're what the plan's precision gate is expressed in.
    //
    std::vector<uint32_t> max_ulp(channels, 0);
    std::vector<size_t> diverged(channels, 0);

    size_t first_diff_pixel = 0;
    uint32_t first_diff_channel = 0;
    uint32_t first_diff_ulp = 0;
    float first_diff_cpu = 0.0f;
    float first_diff_gpu = 0.0f;
    bool saw_any_diff = false;

    for (size_t p = 0; p < pixels; ++p)
    {
        for (uint32_t c = 0; c < channels; ++c)
        {
            float a = cpu_ptr[p * channels + c];
            float b = gpu_ptr[p * channels + c];
            uint32_t d = ulp_diff(a, b);
            if (d > 0)
            {
                diverged[c]++;
                if (d > max_ulp[c])
                    max_ulp[c] = d;
                if (!saw_any_diff)
                {
                    saw_any_diff = true;
                    first_diff_pixel = p;
                    first_diff_channel = c;
                    first_diff_ulp = d;
                    first_diff_cpu = a;
                    first_diff_gpu = b;
                }
            }
        }
    }

    static const char *const channel_labels[] = { "ch0", "ch1", "ch2", "ch3" };

    fprintf(stdout, "parity-check: %s\n", inputFile);
    fprintf(stdout, "  pixels: %zu  channels: %u\n", pixels, channels);
    uint32_t overall_max = 0;
    size_t overall_div = 0;
    for (uint32_t c = 0; c < channels; ++c)
    {
        const char *label = (c < 4) ? channel_labels[c] : "ch?";
        fprintf(stdout, "  %s: max ULP=%u, diverged=%zu / %zu\n",
                label, max_ulp[c], diverged[c], pixels);
        if (max_ulp[c] > overall_max)
            overall_max = max_ulp[c];
        overall_div += diverged[c];
    }

    if (saw_any_diff)
    {
        fprintf(stdout,
                "  first difference at pixel %zu, channel %u: "
                "CPU=%.9g GPU=%.9g (%u ULP)\n",
                first_diff_pixel, first_diff_channel,
                first_diff_cpu, first_diff_gpu, first_diff_ulp);
    }

    std::string cpu_out = insert_tag(outputFile, "cpu");
    std::string gpu_out = insert_tag(outputFile, "gpu");

    if (cpu_fmt.squish)
        cpu_fb.swizzle(0, true);
    if (gpu_fmt.squish)
        gpu_fb.swizzle(0, true);

    write_fb(cpu_out.c_str(), output_scale, cpu_fb, &cpu_fmt, compression);
    write_fb(gpu_out.c_str(), output_scale, gpu_fb, &gpu_fmt, compression);

    fprintf(stdout, "  wrote CPU output: %s\n", cpu_out.c_str());
    fprintf(stdout, "  wrote GPU output: %s\n", gpu_out.c_str());

    if (overall_div == 0)
    {
        fprintf(stdout, "  parity: PASS (0 ULP, bit-exact)\n");
        return 0;
    }
    if (overall_max <= maxUlpThreshold)
    {
        fprintf(stdout,
                "  parity: PASS (max %u ULP ≤ threshold %u across "
                "%zu diverged samples)\n",
                overall_max, maxUlpThreshold, overall_div);
        return 0;
    }
    fprintf(stdout,
            "  parity: FAIL (max %u ULP exceeds threshold %u across "
            "%zu diverged samples)\n",
            overall_max, maxUlpThreshold, overall_div);
    return 1;
}
