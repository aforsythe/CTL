///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#include "Repl.h"

#include "ValueFormatter.h"

#include <CtlExprEval.h>
#include <CtlPixelEvolution.h>
#include <CtlSimdInspector.h>
#include <CtlSimdInterpreter.h>
#include <CtlSimdInst.h>
#include <CtlSimdXContext.h>

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <sstream>

namespace ctldb {

Repl::Repl (Ctl::SimdInterpreter &interp)
:
    _interp (interp),
    _stopAtEntry (false),
    _stepMode (StepMode::None),
    _stepFromLine (-1),
    _stepFromDepth (0),
    _curXContext (nullptr),
    _curInst (nullptr),
    _curDepth (0),
    _lastBpLine (-1)
{
    // empty
}

void
Repl::addBreakpoint (const std::string &file, int line,
                     const std::string &condition,
                     const std::string &logMessage)
{
    Breakpoint bp{file, line, condition, logMessage};
    _breakpoints.insert (bp);
    std::cout << "Breakpoint set at " << file << ":" << line;
    if (!condition.empty())  std::cout << "  if "  << condition;
    if (!logMessage.empty()) std::cout << "  log " << logMessage;
    std::cout << "\n";
}

void
Repl::onCallEnter (const std::string &fn,
                   const std::string &cf, int cl)
{
    CallFrame f{fn, cf, cl};
    _callStack.push_back (f);
}

void
Repl::onCallExit (const std::string &)
{
    if (!_callStack.empty()) _callStack.pop_back();
}

//
// Unified beforeInst: checks _stopAtEntry first, then step modes, then
// breakpoints.  Calls enterRepl() whenever we should pause.
//
void
Repl::beforeInst (Ctl::SimdXContext &xc,
                  const Ctl::SimdInst *inst,
                  std::size_t depth)
{
    // 1. Stop-at-entry: pause on the very first instruction, then clear.
    if (_stopAtEntry)
    {
        _stopAtEntry = false;
        enterRepl (xc, inst, depth);
        return;
    }

    // 2. Step-mode checks.
    switch (_stepMode)
    {
      case StepMode::StepIn:
        if (inst->lineNumber() != _stepFromLine)
        {
            enterRepl (xc, inst, depth);
            return;
        }
        break;

      case StepMode::StepOver:
        if (depth == _stepFromDepth && inst->lineNumber() != _stepFromLine)
        {
            enterRepl (xc, inst, depth);
            return;
        }
        if (depth < _stepFromDepth)
        {
            // Function returned without a line change — still pause.
            enterRepl (xc, inst, depth);
            return;
        }
        break;

      case StepMode::StepOut:
        // Pop above original depth AND advance to a line different
        // from the call site, so we skip the trailing assign-return
        // inst (which is still tagged at the call line) and the
        // newly-assigned local is visible when we land.
        if (depth < _stepFromDepth &&
            (_stepFromLine <= 0 || inst->lineNumber() != _stepFromLine))
        {
            enterRepl (xc, inst, depth);
            return;
        }
        break;

      default:
        break;
    }

    // 3. Breakpoint check — suffix match on file so users can type basenames.
    // Skip while an active step mode is in progress: the step takes priority
    // so that `next` over a line that has a breakpoint doesn't re-stop there.
    if (_stepMode != StepMode::None && _stepMode != StepMode::Continue)
        return;

    const std::string &curFile = xc.fileName();
    int curLine = inst->lineNumber();

    // Suppress re-firing at the same file:line we just stopped at.  The
    // compiler emits multiple instructions per source line; without this
    // guard, `continue` would immediately re-trigger the breakpoint on the
    // very next instruction.
    if (curLine == _lastBpLine && curFile == _lastBpFile)
        return;

    for (const auto &bp : _breakpoints)
    {
        if (bp.line != curLine) continue;
        bool fileMatches = (curFile == bp.file ||
            (curFile.size() > bp.file.size() &&
             curFile.compare (curFile.size() - bp.file.size(),
                              bp.file.size(), bp.file) == 0));
        if (!fileMatches) continue;

        // Optional condition gate: evaluate against the live frame's
        // locals.  Fail-open on parse error so a typo doesn't silently
        // swallow the BP.
        if (!bp.condition.empty())
        {
            std::string curFn;
            if (!_callStack.empty()) curFn = _callStack.back().functionName;
            auto vars = Ctl::inspectVariables (_interp, xc, xc.fileName(),
                                               inst->lineNumber(), curFn);
            Ctl::EvalResult r = Ctl::evalExpression (bp.condition, vars);
            if (r.ok && r.value == 0.0) continue;   // condition false → skip
        }

        // Logpoint: render ${expr} substitutions, print to stdout, do
        // NOT pause.  Match the latch behavior of regular BPs so we
        // don't fire twice on adjacent insts with the same source line.
        if (!bp.logMessage.empty())
        {
            std::string rendered = renderLogMessage (xc, bp.logMessage);
            std::cout << rendered << "\n";
            _lastBpFile = curFile;
            _lastBpLine = curLine;
            return;
        }

        _lastBpFile = curFile;
        _lastBpLine = curLine;
        enterRepl (xc, inst, depth);
        return;
    }
}

// Resolve `${expr}` substitutions in a logpoint message against the
// current frame's locals.  Bare-name lookups format the value via
// formatValue (so arrays render as "[a, b, c]"), arithmetic falls
// back to the numeric evaluator.  Unresolved substitutions surface
// as the literal `${expr}` so the user sees what was attempted.
std::string
Repl::renderLogMessage (Ctl::SimdXContext &xc, const std::string &raw)
{
    std::string curFn;
    if (!_callStack.empty()) curFn = _callStack.back().functionName;
    auto vars = Ctl::inspectVariables (_interp, xc, xc.fileName(),
                                       xc.lineNumber(), curFn);

    std::string out;
    out.reserve (raw.size());
    std::size_t i = 0;
    while (i < raw.size())
    {
        if (raw[i] == '$' && i + 1 < raw.size() && raw[i + 1] == '{')
        {
            std::size_t close = raw.find ('}', i + 2);
            if (close == std::string::npos)
            {
                out.append (raw, i, std::string::npos);
                break;
            }
            std::string expr = raw.substr (i + 2, close - (i + 2));
            std::string rendered;
            bool resolved = false;
            for (const auto &v : vars)
            {
                if (v.name == expr)
                {
                    rendered = formatValue (v.type, v.data, 0);
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
                rendered = "${" + expr + "}";
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
Repl::enterRepl (Ctl::SimdXContext &xc,
                 const Ctl::SimdInst *inst,
                 std::size_t depth)
{
    _curXContext = &xc;
    _curInst = inst;
    _curDepth = depth;
    _stepMode = StepMode::None;

    std::cout << "* Stopped at " << xc.fileName() << ":"
              << inst->lineNumber();
    if (!_callStack.empty())
        std::cout << " in " << _callStack.back().functionName << "()";
    std::cout << "\n";

    // Optional pixel-evolution snapshot — every color-shaped local in
    // the current frame, one line, before the prompt.  Mirrors the
    // ctldap `pixelEvolution` Debug Console line.
    if (_pixelEvolution)
    {
        std::string curFn;
        if (!_callStack.empty()) curFn = _callStack.back().functionName;
        auto vars = Ctl::inspectVariables (_interp, xc, xc.fileName(),
                                           inst->lineNumber(), curFn);
        std::string body = Ctl::formatColorEvolution (vars);
        if (!body.empty())
            std::cout << "→ paused " << body << "\n";
    }

    while (true)
    {
        printPrompt();
        std::string line;
        if (!std::getline (std::cin, line))
        {
            // EOF — treat as quit.
            cmdQuit();
            return;
        }

        // Trim leading/trailing whitespace.
        auto first = line.find_first_not_of (" \t");
        if (first == std::string::npos) continue;
        auto last  = line.find_last_not_of  (" \t");
        line = line.substr (first, last - first + 1);

        if (line.empty()) continue;

        // Split into command + arg.
        auto sp = line.find (' ');
        std::string cmd = (sp == std::string::npos) ? line : line.substr (0, sp);
        std::string arg = (sp == std::string::npos) ? ""   : line.substr (sp + 1);

        bool release = false;
        if      (cmd == "continue" || cmd == "c")       release = cmdContinue();
        else if (cmd == "next"     || cmd == "n")       release = cmdNext();
        else if (cmd == "step"     || cmd == "s")       release = cmdStep();
        else if (cmd == "finish"   || cmd == "f")       release = cmdFinish();
        else if (cmd == "quit"     || cmd == "q")       release = cmdQuit();
        else if (cmd == "help"     || cmd == "h" || cmd == "?")
                                                                 cmdHelp();
        else if (cmd == "break"    || cmd == "b")       cmdBreak (arg);
        else if (cmd == "clear")                        cmdClear (arg);
        else if (cmd == "info")                         cmdInfo  (arg);
        else if (cmd == "print"    || cmd == "p")       cmdPrint (arg);
        else if (cmd == "backtrace"|| cmd == "bt")      cmdBacktrace();
        else
            std::cout << "unknown command: " << cmd
                      << "  (type 'help')\n";

        if (release) return;
    }
}

void
Repl::printPrompt ()
{
    std::cout << "(ctldb) " << std::flush;
}

bool Repl::cmdContinue ()
{
    _stepMode = StepMode::Continue;
    return true;
}

bool Repl::cmdNext ()
{
    _stepMode = StepMode::StepOver;
    _stepFromLine  = _curInst->lineNumber();
    _stepFromDepth = _curDepth;
    return true;
}

bool Repl::cmdStep ()
{
    _stepMode = StepMode::StepIn;
    _stepFromLine  = _curInst->lineNumber();
    _stepFromDepth = _curDepth;
    return true;
}

bool Repl::cmdFinish ()
{
    if (_curDepth == 0)
    {
        std::cout << "already at top frame; use 'continue' instead\n";
        return false;
    }
    _stepMode = StepMode::StepOut;
    _stepFromDepth = _curDepth;
    // Capture the call-site line in the parent frame so the StepOut
    // predicate can skip past the trailing assign-return-value
    // instruction (which is tagged at the call line and would
    // otherwise leave us paused BEFORE the LHS commits, hiding any
    // newly-assigned local from `print`).
    _stepFromLine = -1;
    if (!_callStack.empty())
        _stepFromLine = _callStack.back().callerLine;
    return true;
}

bool Repl::cmdQuit ()
{
    std::cout << "(quit)\n";
    std::exit (0);
    return true;  // unreachable
}

bool Repl::cmdHelp ()
{
    std::cout <<
        "Commands:\n"
        "  continue, c           run until next breakpoint\n"
        "  next, n               step over (next line, same frame)\n"
        "  step, s               step in (next line, any frame)\n"
        "  finish, f             step out (run until current frame returns)\n"
        "  break <file>:<line>            set a breakpoint\n"
        "  break <file>:<line> if EXPR    conditional breakpoint\n"
        "                                 (e.g. `break demo.ctl:10 if rIn > 0.5`)\n"
        "  break <file>:<line> log MSG    LOG MSG without pausing (\"logpoint\")\n"
        "                                 (e.g. `break demo.ctl:10 log \"x=${rIn}\"`)\n"
        "  clear <file>:<line>   remove a breakpoint\n"
        "  info bp               list breakpoints\n"
        "  info stack            show call stack\n"
        "  print, p EXPR         print a variable or evaluate an expression in scope\n"
        "                        (bare names, +-*/, ==, !=, <, <=, >, >=, &&, ||,\n"
        "                         array indexing — same grammar as conditional BPs)\n"
        "  backtrace, bt         show call stack (alias for 'info stack')\n"
        "  quit, q               exit ctldb\n";
    return false;
}

bool Repl::cmdBreak (const std::string &arg)
{
    // Syntax:
    //   break <file>:<line>                  unconditional pausing BP
    //   break <file>:<line> if <expr>        conditional pausing BP
    //   break <file>:<line> log <message>    logpoint (no pause)
    //
    // The `if` and `log` keywords are mutually exclusive in one entry
    // (declare a separate BP if you want both conditional + logging).
    auto colon = arg.find (':');
    if (colon == std::string::npos)
    {
        std::cout << "usage: break <file>:<line> [if <expr> | log <msg>]\n";
        return false;
    }
    std::string file = arg.substr (0, colon);

    // The line number runs from after the colon to either end-of-arg
    // OR the next whitespace before the `if`/`log` keyword.
    std::string rest = arg.substr (colon + 1);
    std::size_t sp = rest.find_first_of (" \t");
    std::string lineStr = (sp == std::string::npos) ? rest : rest.substr (0, sp);
    std::string tail    = (sp == std::string::npos) ? std::string()
                                                    : rest.substr (sp);
    int line = std::atoi (lineStr.c_str());
    if (line <= 0)
    {
        std::cout << "invalid line number\n";
        return false;
    }

    // Trim leading whitespace on tail.
    auto firstNonWs = tail.find_first_not_of (" \t");
    if (firstNonWs != std::string::npos) tail = tail.substr (firstNonWs);
    else                                  tail.clear();

    std::string condition, logMessage;
    if (!tail.empty())
    {
        if (tail.size() >= 3 && tail.substr (0, 3) == "if " )
        {
            condition = tail.substr (3);
            auto cleft = condition.find_first_not_of (" \t");
            if (cleft != std::string::npos)
                condition = condition.substr (cleft);
        }
        else if (tail.size() >= 4 && tail.substr (0, 4) == "log ")
        {
            logMessage = tail.substr (4);
            auto lleft = logMessage.find_first_not_of (" \t");
            if (lleft != std::string::npos)
                logMessage = logMessage.substr (lleft);
            // Strip surrounding double quotes if present, since users
            // habitually write log "msg ${x}".
            if (logMessage.size() >= 2 &&
                logMessage.front() == '"' && logMessage.back() == '"')
                logMessage = logMessage.substr (1, logMessage.size() - 2);
        }
        else
        {
            std::cout << "usage: break <file>:<line> [if <expr> | log <msg>]\n";
            return false;
        }
    }

    addBreakpoint (file, line, condition, logMessage);
    return false;
}

bool Repl::cmdClear (const std::string &arg)
{
    auto colon = arg.find (':');
    if (colon == std::string::npos)
    {
        std::cout << "usage: clear <file>:<line>\n";
        return false;
    }
    Breakpoint bp{arg.substr (0, colon),
                  std::atoi (arg.substr (colon + 1).c_str())};
    auto it = _breakpoints.find (bp);
    if (it == _breakpoints.end())
        std::cout << "no breakpoint at " << arg << "\n";
    else
    {
        _breakpoints.erase (it);
        std::cout << "cleared\n";
    }
    return false;
}

bool Repl::cmdInfo (const std::string &arg)
{
    if (arg == "bp" || arg == "breakpoints")
    {
        if (_breakpoints.empty())
            std::cout << "no breakpoints\n";
        else
            for (const auto &bp : _breakpoints)
            {
                std::cout << "  " << bp.file << ":" << bp.line;
                if (!bp.condition.empty())  std::cout << "  if "  << bp.condition;
                if (!bp.logMessage.empty()) std::cout << "  log " << bp.logMessage;
                std::cout << "\n";
            }
    }
    else if (arg == "stack" || arg == "frames")
    {
        cmdBacktrace();
    }
    else
    {
        std::cout << "info what?  try 'info bp' or 'info stack'\n";
    }
    return false;
}

bool Repl::cmdPrint (const std::string &arg)
{
    if (arg.empty())
    {
        std::cout << "usage: print <name>\n";
        return false;
    }

    bool found = false;

    // 1. Check inspectable vars from the symbol table (globals and any
    //    module-scope constants that survive loadModule) plus function-local
    //    variables from the per-module snapshot in the current function frame.
    {
        std::string curFn;
        if (!_callStack.empty())
            curFn = _callStack.back().functionName;

        auto vars = Ctl::inspectVariables (_interp, *_curXContext,
                                           _curXContext->fileName(),
                                           _curInst->lineNumber(),
                                           curFn);
        for (const auto &v : vars)
        {
            // Match by exact name OR suffix after '$' or '::' separator
            // (e.g. user types "a" but symbol is qualified as "ns::main$a"
            // or "::FLT_MAX").
            bool match = (v.name == arg);
            if (!match && v.name.size() > arg.size())
            {
                std::size_t off = v.name.size() - arg.size();
                match = (v.name.compare (off, arg.size(), arg) == 0 &&
                         (v.name[off - 1] == '$' || v.name[off - 1] == ':'));
            }
            if (match)
            {
                // Print the user-supplied name rather than the fully-qualified
                // internal name (which contains anonymous namespace tokens like
                // "N0", "N1", etc. that are not visible in CTL source).
                std::cout << arg << " = "
                          << formatValue (v.type, v.data, 0) << "\n";
                found = true;
            }
        }
    }

    // 2. If not found in the symbol table, check the top-level function
    //    call's input and output args by name.  These survive loadModule
    //    because they're stored in the FunctionCall object, not the symbol
    //    table.
    if (!found && _fc)
    {
        for (std::size_t i = 0; i < _fc->numInputArgs() && !found; ++i)
        {
            Ctl::FunctionArgPtr a = _fc->inputArg (i);
            if (a && a->name() == arg)
            {
                std::cout << a->name() << " = "
                          << formatValue (a->type(),
                                          static_cast<const void*>(a->data()),
                                          0) << "\n";
                found = true;
            }
        }
        for (std::size_t i = 0; i < _fc->numOutputArgs() && !found; ++i)
        {
            Ctl::FunctionArgPtr a = _fc->outputArg (i);
            if (a && a->name() == arg)
            {
                std::cout << a->name() << " = "
                          << formatValue (a->type(),
                                          static_cast<const void*>(a->data()),
                                          0) << "\n";
                found = true;
            }
        }
    }

    // 3. If `arg` looks like a bare identifier (no operators, no
    //    brackets, no whitespace), the user expected a variable; the
    //    fail-soft "no variable named X" message is what every other
    //    debugger produces and is what existing tests expect.
    //    For anything that looks like a real expression — arithmetic,
    //    comparisons, indexing, parens — fall through to the
    //    evaluator so `print rIn * 2` etc. work.
    if (!found)
    {
        bool looksLikeExpression = false;
        for (char c : arg)
        {
            if (c == '+' || c == '-' || c == '*' || c == '/' ||
                c == '(' || c == ')' || c == '[' || c == ']' ||
                c == '<' || c == '>' || c == '=' || c == '!' ||
                c == '&' || c == '|' || c == ' ' || c == '\t')
            {
                looksLikeExpression = true;
                break;
            }
        }
        if (!looksLikeExpression)
        {
            std::cout << "no variable named '" << arg << "' in scope\n";
            return false;
        }
        std::string curFn;
        if (!_callStack.empty()) curFn = _callStack.back().functionName;
        auto vars = Ctl::inspectVariables (_interp, *_curXContext,
                                           _curXContext->fileName(),
                                           _curInst->lineNumber(),
                                           curFn);
        Ctl::EvalResult er = Ctl::evalExpression (arg, vars);
        if (er.ok)
            std::cout << arg << " = " << er.value << "\n";
        else
            std::cout << "cannot evaluate '" << arg << "': "
                      << er.error << "\n";
    }
    return false;
}

bool Repl::cmdBacktrace ()
{
    if (_callStack.empty())
    {
        std::cout << "no active calls (top frame is the entry point)\n";
        return false;
    }
    int i = 0;
    for (auto it = _callStack.rbegin(); it != _callStack.rend(); ++it, ++i)
    {
        std::cout << "#" << i << " " << it->functionName
                  << "  (called from " << it->callerFile
                  << ":" << it->callerLine << ")\n";
    }
    return false;
}

} // namespace ctldb
