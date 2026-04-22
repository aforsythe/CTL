///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#ifndef INCLUDED_CTL_METAL_INTERPRETER_H
#define INCLUDED_CTL_METAL_INTERPRETER_H

//-----------------------------------------------------------------------------
//
//  class MetalInterpreter -- an Apple Silicon Metal GPU backend for the
//  Color Transformation Language. API-compatible with Ctl::SimdInterpreter
//  so that front-end code (e.g. ctlrender) can swap implementations.
//  Compiles each loaded module to MSL on first dispatch and caches the
//  resulting MTLComputePipelineState for reuse.
//
//-----------------------------------------------------------------------------

#include <CtlInterpreter.h>
#include <string>

namespace Ctl {

class SimdInterpreter;
class MetalSidecarCache;

class MetalInterpreter : public Interpreter
{
  public:

    MetalInterpreter();
    virtual ~MetalInterpreter();

    virtual size_t  maxSamples() const;
    virtual void    setMaxInstCount(unsigned long count);
    virtual void    abortAllPrograms();

    // Name of the MTLDevice this interpreter is bound to (for diagnostics).
    std::string     deviceName() const;

    //
    // The host-side SIMD sidecar. Every module that loads into this
    // MetalInterpreter also loads (and runs its init code) on this sidecar,
    // so that Metal codegen can substitute evaluated literal values for
    // module-scope const initializers whose RHS would otherwise be
    // rejected by MSL (user-function calls, RBF solves, etc.).
    //
    // Exposed so MetalVariableNode::generateCode and the test suite can
    // read back evaluated values by absolute CTL name.
    //
    SimdInterpreter &       sidecar() const;

    //
    // Try to preload the persistent sidecar cache for `topSourcePath` (an
    // absolute CTL source-file path). Returns true iff the on-disk cache
    // exists and every source it was built from still matches on disk —
    // in that case subsequent `loadFile`/`loadModule` calls will skip
    // the sidecar's parse + codegen + runInitCode pass and consumers
    // (MetalVariableNode::generateCode, MetalFunctionCall's default-arg
    // harvest) read bytes from the cache instead. Returns false on cold
    // / invalidated cache: callers then run sidecar loads as before and
    // should invoke `flushSidecarCache` after the top-level load
    // finishes so the cache is refreshed for the next invocation.
    //
    bool                    preloadSidecarCache (const std::string &topSourcePath);

    //
    // Persist the cache accumulated during sidecar loads to disk. Safe
    // to call even if preloadSidecarCache returned true (noop: nothing
    // fresh to write). Best-effort: on error the cache is simply not
    // written and the next cold run will re-harvest.
    //
    void                    flushSidecarCache (const std::string &topSourcePath);

    //
    // Consumer-facing accessor. Looks the symbol up in the cache first;
    // if absent (cache cold, or symbol not harvested), falls through to
    // the live sidecar's symbol table. Returns nullptr if neither has
    // it. `byteCountOut` is filled with the blob size on a hit.
    //
    const char *            lookupSidecarBytes (const std::string &absoluteName,
                                                size_t &byteCountOut) const;

    //
    // True iff `preloadSidecarCache` succeeded for the current session.
    // When true, `newLContext` skips the sidecar load path and
    // consumers route exclusively through the cache.
    //
    bool                    sidecarCacheValid () const;

  private:

    virtual FunctionCallPtr newFunctionCallInternal
                                    (const SymbolInfoPtr info,
                                     const std::string &functionName);

    virtual Module *    newModule
                            (const std::string &moduleName,
                             const std::string &fileName);

    virtual LContext *  newLContext
                            (std::istream &file,
                             Module *module,
                             SymbolTable &symtab) const;

    struct Data;
    Data *_data;
};

} // namespace Ctl

#endif
