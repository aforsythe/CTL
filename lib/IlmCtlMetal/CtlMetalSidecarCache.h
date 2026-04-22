///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#ifndef INCLUDED_CTL_METAL_SIDECAR_CACHE_H
#define INCLUDED_CTL_METAL_SIDECAR_CACHE_H

//-----------------------------------------------------------------------------
//
//  MetalSidecarCache -- persistent cache of sidecar-evaluated module-scope
//  values so cold ctlrender-metal invocations can skip the host-side
//  SimdInterpreter's parse + codegen + runInitCode pass (~393 ms on
//  aces_combined at the time this landed).
//
//  The sidecar exists to populate two consumer sites at Metal-parse time:
//  `MetalVariableNode::generateCode` (module-scope const initializers whose
//  RHS contains a user-function call) and `MetalFunctionCall`'s ctor
//  (default-parameter-value statics named `<funcAbs>$<paramName>`). Both
//  read a single (non-varying) byte blob from a `SimdReg`. That's all the
//  cache needs to preserve.
//
//  On disk the cache lives under `<cache-root>/ctl-metal-sidecar/<hash>.bin`
//  where `<cache-root>` is `$XDG_CACHE_HOME` or `~/.cache` on POSIX and
//  `~/Library/Caches` on macOS. `<hash>` is a stable hash of the absolute
//  path of the top-level CTL file. Each file header lists every CTL source
//  (top + transitive imports) with its absolute path, mtime (s + ns), and
//  size; a mismatch on any entry invalidates the cache.
//
//  Multi-top processes: the in-memory cache is a UNION of every top that
//  has been successfully preloaded in this process. `tryLoad` merges new
//  entries on top of whatever is already in memory instead of clearing,
//  so `ctlrender-metal -ctl A.ctl -ctl B.ctl` finds A's bytes in the
//  cache even after B's cache was loaded. Persisted cache files may
//  therefore contain bytes and sources from earlier tops; the source
//  list is the single source of truth for validation and covers every
//  byte in the file.
//
//-----------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Ctl {

class MetalSidecarCache
{
  public:

    MetalSidecarCache();
    ~MetalSidecarCache();

    //
    // Look up bytes previously stored under `absoluteName`. Returns
    // nullptr if the entry is absent. The returned pointer is owned by
    // the cache; it is invalidated by the next `tryLoad`, `add` for
    // the same name, `clear`, or destruction of the cache. Consumers
    // copy or consume the bytes immediately (see
    // `MetalVariableNode::generateCode` and
    // `MetalFunctionCall::populateDefault`).
    //
    const char *        lookup (const std::string &absoluteName,
                                size_t &byteCountOut) const;

    //
    // Insert (or overwrite) the bytes for `absoluteName`. Intended for
    // the harvest pass that runs right after a successful sidecar
    // `loadModule`; see `MetalInterpreter::harvestSidecarSymbols`.
    //
    void                add (const std::string &absoluteName,
                             const char *bytes,
                             size_t byteCount);

    //
    // Note a source file that contributed to this cache. Used both to
    // populate the cache's validation header on write and to check the
    // cached header against on-disk reality on read. The path is
    // canonicalized via `realpath` before storage so symlink swaps and
    // cwd-relative inputs produce the same validation key across runs.
    // If `realpath` fails (e.g., path component unreadable), the entry
    // is silently dropped — the cache will then pessimistically miss on
    // the next preload and re-harvest.
    //
    void                markSource (const std::string &path);

    //
    // Attempt to load a cache for the given top-level CTL source.
    // Returns true iff the cache file exists and every recorded source
    // entry still matches on disk. On success, the file's entries are
    // MERGED into the existing in-memory state (overwriting by name on
    // collision) and its sources are appended to `_sources` (deduped
    // by path). On failure the in-memory state is left untouched — so
    // a hit on top A followed by a miss on top B still has A's bytes
    // available for lookup.
    //
    bool                tryLoad (const std::string &topSourcePath);

    //
    // Persist the current in-memory state to disk keyed by
    // `topSourcePath`. Non-fatal on error (best-effort cache; a write
    // failure leaves the cache file stale, which a subsequent run will
    // naturally invalidate and rewrite).
    //
    void                writeToDisk (const std::string &topSourcePath);

    //
    // True iff the most recent `tryLoad` succeeded. Consumers use this
    // to decide whether the live sidecar parse can be skipped for the
    // top currently being loaded. The flag reflects the latest
    // preload, not the union: preload(A)=hit, preload(B)=miss sets it
    // to false even though A's bytes are still in memory. That's the
    // right signal — if B missed, B's modules need the sidecar even
    // though A's don't.
    //
    bool                isValid () const { return _valid; }

    //
    // Empty the in-memory state. Does not touch the on-disk file.
    //
    void                clear ();


  private:

    struct SourceEntry
    {
        std::string     path;
        int64_t         mtime;    // seconds since epoch
        int64_t         mtimeNs;  // nanoseconds (0 on hosts without ns stat)
        uint64_t        size;
    };

    static std::string  cachePathFor (const std::string &topSourcePath);
    static bool         statFile (const std::string &path,
                                  int64_t &mtime,
                                  int64_t &mtimeNs,
                                  uint64_t &size);

    std::unordered_map<std::string, std::vector<char>>  _bytes;
    std::vector<SourceEntry>                             _sources;
    bool                                                 _valid;
};

} // namespace Ctl

#endif
