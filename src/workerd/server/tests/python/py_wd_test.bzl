load("@bazel_skylib//rules:expand_template.bzl", "expand_template")
load("//:build/wd_test.bzl", "wd_test")

FEATURE_FLAGS = {
    "0.26.0a2": [],
    "0.27.1": ["python_workers_20250116"],
    "development": ["python_workers_development", "python_external_packages"],
}

def _py_wd_test_helper(
        name,
        src,
        python_flag,
        snapshot,
        *,
        args = [],
        **kwargs):
    name_flag = name + "_" + python_flag
    templated_src = name_flag.replace("/", "-") + "@template"
    templated_src = "/".join(src.split("/")[:-1] + [templated_src])
    flags = FEATURE_FLAGS[python_flag] + ["python_workers"]
    feature_flags_txt = ",".join(['"{}"'.format(flag) for flag in flags])

    expand_template(
        name = name_flag + "@rule",
        out = templated_src,
        template = src,
        substitutions = {"%PYTHON_FEATURE_FLAGS": feature_flags_txt},
    )

    wd_test(
        src = templated_src,
        name = name_flag + "@",
        args = args,
        python_snapshot_test = snapshot,
        **kwargs
    )

def py_wd_test(
        directory = None,
        *,
        src = None,
        data = None,
        name = None,
        python_flags = "all",
        skip_python_flags = [],
        args = [],
        size = "enormous",
        tags = [],
        make_snapshot = True,
        **kwargs):
    if python_flags == "all":
        python_flags = FEATURE_FLAGS.keys()
    python_flags = [flag for flag in python_flags if flag not in skip_python_flags]
    if data == None and directory != None:
        data = native.glob(
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
    data += ["//src/workerd/server/tests/python:pyodide_dev.capnp.bin@rule"]
    args = args + [
        "--pyodide-bundle-disk-cache-dir",
        "$(location //src/workerd/server/tests/python:pyodide_dev.capnp.bin@rule)/..",
        "--experimental",
        "--pyodide-package-disk-cache-dir",
        ".",
    ]
    tags = tags + ["py_wd_test"]

    for python_flag in python_flags:
        _py_wd_test_helper(
            name,
            src,
            python_flag,
            snapshot = make_snapshot,
            data = data,
            args = args,
            size = size,
            tags = tags,
        )
