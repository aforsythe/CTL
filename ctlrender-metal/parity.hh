///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#ifndef CTL_RENDER_GPU_PARITY_HH
#define CTL_RENDER_GPU_PARITY_HH

#include <cstdint>

#include "transform.hh"

// Runs BOTH the CPU SIMD backend (Ctl::SimdInterpreter) and the Metal
// GPU backend (Ctl::MetalInterpreter) on the given input, writes both
// results as <outputFile-stem>.cpu.<ext> and <outputFile-stem>.gpu.<ext>,
// and prints a per-channel max-ULP parity report to stdout.
//
// `maxUlpThreshold` is the highest per-channel ULP deviation that still
// counts as PASS. The default is 0 (bit-exact). For transforms with
// known FP32-floor drift (e.g. ACES v2 with ~50 transcendentals per
// pixel), callers pass a non-zero bound — see `lib/IlmCtlMetal/
// PRECISION.md` for per-transform measured bounds.
//
// Returns 0 iff every channel's max ULP is ≤ `maxUlpThreshold`,
// non-zero otherwise. Used by ctlrender-metal's --parity-check CLI mode.
int run_parity_check(const char *inputFile, const char *outputFile,
                     float input_scale, float output_scale,
                     format_t *image_format,
                     Compression *compression,
                     const CTLOperations &ctl_operations,
                     const CTLParameters &global_parameters,
                     uint32_t maxUlpThreshold = 0);

#endif
