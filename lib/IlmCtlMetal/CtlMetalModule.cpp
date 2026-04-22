///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#include <CtlMetalModule.h>
#include <CtlMetalDispatch.h>

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

std::string
MetalModule::nextStaticName()
{
    return "static" + std::to_string(_nextStaticIndex++);
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
        new MetalPipeline(_codegen.sourceForKernel(kernelName), kernelName));
    MetalPipeline &ref = *p;
    _pipelines.emplace(kernelName, std::move(p));
    return ref;
}

} // namespace Ctl
