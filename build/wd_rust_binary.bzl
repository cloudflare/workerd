load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load("@rules_rust//rust:defs.bzl", "rust_binary", "rust_test")
load("@workerd//:build/wd_rust_crate.bzl", "rust_cxx_bridge", "rust_cxx_include_prefix")

def _coverage_runtime_objects_impl(ctx):
    # Bazel's LLVM coverage collector (collect_cc_coverage.sh) runs `llvm-cov export` over exactly
    # the binaries named in the *runtime_objects_list.txt files among a test's coverage metadata.
    # cc_binary writes that file for its executable; rust_binary does not, so a Rust binary that a
    # test spawns (workerd, under every wd_test) would write its .profraw for nothing: no line linked
    # into it, C++ or Rust, would reach the report. This rule writes the file, in Bazel's format (the
    # executable's exec path), and the binary picks it up as coverage metadata through `link_deps`. The
    # path is computed rather than taken from the binary, which cannot be a dependency of its own
    # dependency; it is the same computation Bazel makes for the binary's output.
    #
    # The collector also needs GENERATE_LLVM_LCOV and the LLVM tools' paths in the test's
    # environment, which cc tests get from the C++ toolchain and other tests get from the coverage
    # config in .bazelrc.
    prefix = ctx.bin_dir.path
    if ctx.label.workspace_root:
        prefix += "/" + ctx.label.workspace_root
    exec_path = "{}/{}/{}".format(prefix, ctx.label.package, ctx.attr.binary_name)
    out = ctx.actions.declare_file(ctx.attr.binary_name + "runtime_objects_list.txt")
    ctx.actions.write(out, exec_path + "\n")
    return [
        # Lets rust_binary accept this target in `link_deps`; there is nothing to link.
        CcInfo(),
        coverage_common.instrumented_files_info(ctx, metadata_files = [out]),
    ]

_coverage_runtime_objects = rule(
    implementation = _coverage_runtime_objects_impl,
    attrs = {
        "binary_name": attr.string(mandatory = True),
    },
)

def wd_rust_binary(
        name,
        srcs = None,
        crate_root = None,
        deps = [],
        link_deps = [],
        proc_macro_deps = [],
        data = [],
        rustc_env = {},
        malloc = None,
        tool = False,
        test = True,
        visibility = None,
        tags = [],
        cxx_bridge_src = None,
        cxx_bridge_deps = [],
        cxx_bridge_hdrs = None,
        test_size = "small"):
    """Define rust binary.

    Args:
        name: crate name.
        srcs: crate sources; defaults to every .rs file in the package. Name them explicitly when
            the package also holds other crates or C++.
        crate_root: the crate's root module, if not main.rs or <name>.rs.
        deps: crate dependencies: rust crates.
        link_deps: c/c++ libraries to link with the rust binary
        visibility: crate visibility.
        data: additional data files.
        proc_macro_deps: proc_macro dependencies.
        rustc_env: additional rustc environment variables,
        malloc: the malloc implementation to link, if not the default.
        tool: True for a binary where performance matters: linked with linkopts_tool and given a
            <name>_cross alias, as wd_cc_binary does. False for development tools and tests
            (linkopts_default).
        test: whether to define a <name>_test target for the binary's own tests.
        tags: rule tags
        cxx_bridge_hdrs: headers the bridge include!()s; defaults to every .h file in the package.
    """
    if srcs == None:
        srcs = native.glob(["**/*.rs"])
    crate_name = name.replace("-", "_")

    if cxx_bridge_src:
        hdrs = cxx_bridge_hdrs
        if hdrs == None:
            hdrs = native.glob(["**/*.h"], allow_empty = True)

        rust_cxx_bridge(
            name = name + "@cxx",
            src = cxx_bridge_src,
            hdrs = hdrs,
            include_prefix = rust_cxx_include_prefix(),
            strip_include_prefix = "",
            # Not applying visibility here – if you import the cxxbridge header, you will likely
            # also need the rust library itself to avoid linker errors.
            deps = cxx_bridge_deps + [
                "//src/rust/cxx:core",
            ],
        )

        deps.append("//src/rust/cxx:cxx")
        link_deps = link_deps + [name + "@cxx"]

    binary_kwargs = {}
    if crate_root != None:
        binary_kwargs["crate_root"] = crate_root
    if malloc != None:
        binary_kwargs["malloc"] = malloc

    # Coverage from tests that run this binary; see _coverage_runtime_objects_impl.
    _coverage_runtime_objects(
        name = name + ".coverage_objects",
        binary_name = name,
        visibility = ["//visibility:private"],
    )
    link_deps = link_deps + [name + ".coverage_objects"]

    rust_binary(
        name = name,
        crate_name = crate_name,
        srcs = srcs,
        rustc_env = rustc_env,
        deps = deps,
        link_deps = link_deps + [
            "//build/deps:linkopts_tool" if tool else "//build/deps:linkopts_default",
            "@@//deps:rust_runtime",
        ],
        visibility = visibility,
        data = data,
        experimental_use_cc_common_link = 1,
        proc_macro_deps = proc_macro_deps,
        # linkopts_tool links with full optimization, so it is given more CPUs.
        tags = tags + ["cpu:8" if tool else "cpu:4"],
        target_compatible_with = select({
            "@//build/config:no_build": ["@platforms//:incompatible"],
            "//conditions:default": [],
        }),
        **binary_kwargs
    )

    # Same as wd_cc_binary: lets cross builds substitute a prebuilt binary. Only production
    # binaries have prebuilts (the substitute target must exist in the root repository of the
    # cross build), so only they get the alias; build-time tools run on the host.
    if tool:
        pkg = native.package_name().removeprefix("src/")
        native.alias(
            name = name + "_cross",
            visibility = visibility,
            actual = select({
                "@//build/config:prebuilt_binaries_arm64": "@//:bin.arm64/tmp/{}/{}.aarch64-linux-gnu".format(pkg, name),
                "//conditions:default": name,
            }),
        )

    if not test:
        return

    rust_test(
        name = name + "_test",
        crate = ":" + name,
        env = {
            "RUST_BACKTRACE": "1",
            # rust test runner captures stderr by default, which makes debugging tests very hard
            "RUST_TEST_NOCAPTURE": "1",
            # our tests are usually very heavy and do not support concurrent invocation
            "RUST_TEST_THREADS": "1",
        },
        target_compatible_with = select({
            "@//build/config:no_build": ["@platforms//:incompatible"],
            "//conditions:default": [],
        }),
        experimental_use_cc_common_link = 1,
        link_deps = ["//build/deps:linkopts_default", "@@//deps:rust_runtime"],
        size = test_size,
        # Tag with cpu:4 since this target depends on linkopts_default.
        tags = ["no-coverage", "cpu:4"],
    )
