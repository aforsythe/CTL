#!/bin/bash
# End-to-end DAP chain session test for ctldap (two-stage pipeline).
# Stage1: rOut = rIn * 3   (input rIn=5 → rOut=15)
# Stage2: rOut = rIn + 7   (input rIn=15 → rOut=22)

set -e

CTLDAP="${CTLDAP:-./build-dbg-on/ctldap/ctldap}"
HERE="$(cd "$(dirname "$0")" && pwd)"
STAGE1="${HERE}/run_chain_stage1.ctl"
STAGE2="${HERE}/run_chain_stage2.ctl"
EXPECTED="${HERE}/expected_chain_session.txt"

ACTUAL=$(mktemp)
trap "rm -f $ACTUAL" EXIT

python3 - "$CTLDAP" "$STAGE1" "$STAGE2" "$ACTUAL" <<'PYEOF'
import subprocess, json, sys, threading, time

ctldap, stage1, stage2, actual_path = sys.argv[1:5]
out = open(actual_path, "w")

p = subprocess.Popen([ctldap], stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)

def send(msg):
    body = json.dumps(msg, sort_keys=True).encode()
    p.stdin.write(b"Content-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body)
    p.stdin.flush()

def reader():
    while True:
        line = p.stdout.readline()
        if not line:
            break
        if line.startswith(b"Content-Length:"):
            n = int(line.split(b":")[1].strip())
            p.stdout.readline()  # blank
            body = p.stdout.read(n)
            try:
                obj = json.loads(body)
                # Scrub non-deterministic fields for stable golden file.
                if "seq" in obj: obj["seq"] = "<seq>"
                if "request_seq" in obj: obj["request_seq"] = "<seq>"
                def scrub(o):
                    if isinstance(o, dict):
                        if "path" in o and isinstance(o["path"], str):
                            o["path"] = "<path>" if "/" in o["path"] else o["path"]
                        if "file" in o and isinstance(o["file"], str):
                            o["file"] = "<path>" if "/" in o["file"] else o["file"]
                        for k, v in o.items(): scrub(v)
                    elif isinstance(o, list):
                        for v in o: scrub(v)
                scrub(obj)
                out.write(json.dumps(obj, sort_keys=True) + "\n")
            except Exception as e:
                out.write(f"PARSE_ERROR: {e} body={body!r}\n")

t = threading.Thread(target=reader, daemon=True)
t.start()

# initialize + launch with two stages via programs/functions arrays
send({"seq": 1, "type": "request", "command": "initialize", "arguments": {}})
time.sleep(0.05)
send({"seq": 2, "type": "request", "command": "launch",
      "arguments": {
          "programs":  [stage1, stage2],
          "functions": ["chain_stage1::main", "chain_stage2::main"],
          "pixel": [5.0, 0.0, 0.0]
      }})
time.sleep(0.05)

# Set breakpoint at line 7 of stage1 (rOut = rIn * 3.0)
send({"seq": 3, "type": "request", "command": "setBreakpoints",
      "arguments": {"source": {"path": stage1},
                    "breakpoints": [{"line": 7}]}})
time.sleep(0.05)
# Set breakpoint at line 7 of stage2 (rOut = rIn + 7.0)
send({"seq": 4, "type": "request", "command": "setBreakpoints",
      "arguments": {"source": {"path": stage2},
                    "breakpoints": [{"line": 7}]}})
time.sleep(0.05)
send({"seq": 5, "type": "request", "command": "configurationDone", "arguments": {}})
time.sleep(0.5)   # wait for stopped at stage1 bp

# Resume through stage1 breakpoint
send({"seq": 6, "type": "request", "command": "continue", "arguments": {"threadId": 1}})
time.sleep(0.5)   # wait for stopped at stage2 bp

# Resume through stage2 breakpoint
send({"seq": 7, "type": "request", "command": "continue", "arguments": {"threadId": 1}})
time.sleep(0.5)   # wait for terminated

send({"seq": 8, "type": "request", "command": "disconnect", "arguments": {}})
time.sleep(0.2)
p.stdin.close()
p.wait(timeout=5)
out.close()
PYEOF

if [[ ! -f "$EXPECTED" ]]; then
    echo "First run — writing expected file:"
    sort "$ACTUAL" > "$EXPECTED"
    cat "$EXPECTED"
    echo "PASS (golden initialized)"
    exit 0
fi

# Sort both sides to canonicalise race between terminated + continue response.
SORTED_EXPECTED=$(mktemp)
SORTED_ACTUAL=$(mktemp)
trap "rm -f $ACTUAL $SORTED_EXPECTED $SORTED_ACTUAL" EXIT

sort "$EXPECTED" > "$SORTED_EXPECTED"
sort "$ACTUAL"   > "$SORTED_ACTUAL"

if ! diff -u "$SORTED_EXPECTED" "$SORTED_ACTUAL"; then
    echo "FAIL: ctldap chain session differs from expected"
    echo "--- actual (unsorted) ---"
    cat "$ACTUAL"
    exit 1
fi
echo "PASS"
