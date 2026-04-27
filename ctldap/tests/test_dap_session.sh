#!/bin/bash
# End-to-end DAP session test for ctldap.

set -e

CTLDAP="${CTLDAP:-./build-dbg-on/ctldap/ctldap}"
HERE="$(cd "$(dirname "$0")" && pwd)"
FIXTURE="${HERE}/run_dap_session.ctl"
EXPECTED="${HERE}/expected_dap_session.txt"

ACTUAL=$(mktemp)
trap "rm -f $ACTUAL" EXIT

python3 - "$CTLDAP" "$FIXTURE" "$ACTUAL" <<'PYEOF'
import subprocess, json, sys, threading, time

ctldap, fixture, actual_path = sys.argv[1:4]
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
                # Filter out the seq numbers (non-deterministic) and
                # source paths (host-dependent) for stable golden file.
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

send({"seq": 1, "type": "request", "command": "initialize", "arguments": {}})
time.sleep(0.05)
send({"seq": 2, "type": "request", "command": "launch",
      "arguments": {"program": fixture,
                    "function": "ctldap_test::main",
                    "pixel": [2.0, 0, 0]}})
time.sleep(0.05)
send({"seq": 3, "type": "request", "command": "setBreakpoints",
      "arguments": {"source": {"path": fixture},
                    "breakpoints": [{"line": 7}]}})
time.sleep(0.05)
send({"seq": 4, "type": "request", "command": "configurationDone", "arguments": {}})
time.sleep(0.5)   # wait for first stopped event

send({"seq": 5, "type": "request", "command": "stackTrace", "arguments": {"threadId": 1}})
time.sleep(0.05)
send({"seq": 6, "type": "request", "command": "scopes", "arguments": {"frameId": 0}})
time.sleep(0.05)
send({"seq": 7, "type": "request", "command": "variables", "arguments": {"variablesReference": 1}})
time.sleep(0.05)
send({"seq": 8, "type": "request", "command": "evaluate",
      "arguments": {"expression": "interim", "frameId": 0}})
time.sleep(0.05)
send({"seq": 9, "type": "request", "command": "continue", "arguments": {"threadId": 1}})
time.sleep(0.5)
send({"seq": 10, "type": "request", "command": "disconnect", "arguments": {}})
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

# Sort both sides before diffing: the `terminated` event and the `continue`
# response race (interpreter thread vs dispatch thread), so their relative
# arrival order is nondeterministic.  Sorting canonicalises that pair while
# still catching any content regressions.
SORTED_EXPECTED=$(mktemp)
SORTED_ACTUAL=$(mktemp)
trap "rm -f $ACTUAL $SORTED_EXPECTED $SORTED_ACTUAL" EXIT

sort "$EXPECTED" > "$SORTED_EXPECTED"
sort "$ACTUAL"   > "$SORTED_ACTUAL"

if ! diff -u "$SORTED_EXPECTED" "$SORTED_ACTUAL"; then
    echo "FAIL: ctldap session differs from expected"
    echo "--- actual (unsorted) ---"
    cat "$ACTUAL"
    exit 1
fi
echo "PASS"
