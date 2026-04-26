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

#include "testCoverage.h"
#include <CtlSimdCoverage.h>
#include <CtlSimdInst.h>
#include <CtlSimdInterpreter.h>
#include <CtlSimdModule.h>
#include <CtlFunctionCall.h>

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string slurp (const std::string &path)
{
    std::ifstream in (path);
    std::ostringstream os;
    os << in.rdbuf();
    return os.str();
}

//----------------------------------------------------------------------------
// Mock SimdInst helpers — let the discoverChain branch/loop/call tests
// build synthetic chains without spinning up an interpreter. SimdInst's
// pure-virtual execute/print are stubbed; the test only exercises the
// nextInPath / lineNumber / dynamic_cast path that discoverChain reads.
//----------------------------------------------------------------------------

void mockNoopThunk (const Ctl::SimdInst *, Ctl::SimdBoolMask &, Ctl::SimdXContext &)
{
    // unreachable — discoverChain never executes any inst.
}

class MockInst : public Ctl::SimdInst
{
  public:
    explicit MockInst (int line) : Ctl::SimdInst (mockNoopThunk, line) {}
    void execute (Ctl::SimdBoolMask &, Ctl::SimdXContext &) const override {}
    void print   (int) const override {}
};

// Owns a sequence of inst objects and links them in order via setNextInPath.
// Returns the head; safe to walk via nextInPath().
class Chain
{
  public:
    Ctl::SimdInst * push (std::unique_ptr<Ctl::SimdInst> inst)
    {
        Ctl::SimdInst *raw = inst.get();
        if (_last) _last->setNextInPath (raw);
        else       _head = raw;
        _last = raw;
        _own.push_back (std::move (inst));
        return raw;
    }
    Ctl::SimdInst * head () { return _head; }
  private:
    std::vector<std::unique_ptr<Ctl::SimdInst>> _own;
    Ctl::SimdInst *_head = nullptr;
    Ctl::SimdInst *_last = nullptr;
};

void testRecordAndFlush()
{
    using Ctl::SimdCoverage;
    if (!SimdCoverage::enabled())
    {
        std::cout << "  (coverage disabled at build time, skipping)\n";
        return;
    }
    SimdCoverage::reset();

    SimdCoverage::registerLine ("/tmp/foo.ctl", 10);
    SimdCoverage::registerLine ("/tmp/foo.ctl", 11);
    SimdCoverage::registerLine ("/tmp/foo.ctl", 12);
    SimdCoverage::record       ("/tmp/foo.ctl", 10);
    SimdCoverage::record       ("/tmp/foo.ctl", 10);
    SimdCoverage::record       ("/tmp/foo.ctl", 12);

    const std::string out = "/tmp/ctl_coverage_test.info";
    bool ok = SimdCoverage::flushToLcov (out, "test");
    assert (ok);

    std::string body = slurp (out);
    assert (body.find ("SF:/tmp/foo.ctl") != std::string::npos);
    assert (body.find ("DA:10,2")        != std::string::npos);
    assert (body.find ("DA:11,0")        != std::string::npos);
    assert (body.find ("DA:12,1")        != std::string::npos);
    assert (body.find ("LF:3")           != std::string::npos);
    assert (body.find ("LH:2")           != std::string::npos);
    assert (body.find ("TN:test")        != std::string::npos);
    assert (body.find ("end_of_record")  != std::string::npos);

    std::remove (out.c_str());
    SimdCoverage::reset();
}

void testEndToEnd()
{
    using Ctl::SimdCoverage;
    if (!SimdCoverage::enabled())
    {
        std::cout << "  (coverage disabled at build time, skipping)\n";
        return;
    }
    SimdCoverage::reset();

    // Write a tiny CTL module to a temp path.
    const std::string ctlPath = "/tmp/ctl_cov_e2e.ctl";
    {
        std::ofstream out (ctlPath);
        out << "void main(output float r, input float a)\n"          // line 1
               "{\n"                                                   // line 2
               "    if (a > 1000.0)\n"                                 // line 3
               "    {\n"                                               // line 4
               "        r = -1.0;\n"                                   // line 5  (unreached branch in main)
               "    }\n"                                               // line 6
               "    else\n"                                            // line 7
               "    {\n"                                               // line 8
               "        r = a + 1.0;\n"                                // line 9  (reached)
               "    }\n"                                               // line 10
               "}\n"                                                   // line 11
               "\n"                                                    // line 12
               "void never_called(output float r, input float a)\n"    // line 13
               "{\n"                                                   // line 14
               "    r = a * 2.0;\n"                                    // line 15  (function NEVER called)
               "}\n";                                                  // line 16
    }

    Ctl::SimdInterpreter interp;
    interp.setUserModulePath ({"/tmp"}, true);
    interp.loadModule ("ctl_cov_e2e");

    Ctl::FunctionCallPtr fc = interp.newFunctionCall ("main");
    fc->inputArg(0)->setDefaultValue();   // input a (default 0, takes false branch)
    fc->callFunction (1);

    const std::string out = "/tmp/ctl_cov_e2e.info";
    bool ok = SimdCoverage::flushToLcov (out, "e2e");
    assert (ok);

    std::string body;
    {
        std::ifstream in (out);
        std::ostringstream os;
        os << in.rdbuf();
        body = os.str();
    }
    // Find the SF: block for our fixture and search only inside it.
    const std::string sfMarker = "SF:" + ctlPath;
    auto sfPos = body.find (sfMarker);
    assert (sfPos != std::string::npos);

    auto endPos = body.find ("end_of_record", sfPos);
    assert (endPos != std::string::npos);

    const std::string ourBlock = body.substr (sfPos, endPos - sfPos);
    // False branch (line 9) was reached:
    assert (ourBlock.find ("DA:9,")  != std::string::npos);
    assert (ourBlock.find ("DA:9,0") == std::string::npos);
    // True branch (line 5) was registered by discoverChain but never hit:
    assert (ourBlock.find ("DA:5,0") != std::string::npos);
    // never_called() is loaded but never invoked.  Eager discoverModule
    // must register its lines so they appear as DA:15,0 (not omitted).
    assert (ourBlock.find ("DA:15,0") != std::string::npos);

    std::remove (ctlPath.c_str());
    std::remove (out.c_str());
    SimdCoverage::reset();
}

// ============================================================================
// Hardening pass — broaden coverage of the SimdCoverage public API.
// ============================================================================

void testRegisterLineIdempotent()
{
    using Ctl::SimdCoverage;
    if (!SimdCoverage::enabled()) return;
    SimdCoverage::reset();

    // line <= 0 is rejected.
    SimdCoverage::registerLine ("/tmp/idem.ctl", 0);
    SimdCoverage::registerLine ("/tmp/idem.ctl", -1);
    // Same (file,line) registered twice: still one DA entry.
    SimdCoverage::registerLine ("/tmp/idem.ctl", 7);
    SimdCoverage::registerLine ("/tmp/idem.ctl", 7);

    const std::string out = "/tmp/ctl_cov_idem.info";
    bool ok = SimdCoverage::flushToLcov (out, "idem");
    assert (ok);

    std::string body = slurp (out);
    // No DA:0,* or DA:-1,* — line<=0 must be silently ignored.
    assert (body.find ("DA:0,")  == std::string::npos);
    assert (body.find ("DA:-1,") == std::string::npos);
    // Exactly one DA:7,0 line.
    auto first = body.find ("DA:7,0");
    assert (first != std::string::npos);
    assert (body.find ("DA:7,0", first + 1) == std::string::npos);
    // LF == 1 (one instrumentable line registered).
    assert (body.find ("LF:1") != std::string::npos);

    std::remove (out.c_str());
    SimdCoverage::reset();
}

void testRecordAccumulatesAndAutoRegisters()
{
    using Ctl::SimdCoverage;
    if (!SimdCoverage::enabled()) return;
    SimdCoverage::reset();

    // line<=0 rejected for record() too.
    SimdCoverage::record ("/tmp/acc.ctl", 0);
    SimdCoverage::record ("/tmp/acc.ctl", -2);
    // Auto-register on first record (no prior registerLine call).
    SimdCoverage::record ("/tmp/acc.ctl", 5);
    SimdCoverage::record ("/tmp/acc.ctl", 5);
    SimdCoverage::record ("/tmp/acc.ctl", 5);

    const std::string out = "/tmp/ctl_cov_acc.info";
    assert (SimdCoverage::flushToLcov (out, "acc"));
    std::string body = slurp (out);

    assert (body.find ("DA:5,3")  != std::string::npos);
    assert (body.find ("DA:0,")   == std::string::npos);
    assert (body.find ("LF:1")    != std::string::npos);
    assert (body.find ("LH:1")    != std::string::npos);

    std::remove (out.c_str());
    SimdCoverage::reset();
}

void testResetClearsHitsAndDedup()
{
    using Ctl::SimdCoverage;
    if (!SimdCoverage::enabled()) return;
    SimdCoverage::reset();

    Chain c;
    c.push (std::unique_ptr<Ctl::SimdInst>(new MockInst (10)));
    c.push (std::unique_ptr<Ctl::SimdInst>(new MockInst (11)));

    SimdCoverage::discoverChain (c.head(), "/tmp/r.ctl");
    SimdCoverage::record ("/tmp/r.ctl", 10);

    {
        const std::string out = "/tmp/ctl_cov_reset_pre.info";
        assert (SimdCoverage::flushToLcov (out, "r"));
        std::string body = slurp (out);
        assert (body.find ("DA:10,1") != std::string::npos);
        assert (body.find ("DA:11,0") != std::string::npos);
        std::remove (out.c_str());
    }

    SimdCoverage::reset();

    // After reset, recording the same line starts at 1 (g_hits cleared) and
    // discoverChain on the same head re-registers (g_discovered cleared).
    SimdCoverage::record ("/tmp/r.ctl", 10);
    SimdCoverage::discoverChain (c.head(), "/tmp/r.ctl");

    const std::string out = "/tmp/ctl_cov_reset_post.info";
    assert (SimdCoverage::flushToLcov (out, "r"));
    std::string body = slurp (out);
    assert (body.find ("DA:10,1") != std::string::npos);
    assert (body.find ("DA:11,0") != std::string::npos);
    std::remove (out.c_str());

    SimdCoverage::reset();
}

void testDiscoverChainBranch()
{
    using Ctl::SimdCoverage;
    if (!SimdCoverage::enabled()) return;
    SimdCoverage::reset();

    // truePath: lines [20, 21]   falsePath: line [30]
    Chain truePath;
    truePath.push (std::unique_ptr<Ctl::SimdInst>(new MockInst (20)));
    truePath.push (std::unique_ptr<Ctl::SimdInst>(new MockInst (21)));

    Chain falsePath;
    falsePath.push (std::unique_ptr<Ctl::SimdInst>(new MockInst (30)));

    Ctl::SimdBranchInst br (truePath.head(), falsePath.head(),
                            /*mergeResults=*/false, /*lineNumber=*/15);

    SimdCoverage::discoverChain (&br, "/tmp/b.ctl");

    const std::string out = "/tmp/ctl_cov_br.info";
    assert (SimdCoverage::flushToLcov (out, "br"));
    std::string body = slurp (out);
    assert (body.find ("DA:15,0") != std::string::npos);   // branch inst itself
    assert (body.find ("DA:20,0") != std::string::npos);   // true path
    assert (body.find ("DA:21,0") != std::string::npos);
    assert (body.find ("DA:30,0") != std::string::npos);   // false path
    std::remove (out.c_str());
    SimdCoverage::reset();
}

void testDiscoverChainLoop()
{
    using Ctl::SimdCoverage;
    if (!SimdCoverage::enabled()) return;
    SimdCoverage::reset();

    Chain cond;
    cond.push (std::unique_ptr<Ctl::SimdInst>(new MockInst (40)));
    Chain body;
    body.push (std::unique_ptr<Ctl::SimdInst>(new MockInst (50)));
    body.push (std::unique_ptr<Ctl::SimdInst>(new MockInst (51)));

    Ctl::SimdLoopInst lp (cond.head(), body.head(), /*lineNumber=*/35);

    SimdCoverage::discoverChain (&lp, "/tmp/l.ctl");

    const std::string out = "/tmp/ctl_cov_lp.info";
    assert (SimdCoverage::flushToLcov (out, "lp"));
    std::string s = slurp (out);
    assert (s.find ("DA:35,0") != std::string::npos);
    assert (s.find ("DA:40,0") != std::string::npos);
    assert (s.find ("DA:50,0") != std::string::npos);
    assert (s.find ("DA:51,0") != std::string::npos);
    std::remove (out.c_str());
    SimdCoverage::reset();
}

void testDiscoverChainCall()
{
    using Ctl::SimdCoverage;
    if (!SimdCoverage::enabled()) return;
    SimdCoverage::reset();

    Chain callBody;
    callBody.push (std::unique_ptr<Ctl::SimdInst>(new MockInst (60)));
    callBody.push (std::unique_ptr<Ctl::SimdInst>(new MockInst (61)));

    Ctl::SimdCallInst cl (callBody.head(),
                          /*numParameters=*/0,
                          /*lineNumber=*/55);

    SimdCoverage::discoverChain (&cl, "/tmp/c.ctl");

    const std::string out = "/tmp/ctl_cov_cl.info";
    assert (SimdCoverage::flushToLcov (out, "cl"));
    std::string s = slurp (out);
    assert (s.find ("DA:55,0") != std::string::npos);
    assert (s.find ("DA:60,0") != std::string::npos);
    assert (s.find ("DA:61,0") != std::string::npos);
    std::remove (out.c_str());
    SimdCoverage::reset();
}

void testDiscoverChainFileNameTransition()
{
    using Ctl::SimdCoverage;
    if (!SimdCoverage::enabled()) return;
    SimdCoverage::reset();

    // Chain: [Mock@1, FileNameInst("file2"), Mock@2]
    // Expected: file1 has DA:1,0; file2 has DA:2,0. The file-name inst
    // itself is NOT a real source line and must not register.
    Chain c;
    c.push (std::unique_ptr<Ctl::SimdInst>(new MockInst (1)));
    c.push (std::unique_ptr<Ctl::SimdInst>(
                new Ctl::SimdFileNameInst ("/tmp/file2.ctl",
                                           /*lineNumber=*/9999)));
    c.push (std::unique_ptr<Ctl::SimdInst>(new MockInst (2)));

    SimdCoverage::discoverChain (c.head(), "/tmp/file1.ctl");

    const std::string out = "/tmp/ctl_cov_fn.info";
    assert (SimdCoverage::flushToLcov (out, "fn"));
    std::string body = slurp (out);

    // file1 block contains DA:1,0 only.
    auto sf1 = body.find ("SF:/tmp/file1.ctl");
    auto sf2 = body.find ("SF:/tmp/file2.ctl");
    assert (sf1 != std::string::npos);
    assert (sf2 != std::string::npos);

    auto eof1 = body.find ("end_of_record", sf1);
    auto eof2 = body.find ("end_of_record", sf2);
    assert (eof1 != std::string::npos);
    assert (eof2 != std::string::npos);

    std::string block1 = body.substr (sf1, eof1 - sf1);
    std::string block2 = body.substr (sf2, eof2 - sf2);

    assert (block1.find ("DA:1,0")    != std::string::npos);
    assert (block1.find ("DA:2,")     == std::string::npos);
    assert (block1.find ("DA:9999,")  == std::string::npos);   // file-name inst not registered
    assert (block2.find ("DA:2,0")    != std::string::npos);
    assert (block2.find ("DA:1,")     == std::string::npos);
    assert (block2.find ("DA:9999,")  == std::string::npos);

    std::remove (out.c_str());
    SimdCoverage::reset();
}

void testDiscoverChainDedup()
{
    using Ctl::SimdCoverage;
    if (!SimdCoverage::enabled()) return;
    SimdCoverage::reset();

    Chain c;
    c.push (std::unique_ptr<Ctl::SimdInst>(new MockInst (70)));

    SimdCoverage::discoverChain (c.head(), "/tmp/d.ctl");
    SimdCoverage::record ("/tmp/d.ctl", 70);    // hit count -> 1
    SimdCoverage::discoverChain (c.head(), "/tmp/d.ctl");   // must be dedup'd

    // If discoverChain re-walked, registerLine would be called again, but
    // try_emplace is no-op when key exists, so the hit count is still 1.
    // The stronger guarantee (chain-walk skipped entirely) is observable
    // only via instrumentation of g_discovered, so we settle for behaviour:
    // record before the second discoverChain still shows DA:70,1.
    const std::string out = "/tmp/ctl_cov_dd.info";
    assert (SimdCoverage::flushToLcov (out, "dd"));
    std::string body = slurp (out);
    assert (body.find ("DA:70,1") != std::string::npos);
    std::remove (out.c_str());
    SimdCoverage::reset();
}

void testFlushSyntheticFilter()
{
    using Ctl::SimdCoverage;
    if (!SimdCoverage::enabled()) return;
    SimdCoverage::reset();

    SimdCoverage::registerLine ("",                              5);
    SimdCoverage::registerLine ("unknown",                       5);
    SimdCoverage::registerLine ("<ctltest-embedded-testkit>",    5);
    SimdCoverage::registerLine ("/tmp/real.ctl",                 5);

    const std::string out = "/tmp/ctl_cov_sf.info";
    assert (SimdCoverage::flushToLcov (out, "sf"));
    std::string body = slurp (out);

    // Real path appears.
    assert (body.find ("SF:/tmp/real.ctl")           != std::string::npos);
    // Synthetic paths do NOT appear.
    assert (body.find ("SF:\n")                      == std::string::npos);
    assert (body.find ("SF:unknown")                 == std::string::npos);
    assert (body.find ("<ctltest-embedded-testkit>") == std::string::npos);

    std::remove (out.c_str());
    SimdCoverage::reset();
}

void testFlushPathNormalization()
{
    using Ctl::SimdCoverage;
    if (!SimdCoverage::enabled()) return;
    SimdCoverage::reset();

    // Three forms that all normalise to "/tmp/n/x.ctl".
    SimdCoverage::record ("/tmp/n//x.ctl",        10);
    SimdCoverage::record ("/tmp/n/./x.ctl",       10);
    SimdCoverage::record ("/tmp/n/y/../x.ctl",    10);

    const std::string out = "/tmp/ctl_cov_norm.info";
    assert (SimdCoverage::flushToLcov (out, "n"));
    std::string body = slurp (out);

    // Exactly one normalised SF: block. Codecov requires this so the
    // diff-matcher sees a single canonical path.
    const std::string normSf = "SF:/tmp/n/x.ctl";
    auto first = body.find (normSf);
    assert (first != std::string::npos);
    // Second occurrence must NOT exist.
    assert (body.find (normSf, first + 1) == std::string::npos);
    // None of the un-normalised forms appear.
    assert (body.find ("SF:/tmp/n//x.ctl")     == std::string::npos);
    assert (body.find ("SF:/tmp/n/./x.ctl")    == std::string::npos);
    assert (body.find ("SF:/tmp/n/y/../x.ctl") == std::string::npos);

    std::remove (out.c_str());
    SimdCoverage::reset();
}

void testFlushAlphabeticalOrder()
{
    using Ctl::SimdCoverage;
    if (!SimdCoverage::enabled()) return;
    SimdCoverage::reset();

    // Insert in non-alphabetical order; sortedFiles (std::set) gives alpha.
    SimdCoverage::registerLine ("/tmp/zfile.ctl", 1);
    SimdCoverage::registerLine ("/tmp/mfile.ctl", 1);
    SimdCoverage::registerLine ("/tmp/afile.ctl", 1);

    const std::string out = "/tmp/ctl_cov_ord.info";
    assert (SimdCoverage::flushToLcov (out, "ord"));
    std::string body = slurp (out);

    auto a = body.find ("SF:/tmp/afile.ctl");
    auto m = body.find ("SF:/tmp/mfile.ctl");
    auto z = body.find ("SF:/tmp/zfile.ctl");
    assert (a != std::string::npos);
    assert (m != std::string::npos);
    assert (z != std::string::npos);
    assert (a < m);
    assert (m < z);

    std::remove (out.c_str());
    SimdCoverage::reset();
}

void testFlushOpenFailure()
{
    using Ctl::SimdCoverage;
    if (!SimdCoverage::enabled()) return;
    SimdCoverage::reset();

    SimdCoverage::registerLine ("/tmp/probe.ctl", 1);
    // Path under a directory that almost certainly doesn't exist.
    bool ok = SimdCoverage::flushToLcov (
        "/this/directory/should/not/exist/cov.info", "fail");
    assert (!ok);
    SimdCoverage::reset();
}

// Parse DA:<line>,<count> out of an lcov body. Returns -1 if not present.
int findDA (const std::string &body, int line)
{
    const std::string key = "DA:" + std::to_string(line) + ",";
    const auto p = body.find (key);
    if (p == std::string::npos) return -1;
    const auto e = body.find ('\n', p);
    return std::stoi (body.substr (p + key.size(), e - p - key.size()));
}

void testRecordConcurrent()
{
    using Ctl::SimdCoverage;
    if (!SimdCoverage::enabled()) return;
    SimdCoverage::reset();

    // The header documents that record() takes a mutex and is safe under
    // concurrent calls. Smoke-test the contract: N threads × M record() calls
    // each on the same (file, line); final hit count must equal N*M.
    const int kThreads = 4;
    const int kPerThread = 2000;
    const std::string file = "/tmp/cov_concurrent.ctl";

    std::vector<std::thread> ts;
    ts.reserve (kThreads);
    for (int i = 0; i < kThreads; ++i)
    {
        ts.emplace_back ([&]{
            for (int j = 0; j < kPerThread; ++j)
                SimdCoverage::record (file, 7);
        });
    }
    for (auto &t : ts) t.join();

    const std::string out = "/tmp/ctl_cov_concurrent.info";
    assert (SimdCoverage::flushToLcov (out, "concurrent"));
    std::string body = slurp (out);

    const int got = findDA (body, 7);
    const int want = kThreads * kPerThread;
    if (got != want)
    {
        std::fprintf (stderr,
                      "  testRecordConcurrent: DA:7 got=%d want=%d\n",
                      got, want);
    }
    assert (got == want);

    std::remove (out.c_str());
    SimdCoverage::reset();
}

void testDiscoverModuleEmptyAndNull()
{
    using Ctl::SimdCoverage;
    if (!SimdCoverage::enabled()) return;
    SimdCoverage::reset();

    // Null pointer is a no-op.
    SimdCoverage::discoverModule (nullptr);

    // Module with empty _code: discoverModule iterates module->code()
    // (empty vector) and never calls discoverChain. No SF: block should
    // appear for the module's fileName.
    Ctl::SimdInterpreter interp;
    Ctl::SimdModule mod (interp, "ut_empty_module", "/tmp/ut_empty.ctl");
    SimdCoverage::discoverModule (&mod);

    const std::string out = "/tmp/ctl_cov_empty.info";
    assert (SimdCoverage::flushToLcov (out, "empty"));
    std::string body = slurp (out);
    assert (body.find ("SF:/tmp/ut_empty.ctl") == std::string::npos);

    std::remove (out.c_str());
    SimdCoverage::reset();
}

void testFlushEmpty()
{
    using Ctl::SimdCoverage;
    if (!SimdCoverage::enabled()) return;
    SimdCoverage::reset();

    // No prior register/record: flush still succeeds, producing a file.
    // Body has no SF: blocks. Pin this contract so callers can rely on
    // "always writes a file when called" semantics.
    const std::string out = "/tmp/ctl_cov_empty_flush.info";
    bool ok = SimdCoverage::flushToLcov (out, "empty");
    assert (ok);

    std::string body = slurp (out);
    assert (body.find ("SF:")           == std::string::npos);
    assert (body.find ("end_of_record") == std::string::npos);

    std::remove (out.c_str());
    SimdCoverage::reset();
}

void testRecordAccumulatesAcrossCalls()
{
    using Ctl::SimdCoverage;
    if (!SimdCoverage::enabled()) return;
    SimdCoverage::reset();

    // Tiny CTL fixture: one assignment on line 5, called repeatedly.
    const std::string ctlPath = "/tmp/ctl_cov_repeat.ctl";
    {
        std::ofstream out (ctlPath);
        out << "void main(output float r, input float a)\n"   // line 1
               "{\n"                                            // line 2
               "    float t = a;\n"                             // line 3
               "    t = t + 1.0;\n"                             // line 4
               "    r = t;\n"                                   // line 5
               "}\n";                                           // line 6
    }

    Ctl::SimdInterpreter interp;
    interp.setUserModulePath ({"/tmp"}, true);
    interp.loadModule ("ctl_cov_repeat");

    Ctl::FunctionCallPtr fc = interp.newFunctionCall ("main");
    fc->inputArg(0)->setDefaultValue();

    // First call: record line-5 hit count == N.
    fc->callFunction (1);
    const std::string outA = "/tmp/ctl_cov_repeat_A.info";
    assert (SimdCoverage::flushToLcov (outA, "repeat"));
    std::string bodyA = slurp (outA);
    const int hitsA = findDA (bodyA, 5);
    assert (hitsA > 0);

    // Second call (no reset between): hit count must increase.
    fc->callFunction (1);
    const std::string outB = "/tmp/ctl_cov_repeat_B.info";
    assert (SimdCoverage::flushToLcov (outB, "repeat"));
    std::string bodyB = slurp (outB);
    const int hitsB = findDA (bodyB, 5);
    assert (hitsB == 2 * hitsA);

    std::remove (ctlPath.c_str());
    std::remove (outA.c_str());
    std::remove (outB.c_str());
    SimdCoverage::reset();
}

} // namespace

void testCoverage()
{
    std::cout << "Testing SimdCoverage hit table and lcov writer\n";
    testRecordAndFlush();
    testEndToEnd();
    testRegisterLineIdempotent();
    testRecordAccumulatesAndAutoRegisters();
    testResetClearsHitsAndDedup();
    testDiscoverChainBranch();
    testDiscoverChainLoop();
    testDiscoverChainCall();
    testDiscoverChainFileNameTransition();
    testDiscoverChainDedup();
    testFlushSyntheticFilter();
    testFlushPathNormalization();
    testFlushAlphabeticalOrder();
    testFlushOpenFailure();
    testRecordConcurrent();
    testDiscoverModuleEmptyAndNull();
    testFlushEmpty();
    testRecordAccumulatesAcrossCalls();
    std::cout << "ok\n";
}
