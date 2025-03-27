load("@aspect_rules_esbuild//esbuild:defs.bzl", "esbuild")
load("@bazel_skylib//rules:copy_file.bzl", "copy_file")
load("@bazel_skylib//rules:expand_template.bzl", "expand_template")
load("@bazel_skylib//rules:write_file.bzl", "write_file")
load("//:build/capnp_embed.bzl", "capnp_embed")
load("//:build/js_file.bzl", "js_file")
load("//:build/wd_ts_bundle.bzl", "wd_ts_bundle")

def _out_name(src):
    src = src.removesuffix("//file")
    src = src.removeprefix("@")
    return src.rsplit(":", 2)[-1].rsplit("/", 2)[-1]

def copy_to_generated(src, out_name = None, name = None):
    out_name = out_name or _out_name(src)
    if name == None:
        name = out_name + "@copy"
    copy_file(name = name, src = src, out = "generated/" + out_name)

def copy_and_capnp_embed(src, out_name = None):
    out_name = out_name or _out_name(src)
    copy_to_generated(src, out_name = out_name)
    file = "generated/" + out_name
    capnp_embed(
        name = out_name + "@capnp",
        src = file,
        deps = [out_name + "@copy"],
    )

def python_bundle():
    copy_to_generated("@pyodide//:pyodide/pyodide.asm.wasm")

    copy_to_generated("@pyodide//:pyodide/python_stdlib.zip")

    # pyodide.asm.js patches
    # TODO: all of these should be fixed by linking our own Pyodide or by upstreaming.

    PRELUDE = """
    import { newWasmModule, monotonicDateNow, wasmInstantiate, getRandomValues } from "pyodide-internal:pool/builtin_wrappers";

    // Pyodide uses `new URL(some_url, location)` to resolve the path in `loadPackage`. Setting
    // `location = undefined` makes this throw an error if some_url is not an absolute url. Which is what
    // we want here, it doesn't make sense to load a package from a relative URL.
    const location = undefined;

    function addEventListener(){}

    function reportUndefinedSymbolsPatched(Module) {
        if (Module.API.version === "0.26.0a2") {
            return;
        }
        Module.reportUndefinedSymbols(undefined);
    }

    if (typeof FinalizationRegistry === "undefined") {
        globalThis.FinalizationRegistry = class FinalizationRegistry {
            register(){}
            unregister(){}
        };
    }

    function patchDynlibLookup(Module, libName) {
        try {
            return Module.FS.readFile("/usr/lib/" + libName);
        } catch(e) {
            console.error("Failed to read ", libName, e);
        }
    }
    """

    REPLACEMENTS = [
        [
            # Convert pyodide.asm.js into an es6 module.
            # When we link our own we can pass `-sES6_MODULE` to the linker and it will do this for us
            # automatically.
            "var _createPyodideModule",
            PRELUDE + "export const _createPyodideModule",
        ],
        [
            "globalThis._createPyodideModule = _createPyodideModule;",
            "",
        ],
        [
            "new WebAssembly.Module",
            "newWasmModule",
        ],
        [
            "WebAssembly.instantiate",
            "wasmInstantiate",
        ],
        [
            "Date.now",
            "monotonicDateNow",
        ],
        [
            "reportUndefinedSymbols()",
            "reportUndefinedSymbolsPatched(Module)",
        ],
        [
            "crypto.getRandomValues(",
            "getRandomValues(Module, ",
        ],
        [
            # Direct eval disallowed in esbuild, see:
            # https://esbuild.github.io/content-types/#direct-eval
            "eval(func)",
            "(() => {throw new Error('Internal Emscripten code tried to eval, this should not happen, please file a bug report with your requirements.txt file\\'s contents')})()",
        ],
        [
            "eval(data)",
            "(() => {throw new Error('Internal Emscripten code tried to eval, this should not happen, please file a bug report with your requirements.txt file\\'s contents')})()",
        ],
        [
            "eval(UTF8ToString(ptr))",
            "(() => {throw new Error('Internal Emscripten code tried to eval, this should not happen, please file a bug report with your requirements.txt file\\'s contents')})()",
        ],
        # Dynamic linking patches:
        # library lookup
        [
            "!libData",
            "!(libData ??= patchDynlibLookup(Module, libName))",
        ],
        # for ensuring memory base of dynlib is stable when restoring snapshots
        [
            "getMemory(",
            "Module.getMemoryPatched(Module, libName, ",
        ],
    ]

    expand_template(
        name = "pyodide.asm.js@rule",
        out = "generated/pyodide.asm.js",
        substitutions = dict(REPLACEMENTS),
        template = "@pyodide//:pyodide/pyodide.asm.js",
    )

    js_file(
        name = "pyodide.asm.js@rule_js",
        srcs = ["generated/pyodide.asm.js"],
        deps = ["pyodide.asm.js@rule"],
    )

    esbuild(
        name = "generated/emscriptenSetup",
        srcs = native.glob([
            "internal/pool/*.ts",
        ]) + [
            "generated/pyodide.asm.js",
            "internal/util.ts",
        ],
        config = "internal/pool/esbuild.config.mjs",
        entry_point = "internal/pool/emscriptenSetup.ts",
        external = [
            "child_process",
            "crypto",
            "fs",
            "path",
            "url",
            "vm",
            "ws",
            "node:child_process",
            "node:crypto",
            "node:fs",
            "node:path",
            "node:url",
            "node:vm",
        ],
        format = "esm",
        output = "generated/emscriptenSetup.js",
        target = "esnext",
        deps = ["pyodide.asm.js@rule_js"],
    )

    INTERNAL_MODULES = native.glob(
        [
            "internal/*.ts",
            "internal/topLevelEntropy/*.ts",
            # The pool directory is only needed by typescript, it shouldn't be used at runtime.
            "internal/pool/*.ts",
            "types/*.ts",
            "types/*/*.ts",
        ],
        allow_empty = True,
    )

    MODULES = ["python-entrypoint-helper.ts"]

    INTERNAL_DATA_MODULES = native.glob([
        "internal/*.py",
        "internal/patches/*.py",
        "internal/topLevelEntropy/*.py",
    ]) + [
        "generated/python_stdlib.zip",
        "generated/pyodide.asm.wasm",
        "generated/emscriptenSetup.js",
    ]

    wd_ts_bundle(
        name = "pyodide",
        eslintrc_json = "eslint.config.mjs",
        import_name = "pyodide",
        internal_data_modules = INTERNAL_DATA_MODULES,
        internal_modules = INTERNAL_MODULES,
        js_deps = [
            "generated/emscriptenSetup",
            "pyodide.asm.wasm@copy",
            "python_stdlib.zip@copy",
        ],
        lint = False,
        modules = MODULES,
        schema_id = "0xbcc8f57c63814005",
        tsconfig_json = "tsconfig.json",
    )

    native.genrule(
        name = "pyodide.capnp.bin@rule",
        srcs = [
            ":pyodide.capnp",
            "//src/workerd/jsg:modules.capnp",
        ] + [
            ":pyodide-" + m.replace("/", "_").removesuffix(".ts")
            for m in INTERNAL_MODULES
            if not m.endswith(".d.ts")
        ] + [
            ":pyodide-" + m.replace("/", "_").removesuffix(".ts") + ".d.ts"
            for m in INTERNAL_MODULES
            if not m.endswith(".d.ts") and m.endswith(".ts")
        ] + [
            ":pyodide_" + m.replace("/", "_").removesuffix(".ts")
            for m in MODULES
        ] + [
            ":pyodide_" + m.replace("/", "_").removesuffix(".ts") + ".d.ts"
            for m in MODULES
        ] + [
            ":pyodide-" + m.replace("/", "_")
            for m in INTERNAL_DATA_MODULES
            if m.endswith(".py")
        ] + [
            ":pyodide-internal_generated_emscriptenSetup.js",
            ":pyodide-internal_generated_pyodide.asm.wasm",
            ":pyodide-internal_generated_python_stdlib.zip",
        ],
        outs = ["pyodide.capnp.bin"],
        cmd = " ".join([
            # Annoying logic to deal with different paths in workerd vs downstream.
            # Either need "-I src" in workerd or -I external/workerd/src downstream
            "INCLUDE=$$(stat src > /dev/null 2>&1 && echo src || echo external/workerd/src);",
            "$(execpath @capnp-cpp//src/capnp:capnp_tool)",
            "eval",
            "$(location :pyodide.capnp)",
            "pyodideBundle",
            "-I $$INCLUDE",
            "-o binary",
            "> $@",
        ]),
        tools = ["@capnp-cpp//src/capnp:capnp_tool"],
        visibility = ["//visibility:public"],
    )
