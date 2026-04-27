#!/bin/bash
# DapTransport robustness test: feed malformed framing in front of a valid
# initialize request and verify ctldap survives every kind of garbage and
# still responds to the valid frame.
#
# Exercises the four error paths in DapTransport::readMessage:
#   - bare line with no Content-Length header  (logged + skipped)
#   - Content-Length that won't parse          (defaults to 0 + skipped)
#   - valid Content-Length, garbage body       (json parse error + skipped)
#   - genuine EOF                              (clean shutdown)

set -e

CTLDAP="${CTLDAP:-./build-dbg-on/ctldap/ctldap}"

python3 - "$CTLDAP" <<'PYEOF'
import json, subprocess, sys

ctldap = sys.argv[1]
p = subprocess.Popen([ctldap], stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)

# 1. Bare line, no Content-Length header.
p.stdin.write(b"this is not a DAP frame\r\n\r\n")

# 2. Content-Length that won't parse as a number.
p.stdin.write(b"Content-Length: NOT_A_NUMBER\r\n\r\n")

# 3. Valid Content-Length followed by malformed JSON.
junk = b"this is not json"
p.stdin.write(b"Content-Length: " + str(len(junk)).encode() + b"\r\n\r\n" + junk)

# 4. Valid initialize after all that garbage. Must produce a response.
body = json.dumps({"seq": 99, "type": "request",
                   "command": "initialize", "arguments": {}}).encode()
p.stdin.write(b"Content-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body)
p.stdin.flush()

# Read response frames until we find the initialize response (seq 99).
# DAP servers may emit events (e.g. 'initialized') alongside responses.
def read_one():
    hdr = p.stdout.readline()
    if not hdr.startswith(b"Content-Length:"):
        return None
    n = int(hdr.split(b":")[1].strip())
    p.stdout.readline()    # blank line
    return json.loads(p.stdout.read(n))

found = None
for _ in range(8):
    msg = read_one()
    if msg is None: break
    if msg.get("type") == "response" and msg.get("request_seq") == 99:
        found = msg
        break

# Disconnect cleanly so ctldap exits and the test can clean up.
disc = json.dumps({"seq": 100, "type": "request",
                   "command": "disconnect", "arguments": {}}).encode()
p.stdin.write(b"Content-Length: " + str(len(disc)).encode() + b"\r\n\r\n" + disc)
p.stdin.close()
p.wait(timeout=5)

if found is None:
    print("FAIL: no response with request_seq=99 within 8 frames")
    sys.exit(1)
if found.get("command") != "initialize":
    print(f"FAIL: expected command=initialize, got {found.get('command')}")
    sys.exit(1)
if not found.get("success"):
    print(f"FAIL: initialize did not succeed: {found}")
    sys.exit(1)

print("PASS: DapTransport survived 3 malformed frames, responded to initialize")
PYEOF
