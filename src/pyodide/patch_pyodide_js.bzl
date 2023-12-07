"""
This file patches various details in pyodide.asm.js.

1. `pyodide.asm.js` exposes `_createPyodideModule` via assignment to
   `globalThis` because module workers in Firefox have < 1 year of support. This
   changes it to an es6 export. In the future this would better be accomplished
   by passing `-sEXPORT_ES6` to Emscripten's linker.

2. Pyodide needs to synchronously compile webassembly code with `new
   WebAssembly.Module()`. We replace these with calls to `newWasmModule` from
   `internal:unsafe-eval`. To be paranoid, `builtin_wrappers` adds an extra
   check to `newWasmModule` which checks that the call comes from
   `convertJsFunctionToWasm`. See comment about this in
   `internal/builtin_wrappers.js`.

3. Python uses the current time as a cache key to check whether a directory is
   modified. It needs modifying a directory to change the modification time of
   the directory. The disabled timers interfere with this. `monotonicDateNow` is
   a wrapper around `Date.now()` that always increments the returned time by at
   least one millisecond.

4. Various junk that is linked into Pyodide uses `addEventListener` in a way
   that throws errors so we dummy it out. This junk is unnecessary for us and
   wasting large amounts of initialization time, when we set up our own Pyodide
   build we'll get rid of it and this won't be necessary.

Most of these changes can be removed when we make our own Pyodide build, but the
replacements for `Date.now()` and `WebAssembly.Module` will most likely need to
stay.
"""

PRELUDE = """
import { newWasmModule, monotonicDateNow } from "pyodide-internal:builtin_wrappers";

function addEventListener(){}
"""

REPLACEMENTS = [
    ["var _createPyodideModule", PRELUDE + "export const _createPyodideModule"],
    ["globalThis._createPyodideModule = _createPyodideModule;", ""],
    ["new WebAssembly.Module", "newWasmModule"],
    ["Date.now", "monotonicDateNow"],
]

def _impl(ctx):
    args = ctx.actions.args()
    args.add_all(ctx.files.input)
    args.add(ctx.outputs.output)

    ctx.actions.expand_template(
        output = ctx.outputs.output,
        template = ctx.files.input[0],
        substitutions = dict(REPLACEMENTS),
    )

patch_pyodide_js = rule(
    implementation = _impl,
    attrs = {
        "input": attr.label(
            allow_single_file = True,
            mandatory = True,
        ),
        "output": attr.output(
            mandatory = True,
        ),
        "deps": attr.label_list(),
    },
)
