///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#include <CtlMetalSidecarCache.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

namespace Ctl {

namespace {

//
// Magic + version guard rail. Bump the version byte on any layout change
// so stale files (written by an older binary) invalidate cleanly instead
// of mis-parsing into bogus bytes.
//
constexpr uint32_t kMagic   = 0x434D5343;   // 'CMSC' (Ctl Metal Sidecar Cache)
//
// Version history:
//   v1: initial layout (seconds-resolution mtime only).
//   v2: add nanosecond mtime to SourceEntry; tighten invalidation
//       against same-second edits that preserve size. Old v1 files
//       cleanly invalidate on the version check.
//
constexpr uint32_t kVersion = 2;

//
// Simple 64-bit FNV-1a over a std::string; used only to derive the
// cache filename from the absolute top-source path, so it doesn't need
// to be cryptographic.
//
uint64_t
fnv1a64 (const std::string &s)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return h;
}

std::string
hex16 (uint64_t v)
{
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(v));
    return std::string(buf, 16);
}

//
// Cache-root resolution. Metal is Apple-only so ~/Library/Caches is the
// conventional location; on non-Apple hosts we fall back to XDG paths
// so a future non-Metal backend port inherits the same interface.
//
std::string
cacheRoot ()
{
#if defined(__APPLE__)
    if (const char *h = std::getenv("HOME"))
        return std::string(h) + "/Library/Caches/ctl-metal-sidecar";
#else
    if (const char *x = std::getenv("XDG_CACHE_HOME"))
        return std::string(x) + "/ctl-metal-sidecar";
    if (const char *h = std::getenv("HOME"))
        return std::string(h) + "/.cache/ctl-metal-sidecar";
#endif
    return "/tmp/ctl-metal-sidecar";
}

//
// `mkdir -p` for the cache directory. Ignores EEXIST; returns true if
// the directory is present (or was created) at function exit.
//
bool
ensureDir (const std::string &path)
{
    std::string cur;
    cur.reserve(path.size() + 1);
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!cur.empty() && cur != "/") {
                if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST)
                    return false;
            }
        }
        if (i < path.size()) cur += path[i];
    }
    return true;
}

bool
writeU32 (std::ostream &os, uint32_t v)
{
    os.write(reinterpret_cast<const char *>(&v), sizeof(v));
    return static_cast<bool>(os);
}
bool
writeU64 (std::ostream &os, uint64_t v)
{
    os.write(reinterpret_cast<const char *>(&v), sizeof(v));
    return static_cast<bool>(os);
}
bool
writeI64 (std::ostream &os, int64_t v)
{
    os.write(reinterpret_cast<const char *>(&v), sizeof(v));
    return static_cast<bool>(os);
}
bool
writeStr (std::ostream &os, const std::string &s)
{
    if (s.size() > 0xFFFFFFFFull) return false;
    if (!writeU32(os, static_cast<uint32_t>(s.size()))) return false;
    os.write(s.data(), static_cast<std::streamsize>(s.size()));
    return static_cast<bool>(os);
}

bool
readU32 (std::istream &is, uint32_t &v)
{
    is.read(reinterpret_cast<char *>(&v), sizeof(v));
    return static_cast<bool>(is);
}
bool
readU64 (std::istream &is, uint64_t &v)
{
    is.read(reinterpret_cast<char *>(&v), sizeof(v));
    return static_cast<bool>(is);
}
bool
readI64 (std::istream &is, int64_t &v)
{
    is.read(reinterpret_cast<char *>(&v), sizeof(v));
    return static_cast<bool>(is);
}
bool
readStr (std::istream &is, std::string &s, uint32_t maxLen = 1u << 20)
{
    uint32_t n;
    if (!readU32(is, n)) return false;
    if (n > maxLen) return false;
    s.resize(n);
    if (n) is.read(&s[0], static_cast<std::streamsize>(n));
    return static_cast<bool>(is);
}

} // anonymous namespace


MetalSidecarCache::MetalSidecarCache ()
    : _valid(false)
{
}

MetalSidecarCache::~MetalSidecarCache ()
{
}

void
MetalSidecarCache::clear ()
{
    _bytes.clear();
    _sources.clear();
    _valid = false;
}

const char *
MetalSidecarCache::lookup (const std::string &absoluteName,
                           size_t &byteCountOut) const
{
    auto it = _bytes.find(absoluteName);
    if (it == _bytes.end()) {
        byteCountOut = 0;
        return nullptr;
    }
    byteCountOut = it->second.size();
    return it->second.data();
}

void
MetalSidecarCache::add (const std::string &absoluteName,
                        const char *bytes,
                        size_t byteCount)
{
    std::vector<char> &slot = _bytes[absoluteName];
    slot.assign(bytes, bytes + byteCount);
}

void
MetalSidecarCache::markSource (const std::string &path)
{
    //
    // Canonicalize before anything else so callers can pass in the
    // raw `module->fileName()` — which may be relative (user passed
    // `-ctl ./foo.ctl`) or go through a symlink (CTL_MODULE_PATH) —
    // and the invalidation key is still stable across cwds. A realpath
    // failure drops the entry: the file is probably missing, and
    // dropping the source makes the cache pessimistically miss on the
    // next preload, which is the safe behaviour.
    //
    char resolved[PATH_MAX];
    if (!::realpath(path.c_str(), resolved)) return;
    const std::string canon(resolved);

    int64_t mtime = 0;
    int64_t mtimeNs = 0;
    uint64_t size = 0;
    if (!statFile(canon, mtime, mtimeNs, size)) return;
    for (const SourceEntry &e : _sources)
        if (e.path == canon) return;             // dedupe
    SourceEntry entry{canon, mtime, mtimeNs, size};
    _sources.push_back(std::move(entry));
}

bool
MetalSidecarCache::statFile (const std::string &path,
                             int64_t &mtime,
                             int64_t &mtimeNs,
                             uint64_t &size)
{
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return false;
    mtime = static_cast<int64_t>(st.st_mtime);
#if defined(__APPLE__)
    mtimeNs = static_cast<int64_t>(st.st_mtimespec.tv_nsec);
#elif defined(__linux__)
    mtimeNs = static_cast<int64_t>(st.st_mtim.tv_nsec);
#else
    mtimeNs = 0;
#endif
    size  = static_cast<uint64_t>(st.st_size);
    return true;
}

std::string
MetalSidecarCache::cachePathFor (const std::string &topSourcePath)
{
    //
    // Fold `CTL_MODULE_PATH` into the cache key. The env var steers
    // import resolution, so flipping it between runs could pull a
    // different mod.ctl out of a different directory while leaving
    // the top path unchanged. The old per-source mtime/size
    // validation would silently approve the stale cache as long as
    // the previously-seen mod.ctl still existed and hadn't changed.
    // Hashing the env var value alongside the top path means a
    // swapped CTL_MODULE_PATH produces a distinct cache file and
    // misses pessimistically.
    //
    std::string key = topSourcePath;
    if (const char *mp = std::getenv("CTL_MODULE_PATH")) {
        key.push_back('\0');
        key.append(mp);
    }
    return cacheRoot() + "/" + hex16(fnv1a64(key)) + ".bin";
}

bool
MetalSidecarCache::tryLoad (const std::string &topSourcePath)
{
    //
    // DO NOT clear `_bytes` / `_sources` up front. A prior
    // `preloadSidecarCache(A)` may have populated them; on miss for
    // `topSourcePath` we want A's bytes to remain available for
    // lookup. All parsing happens into stack-local buffers; we only
    // merge into the member maps at the very end, after every
    // validation step has succeeded.
    //
    _valid = false;

    const std::string path = cachePathFor(topSourcePath);
    std::ifstream is(path, std::ios::binary);
    if (!is) return false;

    uint32_t magic = 0;
    uint32_t version = 0;
    if (!readU32(is, magic) || !readU32(is, version)) return false;
    if (magic != kMagic || version != kVersion) return false;

    //
    // The on-disk source list covers the top file and every import
    // the sidecar touched during the run that wrote this cache. A
    // mismatch on any entry (file missing, mtime-s, mtime-ns, or size
    // changed) invalidates the whole file — pessimistic because we
    // can't attribute individual bytes to individual sources.
    //
    uint32_t sourceCount = 0;
    if (!readU32(is, sourceCount)) return false;
    if (sourceCount == 0 || sourceCount > 1024) return false;

    std::vector<SourceEntry> persisted;
    persisted.reserve(sourceCount);
    for (uint32_t i = 0; i < sourceCount; ++i) {
        SourceEntry e{};
        int64_t mtime = 0;
        int64_t mtimeNs = 0;
        uint64_t size = 0;
        if (!readStr(is, e.path)) return false;
        if (!readI64(is, mtime))  return false;
        if (!readI64(is, mtimeNs)) return false;
        if (!readU64(is, size))   return false;
        e.mtime   = mtime;
        e.mtimeNs = mtimeNs;
        e.size    = size;
        persisted.push_back(std::move(e));
    }

    for (const SourceEntry &e : persisted) {
        int64_t curMtime = 0;
        int64_t curMtimeNs = 0;
        uint64_t curSize = 0;
        if (!statFile(e.path, curMtime, curMtimeNs, curSize)) return false;
        if (curMtime   != e.mtime ||
            curMtimeNs != e.mtimeNs ||
            curSize    != e.size) return false;
    }

    uint32_t symCount = 0;
    if (!readU32(is, symCount)) return false;
    if (symCount > (1u << 20)) return false;   // sanity

    //
    // Stage parsed symbols into a local vector; on any failure below
    // we bail without touching `_bytes`, so a partial read of a
    // corrupt file doesn't clobber the prior top's in-memory entries.
    //
    std::vector<std::pair<std::string, std::vector<char>>> staged;
    staged.reserve(symCount);
    for (uint32_t i = 0; i < symCount; ++i) {
        std::string name;
        uint32_t byteCount = 0;
        if (!readStr(is, name)) return false;
        if (!readU32(is, byteCount)) return false;
        if (byteCount > (1u << 24)) return false;   // 16 MiB per-symbol cap
        std::vector<char> blob(byteCount);
        if (byteCount) is.read(blob.data(),
                               static_cast<std::streamsize>(byteCount));
        if (!is) return false;
        staged.emplace_back(std::move(name), std::move(blob));
    }

    //
    // All reads passed. Merge into the member maps. Symbol names are
    // absolute (module-qualified) so duplicates across tops denote the
    // same value and overwriting is a safe no-op. Sources dedupe by
    // canonical path — two tops that share an import record the same
    // entry once.
    //
    for (auto &kv : staged) {
        _bytes[kv.first] = std::move(kv.second);
    }
    for (SourceEntry &e : persisted) {
        bool dup = false;
        for (const SourceEntry &existing : _sources) {
            if (existing.path == e.path) { dup = true; break; }
        }
        if (!dup) _sources.push_back(std::move(e));
    }

    _valid = true;
    return true;
}

void
MetalSidecarCache::writeToDisk (const std::string &topSourcePath)
{
    const std::string root = cacheRoot();
    if (!ensureDir(root)) return;

    //
    // Make sure the top source is in the source list so `tryLoad` on
    // the next invocation validates it. Imports are added during the
    // harvest pass via `markSource` from the MetalInterpreter
    // sidecar hook.
    //
    markSource(topSourcePath);

    const std::string path = cachePathFor(topSourcePath);

    //
    // Write to a temp file then atomically rename, so a crashed writer
    // can't leave a half-written cache that a concurrent reader would
    // then try (and fail) to parse.
    //
    const std::string tmp = path + ".tmp";
    std::ofstream os(tmp, std::ios::binary | std::ios::trunc);
    if (!os) return;

    if (!writeU32(os, kMagic)) return;
    if (!writeU32(os, kVersion)) return;
    if (!writeU32(os, static_cast<uint32_t>(_sources.size()))) return;
    for (const SourceEntry &e : _sources) {
        if (!writeStr(os, e.path)) return;
        if (!writeI64(os, e.mtime)) return;
        if (!writeI64(os, e.mtimeNs)) return;
        if (!writeU64(os, e.size)) return;
    }
    if (!writeU32(os, static_cast<uint32_t>(_bytes.size()))) return;
    for (const auto &kv : _bytes) {
        if (!writeStr(os, kv.first)) return;
        const std::vector<char> &blob = kv.second;
        if (!writeU32(os, static_cast<uint32_t>(blob.size()))) return;
        if (!blob.empty())
            os.write(blob.data(),
                     static_cast<std::streamsize>(blob.size()));
    }
    os.flush();
    if (!os) return;
    os.close();

    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
    }
}

} // namespace Ctl
