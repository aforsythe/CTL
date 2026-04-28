# Testing the CTL performance branches

`ship/cpu-perf` is the shared base.  Three feature branches sit as
**siblings on top of it**, each adding one orthogonal capability:

```
ship/cpu-perf                  ← shared base: CPU performance + interpreter tests
├─ ship/gpu-metal              ← + Apple Silicon Metal GPU backend
├─ ship/moduleTestFramework-v1 ← + YAML-driven module test framework
└─ ship/ctl-debugger           ← this branch: + ctldb (REPL) + ctldap (DAP server)
```

Each sibling is independently testable: pick the feature you want to
evaluate, check out that branch, build, run `ctest`.  No need to pick
up unrelated work.

---

## If you're on this branch (`ship/ctl-debugger`)

This branch adds a single-pixel debugger for CTL modules: a REPL CLI
(`ctldb`) and a Debug Adapter Protocol server (`ctldap`) consumed by
the [vscode-ctl-debug](https://github.com/aforsythe/vscode-ctl-debug)
extension.  The debugger is gated on `-DCTL_ENABLE_DEBUGGER=ON`; OFF
by default so normal builds pay zero cost.

### Build and test

```bash
git checkout ship/ctl-debugger
cmake -B build-dbg -DCMAKE_BUILD_TYPE=Debug -DCTL_ENABLE_DEBUGGER=ON
cmake --build build-dbg -j
ctest --test-dir build-dbg                            # should be green
```

The full ctest pulls in the cpu-perf interpreter tests (including
`testDebugger` inside `IlmCtlTest`).  To run only the debugger
end-to-end surface:

```bash
ctest --test-dir build-dbg -L 'ctldb|ctldap'          # 7 tests
```

### Try it interactively

```bash
# REPL session against a CTL file:
./build-dbg/ctldb/ctldb -ctl path/to/transform.ctl --break transform.ctl:42

# DAP server (the vscode extension launches this for you):
./build-dbg/ctldap/ctldap < dap-session.txt
```

The cpu-perf base is also active on this branch — the section below
applies if you also want to evaluate the CPU perf work.

---

## If you're on this branch (`ship/cpu-perf`)

This branch contains CPU-side performance work only: vectorized
stdlib transcendentals (Apple Accelerate or sleef), threaded tile
dispatch, file-level `-jobs`, host-native arch + thin LTO + hidden
visibility + profile-guided optimization, and a `-pixel` CLI mode
for single-pixel transforms.  No Metal GPU backend.

### Build and test

```bash
git clone <your-fork>/CTL.git && cd CTL
git checkout ship/cpu-perf
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
(cd build && ctest)                                   # should be green
```

### Run from the build tree — do not `make install`

Test against your own workloads using the binary in the build
directory directly:

```bash
./build/ctlrender/ctlrender -ctl <your.ctl> in.exr out.exr
```

`make install` is not necessary and is not recommended while you're
evaluating; it would place the binary in system directories
(`/usr/local/bin/...`) where it could shadow a production install.
Keep everything inside your checkout.

### Quick single-pixel check

```bash
./build/ctlrender/ctlrender -ctl <your.ctl> -pixel 0.5,0.25,0.1,1.0
./build/ctlrender/ctlrender -help pixel
```

### Opt-outs if you hit a problem

```bash
-DCTL_PGO=OFF                # disable profile-guided optimization
-DCTL_NATIVE_ARCH=OFF        # portable-arch build (older hardware)
-DCTL_LTO=OFF                # disable thin LTO
-DCTL_HIDDEN_VISIBILITY=OFF  # if you're building SHARED libs for an external ABI
-DCTL_USE_ACCELERATE=OFF -DCTL_USE_SLEEF=OFF   # scalar libm (bit-exact to pre-branch master)
```

### Reporting results

Please include:

```bash
/usr/bin/time -p ./build/ctlrender/ctlrender -ctl … in.exr out.exr
```

plus your hardware (CPU model), input image size + format, CTL
transform used, and the measured wall-time.  File an issue at
`<your-repo>/issues` with the numbers.

### Known limits

- Builds on every tier-1 target (Linux, Windows, macOS Intel, macOS
  Apple Silicon).
- Bit-exact against pre-branch master on the default build (no
  Accelerate / sleef).
- ≤1 ULP per transcendental on the vectorized path.
