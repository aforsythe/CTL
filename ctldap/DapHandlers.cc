///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#include "DapHandlers.h"
#include <CtlExprEval.h>
#include <CtlPixelBinder.h>
#include "ValueFormatter.h"

#include <CtlMessage.h>
#include <CtlSimdAddr.h>
#include <CtlSimdInspector.h>
#include <CtlSimdInterpreter.h>
#include <CtlSimdXContext.h>
#include <CtlSymbolTable.h>
#include <CtlModule.h>
#include <CtlModuleSet.h>

#include <cstdlib>
#include <iostream>
#include <map>
#include <stdexcept>

namespace ctldap {

namespace {

// Extract the directory portion of a path (everything before last '/').
std::string pathDirname (const std::string &p)
{
    auto pos = p.find_last_of ('/');
    if (pos == std::string::npos) return ".";
    return p.substr (0, pos);
}

// Extract the basename WITHOUT extension.
std::string pathStem (const std::string &p)
{
    auto sl = p.find_last_of ('/');
    auto base = (sl == std::string::npos) ? p : p.substr (sl + 1);
    auto dot = base.find_last_of ('.');
    if (dot == std::string::npos) return base;
    return base.substr (0, dot);
}

// Extract a user-friendly name from a qualified CTL symbol.
// Strips everything before the last "::" or "$".
//   "ctldap_test::N1::interim" -> "interim"
//   "main$a"                   -> "a"
//   "globalVar"                -> "globalVar"
std::string friendlyName (const std::string &qualified)
{
    std::size_t pos = qualified.find_last_of (":$");
    if (pos == std::string::npos) return qualified;
    return qualified.substr (pos + 1);
}

// Look up a CTL function's defining source file via the symbol table.
// Returns empty string if the function isn't found or has no module.
// Used by onStackTrace to recover the true source location when
// xc.fileName() is stale (lags by an instruction across calls/returns).
std::string functionFile (const Ctl::SimdInterpreter &interp,
                          const std::string &qualifiedFn)
{
    if (qualifiedFn.empty()) return std::string();
    Ctl::SymbolInfoPtr info = interp.symbolTable().lookupSymbol (qualifiedFn);
    if (!info || !info->module()) return std::string();
    return info->module()->fileName();
}

// Stage-chaining helpers (mirrored from ctldb/main.cc)

// Capture float outputs of fc into a name→value map for the next stage.
std::map<std::string, float> captureOutputs (Ctl::FunctionCallPtr &fc)
{
    std::map<std::string, float> result;
    for (std::size_t i = 0; i < fc->numOutputArgs(); ++i)
    {
        Ctl::FunctionArgPtr arg = fc->outputArg (i);
        if (!arg) continue;
        if (arg->type() &&
            arg->type()->cDataType() == Ctl::FloatTypeEnum)
        {
            float v = 0.0f;
            arg->get (&v, 0, 0, 1);
            result[arg->name()] = v;
        }
    }
    return result;
}

// Bind inputs of fc from prevOutputs (outputs of the prior stage).
// Matching: exact name, then Out→In suffix synonym (rOut→rIn, etc.).
void bindFromPriorStage (Ctl::FunctionCallPtr &fc,
                         const std::map<std::string, float> &prevOutputs)
{
    for (std::size_t i = 0; i < fc->numInputArgs(); ++i)
    {
        Ctl::FunctionArgPtr arg = fc->inputArg (i);
        if (!arg) continue;

        const std::string &inName = arg->name();
        bool bound = false;

        // 1) Exact name match.
        {
            auto it = prevOutputs.find (inName);
            if (it != prevOutputs.end())
            {
                float v = it->second;
                arg->set (&v, 0, 0, 1);
                bound = true;
            }
        }

        // 2) In-suffix synonym: replace trailing "In" with "Out" and look up.
        if (!bound && inName.size() >= 2 &&
            inName.substr (inName.size() - 2) == "In")
        {
            std::string candidate = inName.substr (0, inName.size() - 2) + "Out";
            auto it = prevOutputs.find (candidate);
            if (it != prevOutputs.end())
            {
                float v = it->second;
                arg->set (&v, 0, 0, 1);
                bound = true;
            }
        }

        if (!bound)
        {
            if (arg->hasDefaultValue())
                arg->setDefaultValue();
            // else: leave zero-initialized
        }
    }
}

// (Pixel + uniform-param binding lives in lib/IlmCtlDebug so ctldb
// shares the exact same name-matching rules.)

} // namespace

// CTL writes print_*, error messages, and warnings via a global
// outputFunction (CtlMessage.h).  Default is `cerr <<`, which leaves
// us with no way to surface the text to VS Code's Debug Console.
// We install our own hook that routes everything through the DAP
// server as `output` events.  The active server is stashed in this
// pointer because setMessageOutputFunction takes a plain C function
// pointer (no std::function / capture support).
static DapServer *g_messageServer = nullptr;
static void
ctldapMessageHook (const std::string &message)
{
    if (!g_messageServer) return;       // CTL libs initialised but no session
    g_messageServer->sendEvent ("output", {
        {"category", "stdout"},
        {"output",   message}
    });
}

HandlerSet::HandlerSet (DapServer &server)
:
    _server (server)
{
    g_messageServer = &server;
    Ctl::setMessageOutputFunction (ctldapMessageHook);

    _server.on ("initialize",        [this](const nlohmann::json &a){ return onInitialize        (a); });
    _server.on ("launch",            [this](const nlohmann::json &a){ return onLaunch            (a); });
    _server.on ("setBreakpoints",    [this](const nlohmann::json &a){ return onSetBreakpoints    (a); });
    _server.on ("configurationDone", [this](const nlohmann::json &a){ return onConfigurationDone (a); });
    _server.on ("threads",           [this](const nlohmann::json &a){ return onThreads           (a); });
    _server.on ("stackTrace",        [this](const nlohmann::json &a){ return onStackTrace        (a); });
    _server.on ("scopes",            [this](const nlohmann::json &a){ return onScopes            (a); });
    _server.on ("variables",         [this](const nlohmann::json &a){ return onVariables         (a); });
    _server.on ("setVariable",       [this](const nlohmann::json &a){ return onSetVariable       (a); });
    _server.on ("continue",          [this](const nlohmann::json &a){ return onContinue          (a); });
    _server.on ("next",              [this](const nlohmann::json &a){ return onNext              (a); });
    _server.on ("stepIn",            [this](const nlohmann::json &a){ return onStepIn            (a); });
    _server.on ("stepOut",           [this](const nlohmann::json &a){ return onStepOut           (a); });
    _server.on ("evaluate",          [this](const nlohmann::json &a){ return onEvaluate          (a); });
    _server.on ("restart",           [this](const nlohmann::json &a){ return onRestart           (a); });
    _server.on ("disconnect",        [this](const nlohmann::json &a){ return onDisconnect        (a); });
}

HandlerSet::~HandlerSet ()
{
    if (_interpThread.joinable())
    {
        // Best-effort: resume any pending pause so the interpreter can exit.
        if (_dbg) _dbg->doContinue();
        _interpThread.join();
    }
}

nlohmann::json
HandlerSet::onInitialize (const nlohmann::json &)
{
    nlohmann::json caps = {
        {"supportsConfigurationDoneRequest", true},
        {"supportsEvaluateForHovers",         true},
        {"supportsRestartRequest",            true},
        {"supportsLogPoints",                 true},
        {"supportsConditionalBreakpoints",    true}
    };
    _server.sendEvent ("initialized");
    return caps;
}

nlohmann::json
HandlerSet::onLaunch (const nlohmann::json &args)
{
    _stopOnEntry     = args.value ("stopOnEntry", false);
    _pixelEvolution  = args.value ("pixelEvolution", false);

    // Accept either:
    //   New form:   programs: string[], functions: string[]
    //   Back-compat: program: string,   function: string
    _ctlPaths.clear();
    _functionNames.clear();

    if (args.contains ("programs") && args["programs"].is_array())
    {
        for (const auto &p : args["programs"])
            _ctlPaths.push_back (p.get<std::string>());

        if (args.contains ("functions") && args["functions"].is_array())
        {
            for (const auto &f : args["functions"])
                _functionNames.push_back (f.get<std::string>());
        }
    }
    else
    {
        // Single-stage back-compat form.
        std::string prog = args.value ("program", std::string());
        if (!prog.empty())
            _ctlPaths.push_back (prog);
        std::string fn = args.value ("function", std::string("main"));
        _functionNames.push_back (fn);
    }

    if (_ctlPaths.empty())
        throw std::runtime_error ("launch: 'program' or 'programs' is required");

    // Optional extra module search paths from launch args.
    _modulePaths.clear();
    if (args.contains ("modulePaths") && args["modulePaths"].is_array())
    {
        for (const auto &p : args["modulePaths"])
            _modulePaths.push_back (p.get<std::string>());
    }

    // Pixel input.
    auto pixelArr = args.value ("pixel", nlohmann::json::array());
    _pixel.clear();
    for (auto &v : pixelArr) _pixel.push_back (v.get<float>());
    if (_pixel.size() < 3) _pixel = {0.5f, 0.5f, 0.5f};

    // Optional `params` dict — uniform float values bound by name onto
    // any matching CTL input arg (typically `input uniform float gain`
    // and friends).  Format: {"gain": 1.5, "exposure": 0.8}
    _params.clear();
    if (args.contains ("params") && args["params"].is_object())
    {
        for (auto it = args["params"].begin();
             it != args["params"].end(); ++it)
        {
            if (it.value().is_number())
                _params[it.key()] = it.value().get<float>();
        }
    }

    setupSession();

    return nlohmann::json::object();
}

void
HandlerSet::setupSession ()
{
    // Helper: return function name for stage i (default "main").
    auto functionForStage = [this](std::size_t i) -> std::string {
        if (i < _functionNames.size())
            return _functionNames[i];
        return "main";
    };

    // Build module search path:
    //   1. CTL_MODULE_PATH env var (colon-separated)
    //   2. modulePaths from launch args
    //   3. Parent directory of each CTL file (sibling imports)
    _interp.reset (new Ctl::SimdInterpreter());
    {
        std::vector<std::string> paths;

        if (const char *env = std::getenv ("CTL_MODULE_PATH"))
        {
            std::string s = env;
            std::size_t pos = 0;
            while (pos < s.size())
            {
                std::size_t colon = s.find (':', pos);
                std::string seg = s.substr (
                    pos,
                    colon == std::string::npos
                        ? std::string::npos
                        : colon - pos);
                if (!seg.empty())
                    paths.push_back (seg);
                if (colon == std::string::npos) break;
                pos = colon + 1;
            }
        }

        for (const auto &p : _modulePaths)
            paths.push_back (p);

        for (const auto &p : _ctlPaths)
            paths.push_back (pathDirname (p));

        _interp->setUserModulePath (paths, true);
    }

    // Load each module.
    for (const auto &p : _ctlPaths)
    {
        try
        {
            _interp->loadModule (pathStem (p), p);
        }
        catch (const std::exception &e)
        {
            throw std::runtime_error (std::string("loadModule failed: ") + e.what());
        }
    }

    // Create a FunctionCallPtr for each stage.
    _dbg.reset (new DapDebugger (*_interp, _server));
    _dbg->setStopOnEntry (_stopOnEntry);
    _dbg->setPixelEvolution (_pixelEvolution);
    _interp->setDebugger (_dbg.get());

    _stages.clear();
    for (std::size_t stage = 0; stage < _ctlPaths.size(); ++stage)
    {
        std::string fn = functionForStage (stage);
        Ctl::FunctionCallPtr fc = _interp->newFunctionCall (fn);
        if (!fc)
            throw std::runtime_error ("stage " + std::to_string (stage) +
                                      ": function not found: " + fn);
        _stages.push_back (fc);
    }

    // Bind stage-0 pixel inputs now (before configurationDone).
    Ctl::bindPixelInputs   (_stages[0], _pixel);
    Ctl::bindUniformParams (_stages[0], _params);

    // Give debugger a reference to stage 0's FC for variable inspection.
    _dbg->setFunctionCall (_stages[0].pointer());

    // Apply breakpoints.  Pre-launch breakpoints land in
    // _pendingBreakpoints; mid-session ones additionally accumulate in
    // _persistedBreakpoints so a `restart` can re-apply them to the
    // newly-recreated dbg.  Drain pending into persisted, then push
    // everything to the new dbg.
    for (auto &kv : _pendingBreakpoints)
        _persistedBreakpoints[kv.first] = kv.second;
    _pendingBreakpoints.clear();
    for (const auto &kv : _persistedBreakpoints)
        _dbg->setBreakpoints (kv.first, kv.second);
}

int
HandlerSet::allocateStructRef (const Ctl::DataTypePtr &type,
                               const void *data)
{
    if (!type || !data) return 0;
    if (type->cDataType() != Ctl::StructTypeEnum) return 0;
    Ctl::StructTypePtr stype = type.cast<Ctl::StructType>();
    if (!stype) return 0;
    int ref = _nextVarRef++;
    _structRefs[ref] = {data, stype};
    return ref;
}

void
HandlerSet::teardownSession ()
{
    // Stop any running interpreter thread.  If it's paused at a BP,
    // doContinue + a brief sleep gives it the chance to finish the
    // current stage before we join.  If it's mid-execution, it'll
    // either finish the current stage or hit `terminated` naturally.
    if (_interpThread.joinable())
    {
        if (_dbg) _dbg->doContinue();
        _interpThread.join();
    }
    _interpDone = false;
    _stages.clear();
    _dbg.reset();
    _interp.reset();
    _scopeRefs.clear();
    _structRefs.clear();
    _nextVarRef = 1;
    _configDone = false;
}

nlohmann::json
HandlerSet::onRestart (const nlohmann::json &args)
{
    // DAP allows the client to send fresh launch arguments under
    // `arguments` (top-level "arguments" of the restart request body).
    // VS Code uses this to push the latest launch config — including a
    // newly-picked status-bar pixel — into the restarted session.  When
    // omitted, we re-use whatever's cached in the member fields.
    if (args.contains ("arguments") && args["arguments"].is_object())
    {
        // Re-parse the embedded launch args by routing through onLaunch.
        // onLaunch tears nothing down (it just overwrites member state
        // and calls setupSession), so we have to teardown FIRST.
        teardownSession();
        onLaunch (args["arguments"]);
    }
    else
    {
        teardownSession();
        setupSession();
    }

    // Restart the interp thread (configurationDone semantics).
    _configDone = true;
    _interpThread = std::thread ([this]{ runInterpreter(); });

    return nlohmann::json::object();
}

nlohmann::json
HandlerSet::onSetBreakpoints (const nlohmann::json &args)
{
    std::string srcPath = args["source"].value ("path", std::string());
    std::vector<DapDebugger::BpSpec> specs;
    nlohmann::json verified = nlohmann::json::array();
    for (const auto &bp : args.value ("breakpoints", nlohmann::json::array()))
    {
        int ln = bp.value ("line", 0);
        if (ln <= 0) continue;
        std::string cond   = bp.value ("condition",  std::string());
        std::string logMsg = bp.value ("logMessage", std::string());
        specs.push_back ({ln, cond, logMsg});
        verified.push_back ({{"verified", true}, {"line", ln}});
    }

    if (_dbg) {
        _dbg->setBreakpoints (srcPath, specs);
    } else {
        _pendingBreakpoints[srcPath] = specs;
    }
    // Mirror into the persistent cache so a `restart` (which recreates
    // _dbg) can re-apply the user's current breakpoint set.  Empty
    // specs means the user cleared all BPs in this file — drop the
    // entry so we don't keep re-applying them after a restart.
    if (specs.empty())
        _persistedBreakpoints.erase (srcPath);
    else
        _persistedBreakpoints[srcPath] = specs;
    return nlohmann::json{{"breakpoints", verified}};
}

nlohmann::json
HandlerSet::onConfigurationDone (const nlohmann::json &)
{
    if (_stages.empty()) throw std::runtime_error ("configurationDone before launch");
    _configDone = true;
    _interpThread = std::thread ([this]{ runInterpreter(); });
    return nlohmann::json::object();
}

void
HandlerSet::runInterpreter ()
{
    std::map<std::string, float> prevOutputs;
    Ctl::FunctionCallPtr lastFc;

    auto formatOutputs = [](const Ctl::FunctionCallPtr &fc) -> std::string {
        std::string s;
        bool first = true;
        for (std::size_t i = 0; i < fc->numOutputArgs(); ++i)
        {
            Ctl::FunctionArgPtr arg = fc->outputArg (i);
            if (!arg || !arg->type()) continue;
            std::string val =
                ctldb::formatValue (arg->type().cast<Ctl::DataType>(),
                                    arg->data(), 0);
            if (!first) s += ", ";
            s += arg->name() + "=" + val;
            first = false;
        }
        return s;
    };

    bool isChain = _stages.size() > 1;

    for (std::size_t stage = 0; stage < _stages.size(); ++stage)
    {
        Ctl::FunctionCallPtr &fc = _stages[stage];

        // Stage 0 was already pixel-bound in onLaunch; subsequent stages
        // get their inputs from the prior stage's outputs.
        if (stage > 0)
        {
            bindFromPriorStage (fc, prevOutputs);
            // Update debugger's FC reference for variable inspection.
            _dbg->setFunctionCall (fc.pointer());
        }

        try
        {
            fc->callFunction (1);
        }
        catch (const std::exception &e)
        {
            std::cerr << "ctldap: interpreter error (stage " << stage
                      << "): " << e.what() << std::endl;
            break;
        }

        prevOutputs = captureOutputs (fc);
        lastFc = fc;

        // For multi-stage chains, surface each stage's outputs as a
        // separate Debug Console line so the user can see the hand-off
        // (stage N's outputs → stage N+1's inputs) as it happens — not
        // just the final stage's result at termination.  Single-stage
        // sessions skip this; the final summary below is enough.
        if (isChain)
        {
            std::string outs = formatOutputs (fc);
            if (!outs.empty())
            {
                // 1-indexed for parity with the namespace names users see
                // in the Call Stack (stage1::main, stage2::main, …) and
                // with how multi-stage pipelines are labeled in CTL docs.
                std::string line = "→ stage " + std::to_string (stage + 1)
                                 + " of "    + std::to_string (_stages.size())
                                 + ": "      + outs + "\n";
                _server.sendEvent ("output", {
                    {"category", "stdout"},
                    {"output",   line}
                });
            }
        }
    }

    // Surface the final stage's outputs in the Debug Console so the
    // user can see what the transform produced without having to
    // re-launch with stopOnEntry and step to the end.
    if (lastFc)
    {
        std::string outs = formatOutputs (lastFc);
        if (!outs.empty())
        {
            std::string summary = "→ output: " + outs + "\n";
            _server.sendEvent ("output", {
                {"category", "stdout"},
                {"output",   summary}
            });
        }
    }

    // Discoverable end-of-session marker.  VS Code freezes the Watch /
    // Variables panels once `terminated` fires (no live frame to query),
    // and users who don't know that read the resulting "not available"
    // as a debugger bug.  A single visible line in the Debug Console —
    // right where they're already looking at the per-stage output —
    // covers the gap without nagging.  Category "console" tags it as
    // adapter-emitted (vs program stdout).
    _server.sendEvent ("output", {
        {"category", "console"},
        {"output",
         "[ctl-debug] session ended — Watch / Variables won't refresh "
         "until you press F5 to run again.  (Mid-session use Cmd-Shift-F5 "
         "to restart without stopping first.)\n"}
    });

    _interpDone = true;
    _server.sendEvent ("terminated");
}

nlohmann::json
HandlerSet::onThreads (const nlohmann::json &)
{
    return nlohmann::json{{"threads", nlohmann::json::array({{{"id", 1}, {"name", "ctl"}}})}};
}

nlohmann::json
HandlerSet::onStackTrace (const nlohmann::json &)
{
    if (!_dbg) return nlohmann::json::object();

    nlohmann::json frames = nlohmann::json::array();
    const auto &stop  = _dbg->lastStop();
    const auto &stack = _dbg->callStack();

    // Top frame's source file.  xc.fileName() lags by an instruction
    // when stepping into a new module, so when we just stepped INTO a
    // callee the recorded stop.file may still be the caller's file.
    // Recover the true file from the callee's defining module.
    std::string topFile = stop.file;
    std::string topFn   = stack.empty() ? std::string("(top)")
                                        : stack.back().functionName;
    if (!stack.empty())
    {
        std::string defFile = functionFile (_dbg->interpreter(), topFn);
        if (!defFile.empty()) topFile = defFile;
    }

    int frameId = 0;
    frames.push_back ({
        {"id",     frameId++},
        {"name",   topFn},
        {"line",   stop.line},
        {"column", 1},
        {"source", {{"path", topFile}}}
    });

    // Caller frames.  Each entry in callStack is (functionName,
    // callerFile, callerLine).  The CALL SITE inside frame N is stored
    // on entry N+1's callerFile/callerLine — i.e. "where N+1 was called
    // from" is the line currently active inside N.  Walk in pairs.
    for (std::size_t i = stack.size(); i >= 2; --i)
    {
        // stack[i-1] is the callee; stack[i-2] is its caller.
        // The caller's current line is where it called stack[i-1].
        const auto &callee = stack[i - 1];
        const auto &caller = stack[i - 2];

        // Skip synthetic host→CTL boundary (callerFile unset on outermost).
        if (callee.callerLine <= 0 ||
            callee.callerFile.empty() ||
            callee.callerFile == "unknown")
        {
            continue;
        }

        frames.push_back ({
            {"id",     frameId++},
            {"name",   caller.functionName},
            {"line",   callee.callerLine},
            {"column", 1},
            {"source", {{"path", callee.callerFile}}}
        });
    }

    return nlohmann::json{{"stackFrames", frames},
                          {"totalFrames", static_cast<int>(frames.size())}};
}

nlohmann::json
HandlerSet::onScopes (const nlohmann::json &args)
{
    int frameId = args.value ("frameId", 0);
    int localsRef = _nextVarRef++;
    int moduleRef = _nextVarRef++;
    _scopeRefs[localsRef] = {frameId, ScopeKind::Locals};
    _scopeRefs[moduleRef] = {frameId, ScopeKind::Module};

    return nlohmann::json{
        {"scopes", nlohmann::json::array({
            {
                {"name",                "Locals"},
                {"variablesReference",  localsRef},
                {"expensive",           false}
            },
            {
                {"name",                "Module"},
                {"variablesReference",  moduleRef},
                {"expensive",           false}
            }
        })}
    };
}

nlohmann::json
HandlerSet::onVariables (const nlohmann::json &args)
{
    int ref = args.value ("variablesReference", 0);
    if (!_dbg) return nlohmann::json::object();

    // Struct field expansion: when the user opens a struct in the
    // Variables panel, VS Code re-issues `variables` with the ref we
    // handed out for that struct.  Walk the members and emit each;
    // nested structs get their own refs recursively.
    auto sr = _structRefs.find (ref);
    if (sr != _structRefs.end())
    {
        nlohmann::json out = nlohmann::json::array();
        const Ctl::MemberVector &mems = sr->second.type->members();
        for (const auto &m : mems)
        {
            const void *fieldData =
                static_cast<const char*>(sr->second.data) + m.offset;
            int childRef = 0;
            Ctl::DataTypePtr dt = m.type.cast<Ctl::DataType>();
            if (dt && dt->cDataType() == Ctl::StructTypeEnum)
            {
                Ctl::StructTypePtr child = m.type.cast<Ctl::StructType>();
                if (child)
                {
                    childRef = _nextVarRef++;
                    _structRefs[childRef] = {fieldData, child};
                }
            }
            std::string val =
                ctldb::formatValue (m.type.cast<Ctl::DataType>(),
                                    fieldData, 0);
            out.push_back ({
                {"name",                m.name},
                {"value",               val},
                {"variablesReference",  childRef}
            });
        }
        return nlohmann::json{{"variables", out}};
    }

    auto it = _scopeRefs.find (ref);
    if (it == _scopeRefs.end()) return nlohmann::json::object();

    int frameId = it->second.frameId;
    ScopeKind kind = it->second.kind;

    Ctl::SimdXContext *xc = _dbg->lastStop().xcontext;
    if (!xc) return nlohmann::json{{"variables", nlohmann::json::array()}};

    // For non-top frames, swap the xcontext fp to that frame's saved
    // value so inspectVariables resolves THAT frame's locals (CTL
    // addresses are fp-relative).  Restored on exit via RAII.  Module
    // scope doesn't depend on fp so this is a no-op there.  Also stash
    // the per-frame function name + call-site line so we filter to the
    // right frame's symbols and hide post-call-site declarations.
    struct FpSwap {
        Ctl::SimdXContext *xc;
        int saved;
        bool active = false;
        ~FpSwap () { if (active) xc->stack().setFp (saved); }
    } fpSwap{xc, xc->stack().fp()};

    std::string frameFn;
    int         frameLine = -1;
    if (kind == ScopeKind::Locals)
    {
        const auto &cs = _dbg->callStack();
        if (frameId == 0)
        {
            frameFn   = cs.empty() ? std::string() : cs.back().functionName;
            frameLine = _dbg->lastStop().line;
        }
        else
        {
            // Frame N = cs[size-1-N]; the call-site line for that
            // frame is the next-deeper frame's callerLine.
            if (frameId >= static_cast<int>(cs.size()))
                return nlohmann::json{{"variables", nlohmann::json::array()}};
            std::size_t idx = cs.size() - 1 - static_cast<std::size_t>(frameId);
            std::size_t sFp = cs[idx].savedFp;
            if (sFp == static_cast<std::size_t>(-1))
                return nlohmann::json{{"variables", nlohmann::json::array()}};
            xc->stack().setFp (static_cast<int>(sFp));
            fpSwap.active = true;
            frameFn   = cs[idx].functionName;
            frameLine = cs[idx + 1].callerLine;   // call site line in this frame
        }
    }

    // Module scope: user-defined module-scope constants and globals
    // (e.g. `const float GAMMA = 2.2;` at the top of demo.ctl).
    // Excludes CTL stdlib constants — those are defined with
    // info->module() == nullptr; user constants point to their
    // declaring Module.
    if (kind == ScopeKind::Module)
    {
        nlohmann::json mvars = nlohmann::json::array();
        const Ctl::SymbolTable &st = _dbg->interpreter().symbolTable();
        for (auto sit = st.begin(); sit != st.end(); ++sit)
        {
            const Ctl::SymbolInfoPtr &info = sit->second;
            if (!info)              continue;
            if (!info->module())    continue;       // skip stdlib
            if (!info->isData())    continue;       // skip functions/types
            // Skip compiler-synthesized $return slots and default-value
            // statics (e.g. "main$gain" for `uniform float gain = 1.0`)
            // — these would clutter the Module panel with duplicates of
            // the real params.
            if (sit->first.find ("$") != std::string::npos) continue;
            // Inline-literal-backed constants resolve via PushRefInst
            // through info->addr().  Walk the address the same way
            // inspectVariables does for locals.
            Ctl::SimdDataAddrPtr saddr = info->addr().cast<Ctl::SimdDataAddr>();
            if (!saddr) continue;
            const void *data = nullptr;
            try {
                data = saddr->reg(*xc)[0];
            } catch (...) {
                // fp-relative globals would throw — skip silently.
                continue;
            }
            if (!data) continue;
            std::string val = ctldb::formatValue (info->dataType(), data, 0);
            int childRef = allocateStructRef (info->dataType(), data);
            mvars.push_back ({
                {"name",                friendlyName (sit->first)},
                {"value",               val},
                {"variablesReference",  childRef}
            });
        }
        return nlohmann::json{{"variables", mvars}};
    }

    // Use the per-frame function name/line we computed above; for the
    // top frame these collapse to the live currentFn / lastStop.line.
    std::string currentFn = frameFn;
    int         currentLine = (frameLine > 0) ? frameLine : _dbg->lastStop().line;
    if (currentFn.empty() && !_dbg->callStack().empty())
        currentFn = _dbg->callStack().back().functionName;

    // Extract the namespace prefix of the current function so we can
    // filter out symbols that belong to OTHER functions.  E.g. if
    // currentFn is "demo::main", nsPrefix is "demo::"; we then drop any
    // inspector result whose qualified name starts with "helper::".
    // (Phase 1's inspector returns symbols from every loaded module
    // and only filters by fp-relative bounds, which leaks neighbour-
    // function locals as zero-valued entries.  v1.x cleanup.)
    std::string nsPrefix;
    {
        auto rpos = currentFn.rfind ("::");
        if (rpos != std::string::npos)
            nsPrefix = currentFn.substr (0, rpos + 2);     // "demo::"
    }

    nlohmann::json vars = nlohmann::json::array();

    // 1. CTL locals + globals via inspectVariables.  For non-top
    // frames the fp swap above + the currentFn/currentLine plumbing
    // ensures we resolve THAT frame's locals (not the live top frame).
    auto inspected = Ctl::inspectVariables (_dbg->interpreter(), *xc,
                                            xc->fileName(),
                                            currentLine,
                                            currentFn);
    if (const char *tp = std::getenv ("CTLDAP_TRACE"))
    {
        if (FILE *f = std::fopen (tp, "a"))
        {
            std::fprintf (f, "onVariables currentFn=\"%s\" line=%d nsPrefix=\"%s\" inspected=%zu\n",
                          currentFn.c_str(), _dbg->lastStop().line,
                          nsPrefix.c_str(), inspected.size());
            for (const auto &v : inspected)
                std::fprintf (f, "  inspected: %s\n", v.name.c_str());
            std::fflush (f);
            std::fclose (f);
        }
    }
    for (const auto &v : inspected)
    {
        // Filter to only variables in the same namespace as the current
        // function (skips locals from other functions whose slots happen
        // to fall within the fp-relative inspection window).
        if (!nsPrefix.empty() &&
            v.name.compare (0, nsPrefix.size(), nsPrefix) != 0)
        {
            continue;
        }

        // Skip compiler-synthesized symbols.  CTL emits two kinds:
        //   "<fn>$return"  — return-value slot
        //   "<fn>$<param>" — default-value static for a param with a
        //                    default initializer (e.g. uniform float
        //                    gain = 1.0 produces "main$gain")
        // Both have a "$" in the absolute name and would otherwise
        // appear in the Locals panel as zero-valued duplicates of the
        // real param/return.
        if (v.name.find ("$") != std::string::npos) continue;

        std::string name = friendlyName (v.name);

        // Skip CTL stdlib constants (FLT_EPSILON, HALF_MAX, INT_MIN,
        // M_PI, UINT_MAX, etc.).  These all follow the C convention of
        // an UPPER_SNAKE_CASE leaf name; no user variable in actual CTL
        // code uses that style for ordinary scope.  Filtering keeps the
        // Locals panel focused on what's local to the paused function.
        bool looksLikeConstant = !name.empty();
        for (char c : name)
        {
            if (!(c == '_' || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
            {
                looksLikeConstant = false;
                break;
            }
        }
        if (looksLikeConstant) continue;

        std::string val = ctldb::formatValue (v.type, v.data, 0);
        int childRef = allocateStructRef (v.type, v.data);
        vars.push_back ({
            {"name",                name},
            {"value",               val},
            {"variablesReference",  childRef}
        });
    }

    // 2. FunctionCall input/output args (still useful even with locals
    //    because they aren't always returned by inspectVariables).
    if (Ctl::FunctionCall *fc = _dbg->functionCall())
    {
        // Reuse the same approach Repl uses — for now skip to avoid
        // duplicate-name clashes; v1.1 can disambiguate.
        (void) fc;
    }

    if (const char *tp = std::getenv ("CTLDAP_TRACE"))
    {
        if (FILE *f = std::fopen (tp, "a"))
        {
            std::fprintf (f, "  onVariables RETURNED %zu vars\n", vars.size());
            for (const auto &v : vars)
            {
                std::string n = v.value ("name", std::string("?"));
                std::string val = v.value ("value", std::string("?"));
                std::fprintf (f, "    -> %s = %s\n", n.c_str(), val.c_str());
            }
            std::fflush (f);
            std::fclose (f);
        }
    }
    return nlohmann::json{{"variables", vars}};
}

nlohmann::json
HandlerSet::onSetVariable (const nlohmann::json &args)
{
    int ref = args.value ("variablesReference", 0);
    auto it = _scopeRefs.find (ref);
    if (it == _scopeRefs.end() || !_dbg)
        return nlohmann::json{{"value", "<no scope>"}};

    std::string name  = args.value ("name",  std::string());
    std::string value = args.value ("value", std::string());
    if (name.empty())
        return nlohmann::json{{"value", "<no name>"}};

    Ctl::SimdXContext *xc = _dbg->lastStop().xcontext;
    if (!xc) return nlohmann::json{{"value", "<no context>"}};

    // Module-scope consts are read-only; reject explicitly.
    if (it->second.kind == ScopeKind::Module)
        return nlohmann::json{{"value", "<read-only: module consts>"}};

    // Look up the local by friendly name in the inspector snapshot.
    std::string currentFn;
    if (!_dbg->callStack().empty())
        currentFn = _dbg->callStack().back().functionName;
    auto inspected = Ctl::inspectVariables (_dbg->interpreter(), *xc,
                                            xc->fileName(),
                                            _dbg->lastStop().line,
                                            currentFn);
    for (const auto &v : inspected)
    {
        if (friendlyName (v.name) != name && v.name != name) continue;
        if (!v.type || !v.data) break;
        // Mutate lane-0 storage in place.  data is `const void *` from
        // the inspector but it points to the live SimdReg — cast away
        // const for the write.  Only support scalar numeric types.
        void *mut = const_cast<void *>(v.data);
        try
        {
            switch (v.type->cDataType())
            {
              case Ctl::IntTypeEnum:
                *reinterpret_cast<int *>(mut) = std::stoi (value);
                break;
              case Ctl::UIntTypeEnum:
                *reinterpret_cast<unsigned int *>(mut) =
                    static_cast<unsigned int>(std::stoul (value));
                break;
              case Ctl::FloatTypeEnum:
                *reinterpret_cast<float *>(mut) = std::stof (value);
                break;
              default:
                return nlohmann::json{{"value",
                    "<unsupported type for mutation>"}};
            }
        }
        catch (const std::exception &e)
        {
            return nlohmann::json{{"value",
                std::string("<parse error: ") + e.what() + ">"}};
        }
        // Echo the formatted new value back so VS Code refreshes the
        // panel without an extra variables round-trip.
        return nlohmann::json{{"value", ctldb::formatValue (v.type, v.data, 0)}};
    }

    return nlohmann::json{{"value", "<not found: " + name + ">"}};
}

nlohmann::json
HandlerSet::onContinue (const nlohmann::json &)
{
    // Struct-expansion refs point at SimdReg storage that may move on
    // the next pause; invalidate them on every resume.
    _structRefs.clear();
    if (_dbg) _dbg->doContinue();
    return nlohmann::json{{"allThreadsContinued", true}};
}

nlohmann::json
HandlerSet::onNext (const nlohmann::json &)
{
    _structRefs.clear();
    if (_dbg) _dbg->doNext();
    return nlohmann::json::object();
}

nlohmann::json
HandlerSet::onStepIn (const nlohmann::json &)
{
    _structRefs.clear();
    if (_dbg) _dbg->doStepIn();
    return nlohmann::json::object();
}

nlohmann::json
HandlerSet::onStepOut (const nlohmann::json &)
{
    _structRefs.clear();
    if (_dbg) _dbg->doStepOut();
    return nlohmann::json::object();
}

nlohmann::json
HandlerSet::onEvaluate (const nlohmann::json &args)
{
    // Helper: pack {result, variablesReference} into a response.  Null
    // helper makes every return site one line.
    auto reply = [](const std::string &s) {
        return nlohmann::json{{"result", s}, {"variablesReference", 0}};
    };

    std::string expr = args.value ("expression", std::string());
    if (expr.empty())
        return reply ("<empty expression>");

    if (!_dbg)
        return reply ("<no debug session — F5 to launch>");

    // Session terminated naturally.  We keep _dbg around so post-mortem
    // requests don't crash, but no live xcontext exists; tell the user
    // explicitly so the Watch panel doesn't just say "not available".
    if (_interpDone)
        return reply ("<session ended — F5 to run again>");

    Ctl::SimdXContext *xc = _dbg->lastStop().xcontext;
    if (!xc)
        return reply ("<not paused — set a breakpoint and run>");

    std::string currentFn;
    if (!_dbg->callStack().empty())
        currentFn = _dbg->callStack().back().functionName;

    auto inspected = Ctl::inspectVariables (_dbg->interpreter(), *xc,
                                            xc->fileName(),
                                            _dbg->lastStop().line,
                                            currentFn);

    // Fast path: bare-name lookup that returns the original CTL value
    // formatted for display (preserves "[a, b, c]" for arrays etc.).
    for (const auto &v : inspected)
    {
        if (v.name == expr || friendlyName (v.name) == expr)
        {
            return nlohmann::json{{"result", ctldb::formatValue(v.type, v.data, 0)},
                                  {"variablesReference", 0}};
        }
    }

    // Slow path: real expression evaluator (literals, +-*/, indexing,
    // comparisons).  Returns a numeric scalar or an error.
    Ctl::EvalResult er = Ctl::evalExpression (expr, inspected);
    if (er.ok)
    {
        char buf[64];
        std::snprintf (buf, sizeof buf, "%g", er.value);
        return reply (buf);
    }

    // Distinguish "name is real but not in this scope" (a local declared
    // later, a variable from a different function) from "name doesn't
    // exist anywhere".  The former is the most common Watch-panel
    // confusion — user pinned `gammaed[0]` while paused two lines BEFORE
    // gammaed gets declared, and "<unknown name>" makes it look broken.
    //
    // We probe the interpreter's symbol table for the bare leading
    // identifier of the expression: if any qualified symbol ends in
    // "::<name>" (or the name itself), it's a real CTL symbol that's
    // just out of scope at the current pause point.
    std::string leading;
    for (char c : expr)
    {
        if (c == '_' || (c >= '0' && c <= '9') ||
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
            leading += c;
        else
            break;
    }
    if (!leading.empty())
    {
        bool existsElsewhere = false;
        std::string suffix = "::" + leading;

        // 1. Module-scope symbols (globals, constants, function names).
        const Ctl::SymbolTable &st = _dbg->interpreter().symbolTable();
        for (auto sit = st.begin(); sit != st.end() && !existsElsewhere; ++sit)
        {
            const std::string &abs = sit->first;
            if (abs == leading ||
                (abs.size() > suffix.size() &&
                 abs.compare (abs.size() - suffix.size(),
                              suffix.size(), suffix) == 0))
                existsElsewhere = true;
        }

        // 2. Per-module captured local snapshots (function-locals; the
        // parser deletes these from the live symbol table after parsing,
        // but each Module keeps a copy so the inspector can find them).
        // Walk every module's snapshot — a name like `gammaed` declared
        // on a later line of the current function lives here.
        if (!existsElsewhere)
        {
            const Ctl::ModuleSet &mset = _dbg->interpreter().moduleSet();
            for (auto mit = mset.begin();
                 mit != mset.end() && !existsElsewhere; ++mit)
            {
                const Ctl::Module *mod = mit->second;
                if (!mod) continue;
                for (const auto &entry : mod->localSymbols())
                {
                    const std::string &abs = entry.first;
                    if (abs == leading ||
                        (abs.size() > suffix.size() &&
                         abs.compare (abs.size() - suffix.size(),
                                      suffix.size(), suffix) == 0))
                    {
                        existsElsewhere = true;
                        break;
                    }
                }
            }
        }

        if (existsElsewhere)
            return reply ("<not in scope here — declared elsewhere or after this line>");
    }

    return reply ("<unknown: " + er.error + ">");
}

nlohmann::json
HandlerSet::onDisconnect (const nlohmann::json &)
{
    return nlohmann::json::object();
}

} // namespace ctldap
