#!/bin/bash
# End-to-end chain-session test for ctldb.
#
# Exercises a two-stage CTL chain:
#   Stage 1 (run_chain_a.ctl): rOut = rIn * 2.0   (with --pixel 5,0,0 → rOut=10)
#   Stage 2 (run_chain_b.ctl): rOut = rIn + 100.0  (rIn fed from stage 1 rOut → rOut=110)
#
# A breakpoint is set in each stage; we print rIn at each break to confirm
# the chaining:  stage-A sees rIn=5, stage-B sees rIn=10.
# Final output rOut must be 110.
#
# Environment:
#   CTLDB   path to the ctldb binary (default: ./build-dbg/ctldb/ctldb)

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"

CTLDB="${CTLDB:-${REPO_ROOT}/build-dbg/ctldb/ctldb}"

if [ ! -x "$CTLDB" ]; then
    echo "SKIP: ctldb binary not found at $CTLDB"
    exit 0
fi

CTL_A="${HERE}/run_chain_a.ctl"
CTL_B="${HERE}/run_chain_b.ctl"
EXPECTED="${HERE}/expected_chain_session.txt"

ACTUAL=$(mktemp /tmp/ctldb_chain_session_XXXXXX.txt)
trap "rm -f '$ACTUAL'" EXIT

# Session:
#   print rIn   — at stage-A breakpoint: expect 5 (from --pixel)
#   continue    — finish stage A, hit stage-B breakpoint
#   print rIn   — at stage-B breakpoint: expect 10 (chained from stage-A rOut)
#   continue    — finish stage B → prints final rOut=110
#   quit        — belt-and-suspenders
printf 'print rIn\ncontinue\nprint rIn\ncontinue\nquit\n' \
    | "$CTLDB" \
        -ctl "$CTL_A" --function ca::main \
        -ctl "$CTL_B" --function cb::main \
        --pixel 5,0,0 \
        --break run_chain_a.ctl:4 \
        --break run_chain_b.ctl:4 \
    | sed "s|${HERE}/||g" \
    > "$ACTUAL"

if ! diff -u "$EXPECTED" "$ACTUAL"; then
    echo "FAIL: ctldb chain session output differs from expected"
    echo "--- actual ---"
    cat "$ACTUAL"
    exit 1
fi
echo "PASS: ctldb chain session matches expected transcript"
