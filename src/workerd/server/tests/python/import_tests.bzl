load("@bazel_skylib//rules:write_file.bzl", "write_file")
load("//src/workerd/server/tests/python:py_wd_test.bzl", "py_wd_test")

def generate_import_py_file(imports):
    res = ""
    for imp in imports:
        res += "import " + imp + "\n"

    res += "def test():\n"
    res += "   pass"
    return res

WD_FILE_TEMPLATE = """
using Workerd = import "/workerd/workerd.capnp";

const unitTests :Workerd.Config = (
  services = [
    ( name = "python-import-{}",
      worker = (
        modules = [
          (name = "worker.py", pythonModule = embed "./worker.py"),
          (name = "{}", pythonRequirement = ""),
        ],
        compatibilityDate = "2024-05-02",
        compatibilityFlags = [%PYTHON_FEATURE_FLAGS],
      )
    ),
  ]
);"""

def generate_wd_test_file(requirement):
    return WD_FILE_TEMPLATE.format(requirement, requirement)

# to_test is a dictionary from library name to list of imports
def gen_import_tests(to_test, python_version, pkg_skip_versions = {}):
    for lib in to_test.keys():
        if lib.endswith("-tests"):
            # TODO: The pyodide-build-scripts should be updated to not emit these packages. Once
            # that's done we can remove this check.
            continue

        prefix = "import/" + lib
        worker_py_fname = python_version + "/" + prefix + "/worker.py"
        wd_test_fname = python_version + "/" + prefix + "/import.wd-test"
        write_file(
            name = worker_py_fname + "@rule",
            out = worker_py_fname,
            content = [generate_import_py_file(to_test[lib])],
        )
        write_file(
            name = wd_test_fname + "@rule",
            out = wd_test_fname,
            content = [generate_wd_test_file(lib)],
        )

        skip_python_flags = [version for version, packages in pkg_skip_versions.items() if lib in packages]
        py_wd_test(
            name = prefix,
            directory = lib,
            src = wd_test_fname,
            python_flags = [python_version],
            skip_python_flags = skip_python_flags,
            make_snapshot = False,
            args = ["--experimental", "--pyodide-package-disk-cache-dir", "../all_pyodide_wheels"],
            data = [worker_py_fname, "@all_pyodide_wheels//:whls"],
            size = "enormous",
        )
