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
        compatibilityFlags = ["{}"],
      )
    ),
  ]
);"""

def generate_wd_test_file(requirement, compatFlag):
    return WD_FILE_TEMPLATE.format(requirement, requirement, compatFlag)

# to_test is a dictionary from library name to list of imports
def gen_import_tests(to_test):
    for lib in to_test.keys():
        for compatFlag in ["python_workers", "python_workers_development"]:
            worker_py_fname = "import/{}@{}/worker.py".format(lib, compatFlag)
            wd_test_fname = "import/{}@{}/import.wd-test".format(lib, compatFlag)
            write_file(
                name = worker_py_fname + "@rule",
                out = worker_py_fname,
                content = [generate_import_py_file(to_test[lib])],
            )
            write_file(
                name = wd_test_fname + "@rule",
                out = wd_test_fname,
                content = [generate_wd_test_file(lib, compatFlag)],
            )

            py_wd_test(
                src = wd_test_fname,
                args = ["--experimental", "--pyodide-package-disk-cache-dir", "../all_pyodide_wheels"],
                data = [worker_py_fname, "@all_pyodide_wheels//:whls"],
                size = "enormous",
            )
