load("//:build/python_metadata.bzl", "BUNDLE_VERSION_INFO")
load("//src/workerd/server/tests/python:py_wd_test.bzl", "compute_python_flags", "py_wd_test")

def _vendored_py_wd_test(name, version, test_template, main_py_file, vendored_srcs_target_prefix, level, data, **kwds):
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
    substitution_name = name + "_perform_substitution" + "_" + version
    native.genrule(
        name = substitution_name,
        srcs = [
            test_template,
            vendored_srcs_target,
            "//src/workerd/server/tests/python/vendor_pkg_tests:generate_modules.py",
        ],
        outs = [name + ".test.generated" + "_" + version],
        cmd = """
        # Create a file with all the file paths to avoid Windows command line length limits
        echo "$(locations %s)" > paths.txt
        $(execpath @python_3_13//:python3) \
            $(location //src/workerd/server/tests/python/vendor_pkg_tests:generate_modules.py) \
            --level=%s \
            --template=$(location %s) \
            --out=$@ \
            @paths.txt
        """ % (vendored_srcs_target, level, test_template),
        tools = ["@python_3_13//:python3", "@python_3_13//:files"],
    )

    # Create the py_wd_test
    py_wd_test(
        name = name,
        src = ":" + substitution_name,
        python_flags = [version],
        data = data + [
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
        vendored_package_name = None,
        level = 1,
        data = [],
        **kwds):
    python_flags = compute_python_flags(python_flags, skip_python_flags)
    bzl_name = "%s_vendor_test" % name
    if test_template == None:
        test_template = "%s_vendor.wd-test" % name
    if main_py_file == None:
        main_py_file = "%s.py" % name
    if vendored_package_name == None:
        vendored_package_name = name
    if vendored_srcs_target_prefix == None:
        vendored_srcs_target_prefix = "@%s_src" % vendored_package_name

    for flag in python_flags:
        info = BUNDLE_VERSION_INFO[flag]
        if vendored_package_name not in info["vendored_packages_for_tests"]:
            fail("Not found", vendored_package_name, "in", info["vendored_packages_for_tests"])
        _vendored_py_wd_test(bzl_name, info["name"], test_template, main_py_file, vendored_srcs_target_prefix, level = level, data = data, **kwds)
