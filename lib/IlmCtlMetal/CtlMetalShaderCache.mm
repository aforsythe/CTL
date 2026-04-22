///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#include <CtlMetalShaderCache.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#import <Foundation/Foundation.h>

namespace Ctl {

std::string
metalShaderCacheDigest(const std::string &mslSource,
                       const std::string &kernelName)
{
    //
    // FNV-1a over (mslSource | '\0' | kernelName). Stable across
    // processes — unlike std::hash<std::string>, which libc++ seeds
    // randomly on every run — so the same inputs always land on the
    // same cache file.
    //
    uint64_t h = 14695981039346656037ULL;
    const uint64_t prime = 1099511628211ULL;
    for (unsigned char c : mslSource) {
        h ^= c;
        h *= prime;
    }
    h ^= 0;
    h *= prime;
    for (unsigned char c : kernelName) {
        h ^= c;
        h *= prime;
    }

    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(h));
    return std::string(buf);
}

std::string
metalShaderCachePathFor(const std::string &digest)
{
    if (std::getenv("CTL_METAL_DISABLE_CACHE"))
        return std::string();

    @autoreleasepool {
        NSString *dir = nil;
        if (const char *override_ = std::getenv("CTL_METAL_CACHE_DIR")) {
            dir = [NSString stringWithUTF8String:override_];
        } else {
            NSString *home = NSHomeDirectory();
            dir = [home stringByAppendingPathComponent:
                            @"Library/Caches/CTL/metal"];
        }

        NSError *err = nil;
        BOOL ok = [[NSFileManager defaultManager]
            createDirectoryAtPath:dir
            withIntermediateDirectories:YES
                       attributes:nil
                            error:&err];
        if (!ok) {
            //
            // If the cache directory can't be created (sandbox, read-only
            // home), degrade to "cache disabled". Returning an empty
            // path signals the caller to skip disk I/O rather than
            // aborting the whole compile.
            //
            return std::string();
        }

        NSString *file = [dir stringByAppendingPathComponent:
                                  [NSString stringWithFormat:@"%s.binarchive",
                                                             digest.c_str()]];
        return std::string([file UTF8String]);
    }
}

} // namespace Ctl
