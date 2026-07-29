load("@aspect_rules_esbuild//esbuild:defs.bzl", "esbuild")
load("@bazel_skylib//rules:copy_file.bzl", "copy_file")
load("@bazel_skylib//rules:expand_template.bzl", "expand_template")
load("@capnp-cpp//src/capnp:cc_capnp_library.bzl", "cc_capnp_library")
load("//:build/capnp_embed.bzl", "capnp_embed")
load("//:build/js_file.bzl", "js_file")
load("//:build/python_metadata.bzl", "BUNDLE_VERSION_INFO", "PYODIDE_VERSIONS", "PYTHON_LOCKFILES")
load("//:build/wd_js_bundle.bzl", "wd_js_bundle")
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

def _ts_bundle_out(prefix, name, version):
    return ":" + _out_path(prefix + name.removeprefix("internal/").replace("/", "_"), version)

def _copy_to_generated(src, version = None, out_name = None, name = None):
    out_name = out_name or _out_name(src)
    if name == None:
        name = out_name + "@copy"
    if version:
        name += "@" + version
    copy_file(name = name, src = src, out = _out_path(out_name, version))

def _copy_and_capnp_embed(src):
    out_name = _out_name(src)
    _copy_to_generated(src)
    capnp_embed(
        name = out_name + "@capnp",
        src = _out_path(out_name, None),
        deps = [out_name + "@copy"],
    )

def _fmt_python_snapshot_release(
        pyodide_version,
        pyodide_date,
        backport,
        baseline_snapshot_hash,
        flag,
        real_pyodide_version,
        integrity,
        packages = "",
        **_kwds):
    content = ", ".join(
        [
            'pyodide = "%s"' % pyodide_version,
            'realPyodideVersion = "%s"' % real_pyodide_version,
            'pyodideRevision = "%s"' % pyodide_date,
            'packages = "%s"' % packages,
            "backport = %s" % backport,
            'baselineSnapshotHash = "%s"' % baseline_snapshot_hash,
            'flagName = "%s"' % flag,
            'integrity = "%s"' % integrity,
        ],
    )
    return "(%s)" % content

def pyodide_extra():
    _copy_to_generated(
        "pyodide_extra.capnp",
        out_name = "pyodide_extra_tmpl.capnp",
    )

    package_tags = [info["tag"] for info in PYTHON_LOCKFILES]

    expand_template(
        name = "pyodide_extra_expand_template@rule",
        out = "generated/pyodide_extra.capnp",
        substitutions = {
            "%PACKAGE_LOCKS": ",".join(
                [
                    '(packageDate = "%s", lock = embed "pyodide-lock_%s.json")' %
                    (tag, tag)
                    for tag in package_tags
                ],
            ),
            "%PYTHON_RELEASES": ", ".join(
                [_fmt_python_snapshot_release(**info) for info in BUNDLE_VERSION_INFO.values()],
            ),
        },
        template = "generated/pyodide_extra_tmpl.capnp",
    )

    for tag in package_tags:
        _copy_and_capnp_embed("python-lock/pyodide-lock_" + tag + ".json")

    cc_capnp_library(
        name = "pyodide_extra_capnp",
        srcs = ["generated/pyodide_extra.capnp"],
        visibility = ["//visibility:public"],
        deps = [
        ] + [
            ":pyodide-lock_%s.json@capnp" % tag
            for tag in package_tags
        ],
        target_compatible_with = select({
            "@//build/config:no_build": ["@platforms//:incompatible"],
            "//conditions:default": [],
        }),
    )

def _packages_tag_for_version(version):
    # Maps a Pyodide version to the package lock tag whose stdlib wheels should be embedded into
    # that version's bundle. Newer Pyodide versions bundle the stdlib directly and have no lock
    # file / wheels to embed, in which case this returns None.
    for name, info in BUNDLE_VERSION_INFO.items():
        if name == "development":
            continue
        if info["pyodide_version"] == version and "packages" in info:
            return info["packages"]
    return None

def python_bundles(overrides = {}):
    srcs = [_python_bundle_helper(info, overrides) for info in PYODIDE_VERSIONS]
    native.filegroup(
        name = "python_bundles",
        srcs = srcs + [":bundle_version_info"],
    )

def _python_bundle_helper(info, overrides):
    version = info["version"]
    override = overrides.get(version, {})
    return _python_bundle(version, **override)

def pyodide_static():
    internal_data_modules = native.glob([
        "internal/*.py",
        "internal/workers-api/src/*.py",
        "internal/workers-api/src/workers/*.py",
        "internal/topLevelEntropy/*.py",
    ])
    internal_modules = native.glob(
        [
            "internal/*.ts",
            "internal/topLevelEntropy/*.ts",
            "types/*.ts",
            "types/*/*.ts",
        ],
    )
    modules = ["python-entrypoint-helper.ts"]

    wd_ts_bundle(
        name = "pyodide_static",
        eslintrc_json = "eslint.config.mjs",
        import_name = "pyodide",
        internal_data_modules = internal_data_modules,
        internal_modules = internal_modules,
        lint = True,
        modules = modules,
        schema_id = "0xdc8d02dfbdf14025",
        tsconfig_json = "tsconfig.json",
    )

_PRELUDE = """
import {
    addEventListener,
    getRandomValues,
    location,
    monotonicDateNow,
    newWasmModule,
    patchedApplyFunc,
    patchedLoadLibData,
    reportUndefinedSymbolsPatched,
    wasmInstantiate,
    patched_PyEM_CountFuncParams,
} from "pyodide-internal:pool/builtin_wrappers";
"""

# pyodide.asm.mjs patches
# TODO: all of these should be fixed by linking our own Pyodide or by upstreaming.
_REPLACEMENTS_COMMON = [
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
        "function loadLibData(){",
        """
        function loadLibData(){
            var libData = patchedLoadLibData(Module, libName, flags.rpath);
            return flags.loadAsync ? Promise.resolve(libData) : libData;
        }
        function dummiedOutOrigLoadLibData(){
        """,
    ],
    # for ensuring memory base of dynlib is stable when restoring snapshots
    [
        "getMemory(",
        "Module.getMemoryPatched(Module, libName, ",
    ],
    [
        "function _PyEM_CountFuncParams(func){",
        "function _PyEM_CountFuncParams(func){ return patched_PyEM_CountFuncParams(Module, func);",
    ],
    [
        "var tableBase=metadata.tableSize?wasmTable.length:0;",
        "var tableBase=metadata.tableSize?wasmTable.length:0;" +
        "Module.snapshotDebug && console.log('loadWebAssemblyModule', libName, memoryBase, tableBase);",
    ],
    # to ensure we report every fatal error, not just the first one
    [
        'console.error("Recursive call to fatal_error. Inner error was:");',
        'console.error("Recursive call to fatal_error. Inner error was:");\n' +
        "try { API.on_fatal?.(e); } catch(e2) { console.error(e2); }\n",
    ],
]

_REPLACEMENTS_COMMON_0_26_0_28 = [
    # for 0.28.2 or earlier, pyodide.asm.js was a commonjs module
    [
        # Convert pyodide.asm.js into an es6 module.
        # When we link our own we can pass `-sES6_MODULE` to the linker and it will do this for us
        # automatically.
        "var _createPyodideModule",
        _PRELUDE + "export const _createPyodideModule",
    ],
    [
        "globalThis._createPyodideModule = _createPyodideModule;",
        "",
    ],
    # to fix RPC, applies https://github.com/pyodide/pyodide/commit/8da1f38f7
    [
        "nullToUndefined(func.apply(",
        "nullToUndefined(patchedApplyFunc(API, func, ",
    ],
    [
        "nullToUndefined(Function.prototype.apply.apply",
        "nullToUndefined(API.config.jsglobals.Function.prototype.apply.apply",
    ],
]

_REPLACEMENTS = {
    "0.26.0a2": _REPLACEMENTS_COMMON + _REPLACEMENTS_COMMON_0_26_0_28,
    "0.28.2": _REPLACEMENTS_COMMON + _REPLACEMENTS_COMMON_0_26_0_28,
    "314.0.3": _REPLACEMENTS_COMMON + [
        # for 314 or later, pyodide.asm.mjs is es6 module
        [
            "export default _createPyodideModule;",
            # still expose _createPyodideModule for compatibility (import { _createPyodideModule })
            _PRELUDE + "export default _createPyodideModule; export { _createPyodideModule };",
        ],
    ],
}

def _python_bundle(version, *, pyodide_asm_wasm = None, pyodide_asm_mjs = None, python_stdlib_zip = None, emscripten_setup_override = None):
    pyodide_package = "@pyodide-%s//" % version
    if not pyodide_asm_wasm:
        pyodide_asm_wasm = pyodide_package + ":pyodide/pyodide.asm.wasm"

    if not pyodide_asm_mjs:
        if version in ("0.26.0a2", "0.28.2"):
            pyodide_asm_mjs = pyodide_package + ":pyodide/pyodide.asm.js"
        else:
            # for 314 or later, pyodide.asm.mjs is es6 module
            pyodide_asm_mjs = pyodide_package + ":pyodide/pyodide.asm.mjs"

    if not python_stdlib_zip:
        python_stdlib_zip = pyodide_package + ":pyodide/python_stdlib.zip"

    _copy_to_generated(pyodide_asm_wasm, version, out_name = "pyodide.asm.wasm")

    _copy_to_generated(python_stdlib_zip, version, out_name = "python_stdlib.zip")

    expand_template(
        name = "pyodide.asm.mjs@rule@" + version,
        out = _out_path("pyodide.asm.mjs", version),
        substitutions = dict(_REPLACEMENTS[version]),
        template = pyodide_asm_mjs,
    )

    js_file(
        name = "pyodide.asm.mjs@rule_js@" + version,
        srcs = [_out_path("pyodide.asm.mjs", version)],
        deps = ["pyodide.asm.mjs@rule@" + version],
    )

    if emscripten_setup_override:
        _copy_to_generated(out_name = "emscriptenSetup.js", name = "emscriptenSetup", src = emscripten_setup_override, version = version)
    else:
        expand_template(
            name = "esbuild.config.mjs@" + version,
            out = _out_path("esbuild.config.mjs", version),
            substitutions = {
                "%PYODIDE_VERSION%": version,
            },
            template = "internal/pool/esbuild.config.mjs",
        )

        esbuild(
            name = "emscriptenSetup@" + version,
            # exclude emscriptenSetup from source set so that rules_ts won't also try to create a JS output
            # for it. The file is provided in entry_point instead.
            srcs = native.glob([
                "internal/pool/*.ts",
            ], exclude = ["internal/pool/emscriptenSetup.ts"]) + [
                _out_path("pyodide.asm.mjs", version),
                "internal/util.ts",
                "internal/const.ts",
            ],
            config = _out_path("esbuild.config.mjs", version),
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
                "node:stream",
                "node:tls",
                "node:net",
                "node:module",
                "internal:unsafe-eval",
            ],
            format = "esm",
            output = _out_path("emscriptenSetup.js", version),
            target = "esnext",
            deps = ["pyodide.asm.mjs@rule_js@" + version],
        )

    # The CPython stdlib modules and the shared libraries they depend on are embedded directly into
    # the bundle so that the runtime no longer has to download or unpack them at request time. The
    # wheels listed in the (pre-filtered) lock file are extracted at build time into a single
    # PythonPackages capnp message (one entry per file, keyed by install_dir + path; see
    # python_packages.capnp / pack_python_packages.py) which is embedded as the `python_packages`
    # data module. Newer Pyodide versions bundle the stdlib directly and have no wheels to embed.
    packages_tag = _packages_tag_for_version(version)
    internal_data_modules = [
        _out_path("python_stdlib.zip", version),
    ]
    extra_deps = []
    has_packages = bool(packages_tag)
    if has_packages:
        lockfile = "python-lock/pyodide-lock_%s.json" % packages_tag
        wheels = "@all_pyodide_wheels_%s//:whls" % packages_tag
        native.genrule(
            name = "python_packages.bin@rule@" + version,
            srcs = [wheels, lockfile, "python_packages.capnp"],
            outs = [_out_path("python_packages.bin", version)],
            cmd = " ".join([
                "$(execpath :pack_python_packages)",
                "--capnp $(execpath @capnp-cpp//src/capnp:capnp_tool)",
                "--schema $(location python_packages.capnp)",
                "--lock $(location %s)" % lockfile,
                "--out $@",
                "$(locations %s)" % wheels,
            ]),
            tools = [
                ":pack_python_packages",
                "@capnp-cpp//src/capnp:capnp_tool",
            ],
            target_compatible_with = select({
                "@//build/config:no_build": ["@platforms//:incompatible"],
                "//conditions:default": [],
            }),
        )
        internal_data_modules.append(_out_path("python_packages.bin", version))
        extra_deps.append("python_packages.bin@rule@" + version)

    import_name = "pyodideRuntime"
    wd_js_bundle(
        name = "pyodide@" + version,
        import_name = import_name,
        builtin_modules = [],
        schema_id = "0xbcc8f57c63814005",
        internal_modules = [
            _out_path("emscriptenSetup.js", version),
        ],
        internal_wasm_modules = [
            _out_path("pyodide.asm.wasm", version),
        ],
        internal_data_modules = internal_data_modules,
        deps = [
            "emscriptenSetup@" + version,
            "pyodide.asm.wasm@copy@" + version,
            "python_stdlib.zip@copy@" + version,
        ] + extra_deps,
        out_dir = _out_path("", version),
    )

    pyodide_cappn_bin_rule = "pyodide.capnp.bin@rule@" + version
    bin_srcs = [
        ":pyodide@%s.capnp" % version,
        "//src/workerd/jsg:modules.capnp",
        _ts_bundle_out(import_name + "-internal_", "emscriptenSetup", version),
        _ts_bundle_out(import_name + "-internal_", "pyodide.asm.wasm", version),
        _ts_bundle_out(import_name + "-internal_", "python_stdlib.zip", version),
    ]
    if has_packages:
        bin_srcs.append(_ts_bundle_out(import_name + "-internal_", "python_packages.bin", version))
    native.genrule(
        name = pyodide_cappn_bin_rule,
        srcs = bin_srcs,
        outs = [_out_path("pyodide.capnp.bin", version)],
        cmd = " ".join([
            # Annoying logic to deal with different paths in workerd vs downstream.
            # Either need "-I src" in workerd or -I external/+local_repository+workerd/src downstream
            "INCLUDE=$$(stat src > /dev/null 2>&1 && echo src || echo external/+local_repository+workerd/src);",
            "$(execpath @capnp-cpp//src/capnp:capnp_tool)",
            "eval",
            "$(location :pyodide@%s.capnp)" % version,
            import_name + "Bundle",
            "-I $$INCLUDE",
            "-o binary",
            "> $@",
        ]),
        tools = ["@capnp-cpp//src/capnp:capnp_tool"],
        visibility = ["//visibility:public"],
        target_compatible_with = select({
            "@//build/config:no_build": ["@platforms//:incompatible"],
            "//conditions:default": [],
        }),
    )
    return pyodide_cappn_bin_rule
