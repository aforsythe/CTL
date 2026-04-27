#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright Contributors to the CTL project.
#
# Headless exercise of the ctldb (CLI) debugger.  Spawns ctldb,
# interactively pipes scripted REPL input, captures stdout, and
# asserts on what the user would see.  Equivalent in spirit to the
# ctldap headless harness in ../ctldap/tests/run_demo_scenarios.py.
#
# Usage: run_demo_scenarios.py <path-to-ctldb>

import os
import shutil
import subprocess
import sys
import tempfile
import threading


# Same fixtures as the ctldap harness — keeps line numbers identical
# so assertions can be cross-referenced.
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

HELPER_CTL = """\
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

LINE_BOOSTED         = 10
LINE_GAMMA_CORRECTED = 11
HELPER_LINE_BOOST    = 6     # `float scaled = x * 2.5;` in HELPER_CTL above


def run_ctldb(ctldb, demo, helper_dir, repl_input,
              extra_args=None, function="main", pixel="0.4,0,0",
              timeout=10.0):
    """Spawn ctldb with the given args, pipe `repl_input` to stdin,
    return (returncode, stdout, stderr).  All paths absolute."""
    args = [ctldb,
            "-ctl", demo,
            "--function", function,
            "--pixel", pixel,
            "--module-path", helper_dir]
    if extra_args:
        args.extend(extra_args)
    p = subprocess.Popen(args,
                         stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE)
    try:
        out, err = p.communicate(repl_input.encode(), timeout=timeout)
    except subprocess.TimeoutExpired:
        p.kill()
        out, err = p.communicate()
        return -1, out.decode(errors="replace"), err.decode(errors="replace")
    return p.returncode, out.decode(errors="replace"), err.decode(errors="replace")


# ---------------------------------------------------------------------------
# Scenarios.  Each returns a list of (label, ok, detail) tuples.
# ---------------------------------------------------------------------------

def scenario_break_print_continue(ctldb, demo, helper_dir):
    """Set a BP via --break, hit it, print rIn, continue to end."""
    repl = "print rIn\nprint boosted\ncontinue\n"
    rc, out, err = run_ctldb(
        ctldb, demo, helper_dir, repl,
        extra_args=["--break", "demo.ctl:" + str(LINE_BOOSTED)])
    results = []
    results.append(("ctldb exits cleanly (rc=0)",
                    rc == 0, (rc, err[:200])))
    results.append(("BP-set message appears",
                    "Breakpoint set at" in out, out[:200]))
    results.append(("hits BP at the boosted line",
                    f"demo.ctl:{LINE_BOOSTED}" in out, out[:300]))
    results.append(("print rIn shows 0.4",
                    "rIn = 0.4" in out, out[:400]))
    # boosted's decl line IS LINE_BOOSTED but the assign hasn't run
    # yet — ctldb reports "no variable named 'boosted' in scope".
    results.append(("print boosted at BP says not-in-scope",
                    "no variable named 'boosted'" in out, out[:600]))
    return results


def scenario_step_inside_function(ctldb, demo, helper_dir):
    """`step` in ctldb is step-IN — it descends into helper::boost.
    `next` (n) is step-over.  Use `next` to advance past line 10
    without descending, then verify boosted has its post-call value."""
    repl = ("next\n"
            "print boosted\n"
            "continue\n")
    rc, out, err = run_ctldb(
        ctldb, demo, helper_dir, repl,
        extra_args=["--break", "demo.ctl:" + str(LINE_BOOSTED)])
    results = []
    results.append(("ctldb exits cleanly", rc == 0,
                    (rc, err[:200])))
    # After `next`, the helper::boost call has executed → boosted = 1.0.
    results.append(("next shows boosted = 1 (post-call value)",
                    "boosted = 1" in out, out[:600]))
    return results


def scenario_finish_returns_to_main(ctldb, demo, helper_dir):
    """Set BP inside helper::boost via --break, hit it, `finish` to
    step OUT back to main, verify boosted is assigned."""
    repl = ("finish\n"
            "print boosted\n"
            "continue\n")
    rc, out, err = run_ctldb(
        ctldb, demo, helper_dir, repl,
        extra_args=["--break", f"helper.ctl:{HELPER_LINE_BOOST}"])
    results = []
    results.append(("ctldb exits cleanly", rc == 0, (rc, err[:200])))
    results.append(("hit BP inside helper::boost",
                    "helper::boost" in out, out[:600]))
    # finish should pop us back to main where boosted is assigned.
    results.append(("after finish, print boosted shows 1",
                    "boosted = 1" in out, out[:1500]))
    return results


def scenario_backtrace_inside_helper(ctldb, demo, helper_dir):
    """Set a BP inside helper::boost, hit it, backtrace, verify two
    frames."""
    repl = ("backtrace\n"
            "continue\n")
    rc, out, err = run_ctldb(
        ctldb, demo, helper_dir, repl,
        extra_args=["--break", f"helper.ctl:{HELPER_LINE_BOOST}"])
    results = []
    results.append(("ctldb exits cleanly", rc == 0, (rc, err[:200])))
    results.append(("backtrace shows helper::boost on top",
                    "helper::boost" in out, out[:600]))
    results.append(("backtrace shows main as caller frame",
                    "main" in out and "demo.ctl:" in out, out[:600]))
    return results


def scenario_info_bp_listing(ctldb, demo, helper_dir):
    """Set two BPs, ask for `info bp`, expect both listed."""
    repl = "info bp\ncontinue\n"
    rc, out, err = run_ctldb(
        ctldb, demo, helper_dir, repl,
        extra_args=["--break", "demo.ctl:" + str(LINE_BOOSTED),
                    "--break", "demo.ctl:" + str(LINE_GAMMA_CORRECTED)])
    results = []
    results.append(("ctldb exits cleanly", rc == 0, (rc, err[:200])))
    results.append((f"info bp lists demo.ctl:{LINE_BOOSTED}",
                    f"demo.ctl:{LINE_BOOSTED}" in out, out[:600]))
    results.append((f"info bp lists demo.ctl:{LINE_GAMMA_CORRECTED}",
                    f"demo.ctl:{LINE_GAMMA_CORRECTED}" in out, out[:600]))
    return results


def scenario_break_then_clear_mid_session(ctldb, demo, helper_dir):
    """At first BP, add another via REPL `break`, then `clear` the
    original, continue → should hit only the new BP."""
    repl = (f"break demo.ctl:{LINE_GAMMA_CORRECTED}\n"
            f"clear demo.ctl:{LINE_BOOSTED}\n"
            "info bp\n"
            "continue\n"
            "print boosted\n"
            "continue\n")
    rc, out, err = run_ctldb(
        ctldb, demo, helper_dir, repl,
        extra_args=["--break", f"demo.ctl:{LINE_BOOSTED}"])
    results = []
    results.append(("ctldb exits cleanly", rc == 0, (rc, err[:200])))

    # Slice out the `info bp` block — between the "cleared" line and the
    # next "* Stopped" line.  This isolates the output of `info bp` from
    # earlier "Breakpoint set at demo.ctl:10" noise.
    info_block = ""
    if "cleared" in out and "* Stopped" in out:
        a = out.index("cleared")
        b = out.index("* Stopped", a)
        info_block = out[a:b]

    results.append((f"after clear, `info bp` block omits line {LINE_BOOSTED}",
                    f"demo.ctl:{LINE_BOOSTED}" not in info_block,
                    info_block))
    results.append((f"after clear, `info bp` block includes line "
                    f"{LINE_GAMMA_CORRECTED}",
                    f"demo.ctl:{LINE_GAMMA_CORRECTED}" in info_block,
                    info_block))
    # When we hit the second BP, boosted has been assigned (1.0).
    results.append(("at second BP, boosted = 1",
                    "boosted = 1" in out, out[:1500]))
    return results


def scenario_print_undefined(ctldb, demo, helper_dir):
    """`print someBogusName` should report the variable isn't in scope,
    not crash."""
    repl = "print someBogusName\ncontinue\n"
    rc, out, err = run_ctldb(
        ctldb, demo, helper_dir, repl,
        extra_args=["--break", f"demo.ctl:{LINE_BOOSTED}"])
    results = []
    results.append(("ctldb exits cleanly with bogus print",
                    rc == 0, (rc, err[:200])))
    results.append(("print of unknown name produces a sensible message",
                    "not" in out.lower() or
                    "unknown" in out.lower() or
                    "no such" in out.lower() or
                    "scope" in out.lower(),
                    out[:600]))
    return results


def scenario_print_arithmetic(ctldb, demo, helper_dir):
    """`print` accepts arithmetic expressions, not just bare names —
    powered by the same shared ExprEval that ctldap uses.  Verify the
    full small grammar."""
    repl = ("next\n"                         # advance so boosted is in scope
            "print boosted * 2\n"
            "print boosted + rIn\n"
            "print boosted > 0.5\n"
            "print (boosted - rIn) * 10\n"
            "continue\n")
    rc, out, _ = run_ctldb(
        ctldb, demo, helper_dir, repl,
        extra_args=["--break", "demo.ctl:" + str(LINE_BOOSTED)])
    results = []
    results.append(("ctldb exits cleanly", rc == 0, rc))
    # boosted = rIn * 2.5 = 0.4 * 2.5 = 1.0
    results.append(("`print boosted * 2` evaluates to 2",
                    "boosted * 2 = 2" in out, out[:600]))
    results.append(("`print boosted + rIn` evaluates to 1.4",
                    "boosted + rIn = 1.4" in out, out[:800]))
    results.append(("`print boosted > 0.5` evaluates to 1 (true)",
                    "boosted > 0.5 = 1" in out, out[:1000]))
    # (1.0 - 0.4) * 10 = 6
    results.append(("`print (boosted - rIn) * 10` parens + precedence",
                    "(boosted - rIn) * 10 = 6" in out, out[:1200]))
    return results


def scenario_conditional_breakpoint(ctldb, demo, helper_dir):
    """`break ... if EXPR` only pauses when the condition evaluates true."""
    results = []

    # 1. Condition TRUE — should pause.
    repl = "print rIn\ncontinue\n"
    rc, out, _ = run_ctldb(
        ctldb, demo, helper_dir, repl,
        extra_args=["--break",
                    "demo.ctl:" + str(LINE_BOOSTED) + " if rIn > 0.2"])
    results.append(("BP with TRUE cond fires (rIn=0.4 > 0.2)",
                    f"Stopped at " in out and "rIn = 0.4" in out, out[:400]))

    # 2. Condition FALSE — should NOT pause; runs to end without prompt.
    rc, out, _ = run_ctldb(
        ctldb, demo, helper_dir, "",
        extra_args=["--break",
                    "demo.ctl:" + str(LINE_BOOSTED) + " if rIn > 10"])
    results.append(("BP with FALSE cond does NOT fire",
                    "Stopped at" not in out and "function returned" in out,
                    out[:400]))

    # 3. `info bp` shows the condition.
    repl = "info bp\ncontinue\n"
    rc, out, _ = run_ctldb(
        ctldb, demo, helper_dir, repl,
        extra_args=["--break",
                    "demo.ctl:" + str(LINE_BOOSTED) + " if rIn > 0"])
    results.append(("`info bp` shows the condition",
                    "if rIn > 0" in out, out[:600]))
    return results


def scenario_logpoint(ctldb, demo, helper_dir):
    """`break ... log MSG` LOGS the message + does NOT pause; ${expr}
    substitutions resolve via ExprEval."""
    repl = ""        # no REPL input — execution should run to end
    rc, out, _ = run_ctldb(
        ctldb, demo, helper_dir, repl,
        extra_args=["--break",
                    "demo.ctl:" + str(LINE_BOOSTED) +
                    " log \"hit rIn=${rIn} pi=3.14 missing=${nope}\""])
    results = []
    results.append(("ctldb exits cleanly", rc == 0, rc))
    results.append(("logpoint did NOT pause (no `Stopped at`)",
                    "Stopped at" not in out, out[:400]))
    results.append(("logpoint message printed with ${rIn} substitution",
                    "hit rIn=0.4" in out, out[:400]))
    results.append(("literal segments preserved (pi=3.14)",
                    "pi=3.14" in out, out[:400]))
    results.append(("unresolved ${tag} surfaces literally",
                    "${nope}" in out, out[:400]))
    results.append(("transform still ran to completion (function returned)",
                    "function returned" in out, out[:400]))
    return results


def scenario_per_stage_chain_output(ctldb, helper_dir):
    """Multi-stage chain prints `→ stage K of N: …` after each stage.
    Mirrors the ctldap Debug Console line so cross-tool output stays
    consistent."""
    workdir = tempfile.mkdtemp(prefix="ctldb-stage-")
    try:
        s1 = os.path.join(workdir, "s1.ctl")
        s2 = os.path.join(workdir, "s2.ctl")
        with open(s1, "w") as f:
            f.write("namespace s1 {\n"
                    "void main(output float rOut, input float rIn) "
                    "{ rOut = rIn * 3.0; }\n"
                    "}\n")
        with open(s2, "w") as f:
            f.write("namespace s2 {\n"
                    "void main(output float rOut, input float rIn) "
                    "{ rOut = rIn + 7.0; }\n"
                    "}\n")
        # run_ctldb only takes one demo path; build the args by hand.
        p = subprocess.Popen(
            [ctldb, "-ctl", s1, "-ctl", s2,
             "--function", "s1::main", "--function", "s2::main",
             "--pixel", "5,0,0"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE)
        out, _ = p.communicate(b"", timeout=10)
        out = out.decode()
        results = []
        results.append(("ctldb exits cleanly", p.returncode == 0, p.returncode))
        results.append(("stage 1 of 2 line printed (rOut=15)",
                        "→ stage 1 of 2: rOut=15" in out, out[:600]))
        results.append(("stage 2 of 2 line printed (rOut=22)",
                        "→ stage 2 of 2: rOut=22" in out, out[:600]))

        # Single-stage run should NOT emit a per-stage line (the trailing
        # "function returned" already shows the final value).
        single = os.path.join(workdir, "single.ctl")
        with open(single, "w") as f:
            f.write("namespace single {\n"
                    "void main(output float rOut, input float rIn) "
                    "{ rOut = rIn * 2.0; }\n"
                    "}\n")
        p = subprocess.Popen(
            [ctldb, "-ctl", single, "--function", "single::main",
             "--pixel", "3,0,0"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE)
        out2, _ = p.communicate(b"", timeout=10)
        out2 = out2.decode()
        results.append(("single-stage run emits NO per-stage line",
                        "→ stage" not in out2, out2[:400]))
    finally:
        shutil.rmtree(workdir, ignore_errors=True)
    return results


def scenario_pixel_evolution_flag(ctldb, demo, helper_dir):
    """`--pixel-evolution` prints color-shaped (float[3]/float[4])
    locals at every pause.  The default demo is scalar-only, so we
    write a small fixture with float[3] locals to actually trigger
    the line."""
    workdir = tempfile.mkdtemp(prefix="ctldb-evol-")
    try:
        src = os.path.join(workdir, "evol.ctl")
        with open(src, "w") as f:
            f.write("""\
namespace evol {

void
main(output float rOut, input float rIn, input float gIn, input float bIn)
{
    float aces[3]   = {rIn, gIn, bIn};
    float scaled[3] = {aces[0]*2.0, aces[1]*2.0, aces[2]*2.0};
    rOut = scaled[0];
}

}
""")
        # BP at the rOut assignment (line 8) — by then both arrays exist.
        p = subprocess.Popen(
            [ctldb, "-ctl", src, "--function", "evol::main",
             "--pixel", "0.5,0.25,0.1",
             "--break", "evol.ctl:8",
             "--pixel-evolution"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE)
        out, _ = p.communicate(b"continue\n", timeout=10)
        out = out.decode()
        results = []
        results.append(("ctldb exits cleanly", p.returncode == 0, p.returncode))
        results.append(("`→ paused` line emitted at pause",
                        "→ paused" in out, out[:600]))
        results.append(("line names 'aces' and 'scaled'",
                        "aces=" in out and "scaled=" in out, out[:600]))
        results.append(("aces shows launch-pixel values",
                        "0.5" in out, out[:600]))

        # Without --pixel-evolution, the default: NO `→ paused` line.
        p = subprocess.Popen(
            [ctldb, "-ctl", src, "--function", "evol::main",
             "--pixel", "0.5,0.25,0.1",
             "--break", "evol.ctl:8"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE)
        out2, _ = p.communicate(b"continue\n", timeout=10)
        out2 = out2.decode()
        results.append(("default (no flag) emits NO `→ paused` line",
                        "→ paused" not in out2, out2[:400]))
    finally:
        shutil.rmtree(workdir, ignore_errors=True)
    return results


def scenario_pixel_input_flexibility(ctldb):
    """Pixel binds to many name variants — not just rIn/gIn/bIn — via
    the shared Ctl::bindPixelInputs helper.  Same coverage as the
    ctldap scenario."""
    workdir = tempfile.mkdtemp(prefix="ctldb-flex-")
    results = []
    try:
        def run_one(fixture, fn, pixel, args=None):
            src = os.path.join(workdir, fn.replace("::", "_") + ".ctl")
            with open(src, "w") as f:
                f.write(fixture)
            full_args = [ctldb, "-ctl", src, "--function", fn,
                         "--pixel", pixel]
            if args: full_args.extend(args)
            p = subprocess.Popen(full_args,
                                 stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE)
            out, _ = p.communicate(b"", timeout=10)
            return out.decode()

        # 1. r/g/b lowercase scalars.
        out = run_one(
            "namespace t1 { void main(output float out, "
            "input float r, input float g, input float b) "
            "{ out = r * 100.0 + g * 10.0 + b; } }",
            "t1::main", "0.4,0.5,0.6")
        results.append(("scalar names r/g/b bind",
                        "out = 45.6" in out, out[:300]))

        # 2. RED/GREEN/BLUE.
        out = run_one(
            "namespace t2 { void main(output float out, "
            "input float red, input float green, input float blue) "
            "{ out = red + green + blue; } }",
            "t2::main", "0.1,0.2,0.7")
        results.append(("scalar names red/green/blue bind",
                        "out = 1" in out, out[:300]))

        # 3. Aggregate `pixel[3]`.
        out = run_one(
            "namespace t3 { void main(output float out, "
            "input float pixel[3]) "
            "{ out = pixel[0] * pixel[1] * pixel[2]; } }",
            "t3::main", "2,3,4")
        results.append(("aggregate `pixel[3]` binds",
                        "out = 24" in out, out[:300]))

        # 4. rgba[4] alpha auto-fill from 3-channel pixel.
        out = run_one(
            "namespace t4 { void main(output float out, "
            "input float rgba[4]) "
            "{ out = rgba[3]; } }",
            "t4::main", "0,0,0")
        results.append(("rgba[4] alpha auto-fills to 1.0 from 3 channels",
                        "out = 1" in out, out[:300]))

        # 5. Legacy rIn/gIn/bIn still works (no regression).
        out = run_one(
            "namespace t5 { void main(output float out, "
            "input float rIn, input float gIn, input float bIn) "
            "{ out = rIn * 4.0 + gIn * 2.0 + bIn; } }",
            "t5::main", "0.25,0.5,0.75")
        results.append(("legacy rIn/gIn/bIn still bind (no regression)",
                        "out = 2.75" in out, out[:300]))
    finally:
        shutil.rmtree(workdir, ignore_errors=True)
    return results


def scenario_param_uniform_binding(ctldb, helper_dir):
    """`--param NAME=VALUE` binds a uniform input by name."""
    workdir = tempfile.mkdtemp(prefix="ctldb-param-")
    try:
        src = os.path.join(workdir, "p.ctl")
        with open(src, "w") as f:
            f.write("namespace p {\n"
                    "void main(output float rOut, input float rIn, "
                    "input uniform float gain = 1.0) "
                    "{ rOut = rIn * gain; }\n"
                    "}\n")
        # Default (no --param) → gain=1.0 → rOut=0.4
        p = subprocess.Popen(
            [ctldb, "-ctl", src, "--function", "p::main",
             "--pixel", "0.4,0,0"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE)
        out_default, _ = p.communicate(b"", timeout=10)
        out_default = out_default.decode()

        # --param gain=3.5 → rOut=1.4
        p = subprocess.Popen(
            [ctldb, "-ctl", src, "--function", "p::main",
             "--pixel", "0.4,0,0",
             "--param", "gain=3.5"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE)
        out_overridden, _ = p.communicate(b"", timeout=10)
        out_overridden = out_overridden.decode()

        results = []
        results.append(("default param binds (gain=1.0 → rOut=0.4)",
                        "rOut = 0.4" in out_default, out_default[:300]))
        results.append(("--param overrides default (gain=3.5 → rOut=1.4)",
                        "rOut = 1.4" in out_overridden, out_overridden[:300]))
    finally:
        shutil.rmtree(workdir, ignore_errors=True)
    return results


def scenario_quit_immediately(ctldb, demo, helper_dir):
    """At the BP, quit — no continue.  ctldb should exit cleanly."""
    repl = "quit\n"
    rc, out, err = run_ctldb(
        ctldb, demo, helper_dir, repl,
        extra_args=["--break", f"demo.ctl:{LINE_BOOSTED}"])
    results = []
    results.append(("quit exits ctldb (rc=0 or 1, but not crash)",
                    rc in (0, 1), (rc, err[:200])))
    return results


def scenario_no_break_runs_to_end(ctldb, demo, helper_dir):
    """No BPs.  ctldb should run the function to completion and print
    the output args."""
    rc, out, err = run_ctldb(
        ctldb, demo, helper_dir, repl_input="")
    results = []
    results.append(("no-BP run exits cleanly", rc == 0, (rc, err[:200])))
    # Final r = ((1.0)^2.2 + 1.0*2.0) clamped + 0.05 = (1 + 2)→clamped=1 + 0.05 = 1.05
    results.append(("final output 'r' shows 1.05 (or similar)",
                    "r = 1.05" in out or "r = 1.050000" in out, out[:1000]))
    return results


# ---------------------------------------------------------------------------
# Driver.
# ---------------------------------------------------------------------------

def main():
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} <path-to-ctldb>", file=sys.stderr)
        sys.exit(2)
    ctldb = sys.argv[1]
    if not os.access(ctldb, os.X_OK):
        print(f"ctldb not executable: {ctldb}", file=sys.stderr)
        sys.exit(2)

    workdir = tempfile.mkdtemp(prefix="ctldb-demo-test-")
    demo = os.path.join(workdir, "demo.ctl")
    helper = os.path.join(workdir, "helper.ctl")
    with open(demo, "w")   as f: f.write(DEMO_CTL)
    with open(helper, "w") as f: f.write(HELPER_CTL)

    scenarios = [
        ("break_print_continue",         scenario_break_print_continue),
        ("step_inside_function",         scenario_step_inside_function),
        ("finish_returns_to_main",       scenario_finish_returns_to_main),
        ("backtrace_inside_helper",      scenario_backtrace_inside_helper),
        ("info_bp_listing",              scenario_info_bp_listing),
        ("break_then_clear_mid_session", scenario_break_then_clear_mid_session),
        ("print_undefined",              scenario_print_undefined),
        ("print_arithmetic",             scenario_print_arithmetic),
        ("conditional_breakpoint",       scenario_conditional_breakpoint),
        ("logpoint",                     scenario_logpoint),
        ("per_stage_chain_output",       lambda c, _d, hd:
                                             scenario_per_stage_chain_output(c, hd)),
        ("pixel_evolution_flag",         scenario_pixel_evolution_flag),
        ("pixel_input_flexibility",      lambda c, _d, _hd:
                                             scenario_pixel_input_flexibility(c)),
        ("param_uniform_binding",        lambda c, _d, hd:
                                             scenario_param_uniform_binding(c, hd)),
        ("quit_immediately",             scenario_quit_immediately),
        ("no_break_runs_to_end",         scenario_no_break_runs_to_end),
    ]

    overall_ok = True
    try:
        for name, fn in scenarios:
            print(f"\n=== scenario: {name} ===")
            try:
                results = fn(ctldb, demo, workdir)
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
