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
#include <CtlSimdInterpreter.h>
#include <CtlFunctionCall.h>

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

std::string slurp (const std::string &path)
{
    std::ifstream in (path);
    std::ostringstream os;
    os << in.rdbuf();
    return os.str();
}

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

} // namespace

void testCoverage()
{
    std::cout << "Testing SimdCoverage hit table and lcov writer\n";
    testRecordAndFlush();
    testEndToEnd();
    std::cout << "ok\n";
}
