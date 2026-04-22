///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#include <CtlMetalDevice.h>

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

namespace Ctl {

bool
metalBackendAvailable()
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            return false;
        }
        return [device supportsFamily:MTLGPUFamilyApple7];
    }
}

std::string
metalDeviceName()
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            return std::string();
        }
        NSString *name = [device name];
        return std::string([name UTF8String]);
    }
}

} // namespace Ctl
