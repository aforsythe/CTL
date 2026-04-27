# ctldb — single-pixel CLI debugger for CTL

`ctldb` is a gdb-style command-line debugger for CTL modules.  Pass it a
single pixel value, set source-line breakpoints, and step through the CTL
function inspecting every variable in scope.

## Build

`ctldb` is built only when `CTL_ENABLE_DEBUGGER=ON`.  The dispatch hook adds
a per-instruction null-check to the SIMD interpreter's hot loop; the default
build (`CTL_ENABLE_DEBUGGER=OFF`) pays zero cost.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCTL_ENABLE_DEBUGGER=ON
cmake --build build --target ctldb -j8
```

The resulting binary is at `build/ctldb/ctldb`.

To run the end-to-end session test:

```bash
ctest --test-dir build -R "ctldb::session" --output-on-failure
```

## Usage

```
ctldb [options] -ctl <module.ctl> [-ctl <module.ctl> ...]

  --pixel r,g,b[,a]      pixel value (default 0.5,0.5,0.5)
  --break ARG            set a breakpoint before starting (repeatable):
                           <file>:<line>             plain BP
                           <file>:<line> if EXPR     conditional BP
                           <file>:<line> log MSG     LOGPOINT (no pause,
                                                     ${expr} substitution)
  --stop-at-entry        pause on the very first executed instruction
  --function <name>      CTL function to call (default: main; repeatable for
                         multi-stage chains in parallel with -ctl, see below)
  --param NAME=VALUE     bind a uniform input by name (repeatable)
  --module-path <dir>    additional directory for `import` resolution
                         (combined with $CTL_MODULE_PATH and per-file parents,
                         repeatable)
  --pixel-evolution      print color-shaped (float[3]/float[4]) locals at
                         every pause, before the prompt
  -h, --help             this text
```

`--function` is **optional**.  When omitted, every stage's entrypoint
defaults to `main` — the typical case.  Override only when your CTL
function has a different qualified name:

```sh
# Defaults: function=main
ctldb -ctl ODT.ctl --pixel 0.18,0.18,0.18

# Explicit override
ctldb -ctl ODT.ctl --function ODT::run --pixel 0.18,0.18,0.18
```

### Pixel input binding

The pixel binds to the first input arg whose name matches one of the
following (case-insensitive on the leaf name):

- **Per-channel scalars**: `rIn`/`gIn`/`bIn`/`aIn`, `r`/`g`/`b`/`a`,
  `R`/`G`/`B`/`A`, `red`/`green`/`blue`/`alpha`, plus `_in` suffixes
  (`r_in`, `redIn`, `input_r`, …).
- **Aggregate arrays**: `rgbIn[3]`, `rgbaIn[4]`, `rgb`, `rgba`,
  `pixel`, `color`, `pixela`, etc.  4-channel aggregates auto-fill
  alpha=1.0 when the user only supplies 3 channels.

Anything not matched stays at its declared CTL default (or zero
otherwise).  Same rules as the VS Code extension — both share
`Ctl::bindPixelInputs` from `lib/IlmCtlDebug`.

### Uniform parameters

Use `--param NAME=VALUE` (repeatable) to bind a uniform input by name,
typically the runtime knobs an ACES-style transform exposes:

```sh
ctldb -ctl my.ctl --pixel 0.18,0.18,0.18 \
      --param exposure=1.0 \
      --param gain=1.5
```

Any uniform left unset uses its CTL default if one exists.

### Multi-stage chains

Repeat `-ctl` (and optionally `--function`) to run multiple CTL files in
sequence.  The output pixel of each stage feeds the input of the next:

```sh
# Three-stage ACES pipeline; all entrypoints named `main`, so --function
# can be omitted entirely.
CTL_MODULE_PATH=$ACES_DEV/transforms/ctl/lib \
ctldb -ctl IDT.ctl -ctl RRT.ctl -ctl ODT.ctl \
      --pixel 0.18,0.18,0.18

# Same chain but stage 1 uses a non-`main` entrypoint:
ctldb -ctl IDT.ctl --function IDT::run \
      -ctl RRT.ctl \
      -ctl ODT.ctl \
      --pixel 0.18,0.18,0.18
```

`--function` arguments are positional with `-ctl`: the Nth `--function`
maps to the Nth `-ctl`.  Supplying fewer `--function` than `-ctl` (or
omitting `--function` entirely) defaults the remaining stages to `main`.

## REPL commands

| Command | Alias | Description |
|---|---|---|
| `continue` | `c` | Run until the next breakpoint |
| `next` | `n` | Step over — advance to the next source line in the same frame |
| `step` | `s` | Step in — advance to the next source line at any call depth |
| `finish` | `f` | Step out — run until the current frame returns |
| `break <file>:<line>` | `b` | Set a breakpoint (file matched by suffix) |
| `break <file>:<line> if EXPR` | | Conditional BP — pauses only when EXPR is non-zero |
| `break <file>:<line> log MSG` | | Logpoint — prints MSG (with `${expr}` substitution) without pausing |
| `clear <file>:<line>` | | Remove a breakpoint |
| `info bp` | | List all breakpoints (shows condition / log message if any) |
| `info stack` | | Show the current call stack |
| `print EXPR` | `p` | Print a variable, OR evaluate an expression in scope |
| `backtrace` | `bt` | Show the current call stack |
| `quit` | `q` | Exit ctldb |
| `help` | `h`, `?` | Print command summary |

`print` accepts the full small grammar: bare names, arithmetic
(`+ - * /`), comparisons (`== != < <= > >=`), logical combinators
(`&&`, `||`), array indexing (`name[i]`), and parenthesised
sub-expressions.  Same evaluator powers conditional breakpoints
and logpoint `${expr}` substitutions.

Breakpoint file matching is by suffix: typing `example.ctl:5` matches any
loaded file whose path ends in `example.ctl`.

## Example

Given this module:

```ctl
namespace ex {
void main(output float r, input float rIn) {
    float a = rIn * 2.0;
    r = a + 1.0;
}
}
```

Run with a breakpoint and inspect.  The function name is `ex::main`
because the CTL is in `namespace ex`; if it had been the bare top-level
`main`, `--function` could have been omitted entirely.

```
$ ctldb -ctl /tmp/example.ctl --function ex::main \
        --pixel 1.5,0,0 --break example.ctl:4
Breakpoint set at example.ctl:4
* Stopped at /tmp/example.ctl:4 in ex::main()
(ctldb) print rIn
rIn = 1.5
(ctldb) next
* function returned
  r = 4
(ctldb) quit
(quit)
```

Note: `rIn` is visible because it is a function input argument.  The local
variable `a` is NOT visible to `print` in v1 (see limitations below).

## VS Code integration

For an IDE experience with gutter breakpoints, hover-inspection,
call-stack panels, and a status-bar pixel picker, use the
[`vscode-ctl/`](../vscode-ctl/) extension on top of the
[`ctldap/`](../ctldap/) DAP server (both ship with this branch).
`make demo` from the repo root launches an end-to-end VS Code
session against pre-wired sample fixtures with no install required.

## What ctldb cannot do (v1)

- **Multi-pixel debugging.**  N=1 only.  For image-level bugs, narrow the
  failing pixel first with `ctlrender --pixel`, then load that value into
  `ctldb`.

- **Source listing.**  There is no `list` command.  Open the `.ctl` file in
  your editor alongside ctldb.

- **Edit-and-continue.**  Restart ctldb after any source change.

## Architecture

The implementation spans three layers:

1. **`lib/IlmCtlSimd/CtlSimdDebugger.h`** — the public `Ctl::SimdDebugger`
   pure-virtual interface with `beforeInst()`, `onCallEnter()`, and
   `onCallExit()` callbacks.  `DebugController` is a mutex/condvar
   pause-resume primitive for implementations that drive the interpreter from
   a separate thread (used by the `ctldap` DAP server).

2. **`lib/IlmCtlSimd/CtlSimdInspector.h`** — `Ctl::inspectVariables()`
   walks the interpreter's symbol table and returns each visible data symbol
   as an `InspectableVar` with its lane-0 pointer.

3. **`ctldb/`** — the CLI binary.
   - `Repl` implements `SimdDebugger`.  `beforeInst()` checks step state and
     breakpoints; when a stop is triggered it calls `enterRepl()`, which reads
     commands from stdin synchronously (on the interpreter thread — no
     separate coordination needed for the CLI path).
   - `ValueFormatter` pretty-prints any CTL value (bool, int, half, float,
     string, struct, array) to a string.
   - `main.cc` parses arguments, loads modules, binds pixel inputs, and runs
     the interpreter on a dedicated `std::thread` so the main thread can
     `join()` cleanly.

The hook in `SimdInst::executePath` is entirely preprocessor-gated:

```cpp
#ifdef CTL_ENABLE_DEBUGGER
    if (dbg) dbg->beforeInst(xcontext, inst, xcontext.callDepth());
#endif
```

so production builds compiled without `CTL_ENABLE_DEBUGGER` see no overhead
whatsoever — not even a branch.

The `ctldap` DAP server (built into the same `CTL_ENABLE_DEBUGGER=ON`
configuration as ctldb) and the `vscode-ctl` extension share the same
`SimdDebugger` + `inspectVariables` APIs without any changes to the
interpreter.
