///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#ifndef INCLUDED_CTL_METAL_FUNCTION_CALL_H
#define INCLUDED_CTL_METAL_FUNCTION_CALL_H

//-----------------------------------------------------------------------------
//
//  class MetalFunctionCall -- Ctl::FunctionCall subclass for the Metal
//  backend.
//
//  Each FunctionArg owns a host-side std::vector<char> buffer. callFunction
//  forwards those buffers directly to MetalPipeline::dispatch via
//  MetalKernelBinding entries; under Apple UMA the bindings map to
//  MTLStorageMode.shared buffers so host writes are visible to the GPU
//  and vice versa with just a memcpy at the pipeline boundary.
//
//-----------------------------------------------------------------------------

#include <CtlFunctionCall.h>
#include <CtlType.h>
#include <string>
#include <vector>

namespace Ctl {

class MetalInterpreter;
class MetalModule;
class MetalFunctionArg;
typedef RcPtr<MetalFunctionArg> MetalFunctionArgPtr;

//
// MetalFunctionArg -- FunctionArg whose data() is a plain host-memory
// vector. Storage is lazily grown by MetalFunctionCall::callFunction to
// hold `numSamples * elementSize` bytes (or just `elementSize` for
// uniform arguments).
//
class MetalFunctionArg : public FunctionArg
{
  public:
    MetalFunctionArg(const std::string &name,
                     FunctionCall *func,
                     const DataTypePtr &type,
                     bool varying);

    virtual ~MetalFunctionArg();

    virtual char *      data();
    virtual bool        hasDefaultValue();
    virtual void        setDefaultValue();
    virtual size_t      elements() const;

    size_t              elementSize() const { return _elementSize; }

    // Ensure the host buffer can hold the required sample count; invoked
    // by MetalFunctionCall::callFunction before the dispatch so output
    // and intermediate reads land in a correctly sized region.
    void                ensureCapacity(size_t numSamples);
    size_t              currentBytes() const { return _buffer.size(); }

    // Populate the parameter's default-value bytes, one element worth.
    // Called once at MetalFunctionCall construction time after looking
    // up the parser-evaluated default static in the sidecar SIMD
    // interpreter's symbol table. `bytes` must point to `_elementSize`
    // bytes of initialized memory; the arg copies them.
    void                populateDefault(const char *bytes);

  private:
    size_t              _elementSize;
    std::vector<char>   _buffer;
    // One element's worth of default bytes. Empty when the parameter
    // was declared without a default — matching SIMD's null
    // `_defaultReg`. On `setDefaultValue()` this is fanned out across
    // every element of `_buffer` (replicating, for varying args).
    std::vector<char>   _defaultBytes;
};

class MetalFunctionCall : public FunctionCall
{
  public:

    MetalFunctionCall(MetalInterpreter &interpreter,
                      const std::string &name,
                      const FunctionTypePtr &type,
                      MetalModule &module,
                      const std::string &kernelName);

    virtual ~MetalFunctionCall();

    virtual void        callFunction(size_t numSamples);

  private:

    // Parameter-order view of the FunctionArg objects; lets callFunction
    // bind kernel buffers in declaration order, which is the order the
    // emitted MSL expects (__arg0 is parameters()[0], __arg1 is [1], ...).
    //
    struct ParamBinding
    {
        MetalFunctionArgPtr arg;
        bool                readable;
        bool                writable;
    };

    MetalInterpreter &          _interpreter;
    MetalModule &               _module;
    std::string                 _kernelName;
    std::vector<ParamBinding>   _bindings;
    // True when the CTL function has a non-void return type; cached at
    // construction so `callFunction` knows to bind a return-value
    // buffer between the params and the err-flag, matching the slot
    // layout that `MetalFunctionNode::generateCode` emits.
    bool                        _hasReturnBuffer;
};

} // namespace Ctl

#endif
