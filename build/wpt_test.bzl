# Copyright (c) 2017-2022 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

# The public entry point is a macro named wpt_test. It first invokes a private
# rule named _wpt_test_gen to access the files in the wpt filegroup and
# generate a corresponding wd-test file. It then invokes the wd_test macro
# to set up the test.

load("//:build/wd_test.bzl", "wd_test")

def wpt_test(name, wpt_directory, test_config):
    js_gen_rule = "{}@_wpt_js_test_gen".format(name)
    _wpt_js_test_gen(
        name = js_gen_rule,
        test_name = name,
        wpt_directory = wpt_directory,
        test_config = test_config,
    )

    wd_gen_rule = "{}@_wpt_wd_test_gen".format(name)
    _wpt_wd_test_gen(
        name = wd_gen_rule,
        test_name = name,
        wpt_directory = wpt_directory,
        test_config = test_config,
        test_js = js_gen_rule,
    )

    wd_test(
        name = "{}".format(name),
        src = wd_gen_rule,
        args = ["--experimental"],
        data = [
            "//src/wpt:wpt-test-harness",
            test_config,
            js_gen_rule,
            wpt_directory,
            "//src/workerd/io:trimmed-supported-compatibility-date.txt",
        ],
    )

def _wpt_wd_test_gen_impl(ctx):
    wd_src = ctx.actions.declare_file("{}.wd-test".format(ctx.attr.test_name))

    ctx.actions.write(
        output = wd_src,
        content = WD_TEST_TEMPLATE.format(
            test_name = ctx.attr.test_name,
            test_js = wd_relative_path(ctx.file.test_js),
            test_config = wd_relative_path(ctx.file.test_config),
            modules = generate_external_modules(ctx.attr.wpt_directory.files),
        ),
    )

    return DefaultInfo(
        files = depset([wd_src]),
    )

def _wpt_js_test_gen_impl(ctx):
    test_src = ctx.actions.declare_file("{}-test.js".format(ctx.attr.test_name))

    ctx.actions.write(
        output = test_src,
        content = JS_TEST_TEMPLATE.format(
            cases = generate_external_cases(ctx.attr.wpt_directory.files),
        ),
    )

    return DefaultInfo(
        files = depset([test_src]),
    )

WD_TEST_TEMPLATE = """
using Workerd = import "/workerd/workerd.capnp";
const unitTests :Workerd.Config = (
  services = [
    ( name = "{test_name}",
      worker = (
        modules = [
          (name = "worker", esModule = embed "{test_js}"),
          (name = "config", esModule = embed "{test_config}"),
          (name = "harness", esModule = embed "../../../../../workerd/src/wpt/harness.js"),
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

JS_TEST_TEMPLATE = """
import {{ runner }} from 'harness';
import {{ config }} from 'config';

const run = runner(config);

{cases}
"""

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

def generate_external_cases(files):
    result = []
    for file in files.to_list():
        file_path = wd_relative_path(file)
        if file.extension == "js":
            entry = """export const {} = run("{}");""".format(file_to_identifier(file.basename), file.basename)
            result.append(entry)

    return "\n".join(result)

def file_to_identifier(file):
    stem = file.replace(".js", "").replace(".any", "")
    stem_title = "".join([ch for ch in stem.title().elems() if ch.isalnum()])
    return stem_title[0].lower() + stem_title[1:]

_wpt_wd_test_gen = rule(
    implementation = _wpt_wd_test_gen_impl,
    attrs = {
        # A string to use as the test name. Used in the wd-test filename and the worker's name
        "test_name": attr.string(),
        # A file group representing a directory of wpt tests. All files in the group will be embedded.
        "wpt_directory": attr.label(),
        # A JS file containing the config for the test cases
        "test_config": attr.label(allow_single_file = True),
        # The generated JS file invoking each test case
        "test_js": attr.label(allow_single_file = True),
    },
)

_wpt_js_test_gen = rule(
    implementation = _wpt_js_test_gen_impl,
    attrs = {
        # A string to use as the test name. Used in the wd-test filename and the worker's name
        "test_name": attr.string(),
        # A file group representing a directory of wpt tests. All files in the group will be embedded.
        "wpt_directory": attr.label(),
        # A JS file containing the config for the test cases
        "test_config": attr.label(allow_single_file = True),
    },
)
