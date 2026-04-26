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

#ifndef INCLUDED_CTL_SIMD_COVERAGE_H
#define INCLUDED_CTL_SIMD_COVERAGE_H

#include <cstdint>
#include <string>

namespace Ctl {

class SimdInst;

//
// Statement-level coverage tracker for the SIMD interpreter.  All entry
// points are no-ops unless the library is built with -DCTL_ENABLE_COVERAGE=1.
//
// Thread-safety: record() and registerLine() take an internal mutex; safe
// to call from concurrent interpreter threads.  flushToLcov() and reset()
// must not race with record().
//
// Performance: the mutex is taken on EVERY executed SIMD instruction when
// CTL_ENABLE_COVERAGE is on, so coverage builds are 5-15x slower than the
// equivalent coverage-off build.  This is acceptable for the moduletest
// selftest corpus (small fixtures, runs in seconds even with the slowdown)
// but means coverage runs are not appropriate for production-sized images
// or perf benchmarking.  If a future workload needs faster coverage, swap
// the global mutex-protected map for thread-local hit tables merged at
// flushToLcov time (gcov-style).
//
class SimdCoverage
{
  public:

    // Mark (file, line) as instrumentable with zero hits.  Idempotent.
    static void registerLine (const std::string &file, int line);

    // Increment hit count for (file, line).  Auto-registers if unseen.
    static void record (const std::string &file, int line);

    // Walk inst->_nextInPath chain from `head`, registering every
    // (currentFile, lineNumber) pair.  `currentFile` updates each time a
    // SimdFileNameInst is encountered.  Idempotent per `head` pointer;
    // safe to call repeatedly.
    //
    // Lifetime caveat: dedup keys on the raw `head` address.  If a
    // chain is freed and a new chain is allocated at the same address,
    // discovery will skip the new chain.  Callers are
    // SimdInst::executePath (lazy, per chain-head execution) and
    // SimdCoverage::discoverModule (eager, at load time).  Chains are
    // owned by SimdModule whose lifetime matches the interpreter; if
    // module reload / interpreter recycling becomes a thing, call
    // SimdCoverage::reset() at the recycle boundary.
    static void discoverChain (const SimdInst *head,
                               const std::string &startFile);

    // Eagerly walk every SimdInst in `module` and register its line.
    // Call this once per loaded module (at load time) to ensure functions
    // that are never called still appear in lcov as DA:N,0.  Without it,
    // lazy chain-walk discovery in executePath() only sees lines in
    // functions some test actually invokes.
    //
    // Implementation: calls discoverChain() on every inst in module's
    // _code vector with module's fileName() as the starting file context.
    // The per-head dedup in discoverChain makes this safe (and slightly
    // redundant — insts that are actually mid-chain do idempotent
    // registerLine calls).  The O(n²)-in-module-size cost is acceptable
    // for CI-only coverage builds.
    static void discoverModule (const class SimdModule *module);

    // Write accumulated coverage as an lcov v1 .info file at `outPath`.
    // Returns true on success, false on file I/O failure.
    static bool flushToLcov (const std::string &outPath,
                             const std::string &testName = "ctl");

    // Discard all recorded data.  Test-only.
    static void reset ();

    // True if the build defines CTL_ENABLE_COVERAGE; allows callers to
    // skip preparatory work cheaply.
    static constexpr bool enabled ()
    {
#ifdef CTL_ENABLE_COVERAGE
        return true;
#else
        return false;
#endif
    }
};

} // namespace Ctl

#endif
