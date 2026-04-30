///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#ifndef CTL_RENDER_GPU_BENCHMARK_HH
#define CTL_RENDER_GPU_BENCHMARK_HH

#include "transform.hh"

// Times the Metal GPU backend's end-to-end cost on the given input across
// `iterations` runs and prints a JSON summary to stdout. Measures:
//   - cold_first_ms: the first iteration (includes CTL parse, MSL source
//     emission, MTLLibrary compile, and first-call PSO creation).
//   - warm_{min,mean,median,max,p95}_ms: subsequent iterations reusing the
//     same interpreter + FunctionCall (PSO cached, just per-batch buffer
//     set/dispatch/readback).
//
// `iterations` must be >= 1. With iterations=1 only cold timing is
// reported. Called by ctlrender-metal's --benchmark N CLI mode.
int run_benchmark(int iterations,
                  const char *inputFile,
                  float input_scale,
                  format_t *image_format,
                  const CTLOperations &ctl_operations,
                  const CTLParameters &global_parameters);

#endif
