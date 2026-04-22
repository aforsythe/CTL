///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#ifndef INCLUDED_CTL_METAL_ADDR_H
#define INCLUDED_CTL_METAL_ADDR_H

//-----------------------------------------------------------------------------
//
//  class MetalDataAddr -- DataAddr specialization for the Metal backend.
//
//  On the SIMD backend a DataAddr is a frame-pointer offset or a register
//  pointer. The Metal backend emits MSL source text, so each data address
//  is simply the MSL identifier that refers to the value in generated code.
//
//-----------------------------------------------------------------------------

#include <CtlAddr.h>
#include <string>

namespace Ctl {

class MetalDataAddr;
typedef RcPtr<MetalDataAddr> MetalDataAddrPtr;

class MetalDataAddr : public DataAddr
{
  public:

    explicit MetalDataAddr(const std::string &mslName);

    const std::string & mslName() const { return _mslName; }

    virtual void        print(int indent) const;

  private:

    std::string         _mslName;
};

//
// MetalStaticAddr -- identifies a module-scope MSL `constant` global.
// Subclasses MetalDataAddr so `mslNameOf` and friends resolve the
// identifier transparently wherever a regular local variable would be
// referenced. The distinction lets MetalVariableNode::generateCode
// emit the declaration into the codegen header section with the
// `constant` storage qualifier instead of into the kernel body.
//
class MetalStaticAddr;
typedef RcPtr<MetalStaticAddr> MetalStaticAddrPtr;

class MetalStaticAddr : public MetalDataAddr
{
  public:

    explicit MetalStaticAddr(const std::string &mslName);

    virtual void        print(int indent) const;
};

class MetalFunctionAddr;
typedef RcPtr<MetalFunctionAddr> MetalFunctionAddrPtr;

//
// MetalFunctionAddr -- CodeAddr specialization for the Metal backend.
//
// Each CTL function is emitted as one MSL compute kernel; the function
// symbol's addr holds the kernel's MSL name so that MetalFunctionCall
// can locate and dispatch it after codegen has run.
//
class MetalFunctionAddr : public CodeAddr
{
  public:

    //
    // `baseName` is the unqualified CTL function name (e.g. `setOne`).
    // MetalCodegen derives two MSL identifiers from it:
    //   - helperName():  "__ctl_<baseName>"   -- the inline helper
    //   - kernelName():  "<baseName>_kernel"  -- the per-CTL-function
    //                                            compute kernel entry
    //
    explicit MetalFunctionAddr(const std::string &baseName);

    const std::string & baseName() const   { return _baseName; }
    std::string         helperName() const { return "__ctl_" + _baseName; }
    std::string         kernelName() const { return _baseName + "_kernel"; }

    virtual void        print(int indent) const;

  private:

    std::string         _baseName;
};

class MetalStdLibFuncAddr;
typedef RcPtr<MetalStdLibFuncAddr> MetalStdLibFuncAddrPtr;

//
// MetalStdLibFuncAddr -- CodeAddr for a stdlib-provided function.
//
// When a CTL program calls a stdlib name (e.g. `fabs`), codegen emits
// an MSL call to the corresponding helper defined in the module's
// preamble by the stdlib-registration step. `mslName()` returns that
// helper identifier. Kept distinct from MetalFunctionAddr so the call
// site's MetalCallNode can tell user-defined CTL functions apart from
// stdlib helpers (the latter take value parameters and return by value
// rather than through a `thread &ret0` out-parameter).
//
class MetalStdLibFuncAddr : public CodeAddr
{
  public:

    explicit MetalStdLibFuncAddr(const std::string &mslName);

    const std::string & mslName() const { return _mslName; }

    virtual void        print(int indent) const;

  private:

    std::string         _mslName;
};

} // namespace Ctl

#endif
