///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#ifndef INCLUDED_CTL_METAL_SHADER_CACHE_H
#define INCLUDED_CTL_METAL_SHADER_CACHE_H

//-----------------------------------------------------------------------------
//
//  On-disk MTLBinaryArchive cache for compiled compute PSOs.
//
//  Apple's `MTLBinaryArchive` stores GPU-family-specific compiled pipeline
//  states keyed by the underlying MSL function signatures. Re-using the
//  archive on a subsequent process start lets us skip the 200–800 ms PSO
//  compilation step that dominates cold-start latency for ctlrender-metal.
//
//  The archive itself is responsible for invalidating entries that do not
//  match the current Metal toolchain / GPU family — we just key the file
//  name by a hash of the MSL source + kernel name, and trust the archive
//  to ignore unusable entries.
//
//  Pure C++ surface over an Objective-C++ implementation: callers don't
//  need to #import Metal. Path resolution uses
//  `~/Library/Caches/CTL/metal/` by default, overridable via
//  `CTL_METAL_CACHE_DIR`; bypass the cache entirely with
//  `CTL_METAL_DISABLE_CACHE=1`.
//
//-----------------------------------------------------------------------------

#include <cstddef>
#include <string>

namespace Ctl {

//
// Stable 64-bit FNV-1a hex digest over (mslSource, kernelName). Used to
// pick the cache file; not a cryptographic fingerprint — Apple's archive
// layer is the source of truth for toolchain/GPU compatibility.
//
std::string metalShaderCacheDigest(const std::string &mslSource,
                                   const std::string &kernelName);

//
// Absolute path to the cache file for a given digest, or an empty string
// when the cache is disabled. Creates the containing directory if needed.
// The file itself is not created here — `MetalPipeline` writes it on
// first successful compile.
//
std::string metalShaderCachePathFor(const std::string &digest);

} // namespace Ctl

#endif
