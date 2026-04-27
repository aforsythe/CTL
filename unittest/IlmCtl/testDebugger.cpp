///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#include "testDebugger.h"

#include <CtlSimdDebugger.h>
#include <CtlSimdInterpreter.h>
#include <CtlFunctionCall.h>
#include <CtlModule.h>
#include <CtlModuleSet.h>
#include <CtlSymbolTable.h>
#ifdef CTL_ENABLE_DEBUGGER
#  include <CtlSimdInspector.h>
#  include <CtlSimdInst.h>
#  include <CtlSimdXContext.h>
#endif

#include "testRequire.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <set>
#include <thread>

using Ctl::SimdDebugger;
using Ctl::NoopDebugger;
using Ctl::DebugController;
using Ctl::DebugAction;
using Ctl::DebugStop;

namespace {

void testNoopApi()
{
    std::cout << "  Noop debugger API surface\n";

    NoopDebugger *dbg = NoopDebugger::instance();
    REQUIRE (dbg != nullptr);

    // The string-only no-op methods must compile and do nothing.  The
    // beforeInst() noop is exercised end-to-end by downstream tests
    // that construct a real SimdInterpreter+SimdXContext fixture;
    // calling it here with a synthesized null reference would be UB
    // even though the noop body never reads it (forming a reference
    // to a null pointer is ill-formed per [dcl.ref]/5).
    dbg->onCallEnter ("fn", "file.ctl", 1);
    dbg->onCallExit  ("fn");

    // The singleton returns the same instance every time.
    REQUIRE (dbg == NoopDebugger::instance());
}

void testEnabledFlag()
{
    std::cout << "  enabled() reports build state correctly\n";
#ifdef CTL_ENABLE_DEBUGGER
    REQUIRE (SimdDebugger::enabled() == true);
#else
    REQUIRE (SimdDebugger::enabled() == false);
    std::cout << "    (debugger compiled out at build time)\n";
#endif
}

class CountingDebugger : public Ctl::SimdDebugger
{
  public:
    int beforeCount    = 0;
    int callEnterCount = 0;
    int callExitCount  = 0;

    void beforeInst (Ctl::SimdXContext &,
                     const Ctl::SimdInst *,
                     std::size_t) override
    {
        ++beforeCount;
    }
    void onCallEnter (const std::string &,
                      const std::string &, int) override
    {
        ++callEnterCount;
    }
    void onCallExit (const std::string &) override
    {
        ++callExitCount;
    }
};

void testHookFires()
{
    if (!Ctl::SimdDebugger::enabled())
    {
        std::cout << "  (debugger compiled out, skipping hook test)\n";
        return;
    }
    std::cout << "  beforeInst hook fires per executed instruction\n";

    // Tiny CTL: void main(output float r) { r = 1.0; }
    const std::string ctlPath = "/tmp/ctl_dbg_hook.ctl";
    {
        std::ofstream out (ctlPath);
        out << "void main(output float r) { r = 1.0; }\n";
    }

    Ctl::SimdInterpreter interp;
    interp.loadModule ("ctl_dbg_hook", ctlPath);

    CountingDebugger dbg;
    interp.setDebugger (&dbg);

    Ctl::FunctionCallPtr fc = interp.newFunctionCall ("main");
    fc->callFunction (1);

    // The function compiles to multiple instructions (push 1.0, assign,
    // return).  We don't pin the exact count, just that the hook fired
    // at least once.
    REQUIRE (dbg.beforeCount > 0);

    std::remove (ctlPath.c_str());
}

void testCallStackHook()
{
    if (!Ctl::SimdDebugger::enabled())
    {
        std::cout << "  (skipping call-stack test, debugger compiled out)\n";
        return;
    }
    std::cout << "  onCallEnter / onCallExit fire on function entry/exit\n";

    // CTL with a nested call: main calls helper.
    const std::string ctlPath = "/tmp/ctl_dbg_callstack.ctl";
    {
        std::ofstream out (ctlPath);
        out << "namespace cs {\n"
               "void helper(output float r) { r = 2.0; }\n"
               "void main(output float r) { float h; helper(h); r = h + 1.0; }\n"
               "}\n";
    }

    Ctl::SimdInterpreter interp;
    interp.loadModule ("ctl_dbg_callstack", ctlPath);

    CountingDebugger dbg;
    interp.setDebugger (&dbg);

    Ctl::FunctionCallPtr fc = interp.newFunctionCall ("cs::main");
    fc->callFunction (1);

    // The nested helper() invocation should produce at least one enter/exit
    // pair (from SimdCallInst::execute), and the top-level cs::main call
    // from SimdFunctionCall::callFunction should also fire.
    REQUIRE (dbg.callEnterCount >= 1);
    REQUIRE (dbg.callExitCount  >= 1);
    REQUIRE (dbg.callEnterCount == dbg.callExitCount);

    std::remove (ctlPath.c_str());
}

#ifdef CTL_ENABLE_DEBUGGER
// A debugger that pauses on the FIRST instruction and waits for the
// debugger thread to release with `Continue`.  Verifies the
// pause/resume primitive works across threads.
class OneShotPauseDebugger : public Ctl::SimdDebugger
{
  public:
    Ctl::DebugController controller;
    bool paused_at_least_once = false;

    void beforeInst (Ctl::SimdXContext &xc,
                     const Ctl::SimdInst *inst,
                     std::size_t depth) override
    {
        if (paused_at_least_once) return;
        paused_at_least_once = true;
        Ctl::DebugStop stop;
        stop.file      = xc.fileName();
        stop.line      = inst->lineNumber();
        stop.callDepth = depth;
        stop.inst      = inst;
        stop.xcontext  = &xc;
        controller.pause (stop);   // blocks until resume()
    }
    void onCallEnter (const std::string &,
                      const std::string &, int) override {}
    void onCallExit  (const std::string &) override {}
};
#endif // CTL_ENABLE_DEBUGGER

void testPauseResume()
{
#ifndef CTL_ENABLE_DEBUGGER
    std::cout << "  (skipping pause/resume, debugger compiled out)\n";
#else
    std::cout << "  DebugController pause/resume across threads\n";

    const std::string ctlPath = "/tmp/ctl_dbg_pause.ctl";
    {
        std::ofstream out (ctlPath);
        out << "void main(output float r) { r = 1.0; }\n";
    }

    Ctl::SimdInterpreter interp;
    interp.loadModule ("ctl_dbg_pause", ctlPath);

    OneShotPauseDebugger dbg;
    interp.setDebugger (&dbg);

    Ctl::FunctionCallPtr fc = interp.newFunctionCall ("main");

    // Run the interpreter on a worker thread.
    std::thread interpThread ([&]{ fc->callFunction (1); });

    // Wait for the pause.  The OneShotPauseDebugger pauses on the
    // FIRST executed instruction.  We poll for `paused_at_least_once`
    // because the DebugController's internal cv is private to the pair.
    while (!dbg.paused_at_least_once)
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    // Brief grace period for the pause() call to actually block on the cv.
    std::this_thread::sleep_for (std::chrono::milliseconds (10));

    // Inspect: the stop should report a valid file/line.
    REQUIRE (!dbg.controller.lastStop().file.empty());
    REQUIRE (dbg.controller.lastStop().line > 0);

    // Release the interpreter.
    dbg.controller.resume (Ctl::DebugAction::Continue);

    interpThread.join();
    std::remove (ctlPath.c_str());
#endif // CTL_ENABLE_DEBUGGER
}

#ifdef CTL_ENABLE_DEBUGGER
// Pauses at a target line, captures variable snapshot via inspectVariables().
class InspectAtLineDebugger : public Ctl::SimdDebugger
{
  public:
    int targetLine = 0;
    std::string currentFunction;
    std::vector<Ctl::InspectableVar> snapshot;
    Ctl::SimdInterpreter *interp = nullptr;
    bool captured = false;

    void beforeInst (Ctl::SimdXContext &xc,
                     const Ctl::SimdInst *inst,
                     std::size_t) override
    {
        if (captured) return;
        if (inst->lineNumber() != targetLine) return;
        snapshot = Ctl::inspectVariables (*interp, xc,
                                          xc.fileName(),
                                          inst->lineNumber(),
                                          currentFunction);
        captured = true;
    }
    void onCallEnter (const std::string &,
                      const std::string &, int) override {}
    void onCallExit  (const std::string &) override {}
};
#endif // CTL_ENABLE_DEBUGGER

void testInspectVariables()
{
#ifndef CTL_ENABLE_DEBUGGER
    std::cout << "  (skipping inspect, debugger compiled out)\n";
#else
    std::cout << "  inspectVariables() returns named locals at a stop\n";

    // CTL with two named locals; we pause at line 4 and check both.
    const std::string ctlPath = "/tmp/ctl_dbg_inspect.ctl";
    {
        std::ofstream out (ctlPath);
        out << "void main(output float r) {\n"   // 1
               "    float a = 7.5;\n"              // 2
               "    float b = a + 1.0;\n"          // 3
               "    r = b;\n"                      // 4
               "}\n";                              // 5
    }

    Ctl::SimdInterpreter interp;
    interp.loadModule ("ctl_dbg_inspect", ctlPath);

    InspectAtLineDebugger dbg;
    dbg.targetLine = 4;
    // The fixture has no `namespace { }` wrapper, so the parser stamps
    // owningFunction with the bare "main" — same form the runtime
    // announces via onCallEnter.  (With a wrapper it'd be "ns::main".)
    dbg.currentFunction = "main";
    dbg.interp = &interp;
    interp.setDebugger (&dbg);

    Ctl::FunctionCallPtr fc = interp.newFunctionCall ("main");
    fc->callFunction (1);

    REQUIRE (dbg.captured);
    REQUIRE (!dbg.snapshot.empty());

    // Every returned var must have a non-null type and non-null data.
    for (const auto &v : dbg.snapshot)
    {
        REQUIRE (v.type);
        REQUIRE (v.data);
    }

    // Verify that function-local variables 'a' and 'b' are now visible.
    // Their names in the snapshot have a qualified prefix of the form
    // "ctl_dbg_inspect::N<n>::a" (N<n> is the anonymous namespace the
    // CTL parser assigns to the function body).
    float foundA = 0.f, foundB = 0.f;
    bool  sawA = false, sawB = false;
    for (const auto &v : dbg.snapshot)
    {
        // Suffix match: accept any name ending in "::a" or "::b" (or "$a" etc.)
        auto endsWith = [&](const std::string &suffix) {
            return v.name.size() >= suffix.size() &&
                   v.name.compare (v.name.size() - suffix.size(),
                                   suffix.size(), suffix) == 0;
        };
        if (endsWith ("::a") || endsWith ("$a"))
        {
            sawA = true;
            foundA = *static_cast<const float *>(v.data);
        }
        if (endsWith ("::b") || endsWith ("$b"))
        {
            sawB = true;
            foundB = *static_cast<const float *>(v.data);
        }
    }

    REQUIRE (sawA && "local 'a' not found in inspectVariables snapshot");
    REQUIRE (sawB && "local 'b' not found in inspectVariables snapshot");
    REQUIRE (foundA == 7.5f);
    REQUIRE (foundB == 8.5f);

    std::remove (ctlPath.c_str());
#endif // CTL_ENABLE_DEBUGGER
}

#ifdef CTL_ENABLE_DEBUGGER
// Records every (line, depth) pair visited by beforeInst.  Used by the
// return-line-fix test to assert that no inst fires at the closing-brace
// line of the function body.
class LineRecorder : public Ctl::SimdDebugger
{
  public:
    std::set<int> linesVisited;
    void beforeInst (Ctl::SimdXContext &,
                     const Ctl::SimdInst *inst,
                     std::size_t) override
    {
        linesVisited.insert (inst->lineNumber());
    }
    void onCallEnter (const std::string &,
                      const std::string &, int) override {}
    void onCallExit  (const std::string &) override {}
};
#endif

void testReturnLineNumber()
{
#ifndef CTL_ENABLE_DEBUGGER
    std::cout << "  (skipping return-line test, debugger compiled out)\n";
#else
    // Regression: parseReturnStatement used to capture currentLineNumber()
    // AFTER consuming the `;`, which advanced the lexer to the next line.
    // The result was that SimdReturnInst (and the trailing assign that
    // copies the return value into $return) carried the closing-brace
    // line, causing the debugger's per-line step to bounce to `}` before
    // hitting the return body.
    std::cout << "  return statement insts carry the `return`-keyword line\n";

    const std::string ctlPath = "/tmp/ctl_dbg_returnline.ctl";
    {
        std::ofstream out (ctlPath);
        out << "void main(output float r) {\n"  // 1
               "    r = 0.0;\n"                  // 2
               "    return;\n"                   // 3 — return keyword
               "}\n";                            // 4 — closing brace
    }

    Ctl::SimdInterpreter interp;
    interp.loadModule ("ctl_dbg_returnline", ctlPath);

    LineRecorder rec;
    interp.setDebugger (&rec);

    Ctl::FunctionCallPtr fc = interp.newFunctionCall ("main");
    fc->callFunction (1);

    REQUIRE (rec.linesVisited.count (3) > 0 &&
            "no inst visited at the `return` keyword line — fix may have regressed");
    REQUIRE (rec.linesVisited.count (4) == 0 &&
            "an inst is reporting the closing-brace line — return-line fix regressed");

    std::remove (ctlPath.c_str());
#endif
}

void testSymbolStamping()
{
    // Module::localSymbols() snapshots SymbolInfo for each function-local
    // (params + variables).  The parser is supposed to stamp each with its
    // declarationLine and owningFunction so the debugger can filter Locals
    // by line + frame.  Both fields land on the SymbolInfo at parse time
    // and survive deleteAllLocalSymbols(), so we can assert directly off
    // the snapshot without running the interpreter.
    std::cout << "  SymbolInfo carries declarationLine + owningFunction\n";

    const std::string ctlPath = "/tmp/ctl_dbg_stamp.ctl";
    {
        std::ofstream out (ctlPath);
        out << "namespace stamp {\n"           // 1
               "void f (output float r,\n"      // 2
               "        input  float x)\n"      // 3
               "{\n"                            // 4
               "    float a = x + 1.0;\n"       // 5
               "    float b = a * 2.0;\n"       // 6
               "    r = b;\n"                   // 7
               "}\n"                            // 8
               "}\n";                           // 9
    }

    Ctl::SimdInterpreter interp;
    interp.loadModule ("ctl_dbg_stamp", ctlPath);

    const Ctl::Module *mod = nullptr;
    for (auto it = interp.moduleSet().begin(); it != interp.moduleSet().end(); ++it)
    {
        if (it->second && it->second->name() == "ctl_dbg_stamp")
        {
            mod = it->second;
            break;
        }
    }
    REQUIRE (mod && "module not found after loadModule");

    bool sawA = false, sawB = false, sawX = false, sawR = false;
    for (const auto &entry : mod->localSymbols())
    {
        const std::string   &absName = entry.first;
        const Ctl::SymbolInfoPtr &info = entry.second;
        REQUIRE (info);

        // Every captured local must carry the owning function name.
        REQUIRE (info->owningFunction() == "stamp::f" &&
                "owningFunction not stamped (or wrong) on a local symbol");

        // Suffix-match the leaf name to identify which symbol we're at.
        auto endsWith = [&](const std::string &suf) {
            return absName.size() >= suf.size() &&
                   absName.compare (absName.size() - suf.size(),
                                    suf.size(), suf) == 0;
        };
        if (endsWith ("::a")) { sawA = true; REQUIRE (info->declarationLine() == 5); }
        if (endsWith ("::b")) { sawB = true; REQUIRE (info->declarationLine() == 6); }
        if (endsWith ("::x")) { sawX = true; REQUIRE (info->declarationLine() == 3); }
        if (endsWith ("::r")) { sawR = true; REQUIRE (info->declarationLine() == 2); }
    }
    REQUIRE (sawA && "local 'a' missing from snapshot");
    REQUIRE (sawB && "local 'b' missing from snapshot");
    REQUIRE (sawX && "param 'x' missing from snapshot");
    REQUIRE (sawR && "param 'r' missing from snapshot");

    std::remove (ctlPath.c_str());
}

#ifdef CTL_ENABLE_DEBUGGER
// Pause at a target line in a target function; capture the snapshot.
class FrameInspectDebugger : public Ctl::SimdDebugger
{
  public:
    int targetLine = 0;
    std::string targetFile;          // suffix-matched
    std::string currentFunction;
    std::vector<Ctl::InspectableVar> snapshot;
    Ctl::SimdInterpreter *interp = nullptr;
    bool captured = false;

    void beforeInst (Ctl::SimdXContext &xc,
                     const Ctl::SimdInst *inst,
                     std::size_t) override
    {
        if (captured) return;
        if (inst->lineNumber() != targetLine) return;
        if (!targetFile.empty())
        {
            const std::string &f = xc.fileName();
            if (f.size() < targetFile.size() ||
                f.compare (f.size() - targetFile.size(),
                           targetFile.size(), targetFile) != 0)
                return;
        }
        snapshot = Ctl::inspectVariables (*interp, xc,
                                          xc.fileName(),
                                          inst->lineNumber(),
                                          currentFunction);
        captured = true;
    }
    void onCallEnter (const std::string &,
                      const std::string &, int) override {}
    void onCallExit  (const std::string &) override {}
};
#endif

void testInspectorLineFilter()
{
#ifndef CTL_ENABLE_DEBUGGER
    std::cout << "  (skipping line filter, debugger compiled out)\n";
#else
    std::cout << "  inspectVariables hides locals declared at/after current line\n";

    const std::string ctlPath = "/tmp/ctl_dbg_linefilter.ctl";
    {
        std::ofstream out (ctlPath);
        out << "void main(output float r, input float x) {\n"  // 1
               "    float a = x + 1.0;\n"                       // 2
               "    float b = a + 2.0;\n"                       // 3
               "    float c = b + 3.0;\n"                       // 4
               "    r = c;\n"                                   // 5
               "}\n";                                           // 6
    }

    Ctl::SimdInterpreter interp;
    interp.loadModule ("ctl_dbg_linefilter", ctlPath);

    FrameInspectDebugger dbg;
    dbg.targetLine = 4;                       // pause AT the `c =` line
    dbg.currentFunction = "main";
    dbg.interp = &interp;
    interp.setDebugger (&dbg);

    Ctl::FunctionCallPtr fc = interp.newFunctionCall ("main");
    fc->callFunction (1);

    REQUIRE (dbg.captured);

    bool sawA = false, sawB = false, sawC = false;
    for (const auto &v : dbg.snapshot)
    {
        if (v.name.size() >= 3 &&
            v.name.compare (v.name.size() - 3, 3, "::a") == 0) sawA = true;
        if (v.name.size() >= 3 &&
            v.name.compare (v.name.size() - 3, 3, "::b") == 0) sawB = true;
        if (v.name.size() >= 3 &&
            v.name.compare (v.name.size() - 3, 3, "::c") == 0) sawC = true;
    }
    REQUIRE (sawA && "expected 'a' (declared on line 2) to be visible at line 4");
    REQUIRE (sawB && "expected 'b' (declared on line 3) to be visible at line 4");
    REQUIRE (!sawC && "did NOT expect 'c' (declared on line 4) to be visible "
                     "at line 4 — it hasn't executed yet");

    std::remove (ctlPath.c_str());
#endif
}

void testInspectorFunctionFilter()
{
#ifndef CTL_ENABLE_DEBUGGER
    std::cout << "  (skipping fn filter, debugger compiled out)\n";
#else
    std::cout << "  inspectVariables hides callers' locals when paused inside callee\n";

    const std::string ctlPath = "/tmp/ctl_dbg_fnfilter.ctl";
    {
        std::ofstream out (ctlPath);
        out << "namespace ff {\n"                                  // 1
               "float helper(float h)\n"                            // 2
               "{\n"                                                // 3
               "    float scaled = h * 2.0;\n"                      // 4
               "    return scaled;\n"                               // 5
               "}\n"                                                // 6
               "void main(output float r, input float x)\n"         // 7
               "{\n"                                                // 8
               "    float caller_local = x + 100.0;\n"              // 9
               "    r = helper(caller_local);\n"                    // 10
               "}\n"                                                // 11
               "}\n";                                               // 12
    }

    Ctl::SimdInterpreter interp;
    interp.loadModule ("ctl_dbg_fnfilter", ctlPath);

    FrameInspectDebugger dbg;
    // Pause inside helper at the `return` line — main's caller_local is
    // still allocated on the underlying stack but belongs to a different
    // owningFunction, so the inspector should hide it.
    dbg.targetLine = 5;
    dbg.currentFunction = "ff::helper";
    dbg.interp = &interp;
    interp.setDebugger (&dbg);

    Ctl::FunctionCallPtr fc = interp.newFunctionCall ("ff::main");
    fc->callFunction (1);

    REQUIRE (dbg.captured);

    bool sawScaled = false, sawCallerLocal = false, sawH = false;
    for (const auto &v : dbg.snapshot)
    {
        auto endsWith = [&](const std::string &suf) {
            return v.name.size() >= suf.size() &&
                   v.name.compare (v.name.size() - suf.size(),
                                   suf.size(), suf) == 0;
        };
        if (endsWith ("::scaled"))       sawScaled      = true;
        if (endsWith ("::caller_local")) sawCallerLocal = true;
        if (endsWith ("::h"))            sawH           = true;
    }
    REQUIRE (sawScaled       && "helper's local 'scaled' should be visible");
    REQUIRE (sawH            && "helper's param 'h' should be visible");
    REQUIRE (!sawCallerLocal && "main's 'caller_local' must NOT leak into helper's frame");

    std::remove (ctlPath.c_str());
#endif
}

} // namespace

void testDebugger()
{
    std::cout << "Testing SimdDebugger interface\n";
    testNoopApi();
    testEnabledFlag();
    testHookFires();
    testCallStackHook();
    testPauseResume();
    testInspectVariables();
    testReturnLineNumber();
    testSymbolStamping();
    testInspectorLineFilter();
    testInspectorFunctionFilter();
    std::cout << "ok\n";
}
