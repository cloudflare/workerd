load("@//:build/is_pyodide_bzlmod.bzl", "is_pyodide_bzlmod")
load("@bazel_skylib//rules:copy_file.bzl", "copy_file")
load("@bazel_skylib//rules:expand_template.bzl", "expand_template")
load("//:build/python_metadata.bzl", "BUNDLE_VERSION_INFO")
load("//:build/wd_test.bzl", "wd_test")

def _get_enable_flags(python_flag):
    flags = [BUNDLE_VERSION_INFO[python_flag]["enable_flag_name"]]
    if "python_workers" not in flags:
        flags.append("python_workers")
    for key, value in BUNDLE_VERSION_INFO.items():
        # With all-compat-flags variant we might end up accidently using a newer python bundle than
        # intended. To make sure we get the speicific intended version we also need to disable newer
        # python versions.
        if python_flag != key and value["enable_flag_name"] not in ["python_workers", "python_workers_development"]:
            flags.append("no_" + value["enable_flag_name"])
    return flags

def _py_wd_test_helper(
        name,
        src,
        python_flag,
        *,
        make_snapshot,
        use_snapshot,
        args,
        feature_flags,
        data = [],
        **kwargs):
    name_flag = name + "_" + python_flag
    templated_src = name_flag.replace("/", "-") + "@template"
    templated_src = "/".join(src.split("/")[:-1] + [templated_src])

    pkg_tag = BUNDLE_VERSION_INFO[python_flag]["packages"]
    data = data + ["@all_pyodide_wheels_%s//:whls" % pkg_tag]
    args = args + ["--pyodide-package-disk-cache-dir"]

    # TODO(cleanup): We support both mangled and non-mangled wheels paths for now, clean up once
    # downstream repo fully uses bzlmod
    if is_pyodide_bzlmod:
        # +pyodide+ is a bzlmod canonical repository name
        args.append("../+pyodide+all_pyodide_wheels_%s" % pkg_tag)
    else:
        args.append("../all_pyodide_wheels_%s" % pkg_tag)

    load_snapshot = None
    pyodide_version = BUNDLE_VERSION_INFO[python_flag]["real_pyodide_version"]
    if use_snapshot == "stacked":
        if pyodide_version == "0.26.0a2":
            use_snapshot = None
        else:
            use_snapshot = "baseline"
            feature_flags = feature_flags + ["python_dedicated_snapshot"]
    if use_snapshot:
        version_info = BUNDLE_VERSION_INFO[python_flag]

        snapshot = version_info[use_snapshot + "_snapshot"]
        data = data + [":python_snapshots"]
        load_snapshot = snapshot

    if load_snapshot and not make_snapshot:
        args += ["--python-load-snapshot", "load_snapshot.bin"]

    flags = _get_enable_flags(python_flag) + feature_flags
    feature_flags_txt = ",".join(['"{}"'.format(flag) for flag in flags])

    expand_template(
        name = name_flag + "@rule",
        out = templated_src,
        template = src,
        substitutions = {"%PYTHON_FEATURE_FLAGS": feature_flags_txt},
    )

    # Since we bumped the development flag to point to 0.28.2, it doesn't work on windows CI.
    # TODO: Fix this.
    if python_flag == "development":
        kwargs["target_compatible_with"] = select({
            "@platforms//os:windows": ["@platforms//:incompatible"],
            "//conditions:default": [],
        })

    wd_test(
        src = templated_src,
        name = name_flag,
        args = args,
        python_snapshot_test = make_snapshot,
        data = data,
        load_snapshot = load_snapshot,
        # TODO(soon): at the time of disabling these they all passed but because of how slow python
        #             tests are we disabled them for now. We should re-enable them when we have
        #             a better way to run them.
        generate_all_autogates_variant = False,
        generate_all_compat_flags_variant = False,
        **kwargs
    )

def _snapshot_file(snapshot):
    if not snapshot:
        return []
    copy_file(
        name = "pyodide-snapshot-%s@copy" % snapshot,
        src = "@pyodide-snapshot-%s//file" % snapshot,
        out = snapshot,
        visibility = ["//visibility:public"],
    )
    return [":" + snapshot]

def _snapshot_files(
        name,
        baseline_snapshot = None,
        numpy_snapshot = None,
        fastapi_snapshot = None,
        dedicated_fastapi_snapshot = None,
        **_kwds):
    if name == "development":
        return []
    result = []
    result += _snapshot_file(baseline_snapshot)
    result += _snapshot_file(numpy_snapshot)
    result += _snapshot_file(fastapi_snapshot)
    result += _snapshot_file(dedicated_fastapi_snapshot)
    return result

def _snapshot_file_group():
    snapshots = []
    for x in BUNDLE_VERSION_INFO.values():
        snapshots += _snapshot_files(**x)

    native.filegroup(
        name = "python_snapshots",
        data = snapshots,
        visibility = ["//visibility:public"],
    )

def _capnp_bundle(id, **_kwds):
    if id == "dev":
        return
    name = "pyodide_%s.capnp.bin" % id
    copy_file(
        name = name + "@rule",
        src = "@%s//file" % name,
        out = "pyodide-bundle-cache/" + name,
        visibility = ["//visibility:public"],
    )

def _capnp_bundles_file_group():
    # pyodide_dev.capnp.bin represents a custom pyodide version "dev" that is generated
    # at build time using the latest contents of the src/pyodide directory.
    # This is used to run tests to ensure that they are always run against the latest build of
    # the Pyodide bundle.
    copy_file(
        name = "pyodide_dev.capnp.bin@rule",
        src = "//src/pyodide:pyodide.capnp.bin_cross",
        out = "pyodide-bundle-cache/pyodide_dev.capnp.bin",
        visibility = ["//visibility:public"],
    )
    for info in BUNDLE_VERSION_INFO.values():
        _capnp_bundle(**info)

def _capnp_rules():
    return ["//src/workerd/server/tests/python:pyodide_%s.capnp.bin@rule" % info["id"] for info in BUNDLE_VERSION_INFO.values()]

def python_test_setup():
    _capnp_bundles_file_group()
    _snapshot_file_group()

def compute_python_flags(python_flags, skip_python_flags):
    if python_flags == "all":
        python_flags = BUNDLE_VERSION_INFO.keys()
    python_flags = [flag for flag in python_flags if flag not in skip_python_flags and flag in BUNDLE_VERSION_INFO]
    return python_flags

def py_wd_test(
        directory = None,
        *,
        src = None,
        data = None,
        name = None,
        python_flags = "all",
        skip_python_flags = [],
        feature_flags = [],
        args = [],
        size = "enormous",
        tags = [],
        make_snapshot = True,
        use_snapshot = "stacked",
        skip_default_data = False,
        **kwargs):
    python_flags = compute_python_flags(python_flags, skip_python_flags)
    if data == None:
        data = []
    if directory and not skip_default_data:
        data += native.glob(
            [
                directory + "/**",
            ],
            exclude = ["**/*.wd-test"],
        )
    if src == None:
        src = native.glob([directory + "/*.wd-test"])[0]
    if name == None and directory != None:
        name = directory
    elif name == None:
        name = src.removesuffix(".wd-test")
    data += _capnp_rules()
    args = args + [
        "--pyodide-bundle-disk-cache-dir",
        "$(location //src/workerd/server/tests/python:pyodide_dev.capnp.bin@rule)/..",
        "--experimental",
        "--python-snapshot-dir",
        ".",
    ]
    tags = tags + ["py_wd_test", "python"]

    for python_flag in python_flags:
        _py_wd_test_helper(
            name,
            src,
            python_flag,
            make_snapshot = make_snapshot,
            use_snapshot = use_snapshot,
            feature_flags = feature_flags,
            data = data,
            args = args,
            size = size,
            tags = tags,
            **kwargs
        )
