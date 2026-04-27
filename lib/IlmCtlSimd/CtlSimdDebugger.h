///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#ifndef INCLUDED_CTL_SIMD_DEBUGGER_H
#define INCLUDED_CTL_SIMD_DEBUGGER_H

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>

namespace Ctl {

class SimdInst;
class SimdXContext;

//
// Action the debugger asks the interpreter to take after a pause.
//
enum class DebugAction
{
    Continue,    // resume until next breakpoint
    StepOver,    // pause at next instruction with a different lineNumber, same call depth
    StepIn,      // pause at next instruction with a different lineNumber, any depth
    StepOut,     // pause when call depth drops below the current frame
    Quit         // abandon the current call (interpreter throws DebugQuitExc)
};

//
// Snapshot of the interpreter state the debugger sees at each pause.
// Owned by the debugger thread for the duration of the pause.
//
struct DebugStop
{
    std::string             file;
    int                     line;
    std::size_t             callDepth;
    const SimdInst         *inst;
    SimdXContext           *xcontext;     // for variable inspection
};

//
// Pure-virtual interface the SIMD interpreter consults at every executed
// instruction.  Implementations: no-op default (NoopDebugger), CLI REPL
// (ctldb/Repl), DAP server (Phase 2).
//
// All entry points are no-ops unless the library is built with
// -DCTL_ENABLE_DEBUGGER=1.
//
class SimdDebugger
{
  public:
    virtual ~SimdDebugger();

    //
    // Called by SimdInst::executePath before each instruction's _execThunk
    // fires.  callDepth is the number of nested CTL function calls active
    // (1 = top-level CTL function, 2 = a function called from that, etc.).
    //
    // Implementations decide whether to pause (consult breakpoints, step
    // state).  Pausing is implemented by calling DebugController::pause()
    // on a controller the debugger owns.
    //
    virtual void beforeInst (SimdXContext &xcontext,
                             const SimdInst *inst,
                             std::size_t callDepth) = 0;

    //
    // Called by SimdFunctionCall when a CTL function call is entered /
    // exited.  Lets the debugger maintain a call stack in parallel to
    // the interpreter's stack frame chain.
    //
    virtual void onCallEnter (const std::string &functionName,
                              const std::string &callerFile,
                              int callerLine) = 0;
    virtual void onCallExit  (const std::string &functionName) = 0;

    //
    // True if the build defines CTL_ENABLE_DEBUGGER; allows callers to
    // skip preparatory work cheaply.
    //
    static constexpr bool enabled ()
    {
#ifdef CTL_ENABLE_DEBUGGER
        return true;
#else
        return false;
#endif
    }
};

//
// No-op default.  Returned by NoopDebugger::instance() and used when no
// real debugger is attached.  Inlined so the compiler can elide every
// call site at -O1 or higher.
//
class NoopDebugger : public SimdDebugger
{
  public:
    void beforeInst  (SimdXContext &, const SimdInst *, std::size_t) override {}
    void onCallEnter (const std::string &,
                      const std::string &, int) override {}
    void onCallExit  (const std::string &) override {}

    static NoopDebugger * instance ();   // process-wide singleton
};

//
// Helper: pause/resume primitive.  The interpreter thread calls pause(),
// blocking until the debugger thread calls resume(action).  Used by
// SimdDebugger implementations that drive the interpreter from a
// separate thread (CLI REPL, DAP server).
//
class DebugController
{
  public:
    DebugController ();

    // Interpreter-thread side: blocks until the debugger thread releases.
    // Returns the DebugAction the debugger requested.
    DebugAction pause (const DebugStop &stop);

    // Debugger-thread side: releases the interpreter with the given action.
    void resume (DebugAction action);

    // Set after the most recent pause().  The returned reference is
    // unsynchronized — callers MUST only invoke lastStop() while the
    // interpreter thread is blocked inside pause() (i.e., between
    // pause() returning to the debugger thread via the cv notify and
    // the matching resume() call).  Outside that window, fields may
    // race with a concurrent pause() invocation.
    const DebugStop & lastStop () const { return _lastStop; }

  private:
    std::mutex              _mutex;
    std::condition_variable _cv;
    bool                    _paused;
    bool                    _resumed;
    DebugAction             _action;
    DebugStop               _lastStop;
};

} // namespace Ctl

#endif
