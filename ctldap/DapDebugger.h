///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#ifndef CTLDAP_DEBUGGER_H
#define CTLDAP_DEBUGGER_H

#include <CtlSimdDebugger.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Ctl { class SimdInterpreter; class FunctionCall; }

namespace ctldap {

class DapServer;

struct Breakpoint
{
    std::string file;
    int         line;
    std::string condition;     // empty = unconditional
    // Logpoint message: when non-empty, the breakpoint LOGS its
    // formatted message to the Debug Console instead of pausing —
    // a print-style insertion point that doesn't require source edits.
    // ${expr} substitutions are resolved against the current frame's
    // locals via ExprEval.  Empty means a regular pausing breakpoint.
    std::string logMessage;
    bool operator< (const Breakpoint &o) const
    {
        if (file != o.file) return file < o.file;
        return line < o.line;
    }
};

struct CallFrame
{
    std::string functionName;
    std::string callerFile;
    int         callerLine;
    // Saved frame-pointer at function entry, used by onVariables to
    // resolve fp-relative locals for non-top stack frames.  Filled in
    // lazily on the first beforeInst at the new depth (onCallEnter is
    // called before xcontext is reachable here).  Sentinel
    // size_t(-1) = "not yet captured".
    std::size_t savedFp = static_cast<std::size_t>(-1);
    // Source file this function is defined in, looked up once at
    // frame entry from the symbol table.  Used to detect a "stale
    // file" window after descending into a callee — xc.fileName()
    // lags by an instruction across calls, so a BP set on
    // caller.ctl:N would mis-fire on a callee inst that shares
    // line N.  Empty until lazily populated on first beforeInst.
    std::string expectedFile;
};

// Phase-2 SimdDebugger.  Pauses the interpreter via DebugController
// and emits `stopped` events to the DAP server.  The dispatch thread
// handles continue/next/step requests by calling resume() with the
// appropriate DebugAction.
class DapDebugger : public Ctl::SimdDebugger
{
  public:
    DapDebugger (Ctl::SimdInterpreter &interp, DapServer &server);

    // Configuration (called from the dispatch thread, before launch).
    // Three overloads of escalating richness:
    //  - line-only: unconditional pausing breakpoint
    //  - line + condition: conditional pausing breakpoint
    //  - line + condition + logMessage: logpoint when logMessage is non-
    //    empty (LOGS the message, doesn't pause); a regular conditional
    //    breakpoint when both are empty / condition only.
    struct BpSpec { int line; std::string condition; std::string logMessage; };
    void setBreakpoints (const std::string &file,
                         const std::vector<int> &lines);
    void setBreakpoints (const std::string &file,
                         const std::vector<std::pair<int,std::string>> &
                             lineAndConditions);
    void setBreakpoints (const std::string &file,
                         const std::vector<BpSpec> &specs);
    void setStopOnEntry (bool b) { _stopOnEntry = b; }
    // Opt-in: emit a "→ paused: name=[r,g,b] …" Debug Console line at
    // every pause summarizing every color-shaped (float[3] / float[4])
    // local in the current frame.  Lets the user watch the pipeline
    // evolve in the console without scrolling the Variables panel.
    void setPixelEvolution (bool b) { _pixelEvolution = b; }
    // Switch the active FunctionCall (called between pipeline stages).
    // Also clears the BP-dedup latch — the next stage starts execution
    // fresh, and a BP set on its entry line should fire even if it
    // happens to share (depth, line) with the prior stage's last pause.
    void setFunctionCall (Ctl::FunctionCall *fc);

    // Step requests (called from the dispatch thread while interpreter
    // is paused inside controller.pause()).  Each captures the current
    // line/depth, sets the appropriate step mode, and resumes.
    void doContinue ();
    void doNext     ();
    void doStepIn   ();
    void doStepOut  ();

    // Snapshot of current pause state (read by dispatch thread between
    // pause() returning to the debugger thread via the cv notify and
    // the matching resume() call).
    const Ctl::DebugStop & lastStop () const { return _controller.lastStop(); }
    const std::vector<CallFrame> & callStack () const { return _callStack; }
    Ctl::SimdInterpreter & interpreter () const { return _interp; }
    Ctl::FunctionCall *    functionCall () const { return _functionCall; }

    // Wait until the interpreter pauses for the first time (used by
    // launch/configurationDone to sync before sending stopped event).
    void waitForFirstPause ();

    // SimdDebugger overrides — called from interpreter thread.
    void beforeInst  (Ctl::SimdXContext &xc,
                      const Ctl::SimdInst *inst,
                      std::size_t depth) override;
    void onCallEnter (const std::string &functionName,
                      const std::string &callerFile,
                      int callerLine) override;
    void onCallExit  (const std::string &functionName) override;

  private:
    enum class StepMode { None, Continue, StepIn, StepOver, StepOut };

    bool shouldPauseForBreakpoint (const std::string &file, int line) const;
    // Returns the matched Breakpoint pointer (so we can read its
    // condition) or nullptr.  Same suffix-match semantics as the bool form.
    const Breakpoint *findBreakpoint (const std::string &file, int line) const;
    // Evaluate a condition expression in the current paused frame's
    // scope.  Recognized forms (kept intentionally narrow for v1):
    //   <name>                 → truthy if value != 0
    //   <name> <op> <number>   → op ∈ {==, !=, <, <=, >, >=}
    // Anything else returns true so the BP fires (fail-open: a bad
    // condition string shouldn't silently swallow a BP).
    bool evaluateCondition (Ctl::SimdXContext &xc,
                            const std::string &expr) const;
    // Render a logpoint message: replace ${expr} substrings with the
    // result of evaluating <expr> against the current frame's locals.
    // Falls back to the literal "${expr}" tag on eval failure so the
    // user sees what was attempted.
    std::string renderLogMessage (Ctl::SimdXContext &xc,
                                  const std::string &raw) const;
    // Build the pixel-evolution Debug Console line for the current
    // pause: one entry per color-shaped (float[3] / float[4]) local
    // currently in scope.  Empty string if no such locals exist.
    std::string formatPixelEvolutionLine (Ctl::SimdXContext &xc,
                                          const Ctl::SimdInst *inst) const;
    void doPause (Ctl::SimdXContext &xc,
                  const Ctl::SimdInst *inst,
                  std::size_t depth,
                  const std::string &reason);

    Ctl::SimdInterpreter &_interp;
    DapServer            &_server;
    Ctl::DebugController  _controller;
    Ctl::FunctionCall    *_functionCall = nullptr;

    mutable std::mutex    _stateMutex;
    std::set<Breakpoint>  _breakpoints;
    bool                  _stopOnEntry = false;
    StepMode              _stepMode = StepMode::None;
    int                   _stepFromLine = -1;
    std::size_t           _stepFromDepth = 0;
    // Dedup latch: (depth, line) of the location we most recently paused
    // at.  Survives stepping into callees (depth > _resumedFromDepth) so
    // a return to this line doesn't re-fire the BP.  File deliberately
    // NOT included — xc.fileName() lags by an instruction across calls
    // and returns, which would mis-clear or mis-suppress the latch.
    int                   _resumedFromLine  = -1;
    std::size_t           _resumedFromDepth = 0;

    bool                  _pixelEvolution = false;

    std::vector<CallFrame> _callStack;

    std::mutex                _firstPauseMutex;
    std::condition_variable   _firstPauseCv;
    std::atomic<bool>         _hasFirstPause{false};
};

} // namespace ctldap

#endif
