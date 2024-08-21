load("//:build/wd_test.bzl", "wd_test")

def py_wd_test(
        src,
        data = [],
        name = None,
        args = [],
        size = "enormous",
        **kwargs):
    data += ["pyodide_dev.capnp.bin@rule"]
    args += ["--pyodide-bundle-disk-cache-dir", "$(location pyodide_dev.capnp.bin@rule)/.."]

    wd_test(
        src = src,
        data = data,
        name = name,
        args = args,
        size = size,
        **kwargs
    )
