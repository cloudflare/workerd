load("@bazel_skylib//rules:copy_file.bzl", "copy_file")
load("@bazel_skylib//rules:expand_template.bzl", "expand_template")
load("@capnp-cpp//src/capnp:cc_capnp_library.bzl", "cc_capnp_library")
load("//:build/python_metadata.bzl", "BUNDLE_VERSION_INFO")
load("//:build/wd_ts_bundle.bzl", "wd_ts_bundle")

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
    copy_file(
        name = "pyodide_extra_tmpl.capnp@copy",
        src = "pyodide_extra.capnp",
        out = "generated/pyodide_extra_tmpl.capnp",
    )

    expand_template(
        name = "pyodide_extra_expand_template@rule",
        out = "generated/pyodide_extra.capnp",
        substitutions = {
            "%PYTHON_RELEASES": ", ".join(
                [_fmt_python_snapshot_release(**info) for info in BUNDLE_VERSION_INFO.values()],
            ),
        },
        template = "generated/pyodide_extra_tmpl.capnp",
    )

    cc_capnp_library(
        name = "pyodide_extra_capnp",
        srcs = ["generated/pyodide_extra.capnp"],
        visibility = ["//visibility:public"],
    )

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
