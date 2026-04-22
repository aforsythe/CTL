///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#ifndef INCLUDED_CTL_METAL_COMPILE_OPTIONS_H
#define INCLUDED_CTL_METAL_COMPILE_OPTIONS_H

//-----------------------------------------------------------------------------
//
//  Factory for the MTLCompileOptions the CTL Metal backend uses for every
//  generated library. Centralized here so the precision gate is applied
//  uniformly and the Metal-4-vs-legacy math-mode branching only lives in
//  one place.
//
//  Obj-C++ only — include from .mm files.
//
//-----------------------------------------------------------------------------

#ifdef __OBJC__
#import <Metal/Metal.h>

namespace Ctl {

//
// Returns MTLCompileOptions with the IEEE-safe math mode selected.
//
// On macOS 15+ / Metal 4 this sets `mathMode = MTLMathModeSafe`.
// On older toolchains it falls back to the deprecated
// `fastMathEnabled = NO`.
//
// The returned object is autoreleased (ARC).
//
inline MTLCompileOptions *
makeIeeeSafeCompileOptions()
{
    MTLCompileOptions *opts = [[MTLCompileOptions alloc] init];
    if ([opts respondsToSelector:@selector(setMathMode:)]) {
        [opts setMathMode:MTLMathModeSafe];
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [opts setFastMathEnabled:NO];
#pragma clang diagnostic pop
    }
    return opts;
}

} // namespace Ctl

#endif // __OBJC__

#endif
