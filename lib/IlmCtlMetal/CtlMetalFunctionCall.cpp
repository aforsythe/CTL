///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#include <CtlMetalFunctionCall.h>
#include <CtlMetalDispatch.h>
#include <CtlMetalHalfExpLogTable.h>
#include <CtlMetalInterpreter.h>
#include <CtlMetalModule.h>
#include <CtlSimdAddr.h>
#include <CtlSimdInterpreter.h>
#include <CtlSimdReg.h>
#include <CtlSymbolTable.h>
#include <Iex.h>

#include <cstdint>
#include <cstring>

namespace Ctl {

//-----------------------------------------------------------------------------
// MetalFunctionArg
//-----------------------------------------------------------------------------

MetalFunctionArg::MetalFunctionArg(const std::string &name,
                                   FunctionCall *func,
                                   const DataTypePtr &type,
                                   bool varying)
    : FunctionArg(name, func, type, varying),
      _elementSize(type->alignedObjectSize()),
      _buffer()
{
}

MetalFunctionArg::~MetalFunctionArg()
{
}

//
// On first use, pre-size the backing buffer to a full batch so callers
// can write the N samples they plan to dispatch before calling
// callFunction. Matches the Simd backend, whose FunctionArg::data() is
// backed by a varying register pre-allocated to MAX_REG_SIZE samples.
//
// Must stay in sync with MetalInterpreter::maxSamples(). Callers fan
// uniform-source inputs across dst->elements() slots before dispatch,
// which means an arg pre-allocated to less than maxSamples() would
// deliver garbage to GPU threads past the smaller capacity. See the
// comment in CtlMetalInterpreter.mm for rationale on the 4K-fits-in-
// one-dispatch sizing.
//
static const size_t kDefaultVaryingCapacity = 16 * 1024 * 1024;

char *
MetalFunctionArg::data()
{
    if (_buffer.empty()) {
        const size_t n = isVarying() ? kDefaultVaryingCapacity : 1;
        _buffer.resize(n * _elementSize);
    }
    return _buffer.data();
}

bool
MetalFunctionArg::hasDefaultValue()
{
    return !_defaultBytes.empty();
}

void
MetalFunctionArg::setDefaultValue()
{
    //
    // Mirrors SimdFunctionArg::setDefaultValue. If the parameter was
    // declared without a default (or this is an output), `populateDefault`
    // was never called and we silently match SIMD's no-op behavior so
    // that callers who forget to check `hasDefaultValue()` first behave
    // identically across backends.
    //
    if (_defaultBytes.empty())
        return;

    //
    // Ensure the host buffer is sized, then replicate the single-element
    // default across every slot the buffer already holds. For non-varying
    // args that's one memcpy; for varying args it fans the uniform value
    // into every lane, matching SIMD's replication across MAX_REG_SIZE.
    //
    (void)data();
    const size_t slots =
        (_elementSize == 0) ? 0 : (_buffer.size() / _elementSize);
    for (size_t i = 0; i < slots; ++i)
        std::memcpy(_buffer.data() + i * _elementSize,
                    _defaultBytes.data(),
                    _elementSize);
}

void
MetalFunctionArg::populateDefault(const char *bytes)
{
    _defaultBytes.assign(bytes, bytes + _elementSize);
}

size_t
MetalFunctionArg::elements() const
{
    if (_elementSize == 0)
        return 0;
    return _buffer.size() / _elementSize;
}

void
MetalFunctionArg::ensureCapacity(size_t numSamples)
{
    const size_t n = isVarying() ? numSamples : 1;
    const size_t need = n * _elementSize;
    if (_buffer.size() < need)
        _buffer.resize(need);
}

//-----------------------------------------------------------------------------
// MetalFunctionCall
//-----------------------------------------------------------------------------

MetalFunctionCall::MetalFunctionCall(MetalInterpreter &interpreter,
                                     const std::string &name,
                                     const FunctionTypePtr &type,
                                     MetalModule &module,
                                     const std::string &kernelName)
    : FunctionCall(name),
      _interpreter(interpreter),
      _module(module),
      _kernelName(kernelName)
{
    const ParamVector &params = type->parameters();

    //
    // Walk the parameters in declaration order. Each parameter becomes
    // one kernel buffer binding; classification into inputArg / outputArg
    // follows CTL's read/write access, matching the Simd backend.
    //
    _bindings.reserve(params.size());

    size_t inputIdx = 0;
    size_t outputIdx = 0;
    for (const Param &p : params) {
        MetalFunctionArgPtr arg = new MetalFunctionArg(
            p.name, this, p.type, p.varying);

        //
        // If the parameter was declared with a default value, the
        // parser stored its evaluated result as a module-scope static
        // named `<funcAbsoluteName>$<paramName>`. The sidecar SIMD
        // interpreter has loaded the same module, so that static's
        // reg is already populated. Copy the bytes once here; callers
        // pick them up via `FunctionArg::setDefaultValue()`.
        //
        // Pass the owning module so the warm-cache repair path in
        // `lookupSidecarBytes` can demand-load the module (and its
        // imports transitively) when the on-disk cache preload was
        // a HIT but the default-value bytes didn't survive an older
        // incomplete harvest. Without this hook a stale cache would
        // leave `populateDefault` unset and the kernel would see
        // garbage for any `input varying float x = 1.;`-style param.
        //
        // Skip the lookup entirely when `p.defaultValue` is null —
        // the parser only emits the synthetic `$<paramName>` static
        // for params that actually have a default, so a null
        // defaultValue guarantees no static exists. Without this
        // gate, every default-less readable param missed in the
        // cache, fired the warm-cache repair path, and paid the
        // full sidecar load (~300 ms per invocation — visible as a
        // 2× regression on the 1-frame 2K bench for any CTL whose
        // main has no defaults, e.g. aces_combined).
        //
        if (p.isReadable() && p.defaultValue) {
            const std::string staticName = name + "$" + p.name;
            size_t nbytes = 0;
            const char *bytes =
                _interpreter.lookupSidecarBytes(staticName, nbytes, &module);
            if (bytes && nbytes > 0)
                arg->populateDefault(bytes);
        }

        ParamBinding pb;
        pb.arg = arg;
        pb.readable = p.isReadable();
        pb.writable = p.isWritable();
        _bindings.push_back(pb);

        if (pb.writable)
            setOutputArg(outputIdx++, arg);
        else
            setInputArg(inputIdx++, arg);
    }

    //
    // The return value is a conventional FunctionArg so that FunctionCall
    // invariants hold. For non-void returns the emitted kernel binds an
    // extra `device RET* __ret0` buffer between the user params and the
    // err flag; `_hasReturnBuffer` tells `callFunction` to push a
    // matching binding in the same slot.
    //
    DataTypePtr retType = type->returnType();
    _hasReturnBuffer = !retType.cast<VoidType>();
    setReturnValue(new MetalFunctionArg(
        "", this, retType, type->returnVarying()));
}

MetalFunctionCall::~MetalFunctionCall()
{
}

void
MetalFunctionCall::callFunction(size_t numSamples)
{
    if (numSamples == 0)
        throw IEX_NAMESPACE::ArgExc(
            "CTL Metal backend: callFunction numSamples must be > 0.");

    //
    // Resize each host-side arg buffer to match this dispatch; then flatten
    // the parameter list into MetalKernelBinding entries in declaration
    // order so __arg<i> in the emitted MSL aligns to parameters()[i].
    //
    // `bytes` is scaled to this dispatch (numSamples * elementSize for
    // varying, elementSize for uniform) rather than the buffer's full
    // capacity: the pre-allocation in `data()` is sized to the worst
    // case (maxSamples) so the uniform-fanout path in
    // ctlrender/transform.cc:201 fills every slot the kernel will read,
    // but transferring that whole capacity to/from the GPU every
    // dispatch would make small-N launches pay a fixed worst-case
    // memcpy cost. elementSize * N matches the slots actually touched
    // by the kernel and keeps the per-launch overhead flat in N.
    //
    std::vector<MetalKernelBinding> bindings;
    bindings.reserve(_bindings.size() + 1);

    for (const ParamBinding &pb : _bindings) {
        pb.arg->ensureCapacity(numSamples);

        MetalKernelBinding b;
        b.hostData = pb.arg->data();
        const size_t nSlots = pb.arg->isVarying() ? numSamples : 1;
        b.bytes = nSlots * pb.arg->elementSize();
        b.writeToGpu = pb.readable;
        b.readFromGpu = pb.writable;
        bindings.push_back(b);
    }

    //
    // Return-value buffer. Mirrors the `device RET* __ret0` kernel
    // argument emitted by `MetalFunctionNode::generateCode` — present
    // only when the CTL function has a non-void return type. Sized by
    // `ensureCapacity` to `numSamples` slots for a varying return or 1
    // slot for a uniform return (only thread 0 writes, avoiding a
    // cross-thread race).
    //
    if (_hasReturnBuffer) {
        MetalFunctionArgPtr retArg = returnValue().cast<MetalFunctionArg>();
        retArg->ensureCapacity(numSamples);
        MetalKernelBinding b;
        b.hostData = retArg->data();
        const size_t nSlots = retArg->isVarying() ? numSamples : 1;
        b.bytes = nSlots * retArg->elementSize();
        b.writeToGpu = false;
        b.readFromGpu = true;
        bindings.push_back(b);
    }

    //
    // Assertion error-flag buffer. Every kernel emitted by
    // `MetalFunctionNode::generateCode` declares an extra
    // `device atomic_uint* __ctl_err_flag` at the tail of its buffer
    // list, so we bind exactly one 4-byte uint here. We zero the host
    // side, copy it GPU-bound, then read back. A non-zero value means
    // some thread's `assert(cond)` observed `!cond` and we throw
    // `Iex::LogicExc` to mirror the CPU backend's exception surface.
    //
    uint32_t errFlag = 0;
    MetalKernelBinding errBinding;
    errBinding.hostData = reinterpret_cast<char *>(&errFlag);
    errBinding.bytes = sizeof(errFlag);
    errBinding.writeToGpu = true;
    errBinding.readFromGpu = true;
    bindings.push_back(errBinding);

    //
    // halfExpLog table buffers. The emitted kernel wrapper always
    // declares three trailing `device const` pointer buffers — see
    // `MetalFunctionNode::generateCode` in CtlMetalSyntaxTree.cpp —
    // regardless of whether the kernel's body actually calls exp_h /
    // log_h / log10_h / pow10_h / pow_h. The buffers must always be
    // bound; reading from an unbound buffer is UB even if no thread
    // ever dereferences the pointer. Using `persistent=true` means
    // the MTLBuffer is allocated + uploaded once per pipeline and
    // reused on every dispatch, so the ~739 KB of table data does
    // not re-cross the CPU/GPU boundary per call. Pointer identity
    // from the accessor functions is stable across the process
    // lifetime, which is what the per-pipeline cache keys on.
    //
    MetalKernelBinding log10Binding;
    log10Binding.hostData =
        const_cast<char *>(reinterpret_cast<const char *>(halfLog10Table()));
    log10Binding.bytes = 65536 * sizeof(unsigned int);
    log10Binding.writeToGpu = true;
    log10Binding.readFromGpu = false;
    log10Binding.persistent = true;
    bindings.push_back(log10Binding);

    MetalKernelBinding logBinding;
    logBinding.hostData =
        const_cast<char *>(reinterpret_cast<const char *>(halfLogTable()));
    logBinding.bytes = 65536 * sizeof(unsigned int);
    logBinding.writeToGpu = true;
    logBinding.readFromGpu = false;
    logBinding.persistent = true;
    bindings.push_back(logBinding);

    MetalKernelBinding expBinding;
    expBinding.hostData =
        const_cast<char *>(reinterpret_cast<const char *>(halfExpTable()));
    expBinding.bytes = halfExpTableSize() * sizeof(unsigned short);
    expBinding.writeToGpu = true;
    expBinding.readFromGpu = false;
    expBinding.persistent = true;
    bindings.push_back(expBinding);

    MetalPipeline &pipeline = _module.pipelineFor(_kernelName);
    pipeline.dispatch(bindings, numSamples);

    //
    // Bit 0 → `assert(cond)` observed `!cond` on some lane (CPU-parity
    //         LogicExc).
    // Bit 1 → a kernel-reachable call to `scatteredDataToGrid3D`
    //         received a *varying* input triple (`data`, `pMin`, or
    //         `pMax` differ across lanes). The Metal port assumes
    //         uniform inputs and does not run a per-lane RBF solve.
    // Bit 2 → `scatteredDataToGrid3D` received `dataSize` greater
    //         than the compile-time `kMaxRbfSamples` cap (256).
    //         Scratch buffers for the CG solve are sized against
    //         this cap; larger inputs can't land correct results.
    //
    if (errFlag & 4u)
        throw IEX_NAMESPACE::NoImplExc(
            "CTL Metal backend: scatteredDataToGrid3D dataSize exceeds "
            "the compile-time kMaxRbfSamples=256 cap. Rebuild with a "
            "larger cap or reduce the input sample count.");
    if (errFlag & 2u)
        throw IEX_NAMESPACE::NoImplExc(
            "CTL Metal backend: scatteredDataToGrid3D called with "
            "varying inputs (data/pMin/pMax differ across lanes). "
            "Only uniform inputs are supported on the Metal backend.");
    if (errFlag & 1u)
        throw IEX_NAMESPACE::LogicExc("CTL assertion failed.");
}

} // namespace Ctl
