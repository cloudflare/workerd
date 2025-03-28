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

def _out_path(name, version):
    res = "generated/" + name
    if version:
        res = version + "/" + res
    return res

def _out(src, version):
    return _out_path(_out_name(src), version)

def _ts_bundle_out(prefix, name, version):
    return ":" + _out_path(prefix + name.removeprefix("internal/").replace("/", "_"), version)

def copy_to_generated(src, version = None, out_name = None, name = None):
    out_name = out_name or _out_name(src)
    if name == None:
        name = out_name + "@copy"
    if version:
        name += "@" + version
    copy_file(name = name, src = src, out = _out_path(out_name, version))

def copy_and_capnp_embed(src, version = None, out_name = None):
    out_name = out_name or _out_name(src)
    name = out_name + "@capnp"
    if version:
        name += "@" + version
    copy_to_generated(src, out_name = out_name, version = version)
    capnp_embed(
        name = name,
        src = _out_path(out_name, version),
        deps = [out_name + "@copy"],
    )

def python_bundle(version, pyodide_asm_wasm = None, pyodide_asm_js = None, python_stdlib_zip = None, emscripten_setup_override = None):
    if not pyodide_asm_wasm:
        pyodide_asm_wasm = "@pyodide//:pyodide/pyodide.asm.wasm"

    if not pyodide_asm_js:
        pyodide_asm_js = "@pyodide//:pyodide/pyodide.asm.js"

    if not python_stdlib_zip:
        python_stdlib_zip = "@pyodide//:pyodide/python_stdlib.zip"

    copy_and_capnp_embed("python-entrypoint.js", version = version)

    copy_to_generated(pyodide_asm_wasm, version, out_name = "pyodide.asm.wasm")

    copy_to_generated(python_stdlib_zip, version, out_name = "python_stdlib.zip")

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
        name = "pyodide.asm.js@rule@" + version,
        out = _out_path("pyodide.asm.js", version),
        substitutions = dict(REPLACEMENTS),
        template = pyodide_asm_js,
    )

    js_file(
        name = "pyodide.asm.js@rule_js@" + version,
        srcs = [_out_path("pyodide.asm.js", version)],
        deps = ["pyodide.asm.js@rule@" + version],
    )

    if emscripten_setup_override:
        copy_to_generated(out_name = "emscriptenSetup.js", name = "emscriptenSetup", src = emscripten_setup_override, version = version)
    else:
        esbuild(
            name = "emscriptenSetup@" + version,
            srcs = native.glob([
                "internal/pool/*.ts",
            ]) + [
                _out_path("pyodide.asm.js", version),
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
            output = _out_path("emscriptenSetup.js", version),
            target = "esnext",
            deps = ["pyodide.asm.js@rule_js@" + version],
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
        _out_path("python_stdlib.zip", version),
        _out_path("pyodide.asm.wasm", version),
        _out_path("emscriptenSetup.js", version),
    ]

    wd_ts_bundle(
        name = "pyodide@" + version,
        eslintrc_json = "eslint.config.mjs",
        import_name = "pyodide",
        internal_data_modules = INTERNAL_DATA_MODULES,
        internal_modules = INTERNAL_MODULES,
        js_deps = [
            "emscriptenSetup@" + version,
            "pyodide.asm.wasm@copy@" + version,
            "python_stdlib.zip@copy@" + version,
        ],
        lint = False,
        modules = MODULES,
        schema_id = "0xbcc8f57c63814005",
        tsconfig_json = "tsconfig.json",
        out_dir = _out_path("", version),
    )

    native.genrule(
        name = "pyodide.capnp.bin@rule@" + version,
        srcs = [
            ":pyodide@%s.capnp" % version,
            "//src/workerd/jsg:modules.capnp",
        ] + [
            _ts_bundle_out("pyodide-internal_", m.removesuffix(".ts"), version)
            for m in INTERNAL_MODULES
            if not m.endswith(".d.ts")
        ] + [
            ":" + _out_path(m.removesuffix(".ts") + ".d.ts", version)
            for m in INTERNAL_MODULES
            if not m.endswith(".d.ts") and m.endswith(".ts")
        ] + [
            _ts_bundle_out("pyodide_", m.removesuffix(".ts"), version)
            for m in MODULES
        ] + [
            ":" + _out_path(m.removesuffix(".ts") + ".d.ts", version)
            for m in MODULES
        ] + [
            ":" + _out_path("pyodide-" + m.replace("/", "_"), version)
            for m in INTERNAL_DATA_MODULES
            if m.endswith(".py")
        ] + [
            _ts_bundle_out("pyodide-internal_", "emscriptenSetup.js", version),
            _ts_bundle_out("pyodide-internal_", "pyodide.asm.wasm", version),
            _ts_bundle_out("pyodide-internal_", "python_stdlib.zip", version),
        ],
        outs = [_out_path("pyodide.capnp.bin", version)],
        cmd = " ".join([
            # Annoying logic to deal with different paths in workerd vs downstream.
            # Either need "-I src" in workerd or -I external/workerd/src downstream
            "INCLUDE=$$(stat src > /dev/null 2>&1 && echo src || echo external/workerd/src);",
            "$(execpath @capnp-cpp//src/capnp:capnp_tool)",
            "eval",
            "$(location :pyodide@%s.capnp)" % version,
            "pyodideBundle",
            "-I $$INCLUDE",
            "-o binary",
            "> $@",
        ]),
        tools = ["@capnp-cpp//src/capnp:capnp_tool"],
        visibility = ["//visibility:public"],
    )
