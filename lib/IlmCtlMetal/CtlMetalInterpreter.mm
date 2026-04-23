///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------------
//
//  class MetalInterpreter -- Objective-C++ implementation.
//
//  The constructor acquires the system's default MTLDevice, validates that
//  it meets the Apple7+ GPU-family requirement, and creates a command queue
//  shared across FunctionCall dispatches.
//
//-----------------------------------------------------------------------------

#include <CtlMetalAddr.h>
#include <CtlMetalCodegen.h>
#include <CtlMetalFunctionCall.h>
#include <CtlMetalInterpreter.h>
#include <CtlMetalLContext.h>
#include <CtlMetalModule.h>
#include <CtlMetalSidecarCache.h>
#include <CtlMetalStdLibrary.h>
#include <CtlSimdAddr.h>
#include <CtlSimdInterpreter.h>
#include <CtlSimdReg.h>
#include <CtlSymbolTable.h>
#include <CtlType.h>
#include <Iex.h>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>

#import <Metal/Metal.h>

namespace Ctl {

struct MetalInterpreter::Data
{
    id<MTLDevice>       device = nil;
    id<MTLCommandQueue> queue = nil;
    unsigned long       maxInstCount = 100000000;

    //
    // Shared MSL emitter. One per interpreter — every MetalModule loaded
    // here writes into this instance so a kernel from any one module can
    // see the function definitions of every module it imports.
    //
    MetalCodegen        codegen;

    //
    // Host-side SIMD sidecar. Preloaded with every module the user hands
    // to MetalInterpreter so that Metal codegen can read back evaluated
    // module-scope const values at parse time.
    //
    std::unique_ptr<SimdInterpreter> sidecar;

    //
    // Persistent cache of sidecar-evaluated module-scope bytes. When
    // `cacheValid` is true, `newLContext` short-circuits the sidecar
    // load entirely and consumers read from the cache. When false,
    // sidecar loads run normally and the cache is populated by the
    // post-load harvest pass so the next ctlrender-metal invocation
    // can skip the work.
    //
    MetalSidecarCache   cache;
    bool                cacheValid = false;
    bool                cacheDirty = false;
};

MetalInterpreter::MetalInterpreter()
    : Interpreter(),
      _data(new Data)
{
    _data->device = MTLCreateSystemDefaultDevice();
    if (!_data->device) {
        delete _data;
        _data = nullptr;
        throw IEX_NAMESPACE::BaseExc(
            "CTL Metal backend: no Metal device available on this host.");
    }
    if (![_data->device supportsFamily:MTLGPUFamilyApple7]) {
        NSString *name = [_data->device name];
        std::string nameStr = name ? [name UTF8String] : "(unknown)";
        delete _data;
        _data = nullptr;
        throw IEX_NAMESPACE::BaseExc(
            std::string("CTL Metal backend: Apple GPU family 7 or newer "
                        "required; this device (") + nameStr +
            ") is not supported.");
    }
    _data->queue = [_data->device newCommandQueue];
    if (!_data->queue) {
        delete _data;
        _data = nullptr;
        throw IEX_NAMESPACE::BaseExc(
            "CTL Metal backend: failed to create MTLCommandQueue.");
    }

    _data->sidecar.reset(new SimdInterpreter);

    //
    // Register the Metal backend's standard library symbols. Matches the
    // SimdInterpreter pattern: a throwaway MetalModule + MetalLContext is
    // sufficient because `declareMetalStdLibrary` only touches the symbol
    // table — it never emits MSL source or invokes codegen.
    //
    MetalModule stdlibModule(*this, "<stdlib>", "<stdlib>");
    std::stringstream emptySource;
    MetalLContext stdlibLContext(emptySource, &stdlibModule, symtab());
    declareMetalStdLibrary(stdlibLContext);
}

SimdInterpreter &
MetalInterpreter::sidecar() const
{
    return *_data->sidecar;
}

MetalCodegen &
MetalInterpreter::codegen()
{
    return _data->codegen;
}

const MetalCodegen &
MetalInterpreter::codegen() const
{
    return _data->codegen;
}

bool
MetalInterpreter::sidecarCacheValid() const
{
    return _data && _data->cacheValid;
}

bool
MetalInterpreter::preloadSidecarCache(const std::string &topSourcePath)
{
    if (!_data) return false;
    const bool debugPhase = std::getenv("CTL_METAL_CACHE_DEBUG") != nullptr;
    auto t0 = std::chrono::high_resolution_clock::now();
    const bool hit = _data->cache.tryLoad(topSourcePath);
    _data->cacheValid = hit;
    _data->cacheDirty = !hit;
    if (debugPhase) {
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::cerr << "CTL Metal sidecar cache preload "
                  << (hit ? "HIT" : "MISS")
                  << " for " << topSourcePath
                  << " (" << ms << " ms)" << std::endl;
    }
    return hit;
}

void
MetalInterpreter::flushSidecarCache(const std::string &topSourcePath)
{
    if (!_data) return;
    if (!_data->cacheDirty) return;      // cache loaded from disk; nothing new
    const bool debugPhase = std::getenv("CTL_METAL_CACHE_DEBUG") != nullptr;
    auto t0 = std::chrono::high_resolution_clock::now();
    _data->cache.writeToDisk(topSourcePath);
    _data->cacheDirty = false;
    _data->cacheValid = true;
    if (debugPhase) {
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::cerr << "CTL Metal sidecar cache flush for " << topSourcePath
                  << " (" << ms << " ms)" << std::endl;
    }
}

const char *
MetalInterpreter::lookupSidecarBytes(const std::string &absoluteName,
                                     size_t &byteCountOut,
                                     const Module *owningModule)
{
    byteCountOut = 0;
    if (!_data) return nullptr;

    //
    // Cache first. On a valid preload this is the only path that
    // normally returns — the sidecar hasn't been fed any modules so its
    // symbol table is empty. On a cold run the harvest pass adds each
    // symbol to the cache immediately after sidecar.loadModule succeeds,
    // so consumers still see their bytes here.
    //
    const char *bytes = _data->cache.lookup(absoluteName, byteCountOut);
    if (bytes) return bytes;

    //
    // Warm-cache repair: a prior run wrote an on-disk cache that's
    // missing `absoluteName` (e.g. harvest never ran for the owning
    // module, or the module was added to an import graph after the
    // cache was last refreshed). Demand-load the owning module into
    // the sidecar now, harvest its symbols into the cache, and retry
    // the cache lookup. The flushSidecarCache path will refresh the
    // on-disk file before exit so the next run finds it complete.
    //
    // Only runs when the caller supplied the owning module; legacy
    // callers without one fall through to the live-sidecar fallback
    // and ultimately to nullptr if that's empty too.
    //
    if (owningModule && _data->sidecar &&
        !_data->sidecar->moduleIsLoaded(owningModule->name()))
    {
        //
        // Ask the sidecar to resolve the module by name through its own
        // CTL_MODULE_PATH lookup, which transitively pulls every import
        // in the right order. Passing the source text inline here would
        // only load this one module and leave its imports unresolved —
        // causing parse errors like "Applied member access operator to
        // non-struct of type int" when a type defined in Lib.Academy.*
        // isn't in the sidecar's symbol table yet.
        //
        const bool debugPhase =
            std::getenv("CTL_METAL_CACHE_DEBUG") != nullptr;
        auto t0 = std::chrono::high_resolution_clock::now();
        bool loaded = false;
        try {
            //
            // Pass owningModule->fileName() so top-level CTL files
            // that aren't on CTL_MODULE_PATH (the usual case for
            // transform files handed to ctlrender via `-ctl`) still
            // resolve. For imported modules fileName() is likewise
            // populated by the parser. The sidecar's parser then
            // resolves nested `import` clauses via CTL_MODULE_PATH
            // the normal way.
            //
            _data->sidecar->loadModule(owningModule->name(),
                                       owningModule->fileName(),
                                       std::string());
            loaded = true;
        } catch (...) {
            // Fall through to the live-sidecar fallback below.
        }
        if (loaded) {
            if (debugPhase) {
                auto t1 = std::chrono::high_resolution_clock::now();
                double ms = std::chrono::duration<double, std::milli>(
                                t1 - t0).count();
                std::cerr << "CTL Metal sidecar repair.loadModule("
                          << owningModule->name() << ") " << ms << " ms"
                          << std::endl;
            }
            if (!owningModule->fileName().empty())
                _data->cache.markSource(owningModule->fileName());

            //
            // Harvest every module-scope data symbol the sidecar now
            // holds — not just the one we were asked about, and not
            // just from the owning module. The recursive load above
            // brought in imports too, and all of their module-scope
            // consts deserve to land in the persistent cache so we
            // don't re-run this repair on the next warm run.
            //
            const SymbolTable &sideSym = _data->sidecar->symbolTable();
            size_t harvested = 0;
            for (auto it = sideSym.begin(); it != sideSym.end(); ++it) {
                const SymbolInfoPtr &info = it->second;
                if (!info) continue;
                if (!info->module()) continue;
                SimdDataAddrPtr addr = info->addr().cast<SimdDataAddr>();
                if (!addr || !addr->reg()) continue;
                DataTypePtr dt = info->dataType();
                if (!dt) continue;
                const size_t n = dt->objectSize();
                if (n == 0) continue;
                _data->cache.add(it->first, (*addr->reg())[0], n);
                ++harvested;
            }
            if (harvested > 0) _data->cacheDirty = true;
            if (debugPhase) {
                std::cerr << "CTL Metal sidecar repair.harvest total="
                          << harvested << std::endl;
            }
            bytes = _data->cache.lookup(absoluteName, byteCountOut);
            if (bytes) return bytes;
        }
    }

    //
    // Fallback to the live sidecar. Reached during cold runs when the
    // harvest pass hasn't yet populated a symbol the consumer is
    // asking about, and (post-repair) when the repair load populated
    // the sidecar's symbol table but the symbol is an alias or
    // per-invocation computation that doesn't survive harvest.
    //
    if (!_data->sidecar) return nullptr;
    SymbolInfoPtr sym = _data->sidecar->symbolTable().lookupSymbol(absoluteName);
    if (!sym) return nullptr;
    SimdDataAddrPtr addr = sym->addr().cast<SimdDataAddr>();
    if (!addr || !addr->reg()) return nullptr;
    DataTypePtr dt = sym->dataType();
    if (!dt) return nullptr;
    byteCountOut = dt->objectSize();
    return (*addr->reg())[0];
}

MetalInterpreter::~MetalInterpreter()
{
    delete _data;
}

size_t
MetalInterpreter::maxSamples() const
{
    // Coupled with `MetalFunctionArg::data()`'s pre-allocated capacity
    // (kDefaultVaryingCapacity). Callers like `ctlrender` treat
    // `maxSamples()` as the largest batch they can hand to
    // `callFunction(N)` and assume the arg buffers handed back by
    // `data()` have room for that many samples. Raising one value
    // without raising the other causes silent truncation of pixels
    // past the smaller of the two. Kept in sync deliberately — bump
    // both together.
    //
    // Sized to fit a UHD 4K frame (3840x2160 = 8,294,400 px) in one
    // dispatch so a 30-image 4K batch pays ~30 kernel launches instead
    // of ~3,810. Per-arg default pre-allocation is N*elementSize; for a
    // float varying arg this is 64 MB, comfortably within host RAM for
    // the handful of FunctionCalls ctlrender-metal keeps alive.
    return 16 * 1024 * 1024;
}

void
MetalInterpreter::setMaxInstCount(unsigned long count)
{
    _data->maxInstCount = count;
}

void
MetalInterpreter::abortAllPrograms()
{
    throw IEX_NAMESPACE::NoImplExc("MetalInterpreter::abortAllPrograms");
}

std::string
MetalInterpreter::deviceName() const
{
    if (!_data || !_data->device)
        return std::string();
    NSString *name = [_data->device name];
    return name ? std::string([name UTF8String]) : std::string();
}

FunctionCallPtr
MetalInterpreter::newFunctionCallInternal(const SymbolInfoPtr info,
                                          const std::string &functionName)
{
    FunctionTypePtr ftype = info->functionType();
    if (!ftype)
        throw IEX_NAMESPACE::TypeExc(
            std::string("CTL Metal backend: '") + functionName +
            "' is not a function.");

    MetalFunctionAddrPtr addr = info->addr().cast<MetalFunctionAddr>();
    if (!addr)
        throw IEX_NAMESPACE::LogicExc(
            std::string("CTL Metal backend: function '") + functionName +
            "' has no compiled kernel address. Did code generation run?");

    //
    // SymbolInfo::module() is const so that front-end inspectors can't
    // mutate a symbol's owning module, but calling a function needs to
    // lazily compile / cache an MTLLibrary on that module. The const_cast
    // is safe: MetalModule's mutability is an implementation detail of
    // the backend's pipeline cache.
    //
    const Module *constMod = info->module();
    if (!constMod)
        throw IEX_NAMESPACE::LogicExc(
            std::string("CTL Metal backend: function '") + functionName +
            "' has no owning module.");

    MetalModule *mod =
        const_cast<MetalModule *>(dynamic_cast<const MetalModule *>(constMod));
    if (!mod)
        throw IEX_NAMESPACE::LogicExc(
            std::string("CTL Metal backend: function '") + functionName +
            "' is defined in a non-Metal module.");

    return new MetalFunctionCall(
        *this, functionName, ftype, *mod, addr->kernelName());
}

Module *
MetalInterpreter::newModule(const std::string &moduleName,
                            const std::string &fileName)
{
    return new MetalModule(*this, moduleName, fileName);
}

LContext *
MetalInterpreter::newLContext(std::istream &file,
                              Module *module,
                              SymbolTable &symtab) const
{
    //
    // Preload the same source into the host-side SIMD sidecar before we
    // start Metal parsing. MetalVariableNode::generateCode relies on the
    // sidecar having already run `runInitCode` for this module — that's
    // how it substitutes evaluated values for module-scope const
    // initializers whose RHS contains a user-function call (which MSL
    // can't express in a `constant` global initializer).
    //
    // Nested `import` clauses recurse through this same override because
    // Interpreter::_loadModule dispatches to the virtual `newLContext`
    // for every module it loads (top-level and recursive). The sidecar's
    // own loadModule is a no-op on modules it already has, so already-
    // loaded dependencies are cheap.
    //
    //
    // Skip the sidecar load on a warm-cache HIT. The on-disk cache
    // preload has already populated every module-scope-const value
    // we'd otherwise evaluate via `sidecar.runInitCode`, saving
    // ~200-300 ms of parse + init-code on each invocation. On a
    // MISS (cold run, or invalidated cache) we load as before and
    // harvest every symbol the sidecar picks up.
    //
    // The tradeoff: the *live* sidecar symbol table stays empty on
    // HIT, so any path that wants a sidecar-evaluated bytes blob
    // must either find it in the cache directly or fall through the
    // lookupSidecarBytes warm-repair path — which requires the
    // owning module so it can demand-load just what's missing. The
    // two sites that matter (MetalVariableNode for module-scope
    // consts, MetalFunctionCall for `func$param` default statics)
    // both pass owningModule. A future caller that reads the
    // sidecar implicitly (e.g. `sidecar().symbolTable().lookup`)
    // would silently get empty results on warm runs; route such
    // reads through `lookupSidecarBytes` instead.
    //
    if (module && _data && _data->sidecar &&
        !_data->cacheValid &&
        !_data->sidecar->moduleIsLoaded(module->name())) {
        //
        // The istream we were handed is live — consume it into a string,
        // feed that to the sidecar, and rewind the stream so the Metal
        // parser can read the same source next. Both ifstream (file
        // loads) and stringstream (inline-source loads) from
        // Interpreter::_loadModule are seekable, so this round-trip is
        // safe. If that ever changes we'll need to intercept further
        // upstream.
        //
        std::string source((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
        const bool debugPhase = std::getenv("CTL_METAL_CACHE_DEBUG") != nullptr;
        auto t0 = std::chrono::high_resolution_clock::now();
        _data->sidecar->loadModule(module->name(),
                                   module->fileName(),
                                   source);
        if (debugPhase) {
            auto t1 = std::chrono::high_resolution_clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            std::cerr << "CTL Metal sidecar.loadModule(" << module->name()
                      << ") " << ms << " ms, " << source.size() << " bytes"
                      << std::endl;
        }

        //
        // Track the source file so the cache we write at flush time can
        // invalidate itself on the next run when any source changes.
        // Inline modules (no file on disk) have an empty fileName and
        // are silently skipped by markSource.
        //
        if (!module->fileName().empty())
            _data->cache.markSource(module->fileName());

        //
        // Harvest every module-scope data symbol with a populated reg
        // into the cache. This runs after each MetalInterpreter-driven
        // `sidecar.loadModule`, which itself resolves `import` clauses
        // recursively on the sidecar (SimdInterpreter's own parser) —
        // so the sidecar's symbol table after one call contains this
        // module's *and every transitively-imported module's* entries.
        // An earlier version filtered the harvest to symbols whose
        // owning module name matched the currently-loading module's,
        // which quietly dropped every imported module's consts from
        // the persistent cache. Cold runs masked the loss because
        // `lookupSidecarBytes` falls back to the live sidecar symbol
        // table — but on the next warm run the cache preload made
        // `newLContext` skip the sidecar load entirely, leaving no
        // live-sidecar fallback, and consumers of an imported const
        // hit the "host-side sidecar has no evaluated value" path.
        // The fix is to harvest *everything* the sidecar holds: its
        // symbol table only contains modules we've (transitively)
        // loaded, so there's nothing else to exclude.
        //
        const SymbolTable &sideSym = _data->sidecar->symbolTable();
        size_t harvested = 0;
        for (auto it = sideSym.begin(); it != sideSym.end(); ++it) {
            const SymbolInfoPtr &info = it->second;
            if (!info) continue;
            if (!info->module()) continue;
            SimdDataAddrPtr addr = info->addr().cast<SimdDataAddr>();
            if (!addr || !addr->reg()) continue;
            DataTypePtr dt = info->dataType();
            if (!dt) continue;
            const size_t n = dt->objectSize();
            if (n == 0) continue;
            _data->cache.add(it->first, (*addr->reg())[0], n);
            ++harvested;
        }
        if (debugPhase) {
            std::cerr << "CTL Metal sidecar harvest mod=" << module->name()
                      << " harvested=" << harvested
                      << std::endl;
        }
        _data->cacheDirty = true;

        file.clear();
        file.seekg(0);
    }

    return new MetalLContext(file, module, symtab);
}

} // namespace Ctl
