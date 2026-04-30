///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

//
// Regression guard for MetalSidecarCache's invariants.
//
// Exercises the three Tier-1 correctness fixes that landed on top of
// the initial persistence implementation:
//   1. Multi-top merge semantics — `preload(A)=HIT → preload(B)=MISS`
//      must not clobber A's bytes.
//   2. Nanosecond-resolution mtime invalidation — same-second,
//      size-preserving edits must invalidate the cache.
//   3. realpath canonicalization — a cwd-relative and an absolute
//      reference to the same source must dedupe to one header entry.
//
// Plus three baseline-integrity cases: round-trip correctness, v1
// rejection on the version check, size-staleness, corrupt-file
// recovery.
//
// No GPU involvement — this is a pure C++ class, the test isolates
// it from everything else. Cache root is redirected to a per-pid
// temp dir via HOME/XDG_CACHE_HOME override so the user's real cache
// is never touched.
//

#include "testMetalSidecarCache.h"

#include <CtlMetalSidecarCache.h>

#include "testRequire.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace {

std::string
tempRoot(const char *tag)
{
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "/tmp/ctl_sidecar_test_%d_%s",
                  static_cast<int>(getpid()), tag);
    return std::string(buf);
}

void
rmrf(const std::string &path)
{
    DIR *d = opendir(path.c_str());
    if (!d) {
        ::unlink(path.c_str());
        return;
    }
    while (struct dirent *e = readdir(d)) {
        if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, ".."))
            continue;
        std::string p = path + "/" + e->d_name;
        rmrf(p);
    }
    closedir(d);
    ::rmdir(path.c_str());
}

bool
mkdirP(const std::string &path)
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
        if (i < path.size()) cur.push_back(path[i]);
    }
    return true;
}

void
writeFile(const std::string &path, const std::string &contents)
{
    std::ofstream os(path, std::ios::binary | std::ios::trunc);
    os.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

void
setMtime(const std::string &path, time_t sec, long nsec)
{
    //
    // utimensat with a struct timespec[2] lets us pin both seconds
    // and nanoseconds precisely — needed for the ns-staleness case
    // that would otherwise be impossible to observe from userspace
    // (filesystem writes advance the clock by whatever granularity
    // the FS offers, which is usually well under a second).
    //
    struct timespec ts[2];
    ts[0].tv_sec = sec;  ts[0].tv_nsec = nsec;   // atime
    ts[1].tv_sec = sec;  ts[1].tv_nsec = nsec;   // mtime
    int rc = ::utimensat(AT_FDCWD, path.c_str(), ts, 0);
    REQUIRE(rc == 0);
}

//
// Fixture scaffolding. Each case sets HOME (and clears XDG_CACHE_HOME)
// so MetalSidecarCache's cacheRoot() lands inside `root/Library/Caches`
// (Apple) or `root/.cache` (POSIX). We control the whole subtree so
// cleanup is trivial.
//
struct Env
{
    std::string root;
    explicit Env(const char *tag) : root(tempRoot(tag))
    {
        rmrf(root);
        REQUIRE(mkdirP(root));
        ::setenv("HOME", root.c_str(), 1);
        ::unsetenv("XDG_CACHE_HOME");
    }
    ~Env()
    {
        rmrf(root);
    }
    std::string source(const char *name, const std::string &contents)
    {
        const std::string p = root + "/" + name;
        writeFile(p, contents);
        return p;
    }
};

void
addBytes(Ctl::MetalSidecarCache &c, const std::string &key,
         const std::string &blob)
{
    c.add(key, blob.data(), blob.size());
}

bool
lookupEq(const Ctl::MetalSidecarCache &c, const std::string &key,
         const std::string &expected)
{
    size_t n = 0;
    const char *p = c.lookup(key, n);
    if (!p) return false;
    if (n != expected.size()) return false;
    return std::memcmp(p, expected.data(), n) == 0;
}

void
testRoundTrip()
{
    Env e("roundtrip");
    const std::string src = e.source("a.ctl", "const int x = 1;\n");

    {
        Ctl::MetalSidecarCache c;
        c.markSource(src);
        addBytes(c, "a::x", "hello");
        addBytes(c, "a::y", std::string("\x00\x01\x02\x03", 4));
        c.writeToDisk(src);
    }

    Ctl::MetalSidecarCache c2;
    REQUIRE(c2.tryLoad(src));
    REQUIRE(c2.isValid());
    REQUIRE(lookupEq(c2, "a::x", "hello"));
    REQUIRE(lookupEq(c2, "a::y", std::string("\x00\x01\x02\x03", 4)));
    std::cout << "  round-trip write → tryLoad → lookup — ok" << std::endl;
}

void
testVersionMismatch()
{
    //
    // Hand-write a CMSC file with version=1 (the pre-nanosecond
    // layout) and assert tryLoad rejects it on the magic/version
    // check. This is what guarantees an in-place upgrade doesn't
    // return stale bytes from a binary written by an older build.
    //
    Env e("version");
    const std::string src = e.source("a.ctl", "x\n");

    Ctl::MetalSidecarCache probe;
    probe.markSource(src);
    probe.writeToDisk(src);

    std::string path;
    {
        //
        // Recover the path by running probe through its own cachePath
        // logic: tryLoad on a fresh instance should succeed, which
        // tells us the file exists at the expected location. Then we
        // re-open and overwrite with a v1 header.
        //
        Ctl::MetalSidecarCache tmp;
        REQUIRE(tmp.tryLoad(src));
    }
    //
    // The file lives at `$HOME/Library/Caches/ctl-metal-sidecar/<hash>.bin`
    // on Apple — scan the directory for the one .bin file we just
    // wrote and rewrite its version field to 1.
    //
    const std::string dir = e.root + "/Library/Caches/ctl-metal-sidecar";
    DIR *d = opendir(dir.c_str());
    REQUIRE(d);
    std::string binPath;
    while (struct dirent *de = readdir(d)) {
        const char *ext = std::strrchr(de->d_name, '.');
        if (ext && !std::strcmp(ext, ".bin")) {
            binPath = dir + "/" + de->d_name;
            break;
        }
    }
    closedir(d);
    REQUIRE(!binPath.empty());

    //
    // Overwrite bytes 4..7 with 1 (little-endian native uint32). We
    // don't care that the rest of the file is still a v2 body — the
    // magic/version pair is the gate.
    //
    std::fstream f(binPath, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(4);
    uint32_t v1 = 1;
    f.write(reinterpret_cast<const char *>(&v1), sizeof(v1));
    f.close();

    Ctl::MetalSidecarCache c;
    REQUIRE(!c.tryLoad(src));
    REQUIRE(!c.isValid());
    std::cout << "  v1 file rejected on version check — ok" << std::endl;
}

void
testMtimeSecondsStale()
{
    Env e("mtime_s");
    const std::string src = e.source("a.ctl", "abc\n");

    {
        Ctl::MetalSidecarCache c;
        c.markSource(src);
        addBytes(c, "a::k", "v");
        c.writeToDisk(src);
    }

    //
    // Advance mtime by 10 seconds. Size unchanged, same file bytes.
    // Must invalidate.
    //
    struct stat st;
    REQUIRE(::stat(src.c_str(), &st) == 0);
    setMtime(src, st.st_mtime + 10, 0);

    Ctl::MetalSidecarCache c;
    REQUIRE(!c.tryLoad(src));
    std::cout << "  mtime-s bump invalidates — ok" << std::endl;
}

void
testMtimeNanosecondsStale()
{
    //
    // The fix-#2 regression: pre-v2 the cache only tracked
    // seconds-resolution mtime, so a same-second, size-preserving
    // rewrite could go unnoticed. Here we pin the source mtime to
    // an explicit {sec, 0} before writing the cache, then rewrite
    // the same-size content with mtime {sec, 12345}. The seconds
    // component is unchanged; only the nanosecond component moves.
    //
    Env e("mtime_ns");
    const std::string src = e.source("a.ctl", "12345\n");
    setMtime(src, 1700000000, 0);

    {
        Ctl::MetalSidecarCache c;
        c.markSource(src);
        addBytes(c, "a::k", "v");
        c.writeToDisk(src);
    }

    //
    // Rewrite with identical bytes so size stays 6, then bump only
    // the nanosecond component of mtime.
    //
    writeFile(src, "12345\n");
    setMtime(src, 1700000000, 12345);

    Ctl::MetalSidecarCache c;
    REQUIRE(!c.tryLoad(src));
    std::cout << "  mtime-ns bump invalidates — ok" << std::endl;
}

void
testSizeStale()
{
    Env e("size");
    const std::string src = e.source("a.ctl", "abc\n");

    {
        Ctl::MetalSidecarCache c;
        c.markSource(src);
        addBytes(c, "a::k", "v");
        c.writeToDisk(src);
    }

    //
    // Rewrite source with different size. Pin mtime to the same
    // value the write stamps so only size differs.
    //
    struct stat st;
    REQUIRE(::stat(src.c_str(), &st) == 0);
    time_t keepS = st.st_mtime;
#if defined(__APPLE__)
    long keepNs = st.st_mtimespec.tv_nsec;
#elif defined(__linux__)
    long keepNs = st.st_mtim.tv_nsec;
#else
    long keepNs = 0;
#endif
    writeFile(src, "abcdef\n");    // 4 bytes → 7 bytes
    setMtime(src, keepS, keepNs);

    Ctl::MetalSidecarCache c;
    REQUIRE(!c.tryLoad(src));
    std::cout << "  size mismatch invalidates — ok" << std::endl;
}

void
testMultiTopMerge()
{
    //
    // The fix-#1 regression. Two tops A and B each live under their
    // own cache-file key. After seeding A's cache on disk, the
    // sequence preload(A)=HIT → preload(B)=MISS must leave A's
    // harvested bytes lookup-able in memory. Pre-fix, tryLoad cleared
    // `_bytes` at entry and wiped A before any deferred consumer
    // could read it.
    //
    Env e("multitop");
    const std::string a = e.source("a.ctl", "// A\n");
    const std::string b = e.source("b.ctl", "// B\n");

    //
    // Seed A's cache on disk.
    //
    {
        Ctl::MetalSidecarCache c;
        c.markSource(a);
        addBytes(c, "A::alpha", "AAAA");
        c.writeToDisk(a);
    }

    //
    // Live run: one cache instance, two preloads, B never seeded.
    //
    Ctl::MetalSidecarCache c;
    const bool hitA = c.tryLoad(a);
    REQUIRE(hitA);
    REQUIRE(c.isValid());
    REQUIRE(lookupEq(c, "A::alpha", "AAAA"));

    const bool hitB = c.tryLoad(b);
    REQUIRE(!hitB);            // no file for B
    REQUIRE(!c.isValid());     // latest preload's verdict

    //
    // The bug: preload(B) would clear `_bytes` here. Post-fix, A's
    // bytes must still be looked up after the miss.
    //
    REQUIRE(lookupEq(c, "A::alpha", "AAAA"));
    std::cout << "  A=HIT → B=MISS keeps A's bytes — ok" << std::endl;
}

void
testRealpathCanonicalization()
{
    //
    // The fix-#3 regression. Two references to the same file —
    // one via a symlink, one absolute — must produce a single
    // source entry in the cache, because realpath canonicalizes
    // before insertion. Before the fix the raw caller-supplied
    // string went in verbatim, so the dedupe check missed.
    //
    Env e("realpath");
    const std::string src = e.source("real.ctl", "// r\n");
    const std::string link = e.root + "/link.ctl";
    REQUIRE(::symlink(src.c_str(), link.c_str()) == 0);

    Ctl::MetalSidecarCache c;
    c.markSource(src);
    c.markSource(link);
    c.writeToDisk(src);

    //
    // Read the file back and count its source entries. The header
    // layout is:
    //   magic u32, version u32, sourceCount u32, [ path + mtime + ...
    //
    std::string binPath;
    const std::string dir = e.root + "/Library/Caches/ctl-metal-sidecar";
    DIR *d = opendir(dir.c_str());
    REQUIRE(d);
    while (struct dirent *de = readdir(d)) {
        const char *ext = std::strrchr(de->d_name, '.');
        if (ext && !std::strcmp(ext, ".bin")) {
            binPath = dir + "/" + de->d_name;
            break;
        }
    }
    closedir(d);
    REQUIRE(!binPath.empty());

    std::ifstream is(binPath, std::ios::binary);
    uint32_t magic = 0, version = 0, sourceCount = 0;
    is.read(reinterpret_cast<char *>(&magic), 4);
    is.read(reinterpret_cast<char *>(&version), 4);
    is.read(reinterpret_cast<char *>(&sourceCount), 4);
    REQUIRE(magic == 0x434D5343u);
    REQUIRE(version == 2u);
    REQUIRE(sourceCount == 1u);
    std::cout << "  symlink + abs dedupe via realpath — ok" << std::endl;
}

void
testCorruptFileRecovery()
{
    //
    // A partial read of a truncated cache file must return false
    // from tryLoad and leave prior in-memory state intact. The
    // merge-semantics fix relies on NOT modifying `_bytes` until
    // after every validation step passes.
    //
    Env e("corrupt");
    const std::string a = e.source("a.ctl", "// A\n");
    const std::string b = e.source("b.ctl", "// B\n");

    {
        Ctl::MetalSidecarCache c;
        c.markSource(a);
        addBytes(c, "A::k", "AAAA");
        c.writeToDisk(a);
    }
    {
        Ctl::MetalSidecarCache c;
        c.markSource(b);
        addBytes(c, "B::k", "BBBB");
        c.writeToDisk(b);
    }

    //
    // Truncate B's cache file to 32 bytes — well inside a valid
    // header but short of any complete source-entry payload.
    //
    const std::string dir = e.root + "/Library/Caches/ctl-metal-sidecar";
    DIR *d = opendir(dir.c_str());
    REQUIRE(d);
    size_t seen = 0;
    while (struct dirent *de = readdir(d)) {
        const char *ext = std::strrchr(de->d_name, '.');
        if (ext && !std::strcmp(ext, ".bin")) {
            ++seen;
        }
    }
    closedir(d);
    REQUIRE(seen == 2);

    //
    // We don't know which .bin belongs to which top without
    // replicating the hash, so just truncate both and confirm both
    // tryLoads fail without leaving partial state. First seed the
    // cache with A's bytes, then truncate, then tryLoad(b).
    //
    Ctl::MetalSidecarCache c;
    REQUIRE(c.tryLoad(a));
    REQUIRE(lookupEq(c, "A::k", "AAAA"));

    d = opendir(dir.c_str());
    while (struct dirent *de = readdir(d)) {
        const char *ext = std::strrchr(de->d_name, '.');
        if (ext && !std::strcmp(ext, ".bin")) {
            const std::string p = dir + "/" + de->d_name;
            ::truncate(p.c_str(), 12);
        }
    }
    closedir(d);

    REQUIRE(!c.tryLoad(b));
    REQUIRE(!c.isValid());
    //
    // Prior state must survive: A's entry still resolves.
    //
    REQUIRE(lookupEq(c, "A::k", "AAAA"));
    std::cout << "  corrupt-file bail preserves prior state — ok" << std::endl;
}

void
testModulePathKey()
{
    //
    // `CTL_MODULE_PATH` steers import resolution, so flipping it
    // between runs could pull a different mod.ctl out of a different
    // directory while leaving the top path unchanged. Fold it into
    // the cache file's hash so two values produce two distinct .bin
    // files and a swapped value misses pessimistically.
    //
    Env e("modpath");
    const std::string src = e.source("a.ctl", "// p\n");

    ::setenv("CTL_MODULE_PATH", "/nowhere/alpha", 1);
    {
        Ctl::MetalSidecarCache c;
        c.markSource(src);
        addBytes(c, "a::k", "alpha");
        c.writeToDisk(src);
    }

    ::setenv("CTL_MODULE_PATH", "/nowhere/beta", 1);
    {
        Ctl::MetalSidecarCache c;
        REQUIRE(!c.tryLoad(src));
    }

    //
    // Restore the first value and confirm the original bytes are
    // still reachable — each env value gets its own file, neither
    // clobbers the other.
    //
    ::setenv("CTL_MODULE_PATH", "/nowhere/alpha", 1);
    {
        Ctl::MetalSidecarCache c;
        REQUIRE(c.tryLoad(src));
        REQUIRE(lookupEq(c, "a::k", "alpha"));
    }

    //
    // After writing a second cache under the beta value, two .bin
    // files must coexist.
    //
    ::setenv("CTL_MODULE_PATH", "/nowhere/beta", 1);
    {
        Ctl::MetalSidecarCache c;
        c.markSource(src);
        addBytes(c, "a::k", "beta");
        c.writeToDisk(src);
    }

    const std::string dir = e.root + "/Library/Caches/ctl-metal-sidecar";
    DIR *d = opendir(dir.c_str());
    REQUIRE(d);
    size_t binCount = 0;
    while (struct dirent *de = readdir(d)) {
        const char *ext = std::strrchr(de->d_name, '.');
        if (ext && !std::strcmp(ext, ".bin")) ++binCount;
    }
    closedir(d);
    REQUIRE(binCount == 2);

    ::unsetenv("CTL_MODULE_PATH");
    std::cout << "  CTL_MODULE_PATH folded into cache key — ok" << std::endl;
}

} // anonymous namespace

void
testMetalSidecarCache()
{
    std::cout << "Testing Metal sidecar persistence cache" << std::endl;

    testRoundTrip();
    testVersionMismatch();
    testMtimeSecondsStale();
    testMtimeNanosecondsStale();
    testSizeStale();
    testMultiTopMerge();
    testRealpathCanonicalization();
    testCorruptFileRecovery();
    testModulePathKey();
}
