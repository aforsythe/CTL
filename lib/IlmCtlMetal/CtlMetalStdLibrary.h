///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#ifndef INCLUDED_CTL_METAL_STD_LIBRARY_H
#define INCLUDED_CTL_METAL_STD_LIBRARY_H

//-----------------------------------------------------------------------------
//
//  declareMetalStdLibrary -- register the Metal backend's stdlib symbols.
//
//  Mirrors `declareSimdStdLibrary` on the CPU SIMD backend. Populates the
//  interpreter's symbol table with FunctionType symbols bound to
//  `MetalStdLibFuncAddr`s so CTL programs can resolve calls like `fabs` or
//  `floor`. The actual MSL definitions for these helpers are emitted into
//  the module preamble at module-load time by
//  `MetalCodegen::emitStdLibPreamble`.
//
//-----------------------------------------------------------------------------

namespace Ctl {

class LContext;

void declareMetalStdLibrary(LContext &lcontext);

} // namespace Ctl

#endif
