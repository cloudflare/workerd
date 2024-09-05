load("@bazel_skylib//rules:write_file.bzl", "write_file")
load("//:build/wd_test.bzl", "wd_test")

# 0 - name of the service
# 1 - es module file path (ex: url-test for url-test.js)
# 2 - external modules required for the test suite to succeed
WPT_TEST_TEMPLATE = """
using Workerd = import "/workerd/workerd.capnp";

const unitTests :Workerd.Config = (
  services = [
    ( name = "{}",
      worker = (
        modules = [
          (name = "worker", esModule = embed "{}.js"),
          (name = "harness", esModule = embed "../../../../../workerd/src/wpt/harness.js"),
          {}
        ],
        bindings = [
          (name = "wpt", service = "wpt"),
        ],
        compatibilityDate = "2024-07-01",
        compatibilityFlags = ["nodejs_compat_v2"],
      )
    ),
    (
      name = "wpt",
      disk = ".",
    )
  ],
);"""

# Example: generate_wd_test_file("url-test")
def generate_wd_test_file(name, modules = ""):
    return WPT_TEST_TEMPLATE.format(name, name, modules)

def generate_external_modules(directory):
    """
    Generates a string for all files in the given directory in the specified format.
    Example for a JS file:
        (name = "url-origin.any.js", esModule = embed "../../../../../wpt/url/url-origin.any.js"),
    Example for a JSON file:
        (name = "resources/urltestdata.json", json = embed "../../../../../wpt/url/resources/urltestdata.json"),
    """
    files = native.glob([directory + "/**/*"], allow_empty = True)
    result = []

    for file in files:
        file_name = file.split("/")[-1]
        file_path = "../" * 5 + file  # Creates the "../../../../../" prefix

        if file_name.endswith(".js"):
            entry = """(name = {}, esModule = embed "{}"),""".format(file_name, file_path)
        elif file_name.endswith(".json"):
            entry = """(name = {}, json = embed "{}"),""".format(file_name, file_path)
        else:
            # For other file types, you can add more conditions or skip them
            continue

        result.append(entry)

    return result.join("")

def gen_wpt_tests(files):
    for file in files:
        # For url-test.js, it should be url.
        # We'll use this to check wpt/ folder and load necessary files.
        wpt_directory = file.removesuffix("-test.js")
        name = file.removesuffix(".js")
        src = "{}.wd-test".format(name)
        modules = generate_external_modules("@wpt//:" + wpt_directory)
        write_file(
            name = name + "@rule",
            out = src,
            content = [generate_wd_test_file(name, modules)],
        )
        wd_test(
            name = name,
            src = src,
            args = ["--experimental"],
            data = [
                file,
                "//src/wpt:wpt-test-harness",
                "@wpt//:url",
            ],
        )
