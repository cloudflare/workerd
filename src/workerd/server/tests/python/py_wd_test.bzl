load("//:build/wd_test.bzl", "wd_test")

def py_wd_test(
        directory = None,
        src = None,
        data = None,
        name = None,
        args = [],
        size = "enormous",
        tags = [],
        **kwargs):
    if data == None:
        data = native.glob(
            [
                directory + "/**",
            ],
            exclude = ["**/*.wd-test"],
        )
    if src == None:
        src = native.glob([directory + "/*.wd-test"])[0]
    if name == None and directory != None:
        name = directory + "@"
    data += ["//src/workerd/server/tests/python:pyodide_dev.capnp.bin@rule"]
    args = args + ["--pyodide-bundle-disk-cache-dir", "$(location //src/workerd/server/tests/python:pyodide_dev.capnp.bin@rule)/..", "--experimental"]

    wd_test(
        src = src,
        data = data,
        name = name,
        args = args,
        size = size,
        tags = tags + ["py_wd_test"],
        **kwargs
    )
