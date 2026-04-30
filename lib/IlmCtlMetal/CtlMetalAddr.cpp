///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#include <CtlMetalAddr.h>
#include <iostream>

namespace Ctl {

MetalDataAddr::MetalDataAddr(const std::string &mslName)
    : _mslName(mslName)
{
}

void
MetalDataAddr::print(int indent) const
{
    for (int i = 0; i < indent; ++i)
        std::cout << " ";
    std::cout << "MetalDataAddr(" << _mslName << ")";
}

MetalStaticAddr::MetalStaticAddr(const std::string &mslName)
    : MetalDataAddr(mslName)
{
}

void
MetalStaticAddr::print(int indent) const
{
    for (int i = 0; i < indent; ++i)
        std::cout << " ";
    std::cout << "MetalStaticAddr(" << mslName() << ")";
}

MetalFunctionAddr::MetalFunctionAddr(const std::string &baseName)
    : _baseName(baseName)
{
}

void
MetalFunctionAddr::print(int indent) const
{
    for (int i = 0; i < indent; ++i)
        std::cout << " ";
    std::cout << "MetalFunctionAddr(" << _baseName << ")";
}

MetalStdLibFuncAddr::MetalStdLibFuncAddr(const std::string &mslName)
    : _mslName(mslName)
{
}

void
MetalStdLibFuncAddr::print(int indent) const
{
    for (int i = 0; i < indent; ++i)
        std::cout << " ";
    std::cout << "MetalStdLibFuncAddr(" << _mslName << ")";
}

} // namespace Ctl
