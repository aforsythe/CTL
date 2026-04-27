///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#ifndef CTLDB_REPL_H
#define CTLDB_REPL_H

#include <CtlSimdDebugger.h>
#include <CtlFunctionCall.h>

#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Ctl { class SimdInterpreter; }

namespace ctldb {

// Identifies a (file, line) breakpoint with optional condition + logpoint
// message.  `file` matches by suffix — users typically type basenames,
// not absolute paths.
struct Breakpoint
{
    std::string file;
    int         line;
    std::string condition;     // empty = unconditional
    // Logpoint message: when non-empty, the breakpoint LOGS its
    // formatted message to stdout instead of pausing the REPL.
    // ${expr} substitutions resolve against the current frame's locals
    // via Ctl::evalExpression.  Empty means a regular pausing BP.
    std::string logMessage;
    bool operator< (const Breakpoint &o) const
    {
        return std::tie(file, line) < std::tie(o.file, o.line);
    }
};

// One frame on the active call stack.
struct CallFrame
{
    std::string functionName;
    std::string callerFile;
    int         callerLine;
};

class Repl : public Ctl::SimdDebugger
{
  public:
    explicit Repl (Ctl::SimdInterpreter &interp);

    // Pre-run setup (called from main() before the interpreter thread starts).
    void addBreakpoint (const std::string &file, int line,
                        const std::string &condition  = std::string(),
                        const std::string &logMessage = std::string());
    void setStopAtEntry    (bool b) { _stopAtEntry    = b; }
    // Opt-in: print "→ paused name=[r,g,b] …" before the prompt at every
    // pause, summarising every color-shaped (float[3] / float[4]) local
    // in the active frame.  Mirrors ctldap's `pixelEvolution` launch arg.
    void setPixelEvolution (bool b) { _pixelEvolution = b; }

    // Provide the top-level FunctionCall so `print` can inspect
    // input/output args (which remain accessible after loadModule).
    // Call before callFunction().
    void setFunctionCall (Ctl::FunctionCallPtr fc) { _fc = fc; }

    // SimdDebugger overrides — called from the interpreter thread.
    void beforeInst (Ctl::SimdXContext &,
                     const Ctl::SimdInst *,
                     std::size_t) override;
    void onCallEnter (const std::string &,
                      const std::string &, int) override;
    void onCallExit  (const std::string &) override;

  private:
    enum class StepMode
    {
        None,           // run free (no step)
        Continue,       // run until next breakpoint
        StepIn,         // pause at next inst with different line, any depth
        StepOver,       // pause at next inst with different line, same depth
        StepOut         // pause when depth drops below entry depth
    };

    void enterRepl (Ctl::SimdXContext &xcontext,
                    const Ctl::SimdInst *inst,
                    std::size_t depth);

    // Render a logpoint message: replace ${expr} substrings with the
    // result of evaluating <expr> against the current frame's locals.
    // Falls back to the literal "${expr}" tag on eval failure so the
    // user sees what was attempted.
    std::string renderLogMessage (Ctl::SimdXContext &xc,
                                  const std::string &raw);

    void printPrompt ();

    // Command implementations (return true to release the interpreter).
    bool cmdContinue ();
    bool cmdNext ();
    bool cmdStep ();
    bool cmdFinish ();
    bool cmdQuit ();
    bool cmdHelp ();
    bool cmdBreak (const std::string &arg);
    bool cmdClear (const std::string &arg);
    bool cmdInfo (const std::string &arg);     // "info bp", "info stack"
    bool cmdPrint (const std::string &arg);
    bool cmdBacktrace ();

    Ctl::SimdInterpreter   &_interp;
    Ctl::FunctionCallPtr    _fc;        // set by setFunctionCall() in main()

    std::set<Breakpoint>    _breakpoints;
    bool                    _stopAtEntry;
    bool                    _pixelEvolution = false;

    StepMode                _stepMode;
    int                     _stepFromLine;
    std::size_t             _stepFromDepth;

    std::vector<CallFrame>  _callStack;          // updated by onCallEnter/Exit

    // Cached snapshot of the current pause for use by command handlers.
    Ctl::SimdXContext      *_curXContext;
    const Ctl::SimdInst    *_curInst;
    std::size_t             _curDepth;

    // Last breakpoint stop location — used to suppress re-firing a breakpoint
    // on a subsequent instruction at the same source line (which happens
    // because the compiler emits multiple instructions per source line).
    std::string             _lastBpFile;
    int                     _lastBpLine;
};

} // namespace ctldb

#endif
