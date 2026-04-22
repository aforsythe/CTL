///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#ifndef INCLUDED_CTL_METAL_DISPATCH_H
#define INCLUDED_CTL_METAL_DISPATCH_H

//-----------------------------------------------------------------------------
//
//  Low-level Metal kernel dispatch helpers.
//
//  Pure C++ API over an Objective-C++ implementation so callers don't need
//  to #import Metal. The pipeline state holds a compiled MTLLibrary +
//  MTLComputePipelineState pair scoped to a single kernel function.
//
//  Takes MSL source and a set of input/output buffers driven by
//  FunctionArgs.
//
//-----------------------------------------------------------------------------

#include <cstddef>
#include <string>
#include <vector>

namespace Ctl {

class MetalDispatchImpl;
class MetalPipelineImpl;

//-----------------------------------------------------------------------------
// MetalKernelBinding -- one buffer argument of a compute kernel invocation.
//
// `hostData` points to a host-side buffer of `bytes` bytes. Before the
// kernel runs, bytes are copied host→device iff `writeToGpu` is true.
// After the kernel completes, bytes are copied device→host iff
// `readFromGpu` is true. Bindings are bound to kernel buffer slots in
// the order they appear in the vector passed to `dispatch`.
//-----------------------------------------------------------------------------

struct MetalKernelBinding
{
    char *      hostData;
    size_t      bytes;
    bool        writeToGpu;
    bool        readFromGpu;

    //
    // `persistent` bindings are cached on the pipeline by the identity of
    // `hostData`. The first dispatch that sees a given pointer allocates a
    // device-private MTLBuffer, copies the bytes once, and retains the
    // buffer for the pipeline's lifetime. Subsequent dispatches reuse the
    // cached buffer and skip the upload. Used by halfExpLog table hoisting
    // so the ~739 KB of exp/log tables are neither baked into MSL source
    // nor re-uploaded every dispatch.
    //
    // When `persistent` is true `readFromGpu` is ignored — the buffer is
    // read-only from the kernel's perspective.
    //
    bool        persistent = false;
};

//-----------------------------------------------------------------------------
// MetalPipeline -- a compiled MSL library + compute pipeline state.
//-----------------------------------------------------------------------------

class MetalPipeline
{
  public:

    MetalPipeline(const std::string &mslSource,
                  const std::string &kernelName);
    ~MetalPipeline();

    MetalPipeline(const MetalPipeline &) = delete;
    MetalPipeline & operator=(const MetalPipeline &) = delete;

    // Dispatch the kernel over `numSamples` threads. The kernel is bound
    // with one output buffer at [[buffer(0)]] whose contents must be
    // `numSamples * elementSize` bytes. On return `outBytes` holds the
    // kernel's writes.
    void dispatch(void *outBytes,
                  size_t elementSize,
                  size_t numSamples);

    // Dispatch the kernel over `numThreads` threads with an arbitrary
    // set of buffer bindings. Buffers are bound to [[buffer(N)]] in the
    // order they appear in `bindings`.
    void dispatch(const std::vector<MetalKernelBinding> &bindings,
                  size_t numThreads);

  private:

    MetalPipelineImpl * _impl;
};

} // namespace Ctl

#endif
