///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#ifndef CTLDAP_HANDLERS_H
#define CTLDAP_HANDLERS_H

#include "DapServer.h"
#include "DapDebugger.h"

#include <CtlSimdInterpreter.h>
#include <CtlFunctionCall.h>
#include <CtlType.h>

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ctldap {

// Owns the interpreter, the DapDebugger, and the worker thread that
// drives execution.  Registers all DAP request handlers on the server.
class HandlerSet
{
  public:
    HandlerSet (DapServer &server);
    ~HandlerSet ();

  private:
    nlohmann::json onInitialize         (const nlohmann::json &args);
    nlohmann::json onLaunch             (const nlohmann::json &args);
    nlohmann::json onSetBreakpoints     (const nlohmann::json &args);
    nlohmann::json onConfigurationDone  (const nlohmann::json &args);
    nlohmann::json onThreads            (const nlohmann::json &args);
    nlohmann::json onStackTrace         (const nlohmann::json &args);
    nlohmann::json onScopes             (const nlohmann::json &args);
    nlohmann::json onVariables          (const nlohmann::json &args);
    nlohmann::json onSetVariable        (const nlohmann::json &args);
    nlohmann::json onContinue           (const nlohmann::json &args);
    nlohmann::json onNext               (const nlohmann::json &args);
    nlohmann::json onStepIn             (const nlohmann::json &args);
    nlohmann::json onStepOut            (const nlohmann::json &args);
    nlohmann::json onEvaluate           (const nlohmann::json &args);
    nlohmann::json onRestart            (const nlohmann::json &args);
    nlohmann::json onDisconnect         (const nlohmann::json &args);

    void runInterpreter ();          // worker thread body

    // Build a fresh interpreter + DapDebugger + FunctionCall stages from
    // the cached launch args (_ctlPaths/_functionNames/_pixel/_params/...).
    // Used by both onLaunch and onRestart.
    void setupSession ();

    // Tear down the active interpreter thread + dbg + interp state so a
    // fresh setupSession() can run.  Used by onRestart.
    void teardownSession ();

    // Allocate a fresh variablesReference for an expandable struct.
    // Returns 0 (DAP "leaf, not expandable") for non-struct types so
    // callers can write `{"variablesReference", allocateStructRef(...)}`
    // unconditionally.  The returned ref is valid until the next resume.
    int allocateStructRef (const Ctl::DataTypePtr &type, const void *data);

    DapServer                             &_server;
    std::unique_ptr<Ctl::SimdInterpreter>  _interp;
    std::unique_ptr<DapDebugger>           _dbg;

    // Multi-stage pipeline: each entry is a function call for one CTL stage.
    // Stage 0 receives pixel inputs; subsequent stages receive prior outputs.
    std::vector<Ctl::FunctionCallPtr>      _stages;

    std::thread                            _interpThread;
    std::atomic<bool>                      _interpDone{false};

    // Launch parameters
    std::vector<std::string>               _ctlPaths;      // one per stage
    std::vector<std::string>               _functionNames; // one per stage
    std::vector<std::string>               _modulePaths;   // extra search dirs
    std::vector<float>                     _pixel;
    // Optional uniform-input bindings (CTL `input uniform float gain`
    // and friends).  Populated from the launch-config `params` object.
    std::map<std::string, float>           _params;
    bool                                   _stopOnEntry    = false;
    bool                                   _pixelEvolution = false;
    bool                                   _configDone     = false;

    // Scope / variable reference table (DAP requires stable IDs per session).
    enum class ScopeKind { Locals, Module };
    struct ScopeRef { int frameId; ScopeKind kind; };
    int                                    _nextVarRef = 1;
    std::map<int, ScopeRef>                _scopeRefs;

    // Refs allocated lazily for expandable struct values.  Struct member
    // data points into the SimdReg storage of the live xcontext, so it
    // remains valid only while paused; cleared on every resume to avoid
    // serving stale pointers across runs.
    struct StructRef
    {
        const void          *data;
        Ctl::StructTypePtr   type;
    };
    std::map<int, StructRef>               _structRefs;

    // Standard DAP flow sends `setBreakpoints` AFTER `initialized` event but
    // BEFORE `launch`.  At that point _dbg doesn't exist yet, so we stash
    // pre-launch breakpoints here and apply them in onLaunch once _dbg is
    // created.
    // Pre-launch breakpoints per source file.  Each entry carries the
    // line, optional condition, and optional logpoint message.
    std::map<std::string,
             std::vector<DapDebugger::BpSpec>> _pendingBreakpoints;

    // Persistent mirror of every setBreakpoints request the client has
    // made.  The dbg object is recreated on `restart`, so we also keep a
    // copy here and re-apply them to the new dbg in setupSession().
    // Updated whenever onSetBreakpoints fires (pre- or mid-session).
    std::map<std::string,
             std::vector<DapDebugger::BpSpec>> _persistedBreakpoints;
};

} // namespace ctldap

#endif
