///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

//
// Regression guard for the MTLBinaryArchive-backed PSO cache.
//
// Exercises three layers:
//   1. The pure digest/path C++ surface in CtlMetalShaderCache.h — stable,
//      sensitive to source + kernel-name changes, well-formed output.
//   2. The env-var contract advertised in the README: CTL_METAL_CACHE_DIR
//      override, CTL_METAL_DISABLE_CACHE=1 bypass.
//   3. End-to-end: after dispatching a trivial kernel with the cache
//      pointed at a fresh temp dir, at least one .binarchive file lands
//      on disk; a second MetalInterpreter run against the same temp dir
//      succeeds without clobbering (i.e. the stored archive is valid
//      input to the hit path).
//

#include "testMetalShaderCache.h"

#include <CtlFunctionCall.h>
#include <CtlMetalDevice.h>
#include <CtlMetalInterpreter.h>
#include <CtlMetalShaderCache.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace {

const char *kTrivialKernel =
    "namespace sc\n"
    "{\n"
    "    void addOne(input varying float x, output varying float y)\n"
    "    {\n"
    "        y = x + 1.0;\n"
    "    }\n"
    "}\n";

std::string
tempCacheDir(const char *tag)
{
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "/tmp/ctl_metal_cache_test_%d_%s",
                  static_cast<int>(getpid()), tag);
    return std::string(buf);
}

void
rmrfIfExists(const std::string &path)
{
    DIR *d = opendir(path.c_str());
    if (!d)
        return;
    while (struct dirent *e = readdir(d)) {
        if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, ".."))
            continue;
        std::string p = path + "/" + e->d_name;
        ::unlink(p.c_str());
    }
    closedir(d);
    ::rmdir(path.c_str());
}

size_t
countBinArchives(const std::string &dir)
{
    DIR *d = opendir(dir.c_str());
    if (!d)
        return 0;
    size_t n = 0;
    while (struct dirent *e = readdir(d)) {
        const char *ext = std::strrchr(e->d_name, '.');
        if (ext && !std::strcmp(ext, ".binarchive"))
            ++n;
    }
    closedir(d);
    return n;
}

void
testDigestStableAndWellFormed()
{
    const std::string a =
        Ctl::metalShaderCacheDigest("kernel void k() {}", "k");
    const std::string b =
        Ctl::metalShaderCacheDigest("kernel void k() {}", "k");
    assert(a == b);
    assert(a.size() == 16);
    for (char c : a) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        assert(hex);
    }
    std::cout << "  digest stable + 16-hex — ok" << std::endl;
}

void
testDigestSensitivity()
{
    const std::string base =
        Ctl::metalShaderCacheDigest("kernel void k() {}", "k");
    const std::string srcFlip =
        Ctl::metalShaderCacheDigest("kernel void k() {} // x", "k");
    const std::string nameFlip =
        Ctl::metalShaderCacheDigest("kernel void k() {}", "other");
    assert(base != srcFlip);
    assert(base != nameFlip);
    assert(srcFlip != nameFlip);

    //
    // FNV-1a must not collapse (src, name) boundaries — swapping the
    // split between the two inputs has to change the digest, otherwise
    // a kernel name prefix shared with source tail could alias.
    //
    const std::string split1 =
        Ctl::metalShaderCacheDigest("foo", "bar");
    const std::string split2 =
        Ctl::metalShaderCacheDigest("foob", "ar");
    assert(split1 != split2);
    std::cout << "  digest sensitive to src + kernel + split — ok"
              << std::endl;
}

void
testPathEnvOverride()
{
    const std::string dir = tempCacheDir("envpath");
    rmrfIfExists(dir);

    ::setenv("CTL_METAL_CACHE_DIR", dir.c_str(), 1);
    ::unsetenv("CTL_METAL_DISABLE_CACHE");

    const std::string digest = "deadbeefcafef00d";
    const std::string path = Ctl::metalShaderCachePathFor(digest);

    assert(!path.empty());
    assert(path.find(dir) == 0);
    assert(path.find(digest + ".binarchive") != std::string::npos);

    struct stat st;
    assert(::stat(dir.c_str(), &st) == 0);
    assert(S_ISDIR(st.st_mode));

    ::unsetenv("CTL_METAL_CACHE_DIR");
    rmrfIfExists(dir);
    std::cout << "  CTL_METAL_CACHE_DIR override + mkdir — ok" << std::endl;
}

void
testPathDisable()
{
    ::setenv("CTL_METAL_DISABLE_CACHE", "1", 1);
    const std::string path =
        Ctl::metalShaderCachePathFor("deadbeefcafef00d");
    assert(path.empty());
    ::unsetenv("CTL_METAL_DISABLE_CACHE");
    std::cout << "  CTL_METAL_DISABLE_CACHE=1 bypass — ok" << std::endl;
}

void
dispatchAddOne(size_t N, std::vector<float> &out)
{
    Ctl::MetalInterpreter interp;
    interp.loadModule("sc", "sc.ctl", kTrivialKernel);

    Ctl::FunctionCallPtr fn = interp.newFunctionCall("sc::addOne");
    assert(fn);

    const Ctl::FunctionArgPtr &in = fn->inputArg(0);
    float *inData = reinterpret_cast<float *>(in->data());
    for (size_t i = 0; i < N; ++i)
        inData[i] = static_cast<float>(i);

    fn->callFunction(N);

    const Ctl::FunctionArgPtr &outArg = fn->outputArg(0);
    const float *outData =
        reinterpret_cast<const float *>(outArg->data());
    out.assign(outData, outData + N);
}

void
testEndToEndCacheFileCreated()
{
    const std::string dir = tempCacheDir("e2e");
    rmrfIfExists(dir);

    ::setenv("CTL_METAL_CACHE_DIR", dir.c_str(), 1);
    ::unsetenv("CTL_METAL_DISABLE_CACHE");

    const size_t N = 4;
    std::vector<float> firstOut;
    dispatchAddOne(N, firstOut);

    for (size_t i = 0; i < N; ++i)
        assert(firstOut[i] == static_cast<float>(i) + 1.0f);

    //
    // First dispatch populated the archive. File must exist, have
    // content, and the directory must hold at least one .binarchive.
    //
    const size_t archivesAfterCold = countBinArchives(dir);
    assert(archivesAfterCold >= 1);

    //
    // Second dispatch must succeed against the now-populated cache and
    // produce identical outputs. That means the archive written by the
    // first process is valid input to the hit path — the "stale archive
    // detection & retry" fallback in MetalPipeline would surface as a
    // wrong answer or a thrown exception otherwise.
    //
    std::vector<float> secondOut;
    dispatchAddOne(N, secondOut);
    assert(secondOut == firstOut);

    //
    // Hit path must not clobber / re-emit the archive. A regression where
    // we serialize on every run would keep incrementing the directory; a
    // regression where we delete-and-rewrite would show a count dip.
    //
    const size_t archivesAfterWarm = countBinArchives(dir);
    assert(archivesAfterWarm == archivesAfterCold);

    ::unsetenv("CTL_METAL_CACHE_DIR");
    rmrfIfExists(dir);
    std::cout << "  end-to-end cold-then-warm — ok" << std::endl;
}

void
testEndToEndDisableSkipsDisk()
{
    const std::string dir = tempCacheDir("disabled");
    rmrfIfExists(dir);

    ::setenv("CTL_METAL_CACHE_DIR", dir.c_str(), 1);
    ::setenv("CTL_METAL_DISABLE_CACHE", "1", 1);

    const size_t N = 4;
    std::vector<float> out;
    dispatchAddOne(N, out);

    for (size_t i = 0; i < N; ++i)
        assert(out[i] == static_cast<float>(i) + 1.0f);

    //
    // Bypass env var must short-circuit before any disk I/O — the
    // directory should not even be created.
    //
    struct stat st;
    assert(::stat(dir.c_str(), &st) != 0);

    ::unsetenv("CTL_METAL_DISABLE_CACHE");
    ::unsetenv("CTL_METAL_CACHE_DIR");
    std::cout << "  end-to-end with cache disabled — ok" << std::endl;
}

} // anonymous namespace

void
testMetalShaderCache()
{
    std::cout << "Testing Metal PSO shader cache" << std::endl;

    //
    // Pure-C++ surface tests are safe to run without a GPU — no device
    // or MSL compile involved.
    //
    testDigestStableAndWellFormed();
    testDigestSensitivity();
    testPathEnvOverride();
    testPathDisable();

    if (!Ctl::metalBackendAvailable()) {
        std::cout << "  Metal backend unavailable on this host — skipping "
                     "end-to-end cases."
                  << std::endl;
        return;
    }

    testEndToEndCacheFileCreated();
    testEndToEndDisableSkipsDisk();
}
