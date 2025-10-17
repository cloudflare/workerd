load("//:build/python_metadata.bzl", "BUNDLE_VERSION_INFO")
load("//src/workerd/server/tests/python:py_wd_test.bzl", "compute_python_flags", "py_wd_test")

def _vendored_py_wd_test(name, version, test_template, main_py_file, vendored_srcs_target_prefix, **kwds):
    """Creates a Python Workers test which includes vendored packages in its bundle, the
    http_archive target containing the vendored sources should be specified in `vendored_srcs_target_prefix`.

    Args:
        name: Name of the test
        version: The version of the package bundle
        test_template: The .wd-test template file
        main_py_file: The main Python file for the test
        vendored_srcs_target_prefix: The prefix of the Bazel target containing the vendored sources
    """
    vendored_srcs_target = vendored_srcs_target_prefix + "_" + version + "//:all_srcs"

    # Generate module list
    module_list_name = name + "_modules_string" + "_" + version
    native.genrule(
        name = module_list_name,
        srcs = [
            vendored_srcs_target,
            "generate_modules.py",
        ],
        outs = [module_list_name + ".txt"],
        cmd = """
        # Create a file with all the file paths to avoid Windows command line length limits
        echo "$(locations """ + vendored_srcs_target + """)" > paths.txt
        $(execpath @python_3_13//:python3) $(location generate_modules.py) @paths.txt > $@
        """,
        tools = ["@python_3_13//:python3"],
    )

    # Perform substitution to include the generated modules in template
    substitution_name = name + "_perform_substitution" + "_" + version
    native.genrule(
        name = substitution_name,
        srcs = [
            test_template,
            ":" + module_list_name,
        ],
        outs = [name + ".test.generated" + "_" + version],
        cmd = """
        $(execpath @python_3_13//:python3) -c "
import sys
with open('$(location :""" + module_list_name + """)', 'r') as f:
    modules = f.read()
with open('$(location """ + test_template + """)', 'r') as f:
    template = f.read()
result = template.replace('%PYTHON_VENDORED_MODULES%', modules)

with open('$@', 'w') as f:
    f.write(result)
        "
    """,
        tools = ["@python_3_13//:python3"],
    )

    # Create the py_wd_test
    py_wd_test(
        name = name,
        src = ":" + substitution_name,
        python_flags = [version],
        data = [
            main_py_file,
            vendored_srcs_target,
        ],
        # Disable on windows because of flakiness
        # TODO fix this
        target_compatible_with = select({
            "@platforms//os:windows": ["@platforms//:incompatible"],
            "//conditions:default": [],
        }),
        **kwds
    )

def vendored_py_wd_test(
        name,
        test_template = None,
        main_py_file = None,
        vendored_srcs_target_prefix = None,
        python_flags = "all",
        skip_python_flags = [],
        **kwds):
    python_flags = compute_python_flags(python_flags, skip_python_flags)
    bzl_name = "%s_vendor_test" % name
    if test_template == None:
        test_template = "%s_vendor.wd-test" % name
    if main_py_file == None:
        main_py_file = "%s.py" % name
    if vendored_srcs_target_prefix == None:
        vendored_srcs_target_prefix = "@%s_src" % name

    for flag in python_flags:
        info = BUNDLE_VERSION_INFO[flag]
        if name not in info["vendored_packages_for_tests"]:
            fail("Not found", name, "in", info["vendored_packages_for_tests"])
        _vendored_py_wd_test(bzl_name, info["name"], test_template, main_py_file, vendored_srcs_target_prefix, **kwds)
