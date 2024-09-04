load("@bazel_skylib//rules:write_file.bzl", "write_file")
load("//:build/wd_test.bzl", "wd_test")

# 0 - name of the service
# 1 - es module file path (ex: url-test for url-test.js)
WPT_TEST_TEMPLATE = """
using Workerd = import "/workerd/workerd.capnp";

const unitTests :Workerd.Config = (
  services = [
    ( name = "{}",
      worker = (
        modules = [
          (name = "worker", esModule = embed "{}.js"),
          (name = "harness",
           esModule = embed "../../../../../workerd/src/wpt/harness.js"),
          (name = "url-origin.any.js",
           esModule = embed "../../../../../wpt/url/url-origin.any.js"),
          (name = "url-constructor.any.js",
           esModule = embed "../../../../../wpt/url/url-constructor.any.js"),
          (name = "resources/urltestdata.json",
           json = embed "../../../../../wpt/url/resources/urltestdata.json"),
          (name = "resources/urltestdata-javascript-only.json",
           json = embed "../../../../../wpt/url/resources/urltestdata-javascript-only.json"),
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
def generate_wd_test_file(name):
    return WPT_TEST_TEMPLATE.format(name, name)

def gen_wpt_tests(files):
    for file in files:
        name = file.removesuffix(".js")
        src = "{}.wd-test".format(name)
        write_file(
            name = name + "@rule",
            out = src,
            content = [generate_wd_test_file(name)],
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
