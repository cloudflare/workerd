load("@bazel_skylib//rules:expand_template.bzl", "expand_template")
load("//:build/wd_test.bzl", "wd_test")

FEATURE_FLAGS = {
    "0.26.0a2": ["python_workers"],
    "development": ["python_workers_development"],
}

def py_wd_test(
        directory = None,
        src = None,
        data = None,
        name = None,
        python_flags = "all",
        args = [],
        size = "enormous",
        tags = [],
        **kwargs):
    if python_flags == "all":
        python_flags = FEATURE_FLAGS.keys()
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
    args = args + ["--pyodide-bundle-disk-cache-dir", "$(location //src/workerd/server/tests/python:pyodide_dev.capnp.bin@rule)/..", "--experimental"]
    for python_flag in python_flags:
        name_flag = name + "_" + python_flag
        templated_src = name_flag.replace("/", "-") + "@template"
        templated_src = "/".join(src.split("/")[:-1] + [templated_src])
        feature_flags_txt = ",".join(['"{}"'.format(flag) for flag in FEATURE_FLAGS[python_flag]])
        expand_template(
            name = name_flag + "@rule",
            out = templated_src,
            template = src,
            substitutions = {"%PYTHON_FEATURE_FLAGS": feature_flags_txt},
        )

        wd_test(
            src = templated_src,
            data = data,
            name = name_flag + "@",
            args = args,
            size = size,
            tags = tags + ["py_wd_test"],
            **kwargs
        )
