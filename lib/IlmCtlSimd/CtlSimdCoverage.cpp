///////////////////////////////////////////////////////////////////////////
// Copyright (c) 2013 Academy of Motion Picture Arts and Sciences
// ("A.M.P.A.S."). Portions contributed by others as indicated.
// All rights reserved.
//
// A worldwide, royalty-free, non-exclusive right to copy, modify, create
// derivatives, and use, in source and binary forms, is hereby granted,
// subject to acceptance of this license. Performance of any of the
// aforementioned acts indicates acceptance to be bound by the following
// terms and conditions:
//
//  * Copies of source code, in whole or in part, must retain the
//    above copyright notice, this list of conditions and the
//    Disclaimer of Warranty.
//
//  * Use in binary form must retain the above copyright notice,
//    this list of conditions and the Disclaimer of Warranty in the
//    documentation and/or other materials provided with the distribution.
//
//  * Nothing in this license shall be deemed to grant any rights to
//    trademarks, copyrights, patents, trade secrets or any other
//    intellectual property of A.M.P.A.S. or any contributors, except
//    as expressly stated herein.
//
//  * Neither the name "A.M.P.A.S." nor the name of any other
//    contributors to this software may be used to endorse or promote
//    products derivative of or based on this software without express
//    prior written permission of A.M.P.A.S. or the contributors, as
//    appropriate.
//
// This license shall be construed pursuant to the laws of the State of
// California, and any disputes related thereto shall be subject to the
// jurisdiction of the courts therein.
//
// Disclaimer of Warranty: THIS SOFTWARE IS PROVIDED BY A.M.P.A.S. AND
// CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING,
// BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS
// FOR A PARTICULAR PURPOSE, AND NON-INFRINGEMENT ARE DISCLAIMED. IN NO
// EVENT SHALL A.M.P.A.S., OR ANY CONTRIBUTORS OR DISTRIBUTORS, BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, RESITUTIONARY,
// OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
// THE POSSIBILITY OF SUCH DAMAGE.
//
// WITHOUT LIMITING THE GENERALITY OF THE FOREGOING, THE ACADEMY
// SPECIFICALLY DISCLAIMS ANY REPRESENTATIONS OR WARRANTIES WHATSOEVER
// RELATED TO PATENT OR OTHER INTELLECTUAL PROPERTY RIGHTS IN THE ACADEMY
// COLOR ENCODING SYSTEM, OR APPLICATIONS THEREOF, HELD BY PARTIES OTHER
// THAN A.M.P.A.S., WHETHER DISCLOSED OR UNDISCLOSED.
///////////////////////////////////////////////////////////////////////////

#include "CtlSimdCoverage.h"
#include "CtlSimdInst.h"
#include "CtlSimdModule.h"

#ifdef CTL_ENABLE_COVERAGE

#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace Ctl {

namespace {

std::mutex                                               g_mutex;
std::unordered_map<std::string, std::map<int, uint64_t>> g_hits;     // file -> line -> count
std::unordered_set<const void*>                          g_discovered; // chain heads already walked

} // namespace

void SimdCoverage::registerLine (const std::string &file, int line)
{
    if (line <= 0) return;
    std::lock_guard<std::mutex> lk (g_mutex);
    g_hits[file].try_emplace (line, 0);
}

void SimdCoverage::record (const std::string &file, int line)
{
    if (line <= 0) return;
    std::lock_guard<std::mutex> lk (g_mutex);
    g_hits[file][line] += 1;
}

void SimdCoverage::discoverChain (const SimdInst *head,
                                  const std::string &startFile)
{
    if (!head) return;
    {
        // Hold g_mutex only for the dedup check, then release: the per-line
        // registerLine() calls below take g_mutex themselves, and recursive
        // discoverChain() calls re-enter this function and would deadlock if
        // we still held the lock here.
        std::lock_guard<std::mutex> lk (g_mutex);
        if (!g_discovered.insert (static_cast<const void*>(head)).second)
            return;     // already walked this chain
    }

    std::string current = startFile;
    for (const SimdInst *inst = head; inst; inst = inst->nextInPath())
    {
        // SimdFileNameInst sets the file context as we walk.
        if (auto fn = dynamic_cast<const SimdFileNameInst*>(inst))
        {
            current = fn->fileName();
            continue;       // file-name insts aren't real source lines
        }

        registerLine (current, inst->lineNumber());

        // Recurse into any sub-paths the inst owns.
        if (auto br = dynamic_cast<const SimdBranchInst*>(inst))
        {
            discoverChain (br->truePath(),  current);
            discoverChain (br->falsePath(), current);
        }
        else if (auto lp = dynamic_cast<const SimdLoopInst*>(inst))
        {
            discoverChain (lp->conditionPath(), current);
            discoverChain (lp->loopPath(),      current);
        }
        else if (auto cl = dynamic_cast<const SimdCallInst*>(inst))
        {
            discoverChain (cl->callPath(), current);
        }
    }
}

void SimdCoverage::discoverModule (const SimdModule *module)
{
    if (!module) return;
    const std::string fileName = module->fileName();
    for (const SimdInst *inst : module->code())
    {
        discoverChain (inst, fileName);
    }
}

bool SimdCoverage::flushToLcov (const std::string &outPath,
                                const std::string &testName)
{
    std::lock_guard<std::mutex> lk (g_mutex);
    std::FILE *fp = std::fopen (outPath.c_str(), "w");
    if (!fp) return false;

    std::set<std::string> sortedFiles;
    for (auto &kv : g_hits)
    {
        // Skip synthetic file contexts: empty (never had a SimdFileNameInst
        // fire before the chain walker reached it) or angle-bracketed
        // pseudo-paths like "<ctltest-embedded-testkit>".  These have no
        // on-disk file, would crash genhtml without --filter missing, and
        // surface as broken file links in Codecov.  See
        // moduletest/docs/COVERAGE.md.
        const std::string &f = kv.first;
        if (f.empty() || f == "unknown" || f.front() == '<') continue;
        // Normalise path so Codecov's diff-matcher (which is path-string
        // sensitive) can line up "tests/selftest//foo.ctl" with the
        // git-tree path "tests/selftest/foo.ctl".  std::fs::lexically_normal
        // collapses //, ./, foo/../ runs without touching the filesystem.
        sortedFiles.insert (
            std::filesystem::path(f).lexically_normal().string());
    }

    // Re-resolve normalised filename → original key when looking up hits,
    // since g_hits is still keyed by the as-recorded string.
    std::unordered_map<std::string, std::string> normToOrig;
    for (const auto &kv : g_hits)
    {
        const std::string &f = kv.first;
        if (f.empty() || f == "unknown" || f.front() == '<') continue;
        normToOrig[std::filesystem::path(f).lexically_normal().string()] = f;
    }

    for (const auto &file : sortedFiles)
    {
        const auto &lines = g_hits[normToOrig[file]];
        std::fprintf (fp, "TN:%s\n", testName.c_str());
        std::fprintf (fp, "SF:%s\n", file.c_str());

        std::size_t lf = 0, lh = 0;
        for (const auto &kv : lines)
        {
            std::fprintf (fp, "DA:%d,%llu\n", kv.first,
                          static_cast<unsigned long long> (kv.second));
            ++lf;
            if (kv.second > 0) ++lh;
        }
        std::fprintf (fp, "LF:%zu\n", lf);
        std::fprintf (fp, "LH:%zu\n", lh);
        std::fprintf (fp, "end_of_record\n");
    }

    std::fclose (fp);
    return true;
}

void SimdCoverage::reset ()
{
    std::lock_guard<std::mutex> lk (g_mutex);
    g_hits.clear();
    g_discovered.clear();
}

} // namespace Ctl

#else // !CTL_ENABLE_COVERAGE

namespace Ctl {
void SimdCoverage::registerLine   (const std::string &, int) {}
void SimdCoverage::record         (const std::string &, int) {}
void SimdCoverage::discoverChain  (const SimdInst *, const std::string &) {}
void SimdCoverage::discoverModule (const SimdModule *) {}
bool SimdCoverage::flushToLcov    (const std::string &, const std::string &) { return false; }
void SimdCoverage::reset          () {}
} // namespace Ctl

#endif // CTL_ENABLE_COVERAGE
