///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#include <CtlMetalModule.h>
#include <CtlMetalCodegen.h>
#include <CtlMetalDispatch.h>
#include <CtlMetalInterpreter.h>

namespace Ctl {

MetalModule::MetalModule(MetalInterpreter &interpreter,
                         const std::string &name,
                         const std::string &fileName)
    : Module(name, fileName),
      _interpreter(interpreter)
{
}

MetalModule::~MetalModule()
{
}

void
MetalModule::runInitCode()
{
    //
    // Module-init for the Metal backend is evaluated CPU-side via the
    // SIMD interpreter sidecar; nothing to run here.
    //
}

MetalCodegen &
MetalModule::codegen()
{
    return _interpreter.codegen();
}

const MetalCodegen &
MetalModule::codegen() const
{
    return _interpreter.codegen();
}

std::string
MetalModule::nextStaticName()
{
    return _interpreter.codegen().nextStaticName();
}

MetalPipeline &
MetalModule::pipelineFor(const std::string &kernelName)
{
    auto it = _pipelines.find(kernelName);
    if (it != _pipelines.end())
        return *it->second;

    //
    // Compile only the requested kernel wrapper alongside the shared
    // helpers. Modules with many CTL functions (e.g. ACES v2) otherwise
    // pay a large cold MSL compile cost to type-check dozens of
    // kernel entry points that this call will never dispatch.
    //
    std::unique_ptr<MetalPipeline> p(
        new MetalPipeline(codegen().sourceForKernel(kernelName), kernelName));
    MetalPipeline &ref = *p;
    _pipelines.emplace(kernelName, std::move(p));
    return ref;
}

} // namespace Ctl
