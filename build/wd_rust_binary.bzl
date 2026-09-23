load("@rules_rust//rust:defs.bzl", "rust_binary", "rust_test")
load("@workerd//:build/wd_rust_crate.bzl", "rust_cxx_bridge", "rust_cxx_include_prefix")

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
        tags = tags + ["cpu:4" if tool else "cpu:2"],
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
        # Tag with cpu:2 since this target depends on linkopts_default.
        tags = ["no-coverage", "cpu:2"],
    )
