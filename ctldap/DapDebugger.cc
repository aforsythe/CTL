///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#include "DapDebugger.h"

#include "DapServer.h"
#include <CtlValueFormatter.h>

#include <CtlExprEval.h>
#include <CtlPixelEvolution.h>

#include <CtlSimdInterpreter.h>
#include <CtlSimdInspector.h>
#include <CtlSimdInst.h>
#include <CtlSimdXContext.h>
#include <CtlSymbolTable.h>
#include <CtlModule.h>
#include <CtlType.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <typeinfo>

namespace ctldap {

namespace {
// Lightweight trace.  Set CTLDAP_TRACE=/path/to/file in the launch env
// to capture step/BP decisions.  No-op when env unset.
static FILE *g_trace = nullptr;
static bool  g_traceInit = false;
static void traceInit ()
{
    if (g_traceInit) return;
    g_traceInit = true;
    if (const char *p = std::getenv ("CTLDAP_TRACE"))
        g_trace = std::fopen (p, "w");
}
#define TRACE(...) do { traceInit(); if (g_trace) { \
    std::fprintf (g_trace, __VA_ARGS__); std::fflush (g_trace); } } while (0)
} // namespace

DapDebugger::DapDebugger (Ctl::SimdInterpreter &interp, DapServer &server)
:
    _interp (interp), _server (server)
{
}

void
DapDebugger::setBreakpoints (const std::string &file,
                             const std::vector<int> &lines)
{
    std::vector<BpSpec> specs;
    specs.reserve (lines.size());
    for (int ln : lines) specs.push_back ({ln, std::string(), std::string()});
    setBreakpoints (file, specs);
}

void
DapDebugger::setBreakpoints (const std::string &file,
                             const std::vector<std::pair<int,std::string>> &
                                 lineAndConditions)
{
    std::vector<BpSpec> specs;
    specs.reserve (lineAndConditions.size());
    for (const auto &lc : lineAndConditions)
        specs.push_back ({lc.first, lc.second, std::string()});
    setBreakpoints (file, specs);
}

void
DapDebugger::setBreakpoints (const std::string &file,
                             const std::vector<BpSpec> &specs)
{
    std::lock_guard<std::mutex> lk (_stateMutex);
    // Clear existing breakpoints in this file, add new ones.
    for (auto it = _breakpoints.begin(); it != _breakpoints.end(); )
    {
        if (it->file == file) it = _breakpoints.erase (it);
        else                  ++it;
    }
    for (const auto &s : specs)
        _breakpoints.insert (Breakpoint{file, s.line, s.condition, s.logMessage});
}

void
DapDebugger::doContinue ()
{
    std::lock_guard<std::mutex> lk (_stateMutex);
    TRACE ("doContinue\n");
    _stepMode = StepMode::Continue;
    _controller.resume (Ctl::DebugAction::Continue);
}

void
DapDebugger::setFunctionCall (Ctl::FunctionCall *fc)
{
    std::lock_guard<std::mutex> lk (_stateMutex);
    _functionCall = fc;
    // Crossing a stage boundary: clear the dedup latch so a BP set on
    // the next stage's entry line fires even if it happens to land at
    // the same (depth, line) the prior stage paused at.  Without this,
    // chained pipelines where stage1 ends and stage2 begins on the
    // same line at the same depth see stage2's BP suppressed by the
    // latch armed when stage1 paused.
    _resumedFromLine  = -1;
    _resumedFromDepth = 0;
}

void
DapDebugger::doNext ()
{
    std::lock_guard<std::mutex> lk (_stateMutex);
    _stepMode      = StepMode::StepOver;
    _stepFromLine  = lastStop().line;
    _stepFromDepth = lastStop().callDepth;
    TRACE ("doNext stepFromLine=%d stepFromDepth=%zu\n",
           _stepFromLine, _stepFromDepth);
    _controller.resume (Ctl::DebugAction::StepOver);
}

void
DapDebugger::doStepIn ()
{
    std::lock_guard<std::mutex> lk (_stateMutex);
    _stepMode      = StepMode::StepIn;
    _stepFromLine  = lastStop().line;
    _stepFromDepth = lastStop().callDepth;
    TRACE ("doStepIn stepFromLine=%d stepFromDepth=%zu\n",
           _stepFromLine, _stepFromDepth);
    _controller.resume (Ctl::DebugAction::StepIn);
}

void
DapDebugger::doStepOut ()
{
    std::lock_guard<std::mutex> lk (_stateMutex);
    _stepMode      = StepMode::StepOut;
    _stepFromDepth = lastStop().callDepth;
    // Capture the caller's line of the topmost frame so the StepOut
    // predicate can skip past the trailing assign-return-value
    // instruction (which is tagged at the call-site line and would
    // otherwise pause us BEFORE the assignment commits, hiding the
    // newly-assigned local from the Variables panel).
    _stepFromLine  = -1;
    if (!_callStack.empty())
        _stepFromLine = _callStack.back().callerLine;
    TRACE ("doStepOut stepFromDepth=%zu callerLine=%d\n",
           _stepFromDepth, _stepFromLine);
    _controller.resume (Ctl::DebugAction::StepOut);
}

void
DapDebugger::onCallEnter (const std::string &fn,
                          const std::string &cf, int cl)
{
    std::lock_guard<std::mutex> lk (_stateMutex);
    _callStack.push_back (CallFrame{fn, cf, cl});
}

void
DapDebugger::onCallExit (const std::string &)
{
    std::lock_guard<std::mutex> lk (_stateMutex);
    if (!_callStack.empty()) _callStack.pop_back();
}

bool
DapDebugger::shouldPauseForBreakpoint (const std::string &file, int line) const
{
    return findBreakpoint (file, line) != nullptr;
}

namespace {
// Resolve symlinks + normalize the path.  Returns the original on
// error (e.g. file doesn't exist) so callers can still suffix-match.
std::string canonicalize (const std::string &p)
{
    if (p.empty()) return p;
    std::error_code ec;
    auto can = std::filesystem::weakly_canonical (p, ec);
    if (ec) return p;
    return can.string();
}
} // namespace

const Breakpoint *
DapDebugger::findBreakpoint (const std::string &file, int line) const
{
    // Canonicalize the inst's reported file once so we don't pay it
    // for every BP in the loop below.
    std::string fileCan = canonicalize (file);

    for (const auto &bp : _breakpoints)
    {
        if (bp.line != line) continue;
        // 1. Exact match.
        if (file == bp.file) return &bp;
        // 2. Suffix match (basename / partial) — useful when callers
        // give just "demo.ctl" in the launch config.
        if (file.size() > bp.file.size() &&
            file.compare (file.size() - bp.file.size(),
                          bp.file.size(), bp.file) == 0) return &bp;
        // 3. Canonical match — handles symlinked module dirs.
        std::string bpCan = canonicalize (bp.file);
        if (!fileCan.empty() && !bpCan.empty() && fileCan == bpCan)
            return &bp;
    }
    return nullptr;
}

namespace {

// Read a CTL scalar at `data` typed by `type` and convert to double for
// numeric comparison.  Returns false for non-numeric types.
bool readNumeric (const Ctl::DataTypePtr &type, const void *data, double &out)
{
    if (!type || !data) return false;
    switch (type->cDataType())
    {
      case Ctl::IntTypeEnum:
        out = static_cast<double>(*reinterpret_cast<const int *>(data));
        return true;
      case Ctl::UIntTypeEnum:
        out = static_cast<double>(*reinterpret_cast<const unsigned int *>(data));
        return true;
      case Ctl::FloatTypeEnum:
        out = static_cast<double>(*reinterpret_cast<const float *>(data));
        return true;
      case Ctl::HalfTypeEnum: {
        // half is 16-bit IEEE float — read raw bits and decompress via
        // OpenEXR's Imath::half if available.  We only need a numeric
        // value for comparison; cast through float.
        out = static_cast<double>(
            *reinterpret_cast<const float *>(data));   // best-effort
        return true;
      }
      default:
        return false;
    }
}

// Trim ASCII whitespace from both ends of `s`.
std::string trim (const std::string &s)
{
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace (static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace (static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr (a, b - a);
}

} // namespace

bool
DapDebugger::evaluateCondition (Ctl::SimdXContext &xc,
                                const std::string &exprIn) const
{
    if (trim (exprIn).empty()) return true;     // no condition = always fire

    // Hand off to the real recursive-descent evaluator (handles
    // numeric literals, identifiers, array indexing, +-*/, and
    // comparison operators).  Fail-open on parse / lookup error so a
    // typo'd condition doesn't silently swallow the BP.
    std::string currentFn;
    {
        std::lock_guard<std::mutex> lk (_stateMutex);
        if (!_callStack.empty()) currentFn = _callStack.back().functionName;
    }
    auto vars = Ctl::inspectVariables (_interp, xc, xc.fileName(),
                                       xc.lineNumber(), currentFn);
    Ctl::EvalResult r = Ctl::evalExpression (exprIn, vars);
    if (!r.ok) return true;
    return r.value != 0.0;
}

std::string
DapDebugger::renderLogMessage (Ctl::SimdXContext &xc,
                               const std::string &raw) const
{
    std::string currentFn;
    {
        std::lock_guard<std::mutex> lk (_stateMutex);
        if (!_callStack.empty()) currentFn = _callStack.back().functionName;
    }
    auto vars = Ctl::inspectVariables (_interp, xc, xc.fileName(),
                                       xc.lineNumber(), currentFn);

    std::string out;
    out.reserve (raw.size());
    std::size_t i = 0;
    while (i < raw.size())
    {
        // Look for ${...} substitutions.  Plain `$` characters that
        // aren't followed by `{` pass through verbatim.
        if (raw[i] == '$' && i + 1 < raw.size() && raw[i + 1] == '{')
        {
            std::size_t close = raw.find ('}', i + 2);
            if (close == std::string::npos)
            {
                // Unterminated ${ — write the rest as-is and bail.
                out.append (raw, i, std::string::npos);
                break;
            }
            std::string expr = raw.substr (i + 2, close - (i + 2));
            // Try a bare-name fast path so values keep their original
            // CTL formatting (e.g. arrays as "[a, b, c]" not just the
            // first scalar).  Fall back to the numeric evaluator for
            // arithmetic / comparisons / indexing.
            std::string rendered;
            bool resolved = false;
            for (const auto &v : vars)
            {
                if (v.name == expr)
                {
                    rendered = Ctl::formatValue (v.type, v.data, 0);
                    resolved = true;
                    break;
                }
            }
            if (!resolved)
            {
                Ctl::EvalResult er = Ctl::evalExpression (expr, vars);
                if (er.ok)
                {
                    char buf[64];
                    std::snprintf (buf, sizeof buf, "%g", er.value);
                    rendered = buf;
                    resolved = true;
                }
            }
            if (!resolved)
            {
                // Surface the unresolved tag so the user sees what was
                // attempted (typo, out-of-scope name, etc.) rather than
                // a silently empty substitution.
                rendered = "${" + expr + "}";
            }
            out += rendered;
            i = close + 1;
        }
        else
        {
            out += raw[i++];
        }
    }
    return out;
}

void
DapDebugger::beforeInst (Ctl::SimdXContext &xc,
                         const Ctl::SimdInst *inst,
                         std::size_t depth)
{
    std::string reason;
    {
        std::unique_lock<std::mutex> lk (_stateMutex);

        TRACE ("beforeInst file=%s line=%d depth=%zu mode=%d sFL=%d sFD=%zu rFL=%d rFD=%zu inst=%s\n",
               xc.fileName().c_str(), inst->lineNumber(), depth,
               (int)_stepMode, _stepFromLine, _stepFromDepth,
               _resumedFromLine, _resumedFromDepth,
               typeid(*inst).name());

        // Lazily fill in per-frame state on the topmost call-stack
        // frame the first time beforeInst fires inside it.  onCallEnter
        // is called before we have access to xcontext, so we capture
        // fp + the function's defining file here on the first inst at
        // the new depth.
        if (!_callStack.empty() &&
            _callStack.back().savedFp == static_cast<std::size_t>(-1) &&
            depth >= _callStack.size())
        {
            _callStack.back().savedFp =
                static_cast<std::size_t>(xc.stack().fp());
            // Resolve the callee's defining file via the symbol table.
            // Used by the BP check below to detect a stale-file window
            // (xc.fileName() lags the actual file across calls).
            const std::string &fn = _callStack.back().functionName;
            Ctl::SymbolInfoPtr info = _interp.symbolTable().lookupSymbol (fn);
            if (info && info->module())
                _callStack.back().expectedFile = info->module()->fileName();
        }

        // "Synthetic" insts are compiler bookkeeping (file/line
        // metadata, return-slot allocation) — they have a lineNumber
        // but no user-visible action.  Stepping should skip them so
        // the user doesn't see line-number jitter like 25 → 23 → 26
        // when stepping through a function-entry sequence.  We
        // identify them by typeid name to avoid coupling DapDebugger
        // to the inst class definitions.
        const char *instTypeName = typeid(*inst).name();
        bool instIsSynthetic =
            std::strstr (instTypeName, "SimdFileNameInst")        != nullptr ||
            std::strstr (instTypeName, "SimdPushPlaceholderInst") != nullptr;

        if (_stopOnEntry)
        {
            _stopOnEntry = false;
            reason = "entry";
        }
        else if (instIsSynthetic)
        {
            // Skip step + BP checks entirely on synthetic insts; let
            // execution proceed to the next real inst.
        }
        else
        {
            switch (_stepMode)
            {
              case StepMode::StepIn:
                if (inst->lineNumber() != _stepFromLine) reason = "step";
                break;
              case StepMode::StepOver:
                if ((depth == _stepFromDepth && inst->lineNumber() != _stepFromLine) ||
                     depth <  _stepFromDepth)
                    reason = "step";
                break;
              case StepMode::StepOut:
                // Pop above the original depth AND advance to a line
                // different from the call site.  Otherwise the first
                // post-return inst (the assign-return-value) — still
                // tagged at the call line — would pause us before its
                // store commits, hiding the newly-assigned local.
                if (depth < _stepFromDepth &&
                    (_stepFromLine <= 0 ||
                     inst->lineNumber() != _stepFromLine))
                    reason = "step";
                break;
              default:
                break;
            }

            if (reason.empty())
            {
                const std::string &curFile = xc.fileName();
                int curLine = inst->lineNumber();

                // Latch is "active" when we're back at the same depth and
                // same line we resumed from.  Don't peek at the file: it
                // lags by an instruction across calls/returns.
                bool atResumedLocation =
                    (depth == _resumedFromDepth &&
                     curLine == _resumedFromLine &&
                     _resumedFromLine != -1);

                // Clear the latch the moment we reach a different line
                // (or pop past) the original frame.  Inside a callee
                // (depth > _resumedFromDepth) the latch must survive.
                if (!atResumedLocation &&
                    depth <= _resumedFromDepth &&
                    _resumedFromLine != -1)
                {
                    _resumedFromLine  = -1;
                    _resumedFromDepth = 0;
                }

                // While completing a StepOut, suppress BPs at the
                // caller's line: we're crossing back through it and any
                // BP there fired from the original entry to that line
                // — re-firing it would leave the user re-paused at the
                // BP they just stepped out of, never landing on the
                // next user line.
                bool stepOutCrossingCallSite =
                    (_stepMode == StepMode::StepOut &&
                     _stepFromLine > 0 &&
                     curLine == _stepFromLine);

                // Stale-file detection: if the topmost frame has an
                // expectedFile that differs from xc.fileName(), the
                // file is stale (we're inside a callee but xc still
                // reports the caller's file).  In that window a BP
                // set on caller.ctl:N can mis-fire on a callee inst
                // sharing line N — skip it until file settles.  For
                // recursive calls in the same file expectedFile ==
                // xc.fileName() so this is a no-op.
                bool fileIsStale = false;
                if (!_callStack.empty())
                {
                    const std::string &exp = _callStack.back().expectedFile;
                    if (!exp.empty() && exp != curFile)
                        fileIsStale = true;
                }

                if (!atResumedLocation && !stepOutCrossingCallSite &&
                    !fileIsStale)
                {
                    if (const Breakpoint *bp = findBreakpoint (curFile, curLine))
                    {
                        std::string cond    = bp->condition;
                        std::string logMsg  = bp->logMessage;
                        bool fire = true;
                        if (!cond.empty())
                        {
                            // Release the mutex around the condition
                            // eval — evaluateCondition reads _callStack
                            // under its own lock and would deadlock
                            // otherwise.  Re-acquire afterwards so the
                            // surrounding lk RAII is still valid.
                            lk.unlock();
                            try { fire = evaluateCondition (xc, cond); }
                            catch (...) { fire = true; }
                            lk.lock();
                        }
                        if (fire)
                        {
                            if (!logMsg.empty())
                            {
                                // Logpoint: render the message with
                                // ${expr} substitutions and emit a Debug
                                // Console line WITHOUT pausing.  Mutex
                                // released for the eval + send to keep
                                // the interpreter thread responsive.
                                lk.unlock();
                                std::string rendered =
                                    renderLogMessage (xc, logMsg);
                                lk.lock();
                                _server.sendEvent ("output", {
                                    {"category", "stdout"},
                                    {"output",   rendered + "\n"}
                                });
                            }
                            else
                            {
                                reason = "breakpoint";
                            }
                        }
                    }
                }
            }
        }
    }
    if (!reason.empty())
    {
        TRACE ("  -> PAUSE reason=%s\n", reason.c_str());
        doPause (xc, inst, depth, reason);
    }
}

std::string
DapDebugger::formatPixelEvolutionLine (Ctl::SimdXContext &xc,
                                       const Ctl::SimdInst *inst) const
{
    std::string currentFn;
    {
        std::lock_guard<std::mutex> lk (_stateMutex);
        if (!_callStack.empty()) currentFn = _callStack.back().functionName;
    }
    auto vars = Ctl::inspectVariables (_interp, xc, xc.fileName(),
                                       inst ? inst->lineNumber()
                                            : xc.lineNumber(),
                                       currentFn);
    std::string body = Ctl::formatColorEvolution (vars);
    if (body.empty()) return std::string();
    return "→ paused " + body;
}

void
DapDebugger::doPause (Ctl::SimdXContext &xc,
                      const Ctl::SimdInst *inst,
                      std::size_t depth,
                      const std::string &reason)
{
    {
        std::lock_guard<std::mutex> lk (_stateMutex);
        _stepMode          = StepMode::None;
        // Arm the dedup latch at the (depth, line) we're about to pause
        // at, so a BP on this line doesn't immediately re-fire after
        // resume, and a return from a callee back to this line doesn't
        // re-fire either.
        _resumedFromLine   = inst->lineNumber();
        _resumedFromDepth  = depth;
    }
    Ctl::DebugStop stop;
    stop.file      = xc.fileName();
    stop.line      = inst->lineNumber();
    stop.callDepth = depth;
    stop.inst      = inst;
    stop.xcontext  = &xc;

    // Pixel-evolution line — emitted BEFORE the stopped event so the
    // user sees the per-pause snapshot first in the Debug Console.
    if (_pixelEvolution)
    {
        std::string line = formatPixelEvolutionLine (xc, inst);
        if (!line.empty())
        {
            _server.sendEvent ("output", {
                {"category", "stdout"},
                {"output",   line + "\n"}
            });
        }
    }

    // Emit `stopped` event BEFORE blocking on pause(), so the dispatch
    // thread sees the event and can issue follow-up requests.
    _server.sendEvent ("stopped", {
        {"reason",            reason},
        {"threadId",          1},
        {"allThreadsStopped", true}
    });

    // Mark first-pause done (for waitForFirstPause callers).
    {
        std::lock_guard<std::mutex> lk (_firstPauseMutex);
        _hasFirstPause = true;
    }
    _firstPauseCv.notify_all();

    // Block until the dispatch thread calls resume() via doContinue/Next/etc.
    _controller.pause (stop);
}

void
DapDebugger::waitForFirstPause ()
{
    std::unique_lock<std::mutex> lk (_firstPauseMutex);
    _firstPauseCv.wait (lk, [this]{ return _hasFirstPause.load(); });
}

} // namespace ctldap
