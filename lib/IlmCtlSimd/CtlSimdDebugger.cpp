///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#include "CtlSimdDebugger.h"

namespace Ctl {

SimdDebugger::~SimdDebugger () = default;

NoopDebugger *
NoopDebugger::instance ()
{
    static NoopDebugger s_instance;
    return &s_instance;
}

DebugController::DebugController ()
:
    _paused (false),
    _resumed (false),
    _action (DebugAction::Continue)
{
    // empty
}

DebugAction
DebugController::pause (const DebugStop &stop)
{
    std::unique_lock<std::mutex> lk (_mutex);
    _lastStop = stop;
    _paused = true;
    _resumed = false;
    _cv.notify_all();                // wake debugger thread
    _cv.wait (lk, [this]{ return _resumed; });
    _paused = false;
    return _action;
}

void
DebugController::resume (DebugAction action)
{
    std::lock_guard<std::mutex> lk (_mutex);
    _action = action;
    _resumed = true;
    _cv.notify_all();
}

} // namespace Ctl
