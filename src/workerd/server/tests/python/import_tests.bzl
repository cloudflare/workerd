load("@bazel_skylib//rules:write_file.bzl", "write_file")

load("//:build/wd_test.bzl", "wd_test")

def generate_import_py_file(imports):
  res = ""
  for imp in imports:
    res += "import "+imp+"\n"

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
        compatibilityFlags = ["python_workers"],
      )
    ),
  ],
);"""

def generate_wd_test_file(requirement):
  return WD_FILE_TEMPLATE.format(requirement, requirement)

# to_test is a dictionary from library name to list of imports
def gen_import_tests(to_test):
  for lib in to_test.keys():
    worker_py_fname = "import/{}/worker.py".format(lib)
    wd_test_fname = "import/{}/import.wd-test".format(lib)
    write_file(
      name = worker_py_fname + "@rule",
      out = worker_py_fname,
      content = [generate_import_py_file(to_test[lib])],
      tags = ["slow"],
    )
    write_file(
      name = wd_test_fname + "@rule",
      out = wd_test_fname,
      content = [generate_wd_test_file(lib)],
      tags = ["slow"],
    )

    wd_test(
      src = wd_test_fname,
      args = ["--experimental", "--pyodide-package-disk-cache-dir", "../all_pyodide_wheels", "--pyodide-bundle-disk-cache-dir", "$(location pyodide-2.capnp.bin@rule)/.."],
      data = [worker_py_fname, "@all_pyodide_wheels//:whls", "pyodide-2.capnp.bin@rule"],
      tags = ["slow"],
    )
