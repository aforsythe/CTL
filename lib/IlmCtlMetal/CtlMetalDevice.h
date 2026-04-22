///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#ifndef INCLUDED_CTL_METAL_DEVICE_H
#define INCLUDED_CTL_METAL_DEVICE_H

//-----------------------------------------------------------------------------
//
//  Runtime Metal device detection for the CTL Metal backend.
//
//  Header-only C++ API — the implementation is Objective-C++ (Metal.framework)
//  and lives in CtlMetalDevice.mm. Keeping this header pure C++ lets CTL
//  front-end code (and unit tests) probe for backend support without dragging
//  Obj-C into their translation units.
//
//-----------------------------------------------------------------------------

#include <string>

namespace Ctl {

//
// True iff the system has a default Metal device that supports
// MTLGPUFamilyApple7 (the Apple Silicon family required by this backend).
// Pre-Apple-Silicon Macs return false. Safe to call at any time.
//
bool metalBackendAvailable();

//
// Human-readable name of the default Metal device, or an empty string if
// there is no default device.
//
std::string metalDeviceName();

} // namespace Ctl

#endif
