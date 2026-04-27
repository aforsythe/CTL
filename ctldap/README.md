# ctldap — DAP server for CTL

`ctldap` exposes the Phase 1 `Ctl::SimdDebugger` API over the Debug
Adapter Protocol so any DAP-speaking IDE (VS Code, Vim/Neovim,
JetBrains, Emacs) can drive a CTL debug session with the same
single-pixel semantics ctldb provides on the CLI.

## Build

ctldap is built only when CTL_ENABLE_DEBUGGER=ON.

    cmake -B build -DCTL_ENABLE_DEBUGGER=ON
    cmake --build build --target ctldap -j8

## Protocol

ctldap reads DAP messages on stdin (Content-Length-framed JSON) and
writes responses + events on stdout.  See the DAP spec at
https://microsoft.github.io/debug-adapter-protocol/ for the message
format.

### Supported requests

- `initialize`
- `launch` — see schema below.
- `setBreakpoints` — `{source: {path}, breakpoints: [{line}]}`
- `configurationDone`
- `threads`
- `stackTrace`
- `scopes`
- `variables`
- `continue` / `next` / `stepIn` / `stepOut`
- `evaluate` — single variable name only (v1)
- `disconnect`

### `launch` arguments

Single-stage form:

| field | type | default | required | description |
|---|---|---|---|---|
| `program` | string | — | yes | Path to the `.ctl` file to debug. |
| `function` | string | `"main"` | no | CTL function entrypoint (qualified, e.g. `ns::main`). |
| `pixel` | number[] | `[0.5, 0.5, 0.5]` | no | Pixel value bound to inputs `rIn`/`gIn`/`bIn`/`aIn`. |
| `stopOnEntry` | boolean | `false` | no | Pause on the first instruction. |
| `modulePaths` | string[] | `[]` | no | Directories prepended to the `import` search path (combined with `$CTL_MODULE_PATH` and per-file parent directories). |

Multi-stage chain form (alternative to `program`/`function`):

| field | type | default | required | description |
|---|---|---|---|---|
| `programs` | string[] | — | yes | Paths to `.ctl` files run in sequence. |
| `functions` | string[] | per-stage `"main"` | no | Function entrypoints per stage. Missing or shorter-than-`programs` entries default to `main`. |

Stage `N+1`'s inputs are bound from stage `N`'s outputs by exact name match, falling back to channel synonyms (`rOut`→`rIn`, `gOut`→`gIn`, `bOut`→`bIn`, `aOut`→`aIn`).

Minimal launch (single file, all defaults):

```json
{"command": "launch", "arguments": {"program": "/path/to/file.ctl"}}
```

Minimal chain (all stages use `main`):

```json
{"command": "launch", "arguments": {
    "programs": ["IDT.ctl", "RRT.ctl", "ODT.ctl"],
    "pixel":    [0.18, 0.18, 0.18]
}}
```

### Events emitted

- `initialized`
- `stopped` — `{reason: "entry"|"breakpoint"|"step", threadId, allThreadsStopped}`
- `terminated`

### Limitations (v1)

- Single-pixel only (N=1) — no batch/image debugging.
- No `exceptionInfo` / `setExceptionBreakpoints` (CTL exceptions don't
  carry rich info).

## Shared infrastructure (lib/IlmCtlDebug)

The expression evaluator (powering Watch panel, hover, conditional
breakpoints, and logpoint `${expr}` substitutions), the pixel-input
binder (per-channel scalar / aggregate-array name matching), and the
color-evolution formatter all live in `lib/IlmCtlDebug` and are
shared with the [ctldb](../ctldb/) CLI debugger.  Bug fixes and
feature additions to those helpers benefit both binaries automatically.

## VS Code integration

The `vscode-ctl/` extension wraps this server with gutter breakpoints,
variable hover, call-stack panels, and a status-bar pixel picker.  See
[`vscode-ctl/README.md`](../vscode-ctl/README.md) for the user-facing
flow and launch.json reference.

For a one-command end-to-end demo (builds ctldap, generates a temp
workspace, launches VS Code in Extension Development Host mode), run
`make demo` from the repo root.
