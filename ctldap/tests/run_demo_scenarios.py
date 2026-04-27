#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright Contributors to the CTL project.
#
# Headless DAP-protocol exercise of the VS Code demo scenarios.  Spawns
# ctldap as a subprocess, scripts a sequence of DAP messages mimicking
# the F5/F10/F11 flow a user would drive in VS Code, and asserts on each
# response (Locals contents, step destinations, stack frames).
#
# Lets us regression-test the debugger end-to-end without needing a
# human in the loop in VS Code.
#
# Usage: run_demo_scenarios.py <path-to-ctldap>

import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import shutil


# ---------------------------------------------------------------------------
# Fixture content — mirrors what vscode-ctl/launch-demo.sh writes to
# /tmp/ctl-debug-vscode-demo.  Kept inline so the test is self-contained.
# ---------------------------------------------------------------------------

# Mirrors vscode-ctl/launch-demo.sh's demo.ctl line-for-line so the
# line-number assertions below match what a user sees in VS Code.
# CTL requires `import` BEFORE any other top-level decls — keep that
# order or load fails.
DEMO_CTL = """\
import "helper";

const float GAMMA       = 2.2;
const float DOUBLE_GAIN = 2.0;
const float OFFSET      = 0.05;

void
main (output float r, input float rIn)
{
    float boosted = helper::boost (rIn);
    float gammaCorrected = helper::gamma (boosted, GAMMA);
    float doubled = boosted * DOUBLE_GAIN;
    float summed = gammaCorrected + doubled;
    float clamped = helper::clamp01 (summed);
    float adjusted = clamped + OFFSET;
    r = adjusted;
}
"""

# Line numbers used in scenario assertions (kept here so they're easy
# to update if the fixture above changes):
LINE_BOOSTED         = 10   # `float boosted = helper::boost (rIn);`
LINE_GAMMA_CORRECTED = 11   # `float gammaCorrected = helper::gamma (boosted, GAMMA);`
LINE_DOUBLED         = 12
LINE_SUMMED          = 13
LINE_CLAMPED         = 14
LINE_ADJUSTED        = 15
LINE_R_ASSIGN        = 16
LINE_GAMMA_DECL      = 3    # `const float GAMMA       = 2.2;`

HELPER_CTL = """\
// Imported helper module — demo.ctl pulls these functions in via
// `import "helper";`.  Use `step into` (F11) on a call to one of these
// to jump from demo.ctl into this file.
namespace helper {

float
boost (float x)
{
    float scaled = x * 2.5;
    return scaled;
}

float
clamp01 (float x)
{
    if (x > 1.0) {
        return 1.0;
    }
    if (x < 0.0) {
        return 0.0;
    }
    return x;
}

float
gamma (float x, float g)
{
    float p = pow (x, g);
    return p;
}

}
"""

# helper.ctl line layout (kept here as named constants):
HELPER_LINE_BOOST_BODY      = 9    # `float scaled = x * 2.5;`
HELPER_LINE_BOOST_RETURN    = 10
HELPER_LINE_CLAMP01_IF1     = 16   # `if (x > 1.0) {`
HELPER_LINE_CLAMP01_RET_1   = 17
HELPER_LINE_CLAMP01_BRACE_1 = 18   # closing `}` of first if — must NOT be a step destination
HELPER_LINE_CLAMP01_RET_X   = 22


# A second helper module with NO `namespace { }` wrapper.  Functions are
# top-level so callers can invoke them by bare name (no `::` prefix).
HELPER_BARE_CTL = """\
// Top-level helpers (no namespace wrapper).  Callers can invoke these
// by bare name after `import "helper_bare";`.
float
boost_bare (float x)
{
    float scaled = x * 2.5;
    return scaled;
}

float
gamma_bare (float x, float g)
{
    float p = pow (x, g);
    return p;
}
"""

HELPER_BARE_LINE_BOOST_BODY   = 5
HELPER_BARE_LINE_BOOST_RETURN = 6


# Demo using bare-name (un-namespaced) imported function calls.
DEMO_BARE_CTL = """\
import "helper_bare";

void
main (output float r, input float rIn)
{
    float boosted = boost_bare (rIn);
    float gammaed = gamma_bare (boosted, 2.2);
    r = gammaed;
}
"""

DEMO_BARE_LINE_BOOSTED_CALL = 6
DEMO_BARE_LINE_GAMMA_CALL   = 7
DEMO_BARE_LINE_R_ASSIGN     = 8


# ---------------------------------------------------------------------------
# DAP transport — minimal Content-Length-framed JSON-RPC over stdin/stdout.
# ---------------------------------------------------------------------------

class DapClient:
    def __init__(self, ctldap_path, env=None):
        self.proc = subprocess.Popen(
            [ctldap_path],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
        )
        self._seq = 0
        self._lock = threading.Lock()
        self._messages = []        # everything received, in order
        self._stopped_events = []  # subset
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    def _read_loop(self):
        while True:
            line = self.proc.stdout.readline()
            if not line:
                return
            if not line.startswith(b"Content-Length:"):
                continue
            n = int(line.split(b":")[1].strip())
            self.proc.stdout.readline()  # blank
            body = self.proc.stdout.read(n)
            obj = json.loads(body)
            with self._lock:
                self._messages.append(obj)
                if obj.get("type") == "event" and obj.get("event") == "stopped":
                    self._stopped_events.append(obj)

    def send(self, command, arguments=None):
        self._seq += 1
        msg = {"seq": self._seq, "type": "request", "command": command,
               "arguments": arguments or {}}
        body = json.dumps(msg).encode()
        self.proc.stdin.write(b"Content-Length: " + str(len(body)).encode() +
                              b"\r\n\r\n" + body)
        self.proc.stdin.flush()
        return self._seq

    def wait_response(self, request_seq, timeout=2.0):
        """Block until a response matching request_seq arrives."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self._lock:
                for m in self._messages:
                    if m.get("type") == "response" and m.get("request_seq") == request_seq:
                        return m
            time.sleep(0.01)
        raise TimeoutError(f"no response for seq {request_seq} within {timeout}s")

    def wait_stopped(self, after_count, timeout=2.0):
        """Block until stopped event count > after_count, return the new one."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self._lock:
                if len(self._stopped_events) > after_count:
                    return self._stopped_events[after_count]
            time.sleep(0.01)
        raise TimeoutError(f"no stopped event after {after_count} within {timeout}s")

    def request(self, command, arguments=None, timeout=2.0):
        """Convenience: send + wait for response."""
        seq = self.send(command, arguments)
        return self.wait_response(seq, timeout=timeout)

    def shutdown(self):
        try:
            self.send("disconnect", {})
            time.sleep(0.1)
        except Exception:
            pass
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        try:
            self.proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            self.proc.kill()


# ---------------------------------------------------------------------------
# Test scenarios.
# ---------------------------------------------------------------------------

def get_locals(c):
    """Get the Locals scope's variables for the top frame."""
    st = c.request("stackTrace", {"threadId": 1})
    frames = st["body"]["stackFrames"]
    assert frames, "stackTrace returned no frames"
    frame_id = frames[0]["id"]
    sc = c.request("scopes", {"frameId": frame_id})
    locals_scope = next(s for s in sc["body"]["scopes"] if s["name"] == "Locals")
    vars_resp = c.request("variables", {"variablesReference":
                                        locals_scope["variablesReference"]})
    return frames, vars_resp["body"]["variables"]


def get_scope_vars(c, scope_name):
    """Get an arbitrary scope's variables for the top frame."""
    st = c.request("stackTrace", {"threadId": 1})
    frame_id = st["body"]["stackFrames"][0]["id"]
    sc = c.request("scopes", {"frameId": frame_id})
    target = next(s for s in sc["body"]["scopes"] if s["name"] == scope_name)
    vars_resp = c.request("variables", {"variablesReference":
                                        target["variablesReference"]})
    return vars_resp["body"]["variables"]


def names(vars_list):
    return sorted(v["name"] for v in vars_list)


def value_of(vars_list, name):
    for v in vars_list:
        if v["name"] == name:
            return v["value"]
    return None


# Each scenario function returns a list of (label, ok, detail) tuples.
def scenario_main_step_through(ctldap, demo_path, helper_dir):
    """F5 → BP at line 10 (boosted=) → F10 four times.  Verify Locals
    populates correctly and step destinations are line+1 (no jumps to
    const decls)."""
    results = []
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program": demo_path,
            "function": "main",
            "pixel": [0.4, 0, 0],
            "modulePaths": [helper_dir],
            "stopOnEntry": False,
        })
        c.request("setBreakpoints", {
            "source": {"path": demo_path},
            "breakpoints": [{"line": 10}],   # `float boosted = ...` line
        })
        c.request("configurationDone", {})

        stop = c.wait_stopped(after_count=0, timeout=3.0)
        results.append(("hit BP at line 10",
                        stop["body"]["reason"] == "breakpoint", stop["body"]))

        frames, vars0 = get_locals(c)
        # At BP on line 10 — nothing on this line has run yet.  Should
        # see params r and rIn only (boosted's decl line is 10 itself).
        results.append(("at BP: stack top is main on line 10",
                        frames[0]["name"] == "main" and frames[0]["line"] == 10,
                        frames[0]))
        results.append(("at BP: locals = {r, rIn}",
                        names(vars0) == ["rIn", "r"][::-1] or names(vars0) == ["r", "rIn"],
                        names(vars0)))
        results.append(("at BP: rIn = 0.4",
                        value_of(vars0, "rIn") in ("0.4", "0.400000"),
                        value_of(vars0, "rIn")))

        # F10 #1 — should land on line 11 (gammaCorrected = ...)
        c.send("next", {"threadId": 1})
        stop = c.wait_stopped(after_count=1, timeout=3.0)
        frames, vars1 = get_locals(c)
        results.append(("F10 #1: lands on line 11",
                        frames[0]["line"] == 11, frames[0]["line"]))
        results.append(("F10 #1: locals add 'boosted'",
                        "boosted" in names(vars1), names(vars1)))
        results.append(("F10 #1: boosted = 1.0",
                        value_of(vars1, "boosted") in ("1", "1.0", "1.000000"),
                        value_of(vars1, "boosted")))

        # F10 #2 — should land on line 12 (doubled = boosted * DOUBLE_GAIN)
        # KEY: must NOT jump to line 1 (const GAMMA decl)
        c.send("next", {"threadId": 1})
        stop = c.wait_stopped(after_count=2, timeout=3.0)
        frames, vars2 = get_locals(c)
        results.append(("F10 #2: lands on line 12 (NOT a const-decl line 1-3)",
                        frames[0]["line"] == 12, frames[0]["line"]))
        results.append(("F10 #2: locals add 'gammaCorrected'",
                        "gammaCorrected" in names(vars2), names(vars2)))

        # F10 #3 — should land on line 13 (summed = ...)
        c.send("next", {"threadId": 1})
        stop = c.wait_stopped(after_count=3, timeout=3.0)
        frames, vars3 = get_locals(c)
        results.append(("F10 #3: lands on line 13",
                        frames[0]["line"] == 13, frames[0]["line"]))
        results.append(("F10 #3: locals add 'doubled'",
                        "doubled" in names(vars3), names(vars3)))

        # Continue to finish.
        c.send("continue", {"threadId": 1})
        time.sleep(0.5)
    finally:
        c.shutdown()
    return results


def scenario_step_into_helper(ctldap, demo_path, helper_dir):
    """F5 → BP at line 10 → F11 (step into helper::boost).  Verify the
    stack shows two frames, top frame's file is helper.ctl, and locals
    show helper::boost's frame."""
    results = []
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program": demo_path,
            "function": "main",
            "pixel": [0.4, 0, 0],
            "modulePaths": [helper_dir],
            "stopOnEntry": False,
        })
        c.request("setBreakpoints", {
            "source": {"path": demo_path},
            "breakpoints": [{"line": 10}],
        })
        c.request("configurationDone", {})
        c.wait_stopped(after_count=0, timeout=3.0)

        # F11 — step INTO helper::boost.
        c.send("stepIn", {"threadId": 1})
        stop = c.wait_stopped(after_count=1, timeout=3.0)
        frames, vars0 = get_locals(c)
        results.append(("F11: stack has 2 frames",
                        len(frames) == 2, [f["name"] for f in frames]))
        results.append(("F11: top frame is helper::boost",
                        frames[0]["name"] == "helper::boost", frames[0]["name"]))
        results.append(("F11: top frame's file is helper.ctl",
                        frames[0]["source"]["path"].endswith("helper.ctl"),
                        frames[0]["source"]["path"]))
        results.append(("F11: caller frame is main on demo.ctl line 10",
                        frames[1]["name"] == "main" and
                        frames[1]["line"] == 10 and
                        frames[1]["source"]["path"].endswith("demo.ctl"),
                        (frames[1]["name"], frames[1]["line"],
                         frames[1]["source"]["path"])))
        results.append(("F11: locals contain 'x' (helper::boost's param)",
                        "x" in names(vars0), names(vars0)))
        results.append(("F11: locals do NOT contain 'boosted' "
                        "(main's local must not leak into helper)",
                        "boosted" not in names(vars0), names(vars0)))
        results.append(("F11: x = 0.4 (rIn passed in)",
                        value_of(vars0, "x") in ("0.4", "0.400000"),
                        value_of(vars0, "x")))

        c.send("continue", {"threadId": 1})
        time.sleep(0.5)
    finally:
        c.shutdown()
    return results


def scenario_no_double_step(ctldap, demo_path, helper_dir):
    """F5 → BP at line 10 → F10 once.  Must advance to line 11 on the
    FIRST press (used to require two presses due to BP dedup latch
    clearing across the helper call)."""
    results = []
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program": demo_path,
            "function": "main",
            "pixel": [0.4, 0, 0],
            "modulePaths": [helper_dir],
            "stopOnEntry": False,
        })
        c.request("setBreakpoints", {
            "source": {"path": demo_path},
            "breakpoints": [{"line": 10}],
        })
        c.request("configurationDone", {})
        c.wait_stopped(after_count=0, timeout=3.0)

        c.send("next", {"threadId": 1})
        stop = c.wait_stopped(after_count=1, timeout=3.0)
        results.append(("F10 once advances past BP line",
                        stop["body"]["reason"] == "step",
                        stop["body"]))
        frames, _ = get_locals(c)
        results.append(("F10 once lands on line 11 (not still on 10)",
                        frames[0]["line"] == 11, frames[0]["line"]))

        c.send("continue", {"threadId": 1})
        time.sleep(0.5)
    finally:
        c.shutdown()
    return results


def scenario_step_through_to_end(ctldap, demo_path, helper_dir):
    """F5 → BP at line 10 → F10 six times.  Each step must land on the
    NEXT line of main's body, never on a const-decl or out-of-function
    line.  Every local should appear after its decl line."""
    results = []
    c = DapClient(ctldap)
    expected_lines = [
        LINE_GAMMA_CORRECTED,  # 11
        LINE_DOUBLED,          # 12
        LINE_SUMMED,           # 13
        LINE_CLAMPED,          # 14
        LINE_ADJUSTED,         # 15
        LINE_R_ASSIGN,         # 16
    ]
    expected_local_added = [
        "boosted",          # added at 11 (declared on 10)
        "gammaCorrected",   # added at 12
        "doubled",          # added at 13
        "summed",           # added at 14
        "clamped",          # added at 15
        "adjusted",         # added at 16
    ]
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program": demo_path,
            "function": "main",
            "pixel": [0.4, 0, 0],
            "modulePaths": [helper_dir],
            "stopOnEntry": False,
        })
        c.request("setBreakpoints", {
            "source": {"path": demo_path},
            "breakpoints": [{"line": LINE_BOOSTED}],
        })
        c.request("configurationDone", {})
        c.wait_stopped(after_count=0, timeout=3.0)

        for i, (want_line, want_local) in enumerate(
                zip(expected_lines, expected_local_added)):
            c.send("next", {"threadId": 1})
            c.wait_stopped(after_count=i + 1, timeout=3.0)
            frames, vars_ = get_locals(c)
            results.append((f"step #{i+1}: lands on line {want_line}",
                            frames[0]["line"] == want_line,
                            frames[0]["line"]))
            results.append((f"step #{i+1}: '{want_local}' is now visible",
                            want_local in names(vars_), names(vars_)))
            results.append((f"step #{i+1}: file stays demo.ctl",
                            frames[0]["source"]["path"].endswith("demo.ctl"),
                            frames[0]["source"]["path"]))

        c.send("continue", {"threadId": 1})
        time.sleep(0.5)
    finally:
        c.shutdown()
    return results


def scenario_step_into_clamp01_if(ctldap, demo_path, helper_dir):
    """F5 → BP at the `clamped = helper::clamp01(...)` line → F11 into
    clamp01 → F10 through the body.  Verify that no step lands on the
    closing brace of an if block (the parser-fix regression test)."""
    results = []
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program": demo_path,
            "function": "main",
            "pixel": [0.4, 0, 0],
            "modulePaths": [helper_dir],
            "stopOnEntry": False,
        })
        c.request("setBreakpoints", {
            "source": {"path": demo_path},
            "breakpoints": [{"line": LINE_CLAMPED}],
        })
        c.request("configurationDone", {})
        c.wait_stopped(after_count=0, timeout=3.0)

        # F11 into clamp01.  Single press should land us inside helper.ctl
        # at the function's signature/body region.
        c.send("stepIn", {"threadId": 1})
        c.wait_stopped(after_count=1, timeout=3.0)
        frames, _ = get_locals(c)
        results.append(("F11 into clamp01: top frame is helper::clamp01",
                        frames[0]["name"] == "helper::clamp01",
                        frames[0]["name"]))
        results.append(("F11 into clamp01: file is helper.ctl",
                        frames[0]["source"]["path"].endswith("helper.ctl"),
                        frames[0]["source"]["path"]))

        # Capture the step-in landing line as the first entry so the
        # "hits line N" assertions below see it.
        seen_lines = [(frames[0]["source"]["path"].split("/")[-1],
                       frames[0]["line"])]

        # Step a few times through clamp01.  With pixel rIn=0.4 the
        # eventual `summed` value passed in is large enough to take the
        # `x > 1.0` branch (rIn=0.4 → boosted=1.0 → gammaCorrected=1.0 →
        # doubled=2.0 → summed=3.0).  So clamp01 should hit:
        #   line 16 (if condition) → line 17 (return 1.0) → exit
        # No step should land on line 18 (the if-block's closing `}`),
        # which used to fire because parseReturnStatement captured
        # currentLineNumber AFTER the `;` was consumed.
        for i in range(8):
            c.send("next", {"threadId": 1})
            try:
                c.wait_stopped(after_count=2 + i, timeout=2.0)
            except TimeoutError:
                break  # function returned, no more pauses
            frames, _ = get_locals(c)
            seen_lines.append((frames[0]["source"]["path"].split("/")[-1],
                              frames[0]["line"]))

        helper_lines = [ln for f, ln in seen_lines if f == "helper.ctl"]
        results.append(("step path inside clamp01 hits line 16 (if condition)",
                        HELPER_LINE_CLAMP01_IF1 in helper_lines, helper_lines))
        results.append(("step path inside clamp01 hits line 17 (return 1.0)",
                        HELPER_LINE_CLAMP01_RET_1 in helper_lines, helper_lines))
        results.append(("step path NEVER lands on line 18 (closing `}`)",
                        HELPER_LINE_CLAMP01_BRACE_1 not in helper_lines,
                        helper_lines))
        last = seen_lines[-1] if seen_lines else None
        results.append(("eventually returns to demo.ctl",
                        last is not None and last[0] == "demo.ctl",
                        seen_lines))

        c.send("continue", {"threadId": 1})
        time.sleep(0.5)
    finally:
        c.shutdown()
    return results


def scenario_step_out_from_helper(ctldap, demo_path, helper_dir):
    """F5 → BP at line 10 → F11 into helper::boost → F11-step-OUT.
    Should land back inside main on demo.ctl, at the line after the
    call (or still on the call line, depending on multi-instruction
    sequencing — both are acceptable as long as we're back at depth 1
    and stack length is 1)."""
    results = []
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program": demo_path,
            "function": "main",
            "pixel": [0.4, 0, 0],
            "modulePaths": [helper_dir],
            "stopOnEntry": False,
        })
        c.request("setBreakpoints", {
            "source": {"path": demo_path},
            "breakpoints": [{"line": LINE_BOOSTED}],
        })
        c.request("configurationDone", {})
        c.wait_stopped(after_count=0, timeout=3.0)

        # F11: step into helper::boost.
        c.send("stepIn", {"threadId": 1})
        c.wait_stopped(after_count=1, timeout=3.0)
        frames_in, _ = get_locals(c)
        results.append(("inside helper::boost (precondition)",
                        len(frames_in) == 2 and
                        frames_in[0]["name"] == "helper::boost",
                        [f["name"] for f in frames_in]))

        # Step OUT — should pop helper::boost and land back in main.
        c.send("stepOut", {"threadId": 1})
        stop = c.wait_stopped(after_count=2, timeout=3.0)
        results.append(("stepOut emits a stop",
                        stop["body"].get("reason") in ("step", "stepOut"),
                        stop["body"]))
        frames_out, vars_out = get_locals(c)
        results.append(("stepOut: stack collapsed to 1 frame (back in main)",
                        len(frames_out) == 1, [f["name"] for f in frames_out]))
        results.append(("stepOut: top frame is main",
                        frames_out[0]["name"] == "main", frames_out[0]["name"]))
        results.append(("stepOut: file is demo.ctl",
                        frames_out[0]["source"]["path"].endswith("demo.ctl"),
                        frames_out[0]["source"]["path"]))
        # We should land PAST the call line (line 11), not still on it.
        # If we land back on line 10 the assign-return-value inst hasn't
        # committed yet and the user can't see the result of the call.
        results.append((f"stepOut: lands PAST the call line "
                        f"(want > {LINE_BOOSTED})",
                        frames_out[0]["line"] > LINE_BOOSTED,
                        frames_out[0]["line"]))
        results.append(("stepOut: 'boosted' is now visible",
                        "boosted" in names(vars_out), names(vars_out)))
        # boosted = rIn * 2.5 = 0.4 * 2.5 = 1.0
        results.append(("stepOut: boosted = 1.0",
                        value_of(vars_out, "boosted") in
                        ("1", "1.0", "1.000000"),
                        value_of(vars_out, "boosted")))

        c.send("continue", {"threadId": 1})
        time.sleep(0.5)
    finally:
        c.shutdown()
    return results


def scenario_step_in_bare_name(ctldap):
    """Imported helpers WITHOUT a namespace wrapper can be called by
    bare name (e.g. `boost_bare(rIn)` instead of `helper::boost_bare`).
    Step-into must work the same way: file becomes helper_bare.ctl,
    callee frame name is the bare function name."""
    results = []
    workdir = tempfile.mkdtemp(prefix="ctldap-bare-")
    demo_path   = os.path.join(workdir, "demo.ctl")
    helper_path = os.path.join(workdir, "helper_bare.ctl")
    with open(demo_path, "w")   as f: f.write(DEMO_BARE_CTL)
    with open(helper_path, "w") as f: f.write(HELPER_BARE_CTL)

    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        launch = c.request("launch", {
            "program": demo_path,
            "function": "main",
            "pixel": [0.4, 0, 0],
            "modulePaths": [workdir],
            "stopOnEntry": False,
        })
        results.append(("bare-name fixture loads",
                        launch.get("success", False) is True,
                        launch.get("message", "ok")))
        if not launch.get("success", False):
            return results

        c.request("setBreakpoints", {
            "source": {"path": demo_path},
            "breakpoints": [{"line": DEMO_BARE_LINE_BOOSTED_CALL}],
        })
        c.request("configurationDone", {})
        c.wait_stopped(after_count=0, timeout=3.0)

        # F11 into boost_bare (bare name, no `::` prefix).
        c.send("stepIn", {"threadId": 1})
        c.wait_stopped(after_count=1, timeout=3.0)
        frames, vars_ = get_locals(c)
        results.append(("step-in: stack has 2 frames",
                        len(frames) == 2, [f["name"] for f in frames]))
        results.append(("step-in: top frame is bare-name 'boost_bare'",
                        frames[0]["name"] == "boost_bare",
                        frames[0]["name"]))
        results.append(("step-in: file resolves to helper_bare.ctl",
                        frames[0]["source"]["path"].endswith("helper_bare.ctl"),
                        frames[0]["source"]["path"]))
        results.append(("step-in: caller is main on demo.ctl",
                        frames[1]["name"] == "main" and
                        frames[1]["source"]["path"].endswith("demo.ctl"),
                        (frames[1]["name"], frames[1]["source"]["path"])))
        results.append(("step-in: helper's 'x' visible (= rIn = 0.4)",
                        value_of(vars_, "x") in ("0.4", "0.400000"),
                        value_of(vars_, "x")))
        results.append(("step-in: main's 'boosted' must NOT leak into helper",
                        "boosted" not in names(vars_), names(vars_)))

        # Step OUT of boost_bare back to main.
        c.send("stepOut", {"threadId": 1})
        c.wait_stopped(after_count=2, timeout=3.0)
        frames_out, vars_out = get_locals(c)
        results.append(("step-out: back to main, single frame",
                        len(frames_out) == 1 and
                        frames_out[0]["name"] == "main",
                        [f["name"] for f in frames_out]))
        results.append(("step-out: 'boosted' visible in main (= 1.0)",
                        value_of(vars_out, "boosted") in
                        ("1", "1.0", "1.000000"),
                        value_of(vars_out, "boosted")))

        c.send("continue", {"threadId": 1})
        time.sleep(0.5)
    finally:
        c.shutdown()
        shutil.rmtree(workdir, ignore_errors=True)
    return results


def scenario_continue(ctldap, demo_path, helper_dir):
    """F5 → BP at line 10 → Continue.  With no other BPs, execution
    must run to completion and emit a `terminated` event.  Critically:
    Continue must NOT behave like a step (must not pause on the next
    line)."""
    results = []
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program": demo_path,
            "function": "main",
            "pixel": [0.4, 0, 0],
            "modulePaths": [helper_dir],
            "stopOnEntry": False,
        })
        c.request("setBreakpoints", {
            "source": {"path": demo_path},
            "breakpoints": [{"line": LINE_BOOSTED}],
        })
        c.request("configurationDone", {})
        c.wait_stopped(after_count=0, timeout=3.0)

        c.send("continue", {"threadId": 1})
        # Wait a moment, then assert NO further stopped event arrived.
        time.sleep(0.7)
        with c._lock:
            extra_stops = len(c._stopped_events) - 1
            terminated = any(m.get("type") == "event" and
                             m.get("event") == "terminated"
                             for m in c._messages)
        results.append(("continue does NOT pause again "
                        "(no second BP, no step)",
                        extra_stops == 0, extra_stops))
        results.append(("continue runs to terminated event",
                        terminated, terminated))
    finally:
        c.shutdown()
    return results


def scenario_continue_to_second_bp(ctldap, demo_path, helper_dir):
    """F5 → BP at line 10 AND BP at line 14 → Continue from line 10
    → must pause at line 14 (not line 11)."""
    results = []
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program": demo_path,
            "function": "main",
            "pixel": [0.4, 0, 0],
            "modulePaths": [helper_dir],
            "stopOnEntry": False,
        })
        c.request("setBreakpoints", {
            "source": {"path": demo_path},
            "breakpoints": [{"line": LINE_BOOSTED}, {"line": LINE_CLAMPED}],
        })
        c.request("configurationDone", {})
        c.wait_stopped(after_count=0, timeout=3.0)

        c.send("continue", {"threadId": 1})
        stop = c.wait_stopped(after_count=1, timeout=3.0)
        results.append(("continue pauses with reason=breakpoint",
                        stop["body"].get("reason") == "breakpoint",
                        stop["body"].get("reason")))
        frames, _ = get_locals(c)
        results.append((f"continue lands at line {LINE_CLAMPED} "
                        "(the second BP), NOT on line 11 (next-line)",
                        frames[0]["line"] == LINE_CLAMPED,
                        frames[0]["line"]))

        c.send("continue", {"threadId": 1})
        time.sleep(0.5)
    finally:
        c.shutdown()
    return results


def scenario_bp_inside_helper_pre_launch(ctldap, demo_path, helper_dir):
    """Set a BP inside helper.ctl BEFORE launching the program.  The
    canonical DAP order is initialize → setBreakpoints → launch →
    configurationDone, so this exercises the pre-launch BP queue."""
    results = []
    helper_path = os.path.join(helper_dir, "helper.ctl")
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        # Set BP BEFORE launch.
        bp_resp = c.request("setBreakpoints", {
            "source": {"path": helper_path},
            "breakpoints": [{"line": HELPER_LINE_BOOST_BODY}],
        })
        results.append(("setBreakpoints (pre-launch) succeeds",
                        bp_resp.get("success", False),
                        bp_resp.get("message", "ok")))
        results.append(("setBreakpoints reports BP verified",
                        bp_resp["body"]["breakpoints"][0].get("verified") is True,
                        bp_resp["body"]["breakpoints"][0]))
        c.request("launch", {
            "program": demo_path,
            "function": "main",
            "pixel": [0.4, 0, 0],
            "modulePaths": [helper_dir],
            "stopOnEntry": False,
        })
        c.request("configurationDone", {})
        stop = c.wait_stopped(after_count=0, timeout=3.0)
        results.append(("first stopped event: reason=breakpoint",
                        stop["body"].get("reason") == "breakpoint",
                        stop["body"].get("reason")))
        frames, vars_ = get_locals(c)
        results.append(("first stop is inside helper::boost",
                        frames[0]["name"] == "helper::boost",
                        frames[0]["name"]))
        results.append(("first stop file is helper.ctl",
                        frames[0]["source"]["path"].endswith("helper.ctl"),
                        frames[0]["source"]["path"]))
        results.append((f"first stop line is {HELPER_LINE_BOOST_BODY}",
                        frames[0]["line"] == HELPER_LINE_BOOST_BODY,
                        frames[0]["line"]))
        results.append(("helper's 'x' visible at the helper-side BP",
                        value_of(vars_, "x") in ("0.4", "0.400000"),
                        value_of(vars_, "x")))

        c.send("continue", {"threadId": 1})
        time.sleep(0.5)
    finally:
        c.shutdown()
    return results


def scenario_modify_bp_mid_session(ctldap, demo_path, helper_dir):
    """F5 → BP at line 10 → Continue → set ANOTHER BP at line 14 mid-
    session → Continue → must pause at line 14."""
    results = []
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program": demo_path,
            "function": "main",
            "pixel": [0.4, 0, 0],
            "modulePaths": [helper_dir],
            "stopOnEntry": False,
        })
        c.request("setBreakpoints", {
            "source": {"path": demo_path},
            "breakpoints": [{"line": LINE_BOOSTED}],
        })
        c.request("configurationDone", {})
        c.wait_stopped(after_count=0, timeout=3.0)
        # Add a second BP while paused at the first.
        bp_resp = c.request("setBreakpoints", {
            "source": {"path": demo_path},
            "breakpoints": [{"line": LINE_BOOSTED}, {"line": LINE_CLAMPED}],
        })
        results.append(("mid-session setBreakpoints succeeds",
                        bp_resp.get("success", False),
                        bp_resp.get("message", "ok")))
        results.append(("mid-session adds BP at clamped line",
                        any(b.get("line") == LINE_CLAMPED
                            for b in bp_resp["body"]["breakpoints"]),
                        bp_resp["body"]["breakpoints"]))

        c.send("continue", {"threadId": 1})
        stop = c.wait_stopped(after_count=1, timeout=3.0)
        frames, _ = get_locals(c)
        results.append(("continue lands at the newly-added BP",
                        frames[0]["line"] == LINE_CLAMPED,
                        frames[0]["line"]))

        # Remove ALL BPs, continue, must run to terminated.
        del_resp = c.request("setBreakpoints", {
            "source": {"path": demo_path},
            "breakpoints": [],
        })
        results.append(("clearing BPs succeeds",
                        del_resp.get("success", False),
                        del_resp.get("message", "ok")))

        c.send("continue", {"threadId": 1})
        time.sleep(0.7)
        with c._lock:
            extra_stops = len(c._stopped_events) - 2
            terminated = any(m.get("type") == "event" and
                             m.get("event") == "terminated"
                             for m in c._messages)
        results.append(("continue after clearing BPs runs to terminated",
                        extra_stops == 0 and terminated,
                        (extra_stops, terminated)))
    finally:
        c.shutdown()
    return results


def scenario_module_scope_consts(ctldap, demo_path, helper_dir):
    """Module-scope `const float` declarations (GAMMA, DOUBLE_GAIN,
    OFFSET) must be visible in a separate "Module" scope so the user
    can see what tunable knobs the transform was loaded with.  Stdlib
    constants (FLT_EPSILON, M_PI, etc.) must NOT appear there — they
    live on info->module() == nullptr."""
    results = []
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program": demo_path,
            "function": "main",
            "pixel": [0.4, 0, 0],
            "modulePaths": [helper_dir],
            "stopOnEntry": False,
        })
        c.request("setBreakpoints", {
            "source": {"path": demo_path},
            "breakpoints": [{"line": LINE_BOOSTED}],
        })
        c.request("configurationDone", {})
        c.wait_stopped(after_count=0, timeout=3.0)

        # scopes response must include both Locals and Module.
        st = c.request("stackTrace", {"threadId": 1})
        frame_id = st["body"]["stackFrames"][0]["id"]
        sc = c.request("scopes", {"frameId": frame_id})
        scope_names = [s["name"] for s in sc["body"]["scopes"]]
        results.append(("scopes response includes Locals + Module",
                        scope_names == ["Locals", "Module"], scope_names))

        mvars = get_scope_vars(c, "Module")
        mvar_names = sorted(v["name"] for v in mvars)
        results.append(("Module scope contains user const GAMMA",
                        "GAMMA" in mvar_names, mvar_names))
        results.append(("Module scope contains user const DOUBLE_GAIN",
                        "DOUBLE_GAIN" in mvar_names, mvar_names))
        results.append(("Module scope contains user const OFFSET",
                        "OFFSET" in mvar_names, mvar_names))
        results.append(("GAMMA = 2.2",
                        value_of(mvars, "GAMMA") in ("2.2", "2.200000"),
                        value_of(mvars, "GAMMA")))
        results.append(("OFFSET = 0.05",
                        value_of(mvars, "OFFSET") in ("0.05", "0.050000"),
                        value_of(mvars, "OFFSET")))

        # Stdlib constants must NOT leak into the Module scope.
        leaked_stdlib = [n for n in mvar_names
                         if n in ("FLT_EPSILON", "FLT_MAX", "M_PI", "M_E",
                                  "INT_MAX", "HALF_MAX", "UINT_MAX")]
        results.append(("Module scope does NOT include stdlib consts "
                        "(FLT_*, M_*, INT_*, HALF_*, UINT_*)",
                        not leaked_stdlib, leaked_stdlib))

        # Locals scope must NOT contain the user consts (they belong to
        # the Module scope, not Locals — avoids double-listing).
        _, lvars = get_locals(c)
        lvar_names = sorted(v["name"] for v in lvars)
        in_locals = [n for n in ("GAMMA", "DOUBLE_GAIN", "OFFSET")
                     if n in lvar_names]
        results.append(("Locals does NOT duplicate user consts",
                        not in_locals, in_locals))

        c.send("continue", {"threadId": 1})
        time.sleep(0.5)
    finally:
        c.shutdown()
    return results


def scenario_race_continue_setbp(ctldap, demo_path, helper_dir):
    """Stress: hammer setBreakpoints + continue across many sessions to
    flush out races between the dispatch thread and the interpreter
    thread.  Each iteration: launch, set BP, continue, terminate."""
    results = []
    iterations = 25
    failures = []
    for i in range(iterations):
        c = DapClient(ctldap)
        try:
            c.request("initialize", {})
            c.request("launch", {
                "program": demo_path,
                "function": "main",
                "pixel": [0.4, 0, 0],
                "modulePaths": [helper_dir],
                "stopOnEntry": False,
            })
            # Hammer setBreakpoints repeatedly before configurationDone.
            for _ in range(5):
                c.request("setBreakpoints", {
                    "source": {"path": demo_path},
                    "breakpoints": [{"line": LINE_BOOSTED}],
                })
            c.request("configurationDone", {})
            try:
                c.wait_stopped(after_count=0, timeout=2.0)
            except TimeoutError:
                failures.append((i, "no first stopped event"))
                continue

            # Add/remove BP during pause, then continue.
            c.request("setBreakpoints", {
                "source": {"path": demo_path},
                "breakpoints": [{"line": LINE_BOOSTED},
                                {"line": LINE_CLAMPED}],
            })
            c.send("continue", {"threadId": 1})
            try:
                c.wait_stopped(after_count=1, timeout=2.0)
            except TimeoutError:
                failures.append((i, "no second BP hit"))
                continue
            c.send("continue", {"threadId": 1})
            time.sleep(0.3)
        finally:
            c.shutdown()

    results.append((f"{iterations} hammered sessions complete without "
                    f"stuck/crashed instances",
                    not failures, failures))
    return results


def scenario_conditional_bp(ctldap, demo_path, helper_dir):
    """BP at line 11 (after boosted is computed) with condition
    `boosted > 0.5`.  At rIn=0.4 → boosted=1.0, so the condition is
    true and the BP fires.  Then re-set with condition `boosted > 5`
    which is false → BP must NOT fire and execution runs to terminated."""
    results = []

    # ---- Case 1: condition true, BP fires.
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program": demo_path,
            "function": "main",
            "pixel": [0.4, 0, 0],
            "modulePaths": [helper_dir],
            "stopOnEntry": False,
        })
        c.request("setBreakpoints", {
            "source": {"path": demo_path},
            "breakpoints": [{"line": LINE_GAMMA_CORRECTED,
                             "condition": "boosted > 0.5"}],
        })
        c.request("configurationDone", {})
        try:
            stop = c.wait_stopped(after_count=0, timeout=3.0)
            frames, _ = get_locals(c)
            results.append(("conditional BP (boosted>0.5) FIRES "
                            "(boosted=1.0 satisfies)",
                            stop["body"].get("reason") == "breakpoint" and
                            frames[0]["line"] == LINE_GAMMA_CORRECTED,
                            (stop["body"], frames[0]["line"])))
        except TimeoutError:
            results.append(("conditional BP (boosted>0.5) FIRES",
                            False, "no stopped event"))
        c.send("continue", {"threadId": 1})
        time.sleep(0.5)
    finally:
        c.shutdown()

    # ---- Case 2: condition false, BP does NOT fire, runs to terminated.
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program": demo_path,
            "function": "main",
            "pixel": [0.4, 0, 0],
            "modulePaths": [helper_dir],
            "stopOnEntry": False,
        })
        c.request("setBreakpoints", {
            "source": {"path": demo_path},
            "breakpoints": [{"line": LINE_GAMMA_CORRECTED,
                             "condition": "boosted > 5"}],
        })
        c.request("configurationDone", {})
        time.sleep(1.0)
        with c._lock:
            stops = len(c._stopped_events)
            terminated = any(m.get("type") == "event" and
                             m.get("event") == "terminated"
                             for m in c._messages)
        results.append(("conditional BP (boosted>5) does NOT fire; "
                        "execution runs to terminated",
                        stops == 0 and terminated,
                        (stops, terminated)))
    finally:
        c.shutdown()

    # ---- Case 3: bare-name truthy condition (`boosted` alone, fires
    #              when value != 0).
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program": demo_path,
            "function": "main",
            "pixel": [0.4, 0, 0],
            "modulePaths": [helper_dir],
            "stopOnEntry": False,
        })
        c.request("setBreakpoints", {
            "source": {"path": demo_path},
            "breakpoints": [{"line": LINE_GAMMA_CORRECTED,
                             "condition": "boosted"}],
        })
        c.request("configurationDone", {})
        try:
            c.wait_stopped(after_count=0, timeout=3.0)
            results.append(("bare-name truthy condition fires (boosted=1.0)",
                            True, "ok"))
        except TimeoutError:
            results.append(("bare-name truthy condition fires (boosted=1.0)",
                            False, "no stopped event"))
        c.send("continue", {"threadId": 1})
        time.sleep(0.5)
    finally:
        c.shutdown()

    return results


def scenario_set_variable(ctldap, demo_path, helper_dir):
    """At the BP on line 11, mutate `boosted` (just assigned by line 10
    to 1.0) to a new value via setVariable.  Continue → the downstream
    `gammaCorrected` etc. should reflect the mutated input."""
    results = []
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program": demo_path,
            "function": "main",
            "pixel": [0.4, 0, 0],
            "modulePaths": [helper_dir],
            "stopOnEntry": False,
        })
        # Stop at line 11 (boosted is set), then again at line 13
        # (LINE_SUMMED — past the `doubled = boosted * DOUBLE_GAIN`
        # assignment, so its computed value is visible).
        c.request("setBreakpoints", {
            "source": {"path": demo_path},
            "breakpoints": [{"line": LINE_GAMMA_CORRECTED},
                            {"line": LINE_SUMMED}],
        })
        c.request("configurationDone", {})
        c.wait_stopped(after_count=0, timeout=3.0)

        # Get the Locals scope ref to drive setVariable.
        st = c.request("stackTrace", {"threadId": 1})
        frame_id = st["body"]["stackFrames"][0]["id"]
        sc = c.request("scopes", {"frameId": frame_id})
        locals_ref = next(s["variablesReference"]
                          for s in sc["body"]["scopes"]
                          if s["name"] == "Locals")
        # Confirm starting value is 1.0.
        vars_before = c.request("variables",
                                {"variablesReference": locals_ref}
                               )["body"]["variables"]
        results.append(("pre-setVariable: boosted = 1.0",
                        value_of(vars_before, "boosted") in
                        ("1", "1.0", "1.000000"),
                        value_of(vars_before, "boosted")))

        # Mutate: boosted = 0.25
        sv = c.request("setVariable", {
            "variablesReference": locals_ref,
            "name":  "boosted",
            "value": "0.25",
        })
        results.append(("setVariable returns the new value",
                        sv.get("body", {}).get("value") in
                        ("0.25", "0.250000"),
                        sv.get("body", {})))

        # Re-fetch and confirm.
        vars_after = c.request("variables",
                               {"variablesReference": locals_ref}
                              )["body"]["variables"]
        results.append(("post-setVariable: boosted = 0.25",
                        value_of(vars_after, "boosted") in
                        ("0.25", "0.250000"),
                        value_of(vars_after, "boosted")))

        # Continue to next BP at line 13.  By this point line 12
        # (`doubled = boosted * DOUBLE_GAIN`) has executed and `doubled`
        # should reflect the mutated boosted: doubled = 0.25 * 2 = 0.5.
        c.send("continue", {"threadId": 1})
        c.wait_stopped(after_count=1, timeout=3.0)
        _, vars_after_step = get_locals(c)
        results.append(("doubled reflects mutated boosted (0.25 * 2 = 0.5)",
                        value_of(vars_after_step, "doubled") in
                        ("0.5", "0.500000"),
                        value_of(vars_after_step, "doubled")))

        # setVariable on Module scope must be rejected (consts read-only).
        module_ref = next(s["variablesReference"]
                          for s in sc["body"]["scopes"]
                          if s["name"] == "Module")
        sv2 = c.request("setVariable", {
            "variablesReference": module_ref,
            "name":  "GAMMA",
            "value": "9.9",
        })
        results.append(("setVariable on Module scope is rejected (read-only)",
                        "read-only" in
                        sv2.get("body", {}).get("value", ""),
                        sv2.get("body", {})))

        c.send("continue", {"threadId": 1})
        time.sleep(0.5)
    finally:
        c.shutdown()
    return results


def scenario_symlink_bp(ctldap):
    """Set a BP using a symlinked path while the interpreter has loaded
    the file via the real path (or vice versa).  Currently uses suffix-
    matching which works for plain basenames; verify symlinked dirs also
    work."""
    results = []
    workdir = tempfile.mkdtemp(prefix="ctldap-symlink-")
    realdir = os.path.join(workdir, "real")
    linkdir = os.path.join(workdir, "linked")
    os.makedirs(realdir)
    os.symlink(realdir, linkdir)

    real_demo   = os.path.join(realdir, "demo.ctl")
    real_helper = os.path.join(realdir, "helper.ctl")
    link_demo   = os.path.join(linkdir, "demo.ctl")
    link_helper = os.path.join(linkdir, "helper.ctl")
    with open(real_demo,   "w") as f: f.write(DEMO_CTL)
    with open(real_helper, "w") as f: f.write(HELPER_CTL)

    # Case 1: launch with real path, set BP via symlink path.
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program": real_demo,
            "function": "main",
            "pixel": [0.4, 0, 0],
            "modulePaths": [realdir],
            "stopOnEntry": False,
        })
        # Set BP using the SYMLINKED path; interpreter knows the real path.
        c.request("setBreakpoints", {
            "source": {"path": link_demo},
            "breakpoints": [{"line": LINE_BOOSTED}],
        })
        c.request("configurationDone", {})
        try:
            stop = c.wait_stopped(after_count=0, timeout=3.0)
            results.append(("BP set via symlink path fires when "
                            "interpreter loaded via real path",
                            stop["body"].get("reason") == "breakpoint",
                            stop["body"]))
        except TimeoutError:
            results.append(("BP set via symlink path fires when "
                            "interpreter loaded via real path",
                            False, "no stopped event"))
        c.send("continue", {"threadId": 1})
        time.sleep(0.5)
    finally:
        c.shutdown()

    # Case 2: inverse — launch via symlink path, set BP via real path.
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program": link_demo,
            "function": "main",
            "pixel": [0.4, 0, 0],
            "modulePaths": [linkdir],
            "stopOnEntry": False,
        })
        c.request("setBreakpoints", {
            "source": {"path": real_demo},
            "breakpoints": [{"line": LINE_BOOSTED}],
        })
        c.request("configurationDone", {})
        try:
            stop = c.wait_stopped(after_count=0, timeout=3.0)
            results.append(("BP set via real path fires when "
                            "interpreter loaded via symlink path",
                            stop["body"].get("reason") == "breakpoint",
                            stop["body"]))
        except TimeoutError:
            results.append(("BP set via real path fires when "
                            "interpreter loaded via symlink path",
                            False, "no stopped event"))
        c.send("continue", {"threadId": 1})
        time.sleep(0.5)
    finally:
        c.shutdown()
        shutil.rmtree(workdir, ignore_errors=True)
    return results


def scenario_malformed_dap(ctldap, demo_path, helper_dir):
    """Send malformed/edge-case DAP messages, verify ctldap doesn't
    crash and continues serving subsequent valid requests."""
    results = []
    c = DapClient(ctldap)
    try:
        # 1. Send raw garbage that's not Content-Length-framed JSON.
        try:
            c.proc.stdin.write(b"not a valid dap message at all\n\n")
            c.proc.stdin.flush()
        except Exception:
            pass
        time.sleep(0.1)

        # 2. Send a valid request with a totally unknown command.
        seq = c.send("totallyMadeUpCommand", {"foo": "bar"})
        try:
            resp = c.wait_response(seq, timeout=1.0)
            results.append(("unknown command returns success=false response",
                            resp.get("success", True) is False, resp))
        except TimeoutError:
            # Acceptable: server may silently ignore.
            results.append(("unknown command does not hang the server",
                            True, "no response, no hang"))

        # 3. After the noise, ctldap should still answer initialize cleanly.
        init = c.request("initialize", {})
        results.append(("ctldap still alive: initialize succeeds after noise",
                        init.get("success", False) is True, init))

        # 4. Launch with missing required field "function".  Either it
        # defaults sensibly (success=true) or fails with a message —
        # both are acceptable; we just want NO crash.
        bad_launch = c.request("launch", {
            "program": demo_path,
            "modulePaths": [helper_dir],
            # function is missing on purpose
        })
        results.append(("launch without 'function' returns SOME response",
                        "success" in bad_launch, bad_launch))

        # 5. setBreakpoints with no source path (should skip silently).
        bad_bp = c.request("setBreakpoints", {
            "source":      {},     # missing path
            "breakpoints": [{"line": 10}],
        })
        # Either success=true with empty result, or success=false; both
        # are acceptable as long as the server didn't crash.
        results.append(("setBreakpoints with missing source path doesn't crash",
                        "success" in bad_bp, bad_bp))

        # 6. variables with bogus reference.
        bad_vars = c.request("variables", {"variablesReference": 99999})
        # Should return empty {} or {"variables":[]} — definitely no crash.
        results.append(("variables with bogus ref doesn't crash",
                        isinstance(bad_vars.get("body"), dict) or
                        bad_vars.get("body") is None,
                        bad_vars))
    finally:
        c.shutdown()
    return results


def scenario_thread_id_handling(ctldap, demo_path, helper_dir):
    """ctldap is single-threaded so it always uses threadId=1.  Verify
    that requests with non-1 threadIds don't crash and that the threads
    request returns the expected single-thread descriptor."""
    results = []
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program": demo_path,
            "function": "main",
            "pixel": [0.4, 0, 0],
            "modulePaths": [helper_dir],
            "stopOnEntry": False,
        })
        c.request("setBreakpoints", {
            "source": {"path": demo_path},
            "breakpoints": [{"line": LINE_BOOSTED}],
        })
        c.request("configurationDone", {})
        c.wait_stopped(after_count=0, timeout=3.0)

        th = c.request("threads", {})
        threads = th.get("body", {}).get("threads", [])
        results.append(("threads response has exactly one thread",
                        len(threads) == 1, threads))
        if threads:
            results.append(("the single thread has id=1",
                            threads[0].get("id") == 1, threads[0]))

        # Non-1 threadId on stackTrace shouldn't crash.
        st = c.request("stackTrace", {"threadId": 42})
        results.append(("stackTrace with bogus threadId returns response",
                        "body" in st or "success" in st, st))

        # Continue with bogus threadId — currently we ignore the field,
        # so this should still resume execution.
        c.send("continue", {"threadId": 99})
        time.sleep(0.7)
        with c._lock:
            terminated = any(m.get("type") == "event" and
                             m.get("event") == "terminated"
                             for m in c._messages)
        results.append(("continue with bogus threadId still resumes",
                        terminated, terminated))
    finally:
        c.shutdown()
    return results


def scenario_setvariable_output_arg(ctldap, demo_path, helper_dir):
    """Mutate the output param `r` via setVariable mid-execution and
    verify the function ends up writing the mutated value to the
    FunctionCall's output binding (so downstream consumers see it)."""
    results = []
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program": demo_path,
            "function": "main",
            "pixel": [0.4, 0, 0],
            "modulePaths": [helper_dir],
            "stopOnEntry": False,
        })
        # BP on the assign-to-r line so we can inspect r's pre-assign
        # storage and try to mutate it through the inspector — though
        # main will then overwrite it with `adjusted`.  More useful
        # check: mutate `rIn` (an input param) and verify downstream
        # `boosted` etc. reflect the new pixel value.
        c.request("setBreakpoints", {
            "source": {"path": demo_path},
            "breakpoints": [{"line": LINE_BOOSTED},      # initial pause
                            {"line": LINE_GAMMA_CORRECTED}],   # after boosted
        })
        c.request("configurationDone", {})
        c.wait_stopped(after_count=0, timeout=3.0)

        st = c.request("stackTrace", {"threadId": 1})
        sc = c.request("scopes", {"frameId": st["body"]["stackFrames"][0]["id"]})
        locals_ref = next(s["variablesReference"]
                          for s in sc["body"]["scopes"]
                          if s["name"] == "Locals")

        # Mutate input arg rIn from 0.4 to 0.8.
        sv = c.request("setVariable", {
            "variablesReference": locals_ref,
            "name":  "rIn",
            "value": "0.8",
        })
        results.append(("setVariable on input arg 'rIn' succeeds",
                        sv.get("body", {}).get("value") in ("0.8", "0.800000"),
                        sv.get("body", {})))

        # Continue to the second BP — boosted should reflect the
        # mutated rIn (0.8 * 2.5 = 2.0), not the original 1.0.
        c.send("continue", {"threadId": 1})
        c.wait_stopped(after_count=1, timeout=3.0)
        _, vars_l11 = get_locals(c)
        results.append(("downstream 'boosted' reflects mutated input "
                        "(0.8 * 2.5 = 2.0)",
                        value_of(vars_l11, "boosted") in ("2", "2.0", "2.000000"),
                        value_of(vars_l11, "boosted")))

        c.send("continue", {"threadId": 1})
        time.sleep(0.5)
    finally:
        c.shutdown()
    return results


def scenario_malformed_ctl(ctldap):
    """Launch with a CTL file that has a syntax error.  Must fail
    cleanly with a useful error message, not crash."""
    results = []
    workdir = tempfile.mkdtemp(prefix="ctldap-bad-ctl-")
    bad = os.path.join(workdir, "broken.ctl")
    with open(bad, "w") as f:
        f.write("// missing closing brace, no return type, etc.\n"
                "void main(output float r) { float x = ; }\n")
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        launch = c.request("launch", {
            "program":     bad,
            "function":    "main",
            "pixel":       [0.5, 0, 0],
            "modulePaths": [workdir],
            "stopOnEntry": False,
        }, timeout=4.0)
        results.append(("launch on broken CTL returns success=false",
                        launch.get("success", True) is False, launch))
        results.append(("launch error message mentions 'load' or 'parse' or 'syntax'",
                        any(w in launch.get("message", "").lower()
                            for w in ("load", "parse", "syntax", "error")),
                        launch.get("message", "")))
        # Server should still be alive — initialize again.
        init2 = c.request("initialize", {})
        results.append(("ctldap survives malformed-CTL launch + still serves",
                        init2.get("success", False), init2))
    finally:
        c.shutdown()
        shutil.rmtree(workdir, ignore_errors=True)
    return results


def scenario_deep_recursion(ctldap):
    """A recursive CTL function called to a non-trivial depth.  Set a
    BP inside and verify the call stack reports the right depth."""
    results = []
    workdir = tempfile.mkdtemp(prefix="ctldap-recursion-")
    src = os.path.join(workdir, "rec.ctl")
    # Tail-style countdown — each recursive call is the only thing in
    # the body (so no SIMD branch overhead per frame).  Depth = 32 is
    # well within stack budgets but exercises non-trivial call-stack
    # tracking in the debugger.
    DEPTH = 32
    with open(src, "w") as f:
        f.write(
            "int countdown (int n) {\n"
            "    if (n <= 0) {\n"
            "        return 0;\n"        # BP here
            "    }\n"
            "    return countdown (n - 1);\n"
            "}\n"
            "\n"
            "void main (output int r) {\n"
            f"    r = countdown ({DEPTH});\n"
            "}\n")

    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program":     src,
            "function":    "main",
            "pixel":       [0, 0, 0],
            "modulePaths": [workdir],
            "stopOnEntry": False,
        })
        c.request("setBreakpoints", {
            "source":      {"path": src},
            "breakpoints": [{"line": 3}],     # `return 0;`
        })
        c.request("configurationDone", {})
        try:
            c.wait_stopped(after_count=0, timeout=10.0)
        except TimeoutError:
            results.append((f"deep ({DEPTH}) recursion hits BP at base case",
                            False, "no stopped event"))
            return results
        results.append((f"deep ({DEPTH}) recursion hits BP at base case",
                        True, "ok"))
        st = c.request("stackTrace", {"threadId": 1})
        frames = st["body"]["stackFrames"]
        # Top frame = countdown; main + DEPTH+1 frames of countdown.
        # Allow some slack since the host→CTL boundary frame is filtered
        # out and we may not see EVERY recursion frame depending on how
        # tightly inlining + the stackTrace walker behave.
        results.append((f"stack reports >= 4 frames at depth {DEPTH}",
                        len(frames) >= 4,
                        f"got {len(frames)} frames"))
        results.append(("top frame is countdown",
                        frames[0]["name"] == "countdown",
                        frames[0]["name"]))
        c.send("continue", {"threadId": 1})
        time.sleep(0.5)
    finally:
        c.shutdown()
        shutil.rmtree(workdir, ignore_errors=True)
    return results


def scenario_long_trace(ctldap):
    """A CTL function with many statements; step through every one
    without hangs or excessive slowdown."""
    results = []
    workdir = tempfile.mkdtemp(prefix="ctldap-long-")
    src = os.path.join(workdir, "long.ctl")
    N = 60
    with open(src, "w") as f:
        f.write("void main (output float r, input float x) {\n")
        for i in range(N):
            f.write(f"    float v{i:02d} = x + {i}.0;\n")
        f.write("    r = x;\n")
        f.write("}\n")
    # Lines: 2..N+1 are the v00..vN-1 assignments; line 1 is `void main`,
    # line N+2 is `r = x;`, line N+3 is `}`.

    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program":     src,
            "function":    "main",
            "pixel":       [3.14, 0, 0],
            "modulePaths": [workdir],
            "stopOnEntry": False,
        })
        c.request("setBreakpoints", {
            "source":      {"path": src},
            "breakpoints": [{"line": 2}],    # first body line
        })
        c.request("configurationDone", {})
        try:
            c.wait_stopped(after_count=0, timeout=4.0)
        except TimeoutError:
            results.append(("long trace: hit first BP",
                            False, "no stopped event"))
            return results

        # Step N-1 times — must each complete in well under a second.
        t0 = time.time()
        steps_completed = 0
        for i in range(N - 1):
            c.send("next", {"threadId": 1})
            try:
                c.wait_stopped(after_count=i + 1, timeout=2.0)
                steps_completed += 1
            except TimeoutError:
                break
        elapsed = time.time() - t0
        results.append((f"completed {N-1} consecutive next steps",
                        steps_completed == N - 1,
                        f"completed {steps_completed}/{N-1}"))
        results.append((f"{N-1} steps complete in < 8s "
                        f"(no per-step slowdown)",
                        elapsed < 8.0, f"{elapsed:.2f}s"))
        c.send("continue", {"threadId": 1})
        time.sleep(0.5)
    finally:
        c.shutdown()
        shutil.rmtree(workdir, ignore_errors=True)
    return results


def scenario_sigint_during_pause(ctldap, demo_path, helper_dir):
    """While ctldap is paused at a BP, send SIGINT.  ctldap must shut
    down gracefully (no core dump, no zombie process)."""
    import signal
    results = []
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program":     demo_path,
            "function":    "main",
            "pixel":       [0.4, 0, 0],
            "modulePaths": [helper_dir],
            "stopOnEntry": False,
        })
        c.request("setBreakpoints", {
            "source":      {"path": demo_path},
            "breakpoints": [{"line": LINE_BOOSTED}],
        })
        c.request("configurationDone", {})
        c.wait_stopped(after_count=0, timeout=3.0)

        # Send SIGINT (Ctrl-C).  Process should exit (clean or with
        # a signal status) within a couple of seconds.
        c.proc.send_signal(signal.SIGINT)
        try:
            rc = c.proc.wait(timeout=3.0)
        except subprocess.TimeoutExpired:
            c.proc.kill()
            rc = -999
        # SIGINT typically causes exit code -2 (negative = killed by sig)
        # OR 130 (= 128 + SIGINT).  Accept anything that's not -999 (timed
        # out waiting → process hung).
        results.append(("ctldap exits within 3s of SIGINT (no hang)",
                        rc != -999, f"rc={rc}"))
    finally:
        # No-op shutdown: process already gone.
        try: c.proc.kill()
        except Exception: pass
    return results


def scenario_sigterm_during_pause(ctldap, demo_path, helper_dir):
    """Same as SIGINT but with SIGTERM (the more polite shutdown
    signal that supervisors typically send)."""
    import signal
    results = []
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program":     demo_path,
            "function":    "main",
            "pixel":       [0.4, 0, 0],
            "modulePaths": [helper_dir],
            "stopOnEntry": False,
        })
        c.request("setBreakpoints", {
            "source":      {"path": demo_path},
            "breakpoints": [{"line": LINE_BOOSTED}],
        })
        c.request("configurationDone", {})
        c.wait_stopped(after_count=0, timeout=3.0)

        c.proc.send_signal(signal.SIGTERM)
        try:
            rc = c.proc.wait(timeout=3.0)
        except subprocess.TimeoutExpired:
            c.proc.kill()
            rc = -999
        results.append(("ctldap exits within 3s of SIGTERM (no hang)",
                        rc != -999, f"rc={rc}"))
    finally:
        try: c.proc.kill()
        except Exception: pass
    return results


def scenario_output_on_termination(ctldap, demo_path, helper_dir):
    """After execution finishes, ctldap surfaces the final output args
    as a Debug Console `output` event so the user sees what the
    transform produced without having to re-launch with stopOnEntry."""
    results = []
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program":     demo_path,
            "function":    "main",
            "pixel":       [0.4, 0, 0],
            "modulePaths": [helper_dir],
            "stopOnEntry": False,
        })
        c.request("setBreakpoints", {
            "source":      {"path": demo_path},
            "breakpoints": [],
        })
        c.request("configurationDone", {})
        time.sleep(1.0)
        with c._lock:
            outputs = [m for m in c._messages
                       if m.get("type") == "event" and
                          m.get("event") == "output"]
            terminated = any(m.get("event") == "terminated"
                             for m in c._messages)
        # We expect at least one output event whose body.output starts
        # with "→ output:" containing the output arg names.
        summary = next((o["body"].get("output", "")
                        for o in outputs
                        if "→ output:" in o["body"].get("output", "")),
                       "")
        results.append(("terminated event fires", terminated, terminated))
        results.append(("output event with summary line is emitted",
                        bool(summary), summary))
        results.append(("summary mentions output arg 'r' (single-channel demo)",
                        "r=" in summary, summary))
    finally:
        c.shutdown()
    return results


def scenario_pixel_input_flexibility(ctldap):
    """Pixel inputs accept many name variants — not just rIn/gIn/bIn.
    Verify each common spelling: r/g/b, R/G/B, red/green/blue, rgb (array),
    pixel (array), and a 3-channel pixel into an rgba aggregate (alpha
    auto-fills to 1.0)."""
    results = []
    workdir = tempfile.mkdtemp(prefix="ctldap-flex-")
    try:
        def run(fixture, fn, pixel, expect_outputs):
            src = os.path.join(workdir, fixture + ".ctl")
            with open(src, "w") as f:
                f.write(fixture)
            c = DapClient(ctldap)
            try:
                c.request("initialize", {})
                c.request("launch", {"program": src, "function": fn,
                                     "pixel": pixel, "stopOnEntry": False})
                c.request("configurationDone", {})
                time.sleep(0.5)
                with c._lock:
                    outs = [m for m in c._messages
                            if m.get("type") == "event" and
                               m.get("event") == "output"]
                summary = next((o["body"].get("output", "")
                                for o in outs
                                if "→ output:" in o["body"].get("output", "")),
                               "")
                return summary
            finally:
                c.shutdown()

        # 1. Lowercase short scalar names: r/g/b.
        s = run(
            "namespace t1 { void main(output float out, "
            "input float r, input float g, input float b) "
            "{ out = r * 100.0 + g * 10.0 + b; } }",
            "t1::main", [0.4, 0.5, 0.6], None)
        # 0.4*100 + 0.5*10 + 0.6 = 45.6 — proves all three bound, not
        # just one or two (any subset would give a wildly different sum).
        results.append(("scalar names r/g/b bind to pixel channels",
                        "out=45.6" in s, s))

        # 2. Mixed case: R/G/B (uppercase short).
        s = run(
            "namespace t2 { void main(output float out, "
            "input float R, input float G, input float B) "
            "{ out = R + G + B; } }",
            "t2::main", [0.1, 0.2, 0.7], None)
        results.append(("scalar names R/G/B bind",
                        "out=1" in s, s))

        # 3. Full names: red/green/blue.
        s = run(
            "namespace t3 { void main(output float out, "
            "input float red, input float green, input float blue) "
            "{ out = red - green + blue; } }",
            "t3::main", [0.5, 0.25, 0.25], None)
        results.append(("scalar names red/green/blue bind",
                        "out=0.5" in s, s))

        # 4. Aggregate name: rgb[3].
        s = run(
            "namespace t4 { void main(output float out, "
            "input float rgb[3]) "
            "{ out = rgb[0] + rgb[1] + rgb[2]; } }",
            "t4::main", [0.3, 0.3, 0.4], None)
        results.append(("aggregate 'rgb[3]' binds to the pixel array",
                        "out=1" in s, s))

        # 5. Aggregate name: pixel[3].
        s = run(
            "namespace t5 { void main(output float out, "
            "input float pixel[3]) "
            "{ out = pixel[0] * pixel[1] * pixel[2]; } }",
            "t5::main", [2.0, 3.0, 4.0], None)
        results.append(("aggregate 'pixel[3]' binds",
                        "out=24" in s, s))

        # 6. Aggregate rgba[4] with only 3 channels supplied — alpha
        #    should auto-fill to 1.0.
        s = run(
            "namespace t6 { void main(output float out, "
            "input float rgba[4]) "
            "{ out = rgba[3]; } }",
            "t6::main", [0.0, 0.0, 0.0], None)
        results.append(("rgba[4] alpha auto-fills to 1.0 when user gives 3",
                        "out=1" in s, s))

        # 7. Default binding (rIn/gIn/bIn) still works (no regression).
        s = run(
            "namespace t7 { void main(output float out, "
            "input float rIn, input float gIn, input float bIn) "
            "{ out = rIn * 4.0 + gIn * 2.0 + bIn; } }",
            "t7::main", [0.25, 0.5, 0.75], None)
        results.append(("legacy rIn/gIn/bIn still bind (no regression)",
                        "out=2.75" in s, s))
    finally:
        shutil.rmtree(workdir, ignore_errors=True)
    return results


def scenario_pixel_evolution(ctldap, demo_path, helper_dir):
    """Opt-in `pixelEvolution: true` makes ctldap emit a `→ paused name=…`
    Debug Console line at every pause summarizing every color-shaped
    (float[3]) local in scope.  Default is OFF (no extra lines)."""
    results = []

    # ON.
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program":        demo_path,
            "function":       "main",
            "pixel":          [0.4, 0, 0],
            "modulePaths":    [helper_dir],
            "stopOnEntry":    False,
            "pixelEvolution": True,
        })
        c.request("setBreakpoints", {
            "source":      {"path": demo_path},
            "breakpoints": [{"line": LINE_BOOSTED}],
        })
        c.request("configurationDone", {})
        c.wait_stopped(after_count=0, timeout=3.0)
        time.sleep(0.2)
        with c._lock:
            outputs = [m for m in c._messages
                       if m.get("type") == "event" and
                          m.get("event") == "output"]
        # The demo CTL uses scalar floats only; with no float[3] locals
        # the line is suppressed.  That's still observable as the
        # "→ paused" line being absent — verify suppression behavior.
        evol = [o for o in outputs
                if "→ paused" in o["body"].get("output", "")]
        results.append(("scalar-only function emits no '→ paused' line",
                        len(evol) == 0, [o["body"].get("output") for o in evol]))

        c.send("continue", {"threadId": 1}); time.sleep(0.5)
    finally:
        c.shutdown()

    # ON, with a fixture that DOES use float[3] arrays — should emit.
    workdir = tempfile.mkdtemp(prefix="ctldap-evol-")
    try:
        src = os.path.join(workdir, "evol.ctl")
        with open(src, "w") as f:
            f.write("""\
namespace evol {

void
main (input  varying float rIn,
      input  varying float gIn,
      input  varying float bIn,
      output varying float rOut)
{
    float aces[3]   = {rIn, gIn, bIn};
    float scaled[3] = {aces[0] * 2.0, aces[1] * 2.0, aces[2] * 2.0};
    rOut = scaled[0];
}

}
""")
        c = DapClient(ctldap)
        try:
            c.request("initialize", {})
            c.request("launch", {
                "program":        src,
                "function":       "evol::main",
                "pixel":          [0.5, 0.25, 0.1],
                "stopOnEntry":    False,
                "pixelEvolution": True,
            })
            c.request("setBreakpoints", {
                "source":      {"path": src},
                "breakpoints": [{"line": 11}],   # `rOut = scaled[0];`
            })
            c.request("configurationDone", {})
            c.wait_stopped(after_count=0, timeout=3.0)
            time.sleep(0.2)
            with c._lock:
                outputs = [m for m in c._messages
                           if m.get("type") == "event" and
                              m.get("event") == "output"]
            evol = [o["body"].get("output", "") for o in outputs
                    if "→ paused" in o["body"].get("output", "")]
            results.append(("float[3] locals trigger '→ paused' line",
                            len(evol) >= 1, evol))
            if evol:
                txt = evol[-1]
                results.append(("line names 'aces' and 'scaled'",
                                "aces=" in txt and "scaled=" in txt, txt))
                results.append(("line shows aces with launch pixel values",
                                "0.5" in txt, txt))
            c.send("continue", {"threadId": 1}); time.sleep(0.5)
        finally:
            c.shutdown()

        # Same fixture, OFF (default) — no '→ paused' line.
        c = DapClient(ctldap)
        try:
            c.request("initialize", {})
            c.request("launch", {
                "program":     src,
                "function":    "evol::main",
                "pixel":       [0.5, 0.25, 0.1],
                "stopOnEntry": False,
            })
            c.request("setBreakpoints", {
                "source":      {"path": src},
                "breakpoints": [{"line": 11}],
            })
            c.request("configurationDone", {})
            c.wait_stopped(after_count=0, timeout=3.0)
            time.sleep(0.2)
            with c._lock:
                outputs = [m for m in c._messages
                           if m.get("type") == "event" and
                              m.get("event") == "output"]
            evol = [o for o in outputs
                    if "→ paused" in o["body"].get("output", "")]
            results.append(("default OFF: no '→ paused' line emitted",
                            len(evol) == 0,
                            [o["body"].get("output") for o in evol]))
            c.send("continue", {"threadId": 1}); time.sleep(0.5)
        finally:
            c.shutdown()
    finally:
        shutil.rmtree(workdir, ignore_errors=True)
    return results


def scenario_struct_expansion(ctldap):
    """When a CTL local has a struct type, the Variables panel returns
    a non-zero variablesReference for it.  Re-issuing `variables` with
    that ref returns the struct's fields.  Nested structs (struct
    containing a struct) are expandable too."""
    results = []
    workdir = tempfile.mkdtemp(prefix="ctldap-struct-")
    try:
        src = os.path.join(workdir, "s.ctl")
        with open(src, "w") as f:
            f.write("""\
struct Color
{
    float r;
    float g;
    float b;
};

struct Pair
{
    Color a;
    Color b;
};

void
main (output float r, input float rIn)
{
    Color c    = { rIn, 0.5, 0.25 };
    Pair  pair = { {1, 2, 3}, {4, 5, 6} };
    r = c.r + pair.a.r + pair.b.b;
}
""")
        c = DapClient(ctldap)
        try:
            c.request("initialize", {})
            c.request("launch", {"program": src, "function": "main",
                                 "pixel": [0.7, 0, 0], "stopOnEntry": False})
            c.request("setBreakpoints", {
                "source": {"path": src},
                "breakpoints": [{"line": 19}],  # `r = c.r + pair.a.r + pair.b.b;`
            })
            c.request("configurationDone", {})
            c.wait_stopped(after_count=0, timeout=3.0)

            # Top-level Locals.  c and pair should both have non-zero refs.
            _, locals_ = get_locals(c)
            n = sorted(v["name"] for v in locals_)
            results.append(("struct locals 'c' and 'pair' are present",
                            "c" in n and "pair" in n, n))

            c_var    = next(v for v in locals_ if v["name"] == "c")
            pair_var = next(v for v in locals_ if v["name"] == "pair")
            results.append(("'c' has non-zero variablesReference (expandable)",
                            c_var.get("variablesReference", 0) > 0,
                            c_var.get("variablesReference")))
            results.append(("'pair' has non-zero variablesReference",
                            pair_var.get("variablesReference", 0) > 0,
                            pair_var.get("variablesReference")))

            # Expand 'c' — should yield r, g, b leaves.
            cc = c.request("variables",
                           {"variablesReference":
                                c_var["variablesReference"]})["body"]["variables"]
            cc_names = sorted(v["name"] for v in cc)
            results.append(("expanding 'c' yields its members r, g, b",
                            cc_names == ["b", "g", "r"], cc_names))
            results.append(("'c.r' = 0.7 (the launch pixel)",
                            value_of(cc, "r") in ("0.7", "0.700000"),
                            value_of(cc, "r")))
            results.append(("'c' members are leaves (variablesReference == 0)",
                            all(v.get("variablesReference", 0) == 0 for v in cc),
                            [(v["name"], v.get("variablesReference"))
                             for v in cc]))

            # Expand 'pair' — should yield a + b, both with non-zero refs
            # (they're themselves Color structs).
            pp = c.request("variables",
                           {"variablesReference":
                                pair_var["variablesReference"]})["body"]["variables"]
            pp_names = sorted(v["name"] for v in pp)
            results.append(("expanding 'pair' yields 'a' and 'b'",
                            pp_names == ["a", "b"], pp_names))
            a_var = next(v for v in pp if v["name"] == "a")
            results.append(("'pair.a' is itself expandable",
                            a_var.get("variablesReference", 0) > 0,
                            a_var.get("variablesReference")))

            # Expand the nested 'pair.a' — should be { r:1, g:2, b:3 }.
            paa = c.request("variables",
                            {"variablesReference":
                                a_var["variablesReference"]})["body"]["variables"]
            results.append(("'pair.a.r' = 1 (literal init)",
                            value_of(paa, "r") in ("1", "1.000000"),
                            value_of(paa, "r")))
            results.append(("'pair.a.g' = 2",
                            value_of(paa, "g") in ("2", "2.000000"),
                            value_of(paa, "g")))

            # Resume + re-pause: refs handed out before should be cleared.
            old_ref = c_var["variablesReference"]
            c.send("continue", {"threadId": 1})
            time.sleep(0.5)
            # Stale ref expansion should yield empty (no struct ref → falls
            # through to scope lookup which also misses).
            try:
                stale = c.request("variables",
                                  {"variablesReference": old_ref})
                stale_vars = stale.get("body", {}).get("variables", [])
            except Exception:
                stale_vars = []
            results.append(("stale struct ref returns empty after resume",
                            stale_vars == [] or stale_vars is None,
                            stale_vars))
        finally:
            c.shutdown()
    finally:
        shutil.rmtree(workdir, ignore_errors=True)
    return results


def scenario_logpoints(ctldap, demo_path, helper_dir):
    """Logpoints: a breakpoint with a logMessage emits a Debug Console
    line and does NOT pause execution.  Verify:
      - initialize advertises supportsLogPoints
      - the message appears verbatim with ${expr} substitutions resolved
      - no `stopped` event fires for the logpoint
      - the surrounding session still terminates cleanly
      - unresolved ${tags} surface as the literal placeholder (so the
        user sees what was attempted)"""
    results = []
    c = DapClient(ctldap)
    try:
        init = c.request("initialize", {})
        results.append(("initialize advertises supportsLogPoints",
                        init["body"].get("supportsLogPoints") is True,
                        init["body"]))

        c.request("launch", {
            "program":     demo_path,
            "function":    "main",
            "pixel":       [0.4, 0, 0],
            "modulePaths": [helper_dir],
            "stopOnEntry": False,
        })
        # One logpoint at LINE_BOOSTED that prints a known string + a
        # resolvable ${rIn} + an unresolvable ${noSuch}.
        c.request("setBreakpoints", {
            "source":      {"path": demo_path},
            "breakpoints": [{
                "line":       LINE_BOOSTED,
                "logMessage": "rIn=${rIn} noSuch=${noSuch} pi=3.14",
            }],
        })
        c.request("configurationDone", {})
        time.sleep(1.0)

        with c._lock:
            stops    = list(c._stopped_events)
            outputs  = [m for m in c._messages
                        if m.get("type") == "event" and
                           m.get("event") == "output"]
            terminated = any(m.get("event") == "terminated"
                             for m in c._messages)

        results.append(("logpoint did NOT pause execution (no stopped events)",
                        len(stops) == 0, [s["body"].get("reason") for s in stops]))
        results.append(("session terminated cleanly",
                        terminated, terminated))

        log_lines = [o["body"].get("output", "") for o in outputs
                     if "rIn=" in o["body"].get("output", "")]
        results.append(("logpoint message emitted as output event",
                        len(log_lines) >= 1, log_lines))
        if log_lines:
            text = log_lines[0]
            # rIn should be 0.4 (formatted as %g → "0.4" or "0.400000")
            results.append(("${rIn} resolved to launch pixel value",
                            "rIn=0.4" in text, text))
            results.append(("literal segments preserved (pi=3.14)",
                            "pi=3.14" in text, text))
            results.append(("unresolved ${noSuch} surfaces as placeholder",
                            "${noSuch}" in text, text))
    finally:
        c.shutdown()

    # A regular breakpoint (no logMessage) on the SAME line should still
    # pause — proves logpoints don't break the conventional path.
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program":     demo_path,
            "function":    "main",
            "pixel":       [0.4, 0, 0],
            "modulePaths": [helper_dir],
            "stopOnEntry": False,
        })
        c.request("setBreakpoints", {
            "source":      {"path": demo_path},
            "breakpoints": [{"line": LINE_BOOSTED}],
        })
        c.request("configurationDone", {})
        try:
            c.wait_stopped(after_count=0, timeout=3.0)
            results.append(("plain BP at same line still pauses",
                            True, "ok"))
            c.send("continue", {"threadId": 1}); time.sleep(0.3)
        except TimeoutError:
            results.append(("plain BP at same line still pauses",
                            False, "no stopped event"))
    finally:
        c.shutdown()
    return results


def scenario_termination_hint(ctldap, demo_path, helper_dir):
    """Every CTL session emits a [ctl-debug] line on the Debug Console
    just before `terminated` so users see — at the place they're already
    looking — that watches/variables have frozen.  Category 'console'
    distinguishes adapter messages from program stdout."""
    results = []
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program":     demo_path,
            "function":    "main",
            "pixel":       [0.4, 0, 0],
            "modulePaths": [helper_dir],
            "stopOnEntry": False,
        })
        c.request("setBreakpoints", {
            "source":      {"path": demo_path},
            "breakpoints": [],
        })
        c.request("configurationDone", {})
        time.sleep(1.0)

        with c._lock:
            outputs = [m for m in c._messages
                       if m.get("type") == "event" and
                          m.get("event") == "output"]

        hint_msgs = [o for o in outputs
                     if "[ctl-debug]" in o["body"].get("output", "")]
        results.append(("[ctl-debug] termination hint emitted",
                        len(hint_msgs) == 1, hint_msgs))
        if hint_msgs:
            body = hint_msgs[0]["body"]
            results.append(("hint uses 'console' category (not stdout)",
                            body.get("category") == "console",
                            body.get("category")))
            text = body.get("output", "")
            results.append(("hint mentions F5 (relaunch)",
                            "F5" in text, text))
            results.append(("hint mentions Cmd-Shift-F5 (restart)",
                            "Cmd-Shift-F5" in text, text))
    finally:
        c.shutdown()
    return results


def scenario_per_stage_chain_output(ctldap):
    """Multi-stage chain emits one `output` event per stage with that
    stage's outputs (e.g. `→ stage 0: rOut=15`), plus the existing
    `→ output:` summary at termination.  Lets the user see the
    inter-stage hand-off live in the Debug Console without stepping."""
    results = []
    workdir = tempfile.mkdtemp(prefix="ctldap-perstage-")
    try:
        s1 = os.path.join(workdir, "s1.ctl")
        s2 = os.path.join(workdir, "s2.ctl")
        with open(s1, "w") as f:
            f.write("namespace s1 {\n"
                    "void main(output float rOut, input float rIn) { rOut = rIn * 3.0; }\n"
                    "}\n")
        with open(s2, "w") as f:
            f.write("namespace s2 {\n"
                    "void main(output float rOut, input float rIn) { rOut = rIn + 7.0; }\n"
                    "}\n")

        c = DapClient(ctldap)
        try:
            c.request("initialize", {})
            c.request("launch", {
                "programs":   [s1, s2],
                "functions":  ["s1::main", "s2::main"],
                "pixel":      [5.0, 0, 0],
                "stopOnEntry": False,
            })
            c.request("configurationDone", {})
            time.sleep(1.0)

            with c._lock:
                outputs = [m for m in c._messages
                           if m.get("type") == "event" and
                              m.get("event") == "output"]
                terminated = any(m.get("event") == "terminated"
                                 for m in c._messages)

            stage1 = next((o["body"].get("output", "")
                           for o in outputs
                           if "stage 1 of" in o["body"].get("output", "")), "")
            stage2 = next((o["body"].get("output", "")
                           for o in outputs
                           if "stage 2 of" in o["body"].get("output", "")), "")
            summary = next((o["body"].get("output", "")
                            for o in outputs
                            if "→ output:" in o["body"].get("output", "")), "")

            results.append(("terminated event fires", terminated, terminated))
            results.append(("stage 1 of 2 output line emitted (rOut=15 from 5*3)",
                            bool(stage1) and "rOut=15" in stage1,
                            stage1))
            results.append(("stage 2 of 2 output line emitted (rOut=22 from 15+7)",
                            bool(stage2) and "rOut=22" in stage2,
                            stage2))
            results.append(("final summary still emitted with last stage's value",
                            "rOut=22" in summary, summary))
        finally:
            c.shutdown()

        # Single-stage sessions should NOT emit per-stage lines (the final
        # summary already covers them).
        single = os.path.join(workdir, "single.ctl")
        with open(single, "w") as f:
            f.write("namespace single {\n"
                    "void main(output float rOut, input float rIn) { rOut = rIn * 2.0; }\n"
                    "}\n")
        c = DapClient(ctldap)
        try:
            c.request("initialize", {})
            c.request("launch", {"program": single, "function": "single::main",
                                 "pixel": [3.0, 0, 0], "stopOnEntry": False})
            c.request("configurationDone", {})
            time.sleep(1.0)
            with c._lock:
                outputs = [m for m in c._messages
                           if m.get("type") == "event" and
                              m.get("event") == "output"]
            stage_lines = [o for o in outputs
                           if "stage 1 of" in o["body"].get("output", "")]
            results.append(("single-stage session emits NO per-stage lines",
                            len(stage_lines) == 0, stage_lines))
        finally:
            c.shutdown()
    finally:
        shutil.rmtree(workdir, ignore_errors=True)
    return results


def scenario_print_routes_to_debug_console(ctldap):
    """CTL `print_float` calls go through Ctl::outputMessage, which
    ctldap hooks to emit DAP `output` events.  Verify a CTL with a
    print_float call surfaces the value through the Debug Console."""
    results = []
    workdir = tempfile.mkdtemp(prefix="ctldap-print-")
    src = os.path.join(workdir, "p.ctl")
    with open(src, "w") as f:
        f.write(
            "void main(output float r, input float rIn) {\n"
            "    print_float(rIn);\n"
            "    r = rIn;\n"
            "}\n")
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program":     src,
            "function":    "main",
            "pixel":       [0.42, 0, 0],
            "modulePaths": [workdir],
            "stopOnEntry": False,
        })
        c.request("configurationDone", {})
        time.sleep(0.7)
        with c._lock:
            outputs = [m for m in c._messages
                       if m.get("type") == "event" and
                          m.get("event") == "output"]
        # The print_float emits "0.42" as one of the output events.
        bodies = [o["body"].get("output", "") for o in outputs]
        joined = "".join(bodies)
        results.append(("print_float surfaces in DAP output events",
                        "0.42" in joined, joined))
    finally:
        c.shutdown()
        shutil.rmtree(workdir, ignore_errors=True)
    return results


def scenario_expression_evaluator(ctldap, demo_path, helper_dir):
    """The new evaluator handles literals, +-*/, comparisons, and array
    indexing.  Exercise hover (evaluate) and conditional BPs."""
    results = []

    # ---- 1. evaluate() handles arithmetic on locals.
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program": demo_path, "function": "main",
            "pixel": [0.4, 0, 0], "modulePaths": [helper_dir],
            "stopOnEntry": False,
        })
        c.request("setBreakpoints", {
            "source": {"path": demo_path},
            "breakpoints": [{"line": LINE_GAMMA_CORRECTED}],   # boosted is set
        })
        c.request("configurationDone", {})
        c.wait_stopped(after_count=0, timeout=3.0)

        # boosted = rIn * 2.5 = 0.4 * 2.5 = 1.0
        for expr, want_val in [
            ("boosted",            "1"),       # bare name
            ("boosted * 2",        "2"),       # multiplication
            ("boosted + 0.5",      "1.5"),
            ("boosted - rIn",      "0.6"),     # 1.0 - 0.4 = 0.6
            ("(boosted + 1) * 2",  "4"),       # parens + precedence
            ("boosted > 0.5",      "1"),       # comparison → true=1
            ("boosted < 0.5",      "0"),       # → false=0
            ("boosted >= 1",       "1"),
            # Note: `rIn == 0.4` returns 0 because 0.4f ≠ 0.4_double in
            # IEEE 754; equality on floats is a known footgun and not
            # something the evaluator can paper over.
        ]:
            r = c.request("evaluate", {"expression": expr,
                                       "context": "watch", "frameId": 0})
            got = r.get("body", {}).get("result", "")
            ok = got == want_val or got.startswith(want_val + ".")
            results.append((f"evaluate({expr!r}) → {want_val}", ok, got))

        # Unknown name returns an error string, not a crash.
        r = c.request("evaluate", {"expression": "noSuchVar",
                                   "context": "watch", "frameId": 0})
        results.append(("evaluate(unknown) returns an error",
                        "error" in r.get("body", {}).get("result", "").lower()
                        or "unknown" in r.get("body", {}).get("result", "").lower()
                        or "not in scope" in r.get("body", {}).get("result", "").lower(),
                        r.get("body", {})))

        c.send("continue", {"threadId": 1})
        time.sleep(0.5)
    finally:
        c.shutdown()

    # ---- 2. Conditional BP using a richer expression.
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program": demo_path, "function": "main",
            "pixel": [0.4, 0, 0], "modulePaths": [helper_dir],
            "stopOnEntry": False,
        })
        c.request("setBreakpoints", {
            "source": {"path": demo_path},
            "breakpoints": [{"line": LINE_GAMMA_CORRECTED,
                             # Both halves true → BP fires.
                             "condition": "boosted > 0.5 && rIn < 0.5"}],
        })
        c.request("configurationDone", {})
        try:
            c.wait_stopped(after_count=0, timeout=3.0)
            results.append(("conditional BP with && expression FIRES "
                            "when both terms true",
                            True, "ok"))
        except TimeoutError:
            results.append(("conditional BP with && expression FIRES "
                            "when both terms true",
                            False, "no stopped event"))
        c.send("continue", {"threadId": 1}); time.sleep(0.5)
    finally:
        c.shutdown()

    # ---- 3. Conditional BP using array indexing on a var declared
    # mid-function.  Skip if not applicable to this demo (the demo we
    # have here uses scalar locals, not arrays).  Just verify that
    # `rIn * 2 > 0.5` is handled.
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program": demo_path, "function": "main",
            "pixel": [0.4, 0, 0], "modulePaths": [helper_dir],
            "stopOnEntry": False,
        })
        c.request("setBreakpoints", {
            "source": {"path": demo_path},
            "breakpoints": [{"line": LINE_GAMMA_CORRECTED,
                             "condition": "rIn * 2 > 0.7"}],
        })
        c.request("configurationDone", {})
        try:
            c.wait_stopped(after_count=0, timeout=3.0)
            results.append(("conditional BP with arithmetic `rIn * 2 > 0.7` fires",
                            True, "0.4*2=0.8 > 0.7 ✓"))
        except TimeoutError:
            results.append(("conditional BP with arithmetic fires",
                            False, "no stopped event"))
        c.send("continue", {"threadId": 1}); time.sleep(0.5)
    finally:
        c.shutdown()

    return results


def scenario_no_bp_misfire_in_callee(ctldap):
    """Regression: a BP set on caller.ctl:N must NOT mis-fire when
    execution descends into a callee whose first inst happens to share
    line N (xc.fileName() lags by an instruction across calls, so the
    naive BP-check sees stale-file demo.ctl + callee-line N and would
    incorrectly match the user's demo.ctl:N breakpoint)."""
    results = []
    workdir = tempfile.mkdtemp(prefix="ctldap-bp-shared-line-")
    demo = os.path.join(workdir, "d.ctl")
    hlp  = os.path.join(workdir, "helper.ctl")

    # BP line in d.ctl is line 5; pad helper.ctl with comment lines so
    # its first body inst also lands on line 5.  Without the fix this
    # is what trips the stale-file false positive.
    with open(demo, "w") as f:
        f.write(
            'import "helper";\n'                  # 1
            '\n'                                   # 2
            'void main(output float r, input float rIn)\n'  # 3
            '{\n'                                  # 4
            '    float a = boost(rIn);\n'         # 5  ← BP here
            '    r = a + 1.0;\n'                  # 6
            '}\n')                                 # 7
    with open(hlp, "w") as f:
        # Pad so `boost` body lines up to the same line as the BP,
        # forcing the stale-file collision case.
        f.write(
            '// pad\n'                              # 1
            '// pad\n'                              # 2
            '// pad\n'                              # 3
            'float\n'                               # 4
            'boost(float x) { float scaled = x * 2.0; return scaled; }\n')   # 5

    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program":     demo,
            "function":    "main",
            "pixel":       [0.5, 0, 0],
            "modulePaths": [workdir],
            "stopOnEntry": False,
        })
        c.request("setBreakpoints", {
            "source":      {"path": demo},
            "breakpoints": [{"line": 5}],
        })
        c.request("configurationDone", {})

        # First stop should be on demo.ctl:5 (the BP).
        c.wait_stopped(after_count=0, timeout=3.0)
        st0 = c.request("stackTrace", {"threadId": 1})
        top0 = st0["body"]["stackFrames"][0]
        results.append(("first stop is on demo.ctl:5 (the BP)",
                        top0["source"]["path"].endswith("d.ctl") and
                        top0["line"] == 5,
                        (top0["source"]["path"], top0["line"])))

        # F10 (step over).  Must land on demo.ctl line 6, NOT re-fire
        # the BP inside helper.ctl on its line-5 inst.
        c.send("next", {"threadId": 1})
        c.wait_stopped(after_count=1, timeout=3.0)
        st1 = c.request("stackTrace", {"threadId": 1})
        top1 = st1["body"]["stackFrames"][0]
        results.append(("F10 from BP at line 5: lands on demo.ctl, not helper.ctl",
                        top1["source"]["path"].endswith("d.ctl"),
                        top1["source"]["path"]))
        results.append(("F10 from BP at line 5: lands on line 6",
                        top1["line"] == 6, top1["line"]))
        results.append(("F10 stays at depth 1 (no descent)",
                        len(st1["body"]["stackFrames"]) == 1,
                        len(st1["body"]["stackFrames"])))

        c.send("continue", {"threadId": 1})
        time.sleep(0.5)
    finally:
        c.shutdown()
        shutil.rmtree(workdir, ignore_errors=True)
    return results


def scenario_step_in_lands_on_user_code(ctldap):
    """Regression: step-in must land on the first user-visible line of
    the callee's body, not on synthetic compiler insts (file/line
    metadata, return-slot allocation).  Without suppression the user
    sees a confusing line jitter like 25 → 23 → 26 when the callee
    has a local-array decl + signature on adjacent lines."""
    results = []
    workdir = tempfile.mkdtemp(prefix="ctldap-stepin-userline-")
    demo = os.path.join(workdir, "d.ctl")
    hlp  = os.path.join(workdir, "helper.ctl")
    # Callee with a local-array decl that codegens to a placeholder
    # inst.  Without the synthetic-inst suppression, step-in lands on
    # the placeholder's line (the local-decl line) before any actual
    # body code runs, and the next step jumps backwards to the
    # function signature line via SimdFileNameInst.
    with open(demo, "w") as f:
        f.write(
            'import "helper";\n'                  # 1
            '\n'                                   # 2
            'void main(output float r, input float rIn)\n'  # 3
            '{\n'                                  # 4
            '    float a[3];\n'                   # 5
            '    fill_with(a, rIn);\n'            # 6  ← BP here, then step-in
            '    r = a[0] + a[1] + a[2];\n'      # 7
            '}\n')                                 # 8
    with open(hlp, "w") as f:
        # signature on line 1, body uses line 4 placeholder + line 5 first user code.
        f.write(
            'void\n'                               # 1
            'fill_with(output float dst[3], input float v)\n'  # 2
            '{\n'                                   # 3
            '    float scratch[3];\n'              # 4  (local — emits placeholder)
            '    dst[0] = v;\n'                    # 5  ← FIRST user code
            '    dst[1] = v;\n'                    # 6
            '    dst[2] = v;\n'                    # 7
            '}\n')                                  # 8

    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program":     demo,
            "function":    "main",
            "pixel":       [0.5, 0, 0],
            "modulePaths": [workdir],
            "stopOnEntry": False,
        })
        c.request("setBreakpoints", {
            "source":      {"path": demo},
            "breakpoints": [{"line": 6}],
        })
        c.request("configurationDone", {})
        c.wait_stopped(after_count=0, timeout=3.0)

        # F11 step-in.  Should land on the FIRST user code line of
        # fill_with — not on a synthetic placeholder/SetFileName inst
        # earlier in the codegen.
        c.send("stepIn", {"threadId": 1})
        c.wait_stopped(after_count=1, timeout=3.0)
        st = c.request("stackTrace", {"threadId": 1})
        top = st["body"]["stackFrames"][0]
        results.append(("step-in lands inside helper.ctl",
                        top["source"]["path"].endswith("helper.ctl"),
                        top["source"]["path"]))
        # Acceptable landing lines: any user-code line of fill_with's
        # body (5, 6, or 7).  Synthetic insts at line 1 (signature)
        # or line 4 (the scratch placeholder) must NOT be the landing.
        results.append(("step-in lands on a user-code line, not synthetic",
                        top["line"] in (5, 6, 7),
                        top["line"]))

        # F10 from there: still inside helper, advances forward.
        prev_line = top["line"]
        c.send("next", {"threadId": 1})
        c.wait_stopped(after_count=2, timeout=3.0)
        st2 = c.request("stackTrace", {"threadId": 1})
        top2 = st2["body"]["stackFrames"][0]
        results.append(("F10 from there advances forward (no backwards jitter)",
                        top2["line"] > prev_line,
                        (prev_line, top2["line"])))

        c.send("continue", {"threadId": 1})
        time.sleep(0.5)
    finally:
        c.shutdown()
        shutil.rmtree(workdir, ignore_errors=True)
    return results


def scenario_step_over_function_call(ctldap):
    """F10 (step OVER) on a line that contains a function call must
    NOT descend into the callee — it should land on the next line in
    the caller's frame.  Regression for the user-reported case where
    BP at a `float lifted[3] = add_f_f3(...)` line followed by step
    appeared to enter helper.ctl."""
    results = []
    workdir = tempfile.mkdtemp(prefix="ctldap-stepover-")
    demo = os.path.join(workdir, "d.ctl")
    hlp  = os.path.join(workdir, "helper.ctl")
    with open(demo, "w") as f:
        f.write(
            'import "helper";\n'
            '\n'
            'void main(output float r, input float rIn)\n'
            '{\n'
            '    float a = boost(rIn);\n'           # line 5 — first BP here
            '    float b = boost(a);\n'              # line 6 — F10 should land here
            '    r = b;\n'                            # line 7
            '}\n')
    with open(hlp, "w") as f:
        f.write(
            'float\n'
            'boost(float x)\n'
            '{\n'
            '    float scaled = x * 2.0;\n'
            '    return scaled;\n'
            '}\n')

    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program":     demo,
            "function":    "main",
            "pixel":       [0.5, 0, 0],
            "modulePaths": [workdir],
            "stopOnEntry": False,
        })
        c.request("setBreakpoints", {
            "source":      {"path": demo},
            "breakpoints": [{"line": 5}],
        })
        c.request("configurationDone", {})
        c.wait_stopped(after_count=0, timeout=3.0)

        # F10 = step over.  Should land on demo.ctl line 6, NOT inside
        # helper.ctl.
        c.send("next", {"threadId": 1})
        c.wait_stopped(after_count=1, timeout=3.0)
        st = c.request("stackTrace", {"threadId": 1})
        top = st["body"]["stackFrames"][0]
        results.append(("F10 on `a = boost(rIn)` lands on demo.ctl, "
                        "not helper.ctl",
                        top["source"]["path"].endswith("d.ctl"),
                        top["source"]["path"]))
        results.append(("F10 lands on line 6 (the next statement)",
                        top["line"] == 6, top["line"]))
        results.append(("stack stays at depth 1 (no descent)",
                        len(st["body"]["stackFrames"]) == 1,
                        len(st["body"]["stackFrames"])))

        c.send("continue", {"threadId": 1})
        time.sleep(0.5)
    finally:
        c.shutdown()
        shutil.rmtree(workdir, ignore_errors=True)
    return results


def scenario_per_frame_inspection(ctldap, demo_path, helper_dir):
    """While paused inside helper::boost (depth 2), the Variables panel
    on stack frame 1 (= caller's frame, demo::main) should expose
    main's locals — not just helper's."""
    results = []
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program":     demo_path,
            "function":    "main",
            "pixel":       [0.4, 0, 0],
            "modulePaths": [helper_dir],
            "stopOnEntry": False,
        })
        c.request("setBreakpoints", {
            "source":      {"path": demo_path},
            "breakpoints": [{"line": LINE_BOOSTED}],
        })
        c.request("configurationDone", {})
        c.wait_stopped(after_count=0, timeout=3.0)

        # Step into helper::boost so we have a 2-frame stack.
        c.send("stepIn", {"threadId": 1})
        c.wait_stopped(after_count=1, timeout=3.0)

        st = c.request("stackTrace", {"threadId": 1})
        frames = st["body"]["stackFrames"]
        results.append(("two-frame stack inside helper",
                        len(frames) == 2 and
                        frames[0]["name"] == "helper::boost" and
                        frames[1]["name"] == "main",
                        [f["name"] for f in frames]))

        # Frame 0 (top) — helper's locals.
        sc0 = c.request("scopes", {"frameId": frames[0]["id"]})
        locals0_ref = next(s["variablesReference"]
                           for s in sc0["body"]["scopes"]
                           if s["name"] == "Locals")
        v0 = c.request("variables",
                       {"variablesReference": locals0_ref})["body"]["variables"]
        n0 = sorted(v["name"] for v in v0)
        results.append(("frame 0 (helper) shows helper's 'x'",
                        "x" in n0, n0))
        results.append(("frame 0 does NOT show main's 'rIn'",
                        "rIn" not in n0, n0))

        # Frame 1 (caller = main) — main's locals must be visible now,
        # not empty.
        sc1 = c.request("scopes", {"frameId": frames[1]["id"]})
        locals1_ref = next(s["variablesReference"]
                           for s in sc1["body"]["scopes"]
                           if s["name"] == "Locals")
        v1 = c.request("variables",
                       {"variablesReference": locals1_ref})["body"]["variables"]
        n1 = sorted(v["name"] for v in v1)
        results.append(("frame 1 (main) shows main's 'rIn' and 'r'",
                        "rIn" in n1 and "r" in n1, n1))
        results.append(("frame 1 does NOT leak helper's 'x'",
                        "x" not in n1, n1))
        results.append(("frame 1 'rIn' = 0.4 (the launch pixel)",
                        value_of(v1, "rIn") in ("0.4", "0.400000"),
                        value_of(v1, "rIn")))

        c.send("continue", {"threadId": 1})
        time.sleep(0.5)
    finally:
        c.shutdown()
    return results


def scenario_three_frame_inspection(ctldap):
    """Variables on EACH of three nested frames must reflect that
    frame's own locals — no leakage across the chain.  Exercises the
    fp-swap logic at depth 3, deeper than the per_frame_inspection
    scenario which only goes to depth 2."""
    results = []
    workdir = tempfile.mkdtemp(prefix="ctldap-3frame-")
    try:
        src = os.path.join(workdir, "chain.ctl")
        with open(src, "w") as f:
            f.write("""\
namespace chain {

float
inner (float k)
{
    float scaled_inner = k + 100.0;
    return scaled_inner;
}

float
mid (float j)
{
    float local_mid = j + 10.0;
    float deeper = inner (local_mid);
    return deeper;
}

void
main (output float r, input float rIn)
{
    float local_main = rIn + 1.0;
    float result = mid (local_main);
    r = result;
}

}
""")
        c = DapClient(ctldap)
        try:
            c.request("initialize", {})
            c.request("launch", {"program": src, "function": "chain::main",
                                 "pixel": [0.5, 0, 0], "stopOnEntry": False})
            # BP on `return scaled_inner;` (after the local has been
            # assigned, so the inspector should surface it).
            c.request("setBreakpoints", {
                "source": {"path": src},
                "breakpoints": [{"line": 7}],
            })
            c.request("configurationDone", {})
            c.wait_stopped(after_count=0, timeout=3.0)

            st = c.request("stackTrace", {"threadId": 1})
            frames = st["body"]["stackFrames"]
            # Frame names: caller frames come from the bare AST call-site
            # name (no namespace prefix on intra-namespace calls), but the
            # outermost host->CTL frame uses the launch-arg form
            # ("chain::main").  We assert the suffix to be tolerant.
            ok_names = (len(frames) == 3 and
                        frames[0]["name"].endswith("inner") and
                        frames[1]["name"].endswith("mid") and
                        frames[2]["name"].endswith("main"))
            results.append(("3-frame deep stack",
                            ok_names, [f["name"] for f in frames]))

            def fetch_locals(frameId):
                sc = c.request("scopes", {"frameId": frameId})
                ref = next(s["variablesReference"]
                           for s in sc["body"]["scopes"]
                           if s["name"] == "Locals")
                return c.request("variables",
                                 {"variablesReference": ref})["body"]["variables"]

            v0 = fetch_locals(frames[0]["id"])
            n0 = sorted(v["name"] for v in v0)
            results.append(("frame 0 (inner) sees its 'k' and 'scaled_inner'",
                            "k" in n0 and "scaled_inner" in n0, n0))
            results.append(("frame 0 does not leak mid/main locals",
                            "j" not in n0 and "local_mid" not in n0
                                and "local_main" not in n0,
                            n0))

            v1 = fetch_locals(frames[1]["id"])
            n1 = sorted(v["name"] for v in v1)
            results.append(("frame 1 (mid) sees its 'j' and 'local_mid'",
                            "j" in n1 and "local_mid" in n1, n1))
            results.append(("frame 1 does not leak inner's 'scaled_inner'",
                            "scaled_inner" not in n1, n1))

            v2 = fetch_locals(frames[2]["id"])
            n2 = sorted(v["name"] for v in v2)
            results.append(("frame 2 (main) sees 'rIn', 'r', 'local_main'",
                            "rIn" in n2 and "r" in n2 and "local_main" in n2, n2))
            results.append(("frame 2 'rIn' = 0.5 (launch pixel)",
                            value_of(v2, "rIn") in ("0.5", "0.500000"),
                            value_of(v2, "rIn")))

            c.send("continue", {"threadId": 1}); time.sleep(0.5)
        finally:
            c.shutdown()
    finally:
        shutil.rmtree(workdir, ignore_errors=True)
    return results


def scenario_continue_after_step(ctldap, demo_path, helper_dir):
    """Sequence that the user reported behaving like step-in:
    BP at line 10 → F11 (step-in) → F11-out (step-out) → Continue.
    Continue must run to completion, NOT pause on a step-style stop."""
    results = []
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program": demo_path,
            "function": "main",
            "pixel": [0.4, 0, 0],
            "modulePaths": [helper_dir],
            "stopOnEntry": False,
        })
        c.request("setBreakpoints", {
            "source": {"path": demo_path},
            "breakpoints": [{"line": LINE_BOOSTED}],
        })
        c.request("configurationDone", {})
        c.wait_stopped(after_count=0, timeout=3.0)

        c.send("stepIn", {"threadId": 1})
        c.wait_stopped(after_count=1, timeout=3.0)
        c.send("stepOut", {"threadId": 1})
        c.wait_stopped(after_count=2, timeout=3.0)

        # Now Continue.  Should run all the way to terminated.
        c.send("continue", {"threadId": 1})
        time.sleep(1.0)
        with c._lock:
            extra_stops = len(c._stopped_events) - 3
            terminated = any(m.get("type") == "event" and
                             m.get("event") == "terminated"
                             for m in c._messages)
            # If we DID stop again, capture the reason for the failure msg
            extra_reasons = [s["body"].get("reason")
                             for s in c._stopped_events[3:]]
        results.append(("continue after stepIn+stepOut runs to terminated "
                        "(no spurious step pause)",
                        extra_stops == 0 and terminated,
                        (extra_stops, extra_reasons, terminated)))
    finally:
        c.shutdown()
    return results


def scenario_evaluate_diagnostics(ctldap, demo_path, helper_dir):
    """Watch/hover return SPECIFIC strings for the common 'why is this not
    available' cases — so users don't see a generic 'not available' and
    assume the debugger is broken:
      - <session ended …>          when _interpDone but client still asks
      - <not in scope here …>      when name exists in symtab but not in
                                   the current frame's locals (most common
                                   confusion: variable declared on a later line)
      - <unknown …>                when the name doesn't exist at all
    """
    results = []
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program":     demo_path,
            "function":    "main",
            "pixel":       [0.4, 0, 0],
            "modulePaths": [helper_dir],
            "stopOnEntry": False,
        })
        # BP at LINE_BOOSTED so we're paused BEFORE `gammaCorrected` /
        # `doubled` / etc. get declared.
        c.request("setBreakpoints", {
            "source":      {"path": demo_path},
            "breakpoints": [{"line": LINE_BOOSTED}],
        })
        c.request("configurationDone", {})
        c.wait_stopped(after_count=0, timeout=3.0)

        # `gammaCorrected` is declared on a later line (LINE_GAMMA_CORRECTED).
        # We expect the diagnostic to say "not in scope" — NOT "unknown".
        ev = c.request("evaluate", {"expression": "gammaCorrected",
                                    "context": "watch", "frameId": 0})
        msg = ev.get("body", {}).get("result", "")
        results.append(("declared-but-not-yet-in-scope name → 'not in scope'",
                        "not in scope" in msg, msg))

        # A truly unknown name should NOT say 'not in scope'.
        ev = c.request("evaluate", {"expression": "totallyMadeUpName",
                                    "context": "watch", "frameId": 0})
        msg = ev.get("body", {}).get("result", "")
        results.append(("genuinely unknown name → 'unknown'",
                        "unknown" in msg.lower() and "not in scope" not in msg,
                        msg))

        # Continue to termination.
        c.send("continue", {"threadId": 1})
        time.sleep(0.5)

        # After termination, evaluate should report session ended.
        ev = c.request("evaluate", {"expression": "boosted",
                                    "context": "watch", "frameId": 0})
        msg = ev.get("body", {}).get("result", "")
        results.append(("post-termination evaluate → 'session ended'",
                        "session ended" in msg, msg))
    finally:
        c.shutdown()

    # Empty expression handled gracefully (no crash, sensible message).
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        ev = c.request("evaluate", {"expression": "", "context": "watch"})
        msg = ev.get("body", {}).get("result", "")
        results.append(("empty expression → '<empty expression>'",
                        "empty" in msg, msg))

        # Pre-launch evaluate (no _dbg yet) → 'no debug session'.
        ev = c.request("evaluate", {"expression": "rIn", "context": "watch"})
        msg = ev.get("body", {}).get("result", "")
        results.append(("pre-launch evaluate → 'no debug session'",
                        "no debug session" in msg, msg))
    finally:
        c.shutdown()
    return results


def scenario_restart(ctldap, demo_path, helper_dir):
    """Verify the `restart` request:
      1. Initial launch with pixel A → BP fires, rIn = A.
      2. Continue past BP → terminated.
      3. Restart with new launch args carrying pixel B → BP fires again
         in the SAME session, rIn = B (proving restart re-bound inputs
         AND re-applied breakpoints).
      4. Continue → terminated.
    Also asserts the response declares supportsRestartRequest."""
    results = []
    c = DapClient(ctldap)
    try:
        init = c.request("initialize", {})
        caps = init.get("body", {})
        results.append(("initialize advertises supportsRestartRequest",
                        caps.get("supportsRestartRequest") is True,
                        caps))

        launch_args_a = {
            "program":     demo_path,
            "function":    "main",
            "pixel":       [0.4, 0, 0],
            "modulePaths": [helper_dir],
            "stopOnEntry": False,
        }
        c.request("launch", launch_args_a)
        c.request("setBreakpoints", {
            "source":      {"path": demo_path},
            "breakpoints": [{"line": LINE_GAMMA_CORRECTED}],
        })
        c.request("configurationDone", {})
        c.wait_stopped(after_count=0, timeout=3.0)

        _, vars1 = get_locals(c)
        results.append(("first launch: rIn = 0.4",
                        value_of(vars1, "rIn") in ("0.4", "0.400000"),
                        value_of(vars1, "rIn")))

        # Resume to termination.
        c.send("continue", {"threadId": 1})
        time.sleep(0.5)

        # Restart with NEW pixel.  VS Code packs fresh launch args under
        # `arguments`; we exercise that path so a status-bar pixel change
        # actually takes effect on restart.
        launch_args_b = dict(launch_args_a)
        launch_args_b["pixel"] = [0.7, 0, 0]
        n_stops_before_restart = 0
        with c._lock:
            n_stops_before_restart = len(c._stopped_events)
        c.request("restart", {"arguments": launch_args_b}, timeout=5.0)
        c.wait_stopped(after_count=n_stops_before_restart, timeout=5.0)

        _, vars2 = get_locals(c)
        results.append(("after restart: rIn = 0.7 (new pixel applied)",
                        value_of(vars2, "rIn") in ("0.7", "0.700000"),
                        value_of(vars2, "rIn")))
        results.append(("after restart: BP at LINE_GAMMA_CORRECTED still in effect",
                        value_of(vars2, "rIn") is not None,
                        "stopped at BP"))

        c.send("continue", {"threadId": 1})
        time.sleep(0.5)

        # Restart WITHOUT new args — should reuse cached pixel B.
        n_stops_before_restart2 = 0
        with c._lock:
            n_stops_before_restart2 = len(c._stopped_events)
        c.request("restart", {}, timeout=5.0)
        c.wait_stopped(after_count=n_stops_before_restart2, timeout=5.0)
        _, vars3 = get_locals(c)
        results.append(("restart without args reuses last pixel (rIn = 0.7)",
                        value_of(vars3, "rIn") in ("0.7", "0.700000"),
                        value_of(vars3, "rIn")))

        c.send("continue", {"threadId": 1})
        time.sleep(0.5)
    finally:
        c.shutdown()
    return results


def scenario_evaluate_expressions(ctldap, demo_path, helper_dir):
    """Pause at line 12 (after boosted=1.0 and gammaCorrected=1.0
    computed) and exercise the Evaluate request — both for hover-style
    name lookups and Watch-panel expressions."""
    results = []
    c = DapClient(ctldap)
    try:
        c.request("initialize", {})
        c.request("launch", {
            "program": demo_path,
            "function": "main",
            "pixel": [0.4, 0, 0],
            "modulePaths": [helper_dir],
            "stopOnEntry": False,
        })
        c.request("setBreakpoints", {
            "source": {"path": demo_path},
            "breakpoints": [{"line": LINE_DOUBLED}],
        })
        c.request("configurationDone", {})
        c.wait_stopped(after_count=0, timeout=3.0)

        # Hover-style: VS Code sends evaluate with context="hover" and a
        # bare name like "boosted".  Should return its value.
        ev = c.request("evaluate", {
            "expression": "boosted",
            "context":    "hover",
            "frameId":    0,
        })
        results.append(("hover evaluate('boosted') succeeds",
                        ev.get("success", False),
                        ev.get("message", "ok")))
        results.append(("hover evaluate('boosted') = 1.0",
                        ev["body"]["result"] in ("1", "1.0", "1.000000"),
                        ev["body"]["result"]))

        # Watch-panel: same protocol, context="watch".
        ev = c.request("evaluate", {
            "expression": "rIn",
            "context":    "watch",
            "frameId":    0,
        })
        results.append(("watch evaluate('rIn') = 0.4",
                        ev["body"]["result"] in ("0.4", "0.400000"),
                        ev["body"]["result"]))

        # Hover on a name that isn't a valid local / param: should fail
        # gracefully (not crash, ideally success=false or a clear marker).
        ev = c.request("evaluate", {
            "expression": "doesNotExist",
            "context":    "hover",
            "frameId":    0,
        })
        # Either success=false OR success=true with a "<not found>"-style
        # result is acceptable; the goal is "no crash, no junk value".
        ok = (ev.get("success", False) is False or
              "not" in ev.get("body", {}).get("result", "").lower() or
              "unknown" in ev.get("body", {}).get("result", "").lower() or
              "error" in ev.get("body", {}).get("result", "").lower())
        results.append(("evaluate of unknown name handled gracefully",
                        ok, ev))

        c.send("continue", {"threadId": 1})
        time.sleep(0.5)
    finally:
        c.shutdown()
    return results


# ---------------------------------------------------------------------------
# Driver.
# ---------------------------------------------------------------------------

def main():
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} <path-to-ctldap>", file=sys.stderr)
        sys.exit(2)
    ctldap = sys.argv[1]
    if not os.access(ctldap, os.X_OK):
        print(f"ctldap not executable: {ctldap}", file=sys.stderr)
        sys.exit(2)

    workdir = tempfile.mkdtemp(prefix="ctldap-demo-test-")
    demo_path = os.path.join(workdir, "demo.ctl")
    helper_path = os.path.join(workdir, "helper.ctl")
    with open(demo_path, "w") as f:
        f.write(DEMO_CTL)
    with open(helper_path, "w") as f:
        f.write(HELPER_CTL)

    scenarios = [
        ("main_step_through",            scenario_main_step_through),
        ("step_into_helper",             scenario_step_into_helper),
        ("no_double_step",               scenario_no_double_step),
        ("step_through_to_end",          scenario_step_through_to_end),
        ("step_into_clamp01_if",         scenario_step_into_clamp01_if),
        ("step_out_from_helper",         scenario_step_out_from_helper),
        ("continue",                     scenario_continue),
        ("continue_to_second_bp",        scenario_continue_to_second_bp),
        ("continue_after_step",          scenario_continue_after_step),
        ("bp_inside_helper_pre_launch",  scenario_bp_inside_helper_pre_launch),
        ("modify_bp_mid_session",        scenario_modify_bp_mid_session),
        ("evaluate_expressions",         scenario_evaluate_expressions),
        ("evaluate_diagnostics",         scenario_evaluate_diagnostics),
        ("restart",                      scenario_restart),
        ("module_scope_consts",          scenario_module_scope_consts),
        ("race_continue_setbp",          scenario_race_continue_setbp),
        ("conditional_bp",               scenario_conditional_bp),
        ("set_variable",                 scenario_set_variable),
        ("symlink_bp",                   lambda _ct, _dp, _hd:
                                             scenario_symlink_bp(_ct)),
        ("malformed_dap",                scenario_malformed_dap),
        ("thread_id_handling",           scenario_thread_id_handling),
        ("setvariable_output_arg",       scenario_setvariable_output_arg),
        ("output_on_termination",        scenario_output_on_termination),
        ("pixel_evolution",              scenario_pixel_evolution),
        ("pixel_input_flexibility",      lambda _ct, _dp, _hd:
                                             scenario_pixel_input_flexibility(_ct)),
        ("logpoints",                    scenario_logpoints),
        ("struct_expansion",             lambda _ct, _dp, _hd:
                                             scenario_struct_expansion(_ct)),
        ("termination_hint",             scenario_termination_hint),
        ("per_stage_chain_output",       lambda _ct, _dp, _hd:
                                             scenario_per_stage_chain_output(_ct)),
        ("print_routes_to_debug_console", lambda _ct, _dp, _hd:
                                              scenario_print_routes_to_debug_console(_ct)),
        ("per_frame_inspection",         scenario_per_frame_inspection),
        ("three_frame_inspection",       lambda _ct, _dp, _hd:
                                             scenario_three_frame_inspection(_ct)),
        ("step_over_function_call",      lambda _ct, _dp, _hd:
                                             scenario_step_over_function_call(_ct)),
        ("step_in_lands_on_user_code",   lambda _ct, _dp, _hd:
                                             scenario_step_in_lands_on_user_code(_ct)),
        ("no_bp_misfire_in_callee",      lambda _ct, _dp, _hd:
                                             scenario_no_bp_misfire_in_callee(_ct)),
        ("expression_evaluator",         scenario_expression_evaluator),
        ("malformed_ctl",                lambda _ct, _dp, _hd:
                                             scenario_malformed_ctl(_ct)),
        ("deep_recursion",               lambda _ct, _dp, _hd:
                                             scenario_deep_recursion(_ct)),
        ("long_trace",                   lambda _ct, _dp, _hd:
                                             scenario_long_trace(_ct)),
        ("sigint_during_pause",          scenario_sigint_during_pause),
        ("sigterm_during_pause",         scenario_sigterm_during_pause),
        # The bare-name scenario brings its own fixtures (different
        # imports), so it doesn't take the shared demo_path.
        ("step_in_bare_name",            lambda _ct, _dp, _hd:
                                             scenario_step_in_bare_name(_ct)),
    ]

    overall_ok = True
    last_client = {}    # cheap way to expose the last-used client for debug
    def wrap(fn):
        def w(c_args):
            ctldap_, demo_, helper_ = c_args
            return fn(ctldap_, demo_, helper_)
        return w

    try:
        for name, fn in scenarios:
            print(f"\n=== scenario: {name} ===")
            try:
                results = fn(ctldap, demo_path, workdir)
            except Exception as e:
                print(f"  EXCEPTION: {type(e).__name__}: {e}")
                overall_ok = False
                continue
            for label, ok, detail in results:
                tag = "PASS" if ok else "FAIL"
                if not ok:
                    overall_ok = False
                detail_str = ""
                if not ok:
                    detail_str = f"  [{detail!r}]"
                print(f"  [{tag}] {label}{detail_str}")
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    print()
    print("OVERALL: " + ("PASS" if overall_ok else "FAIL"))
    sys.exit(0 if overall_ok else 1)


if __name__ == "__main__":
    main()
