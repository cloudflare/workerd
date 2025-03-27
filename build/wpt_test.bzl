# Copyright (c) 2017-2022 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

load("@bazel_skylib//lib:paths.bzl", "paths")
load("//:build/wd_test.bzl", "wd_test")

### wpt_test macro
### (Invokes wpt_js_test_gen, wpt_wd_test_gen and wd_test to assemble a complete test suite.)
### -----------------------------------------------------------------------------------------

def wpt_test(name, wpt_directory, config, autogates = [], start_server = False, **kwargs):
    """
    Main entry point.

    1. Generates a workerd test suite in JS. This contains the logic to run
       each WPT test file, applying the relevant test config.
    2. Generates a wd-test file for this test suite. This contains all of the
       paths to modules needed to run the test: generated test suite, test config
       file, WPT test scripts, associated JSON resources.
    """

    js_test_gen_rule = "{}@_wpt_js_test_gen".format(name)
    test_config_as_js = config.removesuffix(".ts") + ".js"
    wpt_tsproject = "//src/wpt:wpt-all@tsproject"
    harness_as_js = "//src/wpt:harness/harness.js"
    compat_date = "//src/workerd/io:trimmed-supported-compatibility-date.txt"
    wpt_cacert = "@wpt//:tools/certs/cacert.pem"

    _wpt_js_test_gen(
        name = js_test_gen_rule,
        test_name = name,
        wpt_directory = wpt_directory,
        test_config = test_config_as_js,
        wpt_tsproject = wpt_tsproject,
    )

    wd_test_gen_rule = "{}@_wpt_wd_test_gen".format(name)
    _wpt_wd_test_gen(
        name = wd_test_gen_rule,
        test_name = name,
        wpt_directory = wpt_directory,
        test_config = test_config_as_js,
        test_js_generated = js_test_gen_rule,
        harness = harness_as_js,
        compat_date = compat_date,
        autogates = autogates,
        wpt_cacert = wpt_cacert,
    )

    wd_test(
        name = "{}".format(name),
        src = wd_test_gen_rule,
        args = ["--experimental"],
        sidecar = "@wpt//:entrypoint" if start_server else None,
        data = [
            test_config_as_js,  # e.g. "url-test.js"
            js_test_gen_rule,  # e.g. "url-test.generated.js",
            wpt_directory,  # e.g. wpt/url/**",
            compat_date,  # i.e. trimmed-supported-compatibility-date.txt
            harness_as_js,  # i.e. "harness/harness.js"
            wpt_cacert,  # i.e. "wpt/tools/certs/cacert.pem",
        ],
        **kwargs
    )

### wpt_module macro and rule
### (Discovers test files within the WPT directory tree)
### ----------------------------------------------------

def wpt_module(name):
    """
    Given the name of a directory within the WPT tree, creates a rule providing:

    srcs: All files within the directory
    dir: A reference to the directory itself.

    This info is later used by wpt_test rules to generate JS code and wd-test config
    for each test.
    """
    return _wpt_module(
        name = "{}@module".format(name),
        dir = name,
        srcs = native.glob(["{}/**/*".format(name)]),
        visibility = ["//visibility:public"],
    )

WPTModuleInfo = provider(fields = ["base"])

def _wpt_module_impl(ctx):
    return [
        DefaultInfo(files = depset(ctx.files.srcs)),
        WPTModuleInfo(base = ctx.attr.dir.files.to_list()[0]),
    ]

_wpt_module = rule(
    implementation = _wpt_module_impl,
    attrs = {
        "dir": attr.label(allow_single_file = True),
        "srcs": attr.label_list(allow_files = True),
    },
)

### wpt_js_test_gen rule
### (Generates a .js file for each test)
### ------------------------------------

def _wpt_js_test_gen_impl(ctx):
    """
    Generates a workerd test suite in JS.

    This contains the logic to run each WPT test file, applying the relevant test config.
    """

    src = ctx.actions.declare_file("{}-test.generated.js".format(ctx.attr.test_name))
    base = ctx.attr.wpt_directory[WPTModuleInfo].base

    files = ctx.attr.wpt_directory.files.to_list()
    test_files = [file for file in files if file.extension == "js" and not is_in_resources_directory(file)]

    ctx.actions.write(
        output = src,
        content = WPT_JS_TEST_TEMPLATE.format(
            test_config = ctx.file.test_config.basename,
            cases = generate_external_cases(base, test_files),
            all_test_files = generate_external_file_list(base, test_files),
            test_name = ctx.attr.test_name,
        ),
    )

    return DefaultInfo(
        files = depset([src]),
    )

_wpt_js_test_gen = rule(
    implementation = _wpt_js_test_gen_impl,
    attrs = {
        # A string to use as the test name. Used in the wd-test filename and the worker's name
        "test_name": attr.string(),
        # A file group representing a directory of wpt tests. All files in the group will be embedded.
        "wpt_directory": attr.label(),
        # A JS file containing the test configuration.
        "test_config": attr.label(allow_single_file = True),
        # Dependency: The ts_project rule that compiles the tests to JS
        "wpt_tsproject": attr.label(),
    },
)

WPT_JS_TEST_TEMPLATE = """// This file is autogenerated by wpt_test.bzl
// DO NOT EDIT.
import {{ createRunner }} from 'harness/harness';
import config from '{test_config}';

const allTestFiles = {all_test_files};
const run = createRunner(config, '{test_name}', allTestFiles);

{cases}
"""

def generate_external_cases(base, files):
    """
    Generate a workerd test case that runs each test file in the WPT module.
    """

    result = []
    for file in files:
        relative_path = module_relative_path(base, file)
        result.append("export const {} = run('{}');".format(test_case_name(relative_path), relative_path))

    return "\n".join(result)

def generate_external_file_list(base, files):
    """
    Generate a JS list containing the name of every test file in the WPT module
    """

    return "[{}];".format(", ".join([
        "'{}'".format(module_relative_path(base, file))
        for file in files
    ]))

### wpt_wd_test_gen rule
### (Generates a .wd-test file for each test)
### -----------------------------------------

def _wpt_wd_test_gen_impl(ctx):
    """
    Generates a wd-test file for this test suite.

    This contains all of the paths to modules needed to run the test: generated test suite, test
    config file, WPT test scripts, associated JSON resources.
    """
    src = ctx.actions.declare_file("{}.wd-test".format(ctx.attr.test_name))
    base = ctx.attr.wpt_directory[WPTModuleInfo].base
    ctx.actions.write(
        output = src,
        content = WPT_WD_TEST_TEMPLATE.format(
            test_name = ctx.attr.test_name,
            test_config = ctx.file.test_config.basename,
            test_js_generated = ctx.file.test_js_generated.basename,
            bindings = generate_external_bindings(src, base, ctx.attr.wpt_directory.files),
            harness = wd_test_relative_path(src, ctx.file.harness),
            compat_date = wd_test_relative_path(src, ctx.file.compat_date),
            wpt_cacert = wd_test_relative_path(src, ctx.file.wpt_cacert),
            autogates = generate_autogates_field(ctx.attr.autogates),
        ),
    )

    return DefaultInfo(
        files = depset([src]),
    )

_wpt_wd_test_gen = rule(
    implementation = _wpt_wd_test_gen_impl,
    attrs = {
        # A string to use as the test name. Used in the wd-test filename and the worker's name
        "test_name": attr.string(),
        # A file group representing a directory of wpt tests. All files in the group will be embedded.
        "wpt_directory": attr.label(),
        # A JS file containing the test configuration.
        "test_config": attr.label(allow_single_file = True),
        # An auto-generated JS file containing the test logic.
        "test_js_generated": attr.label(allow_single_file = True),
        # Target specifying the location of the WPT test harness
        "harness": attr.label(allow_single_file = True),
        # Target specifying the location of the trimmed-supported-compatibility-date.txt file
        "compat_date": attr.label(allow_single_file = True),
        # Target specifying the location of the WPT CA certificate
        "wpt_cacert": attr.label(allow_single_file = True),
        # A list of autogates to specify in the generated wd-test file
        "autogates": attr.string_list(),
    },
)

WPT_WD_TEST_TEMPLATE = """
using Workerd = import "/workerd/workerd.capnp";
const unitTests :Workerd.Config = (
  services = [
    ( name = "{test_name}",
      worker = (
        modules = [
          (name = "worker", esModule = embed "{test_js_generated}"),
          (name = "{test_config}", esModule = embed "{test_config}"),
          (name = "harness/harness", esModule = embed "{harness}"),
        ],
        bindings = [
          (name = "wpt", service = "wpt"),
          (name = "unsafe", unsafeEval = void),
          {bindings}
        ],
        compatibilityDate = embed "{compat_date}",
        compatibilityFlags = ["nodejs_compat", "experimental"],
      )
    ),
    ( name = "internet",
      network = (
        allow = ["private"],
        tlsOptions = (
          trustedCertificates = [
            embed "{wpt_cacert}"
          ]
        )
      )
    ),
    (
      name = "wpt",
      disk = ".",
    )
  ],
  {autogates}
);"""

def generate_autogates_field(autogates):
    """
    Generates a capnproto fragment listing the specified autogates.
    """

    if not autogates:
        return ""

    autogate_list = ", ".join(['"{}"'.format(autogate) for autogate in autogates])
    return "autogates = [{}],".format(autogate_list)

def generate_external_bindings(wd_test_file, base, files):
    """
    Generates appropriate bindings for each file in the WPT module:

    - JS files: text binding to allow code to be evaluated
    - JSON files: JSON binding to allow test code to fetch resources
    """

    result = []

    for file in files.to_list():
        if file.extension == "js":
            binding_type = "text"
        elif file.extension == "json":
            binding_type = "json"
        else:
            # Unknown binding type, skip for now
            continue

        result.append('(name = "{}", {} = embed "{}")'.format(module_relative_path(base, file), binding_type, wd_test_relative_path(wd_test_file, file)))

    return ",\n".join(result)

### WPT server entrypoint
### ---------------------
### (Create a single no-args script that starts the WPT server)

WPT_ENTRYPOINT_SCRIPT_TEMPLATE = """
# Make /usr/sbin/sysctl visibile (Python needs to call it on macOS)
export PATH="$PATH:/usr/sbin"

cd $(dirname $0)
{python} wpt.py serve --config {config_json}
"""

def _wpt_server_entrypoint_impl(ctx):
    """
    This rule generates a script that starts the wpt server.

    This script is passed as the sidecar to wd_test.
    We generate a script because Bazel doesn't want arguments in this context, and we use a custom
    rule to do so because we want more control over paths and dependencies than with a genrule.
    """
    start_src = ctx.actions.declare_file("start.sh")
    ctx.actions.write(
        output = start_src,
        is_executable = True,
        content = WPT_ENTRYPOINT_SCRIPT_TEMPLATE.format(
            python = ctx.file.python.short_path,
            config_json = ctx.file.config_json.basename,
        ),
    )

    return DefaultInfo(
        runfiles = ctx.runfiles(
            files = [start_src],
            transitive_files = depset(ctx.files.srcs + [ctx.file.python, ctx.file.config_json]),
        ),
        executable = start_src,
    )

wpt_server_entrypoint = rule(
    implementation = _wpt_server_entrypoint_impl,
    attrs = {
        # Python interpreter to use to run the WPT server
        "python": attr.label(allow_single_file = True),
        # All the Python code that should be visible
        "srcs": attr.label_list(allow_files = True),
        # Config file to pass to wpt serve
        "config_json": attr.label(allow_single_file = True),
    },
)

### Path manipulation
### -----------------

def module_relative_path(module_base, file):
    """
    Return the relative path of a file inside its parent WPT module.

    For example, within the 'dom/abort' module, the path
    'dom/abort/resources/abort-signal-any-tests.js' would be referred to as
    'resources/abort-signal-any-tests.js'
    """

    return paths.relativize(file.short_path, module_base.short_path)

def relative_path(from_path, to_path):
    """
    Finds the relative path from from_path to to_path.
    Based on <https://github.com/nodejs/node/blob/0a91e988cfe2a994e60780a4ce001e44812a9ad9/lib/path.js#L1280>
    """

    # The algorithm we got from Node assumes both paths are absolute,
    # so add a slash rather than change all the logic below
    from_path = "/{}".format(from_path)
    to_path = "/{}".format(to_path)

    from_start = 1
    from_end = len(from_path)
    from_len = from_end - from_start
    to_start = 1
    to_len = len(to_path) - to_start

    length = from_len if from_len < to_len else to_len
    last_common_sep = -1
    i = 0

    for i in range(length):
        from_ch = from_path[from_start + i]
        if from_ch != to_path[to_start + i]:
            break
        elif from_ch == "/":
            last_common_sep = i

    # Compare paths to find the longest common path from root
    if i == length:
        if to_len > length:
            if to_path[to_start + i] == "/":
                # We get here if `from_path` is the exact base path for `to_path`.
                # For example: from='/foo/bar'; to='/foo/bar/baz'
                return to_path[to_start + i + 1:]

            if i == 0:
                # We get here if `from` is the root
                # For example: from='/'; to='/foo'
                return to_path[to_start + i:]
    elif from_len > length:
        if from_path[from_start + i] == "/":
            # We get here if `to` is the exact base path for `from`.
            # For example: from='/foo/bar/baz'; to='/foo/bar'
            last_common_sep = i
        elif i == 0:
            # We get here if `to` is the root.
            # For example: from='/foo/bar'; to='/'
            last_common_sep = 0

    out = ""

    # Generate the relative path based on the path difference between `to`
    # and `from`.
    for i in range(from_start + last_common_sep + 1, from_end + 1):
        if i == from_end or from_path[i] == "/":
            out += ".." if len(out) == 0 else "/.."

    # Lastly, append the rest of the destination (`to`) path that comes after
    # the common path parts.
    return out + to_path[to_start + last_common_sep:]

def wd_test_relative_path(wd_test_file, file):
    """
    Generates a path that can be used in a .wd-test file to refer to another file.

    Paths are relative to the .wd-test file.
    """
    return relative_path(paths.dirname(wd_test_file.short_path), file.short_path)

def is_in_resources_directory(file):
    """
    True if a given file is in the resources/ directory of a WPT module.
    """
    immediate_parent = paths.basename(file.dirname)
    return immediate_parent == "resources"

def test_case_name(filename):
    """
    Converts a JS filename to a valid JS identifier for use as a test case name.

    WPT files are named with the convention some-words-with-hyphens.some-suffix.js.
    We would turn this into someWordsWithHyphensSomeSuffix.
    """

    words = (filename
        .removesuffix(".js")
        .replace(".", "-")
        .replace("/", "-")
        .split("-"))

    return words[0] + "".join([word.capitalize() for word in words[1:]])
