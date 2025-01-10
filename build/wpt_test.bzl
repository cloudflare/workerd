# Copyright (c) 2017-2022 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

# The public entry point is a macro named wpt_test. It first invokes a private
# rule named _wpt_test_gen to access the files in the wpt filegroup and
# generate a corresponding wd-test file. It then invokes the wd_test macro
# to set up the test.

load("//:build/wd_test.bzl", "wd_test")

def wpt_test(name, wpt_directory, test_js):
    test_gen_rule = "{}@_wpt_test_gen".format(name)
    _wpt_test_gen(
        name = test_gen_rule,
        test_name = name,
        wpt_directory = wpt_directory,
        test_js = test_js,
    )

    wd_test(
        name = "{}".format(name),
        src = test_gen_rule,
        args = ["--experimental"],
        data = [
            "//src/wpt:wpt-test-harness",
            test_js,
            wpt_directory,
            "//src/workerd/io:trimmed-supported-compatibility-date.txt",
        ],
    )

def _wpt_test_gen_impl(ctx):
    src = ctx.actions.declare_file("{}.wd-test".format(ctx.attr.test_name))
    ctx.actions.write(
        output = src,
        content = WPT_TEST_TEMPLATE.format(
            test_name = ctx.attr.test_name,
            test_js = wd_relative_path(ctx.file.test_js),
            modules = generate_external_modules(ctx.attr.wpt_directory.files),
        ),
    )

    return DefaultInfo(
        files = depset([src]),
    )

WPT_TEST_TEMPLATE = """
using Workerd = import "/workerd/workerd.capnp";
const unitTests :Workerd.Config = (
  services = [
    ( name = "{test_name}",
      worker = (
        modules = [
          (name = "worker", esModule = embed "{test_js}"),
          (name = "wpt:harness", esModule = embed "../../../../../workerd/src/wpt/harness.js"),
          {modules}
        ],
        bindings = [
          (name = "wpt", service = "wpt"),
        ],
        compatibilityDate = embed "../../../../../workerd/src/workerd/io/trimmed-supported-compatibility-date.txt",
        compatibilityFlags = ["nodejs_compat", "experimental"],
      )
    ),
    (
      name = "wpt",
      disk = ".",
    )
  ],
);"""

def wd_relative_path(file):
    """
    Returns a relative path which can be referenced in the .wd-test file.
    This is four directories up from the bazel short_path
    """
    return "../" * 4 + file.short_path

def generate_external_modules(files):
    """
    Generates a string for all files in the given directory in the specified format.
    Example for a JS file:
        (name = "url-origin.any.js", esModule = embed "../../../../../wpt/url/url-origin.any.js"),
    Example for a JSON file:
        (name = "resources/urltestdata.json", json = embed "../../../../../wpt/url/resources/urltestdata.json"),
    """
    result = []

    for file in files.to_list():
        file_path = wd_relative_path(file)
        if file.extension == "js":
            entry = """(name = "{}", esModule = embed "{}")""".format(file.basename, file_path)
        elif file.extension == "json":
            # TODO(soon): It's difficult to manipulate paths in Bazel, so we assume that all JSONs are in a resources/ directory for now
            entry = """(name = "resources/{}", json = embed "{}")""".format(file.basename, file_path)
        else:
            # For other file types, you can add more conditions or skip them
            continue

        result.append(entry)

    return ",\n".join(result)

_wpt_test_gen = rule(
    implementation = _wpt_test_gen_impl,
    attrs = {
        # A string to use as the test name. Used in the wd-test filename and the worker's name
        "test_name": attr.string(),
        # A file group representing a directory of wpt tests. All files in the group will be embedded.
        "wpt_directory": attr.label(),
        # A JS file containing the actual test logic.
        "test_js": attr.label(allow_single_file = True),
    },
)
