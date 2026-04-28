///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

//
// ctldb - single-pixel debugger for CTL modules.
//
// Usage:
//   ctldb [options] -ctl <module.ctl> [-ctl <module.ctl> ...]
//
// Options:
//   --pixel r,g,b[,a]      pixel value to drive the function on (defaults
//                          to 0.5,0.5,0.5)
//   --break <file>:<line>[ if EXPR | log MSG]
//                          set a breakpoint (repeatable).  Optional `if EXPR`
//                          makes it conditional; optional `log MSG` makes it
//                          a non-pausing logpoint with ${expr} substitution.
//   --stop-at-entry        pause on the first executed instruction
//   --function <name>      function to call for a stage (repeatable;
//                          if fewer than -ctl count, remaining stages
//                          default to "main")
//   --param NAME=VALUE     bind a uniform input parameter by name
//                          (repeatable; e.g. --param exposure=1.0)
//   --module-path DIR      add a directory to the import search path
//                          (repeatable; CTL_MODULE_PATH env var is also
//                          read automatically)
//   --pixel-evolution      print every color-shaped (float[3]/float[4]) local
//                          at every pause, before the prompt
//   -h, --help             this text
//

#include "Repl.h"
#include <CtlValueFormatter.h>

#include <CtlFunctionCall.h>
#include <CtlPixelBinder.h>
#include <CtlSimdInterpreter.h>
#include <CtlStageChain.h>
#include <CtlType.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

// Options

struct BreakSpec
{
    std::string file;
    int         line;
    std::string condition;     // empty = unconditional
    std::string logMessage;    // empty = pausing BP
};

struct Options
{
    std::vector<std::string>              ctlPaths;
    std::vector<std::string>              functions;   // per-stage; may be shorter than ctlPaths
    std::vector<std::string>              modulePaths;
    std::vector<BreakSpec>                breaks;
    std::vector<float>                    pixel;
    std::map<std::string, float>          params;      // uniform param bindings
    bool                                  stopAtEntry;
    bool                                  pixelEvolution;

    Options () : stopAtEntry (false), pixelEvolution (false)
    {
        pixel.push_back (0.5f);
        pixel.push_back (0.5f);
        pixel.push_back (0.5f);
    }

    // Return the function name for stage i (default "main").
    std::string functionForStage (std::size_t i) const
    {
        if (i < functions.size())
            return functions[i];
        return "main";
    }
};

void usage (const char *argv0)
{
    std::fprintf (stderr,
        "usage: %s [options] -ctl <module.ctl> [-ctl ...]\n"
        "  --pixel r,g,b[,a]      pixel value (default: 0.5,0.5,0.5)\n"
        "  --break ARG            set a breakpoint (repeatable):\n"
        "                           <file>:<line>             plain BP\n"
        "                           <file>:<line> if EXPR     conditional BP\n"
        "                           <file>:<line> log MSG     logpoint (no pause,\n"
        "                                                     ${expr} substitution)\n"
        "  --stop-at-entry        pause on first instruction\n"
        "  --function <name>      function to call per stage (repeatable;\n"
        "                         missing entries default to \"main\")\n"
        "  --param NAME=VALUE     bind a uniform input by name (repeatable)\n"
        "  --module-path DIR      add import search directory (repeatable)\n"
        "  --pixel-evolution      print color-shaped locals at every pause\n"
        "  -h, --help             this text\n",
        argv0);
}

bool parseArgs (int argc, char **argv, Options &out)
{
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];

        // Helper lambda to consume next arg value.
        auto need = [&]() -> const char * {
            if (++i >= argc) {
                std::fprintf (stderr,
                    "option %s requires a value\n", a.c_str());
                return nullptr;
            }
            return argv[i];
        };

        if (a == "-ctl")
        {
            const char *v = need();
            if (!v) return false;
            out.ctlPaths.push_back (v);
        }
        else if (a == "--pixel")
        {
            const char *v = need();
            if (!v) return false;
            std::vector<float> p;
            std::string s = v;
            std::size_t pos = 0;
            while (pos < s.size())
            {
                std::size_t comma = s.find (',', pos);
                std::string tok = s.substr (pos,
                    comma == std::string::npos
                        ? std::string::npos
                        : comma - pos);
                p.push_back (static_cast<float>(std::atof (tok.c_str())));
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
            if (p.size() < 3 || p.size() > 4)
            {
                std::fprintf (stderr,
                    "--pixel needs 3 or 4 comma-separated floats\n");
                return false;
            }
            out.pixel = p;
        }
        else if (a == "--break")
        {
            const char *v = need();
            if (!v) return false;
            std::string s = v;
            std::size_t colon = s.find (':');
            if (colon == std::string::npos)
            {
                std::fprintf (stderr,
                    "--break expects <file>:<line> [if EXPR | log MSG]\n");
                return false;
            }
            std::string file = s.substr (0, colon);
            std::string rest = s.substr (colon + 1);
            // Line number runs to either end-of-arg or the first space.
            std::size_t sp = rest.find_first_of (" \t");
            std::string lineStr = (sp == std::string::npos) ? rest
                                                            : rest.substr (0, sp);
            std::string tail    = (sp == std::string::npos) ? std::string()
                                                            : rest.substr (sp);
            int line = std::atoi (lineStr.c_str());
            if (line <= 0)
            {
                std::fprintf (stderr, "--break: invalid line number\n");
                return false;
            }
            auto firstNonWs = tail.find_first_not_of (" \t");
            if (firstNonWs != std::string::npos) tail = tail.substr (firstNonWs);
            else                                  tail.clear();
            std::string condition, logMessage;
            if (!tail.empty())
            {
                if (tail.size() >= 3 && tail.substr (0, 3) == "if ")
                    condition = tail.substr (3);
                else if (tail.size() >= 4 && tail.substr (0, 4) == "log ")
                {
                    logMessage = tail.substr (4);
                    if (logMessage.size() >= 2 &&
                        logMessage.front() == '"' && logMessage.back() == '"')
                        logMessage = logMessage.substr (1, logMessage.size() - 2);
                }
                else
                {
                    std::fprintf (stderr,
                        "--break: trailing tokens must start with `if ` or `log `\n");
                    return false;
                }
            }
            out.breaks.push_back ({file, line, condition, logMessage});
        }
        else if (a == "--function")
        {
            const char *v = need();
            if (!v) return false;
            out.functions.push_back (v);
        }
        else if (a == "--param")
        {
            const char *v = need();
            if (!v) return false;
            std::string s = v;
            std::size_t eq = s.find ('=');
            if (eq == std::string::npos)
            {
                std::fprintf (stderr, "--param expects NAME=VALUE\n");
                return false;
            }
            out.params[s.substr (0, eq)] =
                static_cast<float>(std::atof (s.substr (eq + 1).c_str()));
        }
        else if (a == "--module-path")
        {
            const char *v = need();
            if (!v) return false;
            out.modulePaths.push_back (v);
        }
        else if (a == "--stop-at-entry")
        {
            out.stopAtEntry = true;
        }
        else if (a == "--pixel-evolution")
        {
            out.pixelEvolution = true;
        }
        else if (a == "-h" || a == "--help")
        {
            usage (argv[0]);
            return false;
        }
        else
        {
            std::fprintf (stderr, "unknown argument: %s\n", a.c_str());
            usage (argv[0]);
            return false;
        }
    }

    if (out.ctlPaths.empty())
    {
        std::fprintf (stderr,
            "at least one -ctl <module.ctl> is required\n");
        usage (argv[0]);
        return false;
    }

    if (out.functions.size() > out.ctlPaths.size())
    {
        std::fprintf (stderr,
            "more --function flags (%zu) than -ctl files (%zu)\n",
            out.functions.size(), out.ctlPaths.size());
        return false;
    }

    return true;
}

// (Pixel input binding + stage-chain helpers live in lib/IlmCtlDebug —
// Ctl::bindPixelInputs, Ctl::bindUniformParams, Ctl::captureOutputs,
// Ctl::bindFromPriorStage.  ctldb and ctldap share the lib so the
// rule sets don't drift between them.)

// Output printing

void printOutputs (Ctl::FunctionCallPtr &fc)
{
    std::cout << "* function returned\n";

    for (std::size_t i = 0; i < fc->numOutputArgs(); ++i)
    {
        Ctl::FunctionArgPtr arg = fc->outputArg (i);
        if (!arg) continue;

        std::cout << "  " << arg->name() << " = ";

        // For float outputs (the common case), use the typed get.
        if (arg->type() &&
            arg->type()->cDataType() == Ctl::FloatTypeEnum)
        {
            float v = 0.0f;
            arg->get (&v, 0, 0, 1);
            std::cout << Ctl::formatValue (arg->type(), &v, 0) << "\n";
        }
        else if (arg->type())
        {
            // Generic: read into a raw buffer via the data() pointer.
            // data() returns the underlying storage for lane 0 directly.
            std::cout << Ctl::formatValue (
                arg->type(),
                static_cast<const void*>(arg->data()),
                0) << "\n";
        }
        else
        {
            std::cout << "<unknown type>\n";
        }
    }

    Ctl::FunctionArgPtr rv = fc->returnValue();
    if (rv && rv->type() &&
        rv->type()->cDataType() != Ctl::VoidTypeEnum)
    {
        std::cout << "  return = ";
        if (rv->type()->cDataType() == Ctl::FloatTypeEnum)
        {
            float v = 0.0f;
            rv->get (&v, 0, 0, 1);
            std::cout << Ctl::formatValue (rv->type(), &v, 0) << "\n";
        }
        else
        {
            std::cout << Ctl::formatValue (
                rv->type(),
                static_cast<const void*>(rv->data()),
                0) << "\n";
        }
    }
}

} // namespace

// main

int
main (int argc, char **argv)
{
    Options opts;
    if (!parseArgs (argc, argv, opts))
        return 2;

    try
    {
        Ctl::SimdInterpreter interp;

        // Build module search path:
        //   1. $CTL_MODULE_PATH (colon-separated)
        //   2. --module-path flags
        //   3. Parent directory of each -ctl file (for sibling imports)
        {
            std::vector<std::string> paths =
                Ctl::parseColonPath (std::getenv ("CTL_MODULE_PATH"));
            for (const auto &p : opts.modulePaths) paths.push_back (p);
            for (const auto &p : opts.ctlPaths)    paths.push_back (Ctl::pathDirname (p));
            interp.setUserModulePath (paths, true);
        }

        // Load each module by absolute path.
        for (const auto &p : opts.ctlPaths)
            interp.loadModule (Ctl::pathStem (p), p);

        // Set up the REPL / debugger.
        ctldb::Repl repl (interp);
        for (const auto &b : opts.breaks)
            repl.addBreakpoint (b.file, b.line, b.condition, b.logMessage);
        repl.setStopAtEntry    (opts.stopAtEntry);
        repl.setPixelEvolution (opts.pixelEvolution);

        interp.setDebugger (&repl);

        // Run each stage in sequence.
        std::map<std::string, float> prevOutputs;  // outputs of stage N-1

        for (std::size_t stage = 0; stage < opts.ctlPaths.size(); ++stage)
        {
            std::string funcName = opts.functionForStage (stage);

            Ctl::FunctionCallPtr fc = interp.newFunctionCall (funcName);
            if (!fc)
            {
                std::fprintf (stderr,
                    "stage %zu: function '%s' not found in loaded modules\n",
                    stage, funcName.c_str());
                return 1;
            }

            // Bind inputs.  Stage 0 takes the user's pixel + uniform
            // params; subsequent stages chain from the prior stage's
            // outputs by name (params still apply to every stage in
            // case the same uniform recurs in later stages too).
            if (stage == 0)
                Ctl::bindPixelInputs (fc, opts.pixel);
            else
                Ctl::bindFromPriorStage (fc, prevOutputs);
            Ctl::bindUniformParams (fc, opts.params);

            // Give the Repl a reference to the current FC so it can print
            // input/output args by name.
            repl.setFunctionCall (fc);

            // Run the stage.
            try
            {
                fc->callFunction (1);
            }
            catch (const std::exception &e)
            {
                std::cerr << "interpreter error (stage " << stage
                          << "): " << e.what() << std::endl;
                return 1;
            }

            // Capture outputs for the next stage (last stage also captured
            // so printOutputs can read from fc directly — no need to restore).
            prevOutputs = Ctl::captureOutputs (fc);

            // Per-stage output line in chain mode (>1 stage).  Mirrors
            // the ctldap "→ stage K of N: …" Debug Console line, so a
            // user reading both side-by-side sees the same handoff.
            const std::size_t nStages = opts.ctlPaths.size();
            if (nStages > 1)
            {
                std::cout << "→ stage " << (stage + 1) << " of " << nStages
                          << ": ";
                bool first = true;
                for (std::size_t i = 0; i < fc->numOutputArgs(); ++i)
                {
                    Ctl::FunctionArgPtr arg = fc->outputArg (i);
                    if (!arg || !arg->type()) continue;
                    if (arg->type()->cDataType() != Ctl::FloatTypeEnum) continue;
                    float v = 0.0f;
                    arg->get (&v, 0, 0, 1);
                    if (!first) std::cout << ", ";
                    std::cout << arg->name() << "="
                              << Ctl::formatValue (arg->type(), &v, 0);
                    first = false;
                }
                std::cout << "\n";
            }

            // Print outputs for the final stage.
            if (stage == nStages - 1)
                printOutputs (fc);
        }

        return 0;
    }
    catch (const std::exception &e)
    {
        std::fprintf (stderr, "error: %s\n", e.what());
        return 1;
    }
}
