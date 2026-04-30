///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#include <CtlMetalDispatch.h>
#include <CtlMetalCompileOptions.h>
#include <CtlMetalShaderCache.h>
#include <Iex.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <unordered_map>

#import <Metal/Metal.h>

namespace Ctl {

class MetalPipelineImpl
{
  public:
    id<MTLDevice>               device;
    id<MTLCommandQueue>         queue;
    id<MTLLibrary>              library;
    id<MTLComputePipelineState> pipeline;

    //
    // Cache of persistent MTLBuffers keyed by the host `hostData` pointer.
    // Populated on first dispatch that sees a given `persistent=true`
    // binding; retained for the pipeline's lifetime so subsequent
    // dispatches skip the upload. See `MetalKernelBinding::persistent`.
    //
    std::unordered_map<const void *, id<MTLBuffer>> persistentBufs;

    //
    // Pool of non-persistent MTLBuffers indexed by binding slot.  Each
    // slot's buffer is reused across dispatches; on size-up the old
    // buffer is released and a bigger one allocated.  Saves the
    // `newBufferWithLength:options:MTLResourceStorageModeShared` call
    // (which page-faults-in 64 MB of shared memory on 4K aces_combined
    // frames — measured ~80 ms/frame of allocation cost pre-pool).
    // The pool is scoped to the MTLDevice/queue lifetime; same pipeline
    // object dispatches the same kernel shape frame after frame, so slot
    // layout is stable.
    //
    std::vector<id<MTLBuffer>> bufferPool;
};

//-----------------------------------------------------------------------------

namespace {

[[noreturn]] void
throwError(const char *stage, NSError *error)
{
    std::string msg = std::string("CTL Metal backend: ") + stage + " failed";
    if (error) {
        msg += ": ";
        msg += [[error localizedDescription] UTF8String];
    }
    throw IEX_NAMESPACE::BaseExc(msg);
}

} // anonymous namespace

//-----------------------------------------------------------------------------

MetalPipeline::MetalPipeline(const std::string &mslSource,
                             const std::string &kernelName)
    : _impl(new MetalPipelineImpl)
{
    _impl->device = MTLCreateSystemDefaultDevice();
    if (!_impl->device) {
        delete _impl;
        _impl = nullptr;
        throw IEX_NAMESPACE::BaseExc(
            "CTL Metal backend: no Metal device available.");
    }
    if (![_impl->device supportsFamily:MTLGPUFamilyApple7]) {
        delete _impl;
        _impl = nullptr;
        throw IEX_NAMESPACE::BaseExc(
            "CTL Metal backend: Apple GPU family 7 or newer required.");
    }

    _impl->queue = [_impl->device newCommandQueue];
    if (!_impl->queue) {
        delete _impl;
        _impl = nullptr;
        throw IEX_NAMESPACE::BaseExc(
            "CTL Metal backend: failed to create MTLCommandQueue.");
    }

    if (std::getenv("CTL_METAL_DUMP_MSL")) {
        std::cerr << "==== MSL source (kernel=" << kernelName << ") ====\n"
                  << mslSource
                  << "==== end MSL ====" << std::endl;
    }

    NSString *src = [NSString stringWithUTF8String:mslSource.c_str()];
    MTLCompileOptions *opts = makeIeeeSafeCompileOptions();

    const bool debugCacheEarly = std::getenv("CTL_METAL_CACHE_DEBUG") != nullptr;
    auto libStart = std::chrono::high_resolution_clock::now();

    NSError *err = nil;
    _impl->library = [_impl->device newLibraryWithSource:src
                                                 options:opts
                                                   error:&err];
    if (debugCacheEarly) {
        auto libEnd = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(
                        libEnd - libStart).count();
        std::cerr << "CTL Metal newLibraryWithSource " << ms << " ms kernel="
                  << kernelName << " bytes=" << mslSource.size() << std::endl;
    }
    if (!_impl->library) {
        MetalPipelineImpl *dead = _impl;
        _impl = nullptr;
        NSError *captured = err;
        delete dead;
        throwError("MSL compile", captured);
    }

    NSString *kname = [NSString stringWithUTF8String:kernelName.c_str()];
    id<MTLFunction> fn = [_impl->library newFunctionWithName:kname];
    if (!fn) {
        MetalPipelineImpl *dead = _impl;
        _impl = nullptr;
        delete dead;
        throw IEX_NAMESPACE::BaseExc(
            std::string("CTL Metal backend: kernel function '") +
            kernelName + "' not found in compiled library.");
    }

    //
    // On-disk PSO cache via MTLBinaryArchive.
    //
    // MSL compile (`newLibraryWithSource:`) dominates cold-start latency,
    // but the next-largest cost — PSO creation — can be skipped across
    // process launches by persisting a binary archive and supplying it
    // to `newComputePipelineStateWithDescriptor:`. First attempt uses
    // `FailOnBinaryArchiveMiss` to detect a hit without recompiling;
    // on miss we retry without that flag, then add the newly-built PSO
    // to the archive and write it back to disk. Archive mismatches
    // (toolchain/GPU changes) are handled by the archive layer itself —
    // if the stored entries are unusable the miss path just runs.
    //
    const std::string digest =
        metalShaderCacheDigest(mslSource, kernelName);
    const std::string cachePath = metalShaderCachePathFor(digest);

    id<MTLBinaryArchive> archive = nil;
    if (!cachePath.empty()) {
        MTLBinaryArchiveDescriptor *archDesc =
            [[MTLBinaryArchiveDescriptor alloc] init];
        NSString *nsCachePath =
            [NSString stringWithUTF8String:cachePath.c_str()];
        NSURL *cacheUrl = [NSURL fileURLWithPath:nsCachePath];
        if ([[NSFileManager defaultManager] fileExistsAtPath:nsCachePath]) {
            archDesc.url = cacheUrl;
        }
        NSError *archErr = nil;
        archive = [_impl->device newBinaryArchiveWithDescriptor:archDesc
                                                          error:&archErr];
        if (!archive && archDesc.url) {
            //
            // Stale or corrupt cache file — try again without a URL so
            // we start from an empty archive that the miss path can
            // populate and overwrite.
            //
            archDesc.url = nil;
            archive = [_impl->device newBinaryArchiveWithDescriptor:archDesc
                                                              error:&archErr];
        }
    }

    MTLComputePipelineDescriptor *psoDesc =
        [[MTLComputePipelineDescriptor alloc] init];
    psoDesc.computeFunction = fn;
    if (archive) {
        psoDesc.binaryArchives = @[archive];
    }

    const bool debugCache = std::getenv("CTL_METAL_CACHE_DEBUG") != nullptr;
    auto psoStart = std::chrono::high_resolution_clock::now();

    NSError *missErr = nil;
    if (archive) {
        _impl->pipeline = [_impl->device
            newComputePipelineStateWithDescriptor:psoDesc
                                          options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                       reflection:nil
                                            error:&missErr];
    }

    const bool cacheHit = (_impl->pipeline != nil);
    auto psoMid = std::chrono::high_resolution_clock::now();
    if (debugCache) {
        double ms = std::chrono::duration<double, std::milli>(
                        psoMid - psoStart).count();
        std::cerr << "CTL Metal cache " << (cacheHit ? "HIT" : "MISS")
                  << " (" << ms << " ms)"
                  << " kernel=" << kernelName
                  << " digest=" << digest;
        if (!cacheHit && missErr) {
            std::cerr << " missErr=\""
                      << [[missErr localizedDescription] UTF8String] << "\"";
        }
        std::cerr << std::endl;
    }
    if (!cacheHit) {
        //
        // Either there's no archive at all, or the archive didn't
        // contain a match. Compile normally, then add to the archive
        // (if we have one) and persist.
        //
        _impl->pipeline = [_impl->device
            newComputePipelineStateWithDescriptor:psoDesc
                                          options:MTLPipelineOptionNone
                                       reflection:nil
                                            error:&err];
        if (!_impl->pipeline) {
            MetalPipelineImpl *dead = _impl;
            _impl = nullptr;
            NSError *captured = err;
            delete dead;
            throwError("compute pipeline creation", captured);
        }

        if (archive && !cachePath.empty()) {
            NSError *addErr = nil;
            if ([archive addComputePipelineFunctionsWithDescriptor:psoDesc
                                                             error:&addErr]) {
                NSString *nsCachePath =
                    [NSString stringWithUTF8String:cachePath.c_str()];
                NSURL *cacheUrl = [NSURL fileURLWithPath:nsCachePath];
                NSError *serErr = nil;
                //
                // Serialization failure is non-fatal: the PSO is already
                // built and the process will still run; the next process
                // just pays the compile cost again.
                //
                [archive serializeToURL:cacheUrl error:&serErr];
            }
        }
    }

    if (debugCache) {
        auto psoEnd = std::chrono::high_resolution_clock::now();
        double totalMs = std::chrono::duration<double, std::milli>(
                             psoEnd - psoStart).count();
        std::cerr << "CTL Metal PSO total " << totalMs << " ms ("
                  << (cacheHit ? "hit path" : "miss path") << ")"
                  << std::endl;
    }
}

MetalPipeline::~MetalPipeline()
{
    delete _impl;
}

void
MetalPipeline::dispatch(void *outBytes,
                        size_t elementSize,
                        size_t numSamples)
{
    if (!_impl || !_impl->pipeline) {
        throw IEX_NAMESPACE::BaseExc(
            "CTL Metal backend: MetalPipeline::dispatch on an invalid pipeline.");
    }

    // Wrap every autoreleased MTL temporary this call creates
    // (MTLBuffer, MTLCommandBuffer, MTLComputeCommandEncoder) in a
    // local pool so callers that dispatch in a loop — benchmarks,
    // streaming drivers — do not accumulate autoreleased objects until
    // the next outer pool drain. Without this the runtime eventually
    // corrupts its own method cache (observed at ~20 iterations from
    // `testMetalBenchmark`'s warm loop).
    @autoreleasepool {
        const size_t outBytesLen = elementSize * numSamples;
        id<MTLBuffer> outBuf =
            [_impl->device newBufferWithLength:outBytesLen
                                       options:MTLResourceStorageModeShared];
        if (!outBuf) {
            throw IEX_NAMESPACE::BaseExc(
                "CTL Metal backend: failed to allocate output MTLBuffer.");
        }

        id<MTLCommandBuffer> cmd = [_impl->queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
        [enc setComputePipelineState:_impl->pipeline];
        [enc setBuffer:outBuf offset:0 atIndex:0];

        NSUInteger tew = _impl->pipeline.threadExecutionWidth;
        NSUInteger maxTG = _impl->pipeline.maxTotalThreadsPerThreadgroup;
        NSUInteger tg = tew;
        if (tg == 0) tg = 32;
        if (tg > maxTG) tg = maxTG;

        [enc dispatchThreads:MTLSizeMake(numSamples, 1, 1)
              threadsPerThreadgroup:MTLSizeMake(tg, 1, 1)];
        [enc endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];

        if (cmd.status != MTLCommandBufferStatusCompleted) {
            NSError *e = cmd.error;
            throwError("command buffer execution", e);
        }

        std::memcpy(outBytes, outBuf.contents, outBytesLen);
    }
}

void
MetalPipeline::dispatch(const std::vector<MetalKernelBinding> &bindings,
                        size_t numThreads)
{
    if (!_impl || !_impl->pipeline) {
        throw IEX_NAMESPACE::BaseExc(
            "CTL Metal backend: MetalPipeline::dispatch on an invalid pipeline.");
    }

    // See the matching comment in the simpler dispatch() overload.
    @autoreleasepool {
        std::vector<id<MTLBuffer>> gpuBufs;
        gpuBufs.reserve(bindings.size());

        // Ensure the non-persistent buffer pool has a slot per binding.
        if (_impl->bufferPool.size() < bindings.size())
            _impl->bufferPool.resize(bindings.size(), nil);

        for (size_t slotIdx = 0; slotIdx < bindings.size(); ++slotIdx) {
            const MetalKernelBinding &b = bindings[slotIdx];
            //
            // Persistent bindings: hash on the host pointer's identity.
            // First dispatch allocates and uploads; subsequent dispatches
            // reuse the cached MTLBuffer and skip the memcpy. This is the
            // path halfExpLog's 739 KB tables take, so they ride the GPU
            // once per process and are then free to bind.
            //
            if (b.persistent) {
                auto it = _impl->persistentBufs.find(b.hostData);
                if (it == _impl->persistentBufs.end()) {
                    id<MTLBuffer> buf =
                        [_impl->device newBufferWithLength:b.bytes
                                                   options:MTLResourceStorageModeShared];
                    if (!buf) {
                        throw IEX_NAMESPACE::BaseExc(
                            "CTL Metal backend: failed to allocate persistent "
                            "kernel MTLBuffer.");
                    }
                    if (b.hostData) {
                        std::memcpy(buf.contents, b.hostData, b.bytes);
                    }
                    it = _impl->persistentBufs
                        .emplace(static_cast<const void *>(b.hostData), buf)
                        .first;
                }
                gpuBufs.push_back(it->second);
                continue;
            }

            //
            // Non-persistent: reuse the pooled buffer for this slot if
            // one exists and is big enough.  Resize (allocate new)
            // otherwise.  The allocation-per-dispatch path is the one
            // CTL_METAL_TIMING pointed at as ~80 ms/frame on 4K
            // aces_combined; pooling eliminates it on the common
            // stable-size case.
            //
            id<MTLBuffer> buf = _impl->bufferPool[slotIdx];
            if (!buf || buf.length < b.bytes) {
                buf = [_impl->device newBufferWithLength:b.bytes
                                                 options:MTLResourceStorageModeShared];
                if (!buf) {
                    throw IEX_NAMESPACE::BaseExc(
                        "CTL Metal backend: failed to allocate kernel MTLBuffer.");
                }
                _impl->bufferPool[slotIdx] = buf;
            }
            if (b.writeToGpu && b.hostData) {
                std::memcpy(buf.contents, b.hostData, b.bytes);
            }
            gpuBufs.push_back(buf);
        }

        id<MTLCommandBuffer> cmd = [_impl->queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
        [enc setComputePipelineState:_impl->pipeline];
        for (size_t i = 0; i < gpuBufs.size(); ++i) {
            [enc setBuffer:gpuBufs[i] offset:0 atIndex:(NSUInteger)i];
        }

        NSUInteger tew = _impl->pipeline.threadExecutionWidth;
        NSUInteger maxTG = _impl->pipeline.maxTotalThreadsPerThreadgroup;
        NSUInteger tg = tew;
        if (tg == 0) tg = 32;
        if (tg > maxTG) tg = maxTG;

        [enc dispatchThreads:MTLSizeMake(numThreads, 1, 1)
              threadsPerThreadgroup:MTLSizeMake(tg, 1, 1)];
        [enc endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];

        if (cmd.status != MTLCommandBufferStatusCompleted) {
            NSError *e = cmd.error;
            throwError("command buffer execution", e);
        }

        for (size_t i = 0; i < bindings.size(); ++i) {
            const MetalKernelBinding &b = bindings[i];
            if (b.persistent)
                continue;
            if (b.readFromGpu && b.hostData) {
                std::memcpy(b.hostData, gpuBufs[i].contents, b.bytes);
            }
        }
    }
}

} // namespace Ctl
