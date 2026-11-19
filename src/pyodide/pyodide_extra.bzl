"""Macro that generates the table of Python releases compiled into the binary.

`releases` in the generated pyodide_extra.capnp lists every entry of BUNDLE_VERSION_INFO
(build/python_metadata.bzl): the compatibility flag that selects it, and the bundle and baseline
snapshot it pins. The runtime picks a Worker's release from this table.
"""

load("@bazel_skylib//rules:copy_file.bzl", "copy_file")
load("@bazel_skylib//rules:expand_template.bzl", "expand_template")
load("@capnp-cpp//src/capnp:cc_capnp_library.bzl", "cc_capnp_library")
load("//:build/python_metadata.bzl", "BUNDLE_VERSION_INFO")

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
