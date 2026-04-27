#!/bin/bash
# End-to-end session test for ctldb.  Uses a pre-set breakpoint, pipes a
# scripted REPL session into ctldb on a fixed CTL fixture, normalises
# absolute paths in the output, and diffs against expected_session.txt.
#
# Environment:
#   CTLDB   path to the ctldb binary (default: ./build-dbg/ctldb/ctldb
#           relative to the repo root, resolved below)

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"

# Resolve binary: CTLDB env var beats the default.
CTLDB="${CTLDB:-${REPO_ROOT}/build-dbg/ctldb/ctldb}"

if [ ! -x "$CTLDB" ]; then
    echo "SKIP: ctldb binary not found at $CTLDB"
    exit 0
fi

FIXTURE="${HERE}/run_session.ctl"
EXPECTED="${HERE}/expected_session.txt"
FIXTURE_DIR="${HERE}"

ACTUAL=$(mktemp /tmp/ctldb_session_XXXXXX.txt)
trap "rm -f '$ACTUAL'" EXIT

# Run ctldb with a pre-set breakpoint at line 13 of run_session.ctl.
# Pipe a scripted REPL session:
#   print rIn       — inspect the input arg at the breakpoint (main)
#   step            — step into helper()
#   backtrace       — show the call stack inside helper
#   finish          — step out of helper back to main
#   print postHelper — inspect the local declared in main (now initialized)
#   continue        — run to completion
#   quit            — exit (belt-and-suspenders after the function returns)
printf 'print rIn\nstep\nbacktrace\nfinish\nprint postHelper\ncontinue\nquit\n' \
    | "$CTLDB" \
        -ctl "$FIXTURE" \
        --function ctldb_test::main \
        --pixel 2.0,0,0 \
        --break run_session.ctl:13 \
    | sed "s|${FIXTURE_DIR}/||g" \
    > "$ACTUAL"

if ! diff -u "$EXPECTED" "$ACTUAL"; then
    echo "FAIL: ctldb session output differs from expected"
    echo "--- actual ---"
    cat "$ACTUAL"
    exit 1
fi
echo "PASS: ctldb session matches expected transcript"
