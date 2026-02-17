load("@bazel_skylib//rules:write_file.bzl", "write_file")
load("//:build/python_metadata.bzl", "BUNDLE_VERSION_INFO", "PYTHON_IMPORTS_TO_TEST")
load("//src/workerd/server/tests/python:py_wd_test.bzl", "py_wd_test")

def _generate_import_py_file(imports):
    res = ""
    for imp in imports:
        res += "import " + imp + "\n"

    res += "from workers import WorkerEntrypoint\n"
    res += "class Default(WorkerEntrypoint):\n"
    res += "    def test(self):\n"
    res += "        pass"
    return res

WD_FILE_TEMPLATE = """
using Workerd = import "/workerd/workerd.capnp";

const unitTests :Workerd.Config = (
    services = [
        ( name = "python-import-{name}",
            worker = (
                modules = [
                    (name = "worker.py", pythonModule = embed "./worker.py"),
                    {requirements}
                ],
                compatibilityFlags = [%PYTHON_FEATURE_FLAGS],
            )
        ),
    ]
);"""

def _generate_wd_test_file(name, requirements):
    l = []
    for req in requirements:
        l.append('(name = "{}", pythonRequirement = ""),\n'.format(req))
    requirements = "".join(l)
    return WD_FILE_TEMPLATE.format(name = name, requirements = requirements)

def _test(name, directory, wd_test, py_file, python_version, **kwds):
    py_wd_test(
        name = name,
        directory = directory,
        src = wd_test,
        python_flags = [python_version],
        use_snapshot = None,
        make_snapshot = False,
        skip_default_data = True,
        data = [py_file],
        **kwds
    )

# to_test is a dictionary from library name to list of imports
def _gen_import_tests(to_test, python_version, pkg_skip_versions):
    for lib in to_test.keys():
        skip_python_flags = [version for version, packages in pkg_skip_versions.items() if lib in packages]
        if BUNDLE_VERSION_INFO["development"]["real_pyodide_version"] in skip_python_flags:
            skip_python_flags.append("development")
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
            content = [_generate_import_py_file(to_test[lib])],
        )
        write_file(
            name = wd_test_fname + "@rule",
            out = wd_test_fname,
            content = [_generate_wd_test_file(lib, [lib])],
        )

        _test(
            name = prefix,
            directory = lib,
            wd_test = wd_test_fname,
            py_file = worker_py_fname,
            python_version = python_version,
            skip_python_flags = skip_python_flags,
        )

def gen_import_tests(*, pkg_skip_versions = {}):
    for python_version, info in BUNDLE_VERSION_INFO.items():
        to_test = PYTHON_IMPORTS_TO_TEST[info["packages"]]
        _gen_import_tests(to_test, python_version, pkg_skip_versions = pkg_skip_versions)

def _rotations(lst):
    result = []
    cur = lst
    for i in range(len(lst)):
        result.append(cur)
        cur = cur[1:] + [cur[0]]
    return result

def _pkg_permutations(lst):
    return _rotations(lst) + _rotations(reversed(lst))

def _gen_rust_import_tests(python_version):
    pyodide_version = BUNDLE_VERSION_INFO[python_version]["real_pyodide_version"]
    if pyodide_version == "0.26.0a2":
        pkgs = _rotations(["tiktoken", "pydantic"])
    else:
        pkgs = _pkg_permutations(["cryptography", "jiter", "tiktoken", "pydantic"])

    for res in pkgs:
        name = "-".join(res)
        prefix = "import2/" + name
        worker_py_fname = python_version + "/" + prefix + "/worker.py"
        wd_test_fname = python_version + "/" + prefix + "/import.wd-test"
        write_file(
            name = worker_py_fname + "@rule",
            out = worker_py_fname,
            content = [_generate_import_py_file(res)],
        )
        write_file(
            name = wd_test_fname + "@rule",
            out = wd_test_fname,
            content = [_generate_wd_test_file(name, res)],
        )

        _test(
            name = prefix,
            directory = name,
            wd_test = wd_test_fname,
            py_file = worker_py_fname,
            python_version = python_version,
        )

def gen_rust_import_tests():
    for python_version in BUNDLE_VERSION_INFO.keys():
        _gen_rust_import_tests(python_version)
