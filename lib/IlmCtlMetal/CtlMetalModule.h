///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#ifndef INCLUDED_CTL_METAL_MODULE_H
#define INCLUDED_CTL_METAL_MODULE_H

//-----------------------------------------------------------------------------
//
//  class MetalModule -- Ctl::Module subclass for the Metal backend.
//
//  Owns the emitted MSL source string, the compiled MTLLibrary, and a
//  map from function names to their MTLComputePipelineState.
//
//-----------------------------------------------------------------------------

#include <CtlMetalCodegen.h>
#include <CtlModule.h>
#include <map>
#include <memory>
#include <string>

namespace Ctl {

class MetalInterpreter;
class MetalPipeline;

class MetalModule : public Module
{
  public:

    MetalModule(MetalInterpreter &interpreter,
                const std::string &name,
                const std::string &fileName);

    virtual ~MetalModule();

    virtual void runInitCode();

    MetalInterpreter &  interpreter() { return _interpreter; }

    //
    // The per-module MSL emitter. Syntax-tree-node generateCode() walks
    // write into this; the resulting source compiles on first kernel
    // dispatch.
    //
    MetalCodegen &      codegen()       { return _codegen; }
    const MetalCodegen &codegen() const { return _codegen; }

    //
    // Get (and lazily compile) the MetalPipeline for a kernel named
    // `kernelName`. The first call for a given name compiles the module's
    // current MSL source and builds a compute pipeline state; subsequent
    // calls hit an in-memory cache.
    //
    MetalPipeline &     pipelineFor(const std::string &kernelName);

    //
    // Allocate a fresh MSL identifier for a module-scope static variable
    // (one emitted as a `constant T name = value;` global in the codegen
    // header). Each call returns a name unique within this module:
    // "static0", "static1", ...
    //
    std::string         nextStaticName();

  private:

    MetalInterpreter &  _interpreter;
    MetalCodegen        _codegen;
    std::map<std::string, std::unique_ptr<MetalPipeline>> _pipelines;
    int                 _nextStaticIndex = 0;
};

} // namespace Ctl

#endif
