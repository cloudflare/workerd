"""workerd rust crate dependencies analogous to Cargo.toml file.
"""

load("@rules_rust//crate_universe:defs.bzl", "crate")

# We prefer single-digit dependencies to stay up to date as much as possible
PACKAGES = {
    # When adding packages here, please only enable features as needed to keep compile times and
    # binary sizes bounded.
    "anyhow": crate.spec(version = "1"),
    "capnp": crate.spec(version = "0"),
    "capnpc": crate.spec(version = "0"),
    "clang-ast": crate.spec(version = "0"),
    "clap": crate.spec(version = "4", default_features = False, features = ["derive"]),
    "codespan-reporting": crate.spec(version = "0"),
    "cxx": crate.spec(version = "1"),
    "cxxbridge-cmd": crate.spec(version = "1"),
    "flate2": crate.spec(version = "1"),
    "lol_html_c_api": crate.spec(git = "https://github.com/cloudflare/lol-html.git", tag = "v2.2.0"),
    "nix": crate.spec(version = "0"),
    "pico-args": crate.spec(version = "0"),
    "proc-macro2": crate.spec(version = "1"),
    "quote": crate.spec(version = "1"),
    "ruff_python_ast": crate.spec(git = "https://github.com/astral-sh/ruff.git", tag = "v0.4.10"),
    "ruff_python_parser": crate.spec(git = "https://github.com/astral-sh/ruff.git", tag = "v0.4.10"),
    "serde_json": crate.spec(version = "1"),
    "serde": crate.spec(version = "1", features = ["derive"]),
    "syn": crate.spec(version = "2"),
    "thiserror": crate.spec(version = "2"),
    # tokio is huge, let's enable only features when we actually need them.
    "tokio": crate.spec(version = "1", default_features = False, features = ["net", "rt", "rt-multi-thread", "time"]),
    "tracing": crate.spec(version = "0", default_features = False, features = ["std"]),
    "v8": crate.spec(version = "134.5.0"),
}

ANNOTATIONS = {
    "v8": [
        crate.annotation(
            patches = [
                "@workerd//:patches/rusty-v8/0001-Clean-up-use-of-v8-internals.patch",
                "@workerd//:patches/rusty-v8/0002-removing-the-rest-of-external-api-usages.patch",
                "@workerd//:patches/rusty-v8/0003-massage-build.rs-for-workerd.patch",
            ],
            patch_args = ["-p1"],
            deps = [":rusty_v8"],
            # build_script_deps = ["@workerd-v8//:v8"],
            additive_build_file_content = """
                cc_library(
                    name = "rusty_v8",
                    deps = ["@workerd-v8//:v8"],
                    srcs = ["src/binding.cc"],
                    hdrs = ["src/support.h"],
                    strip_include_prefix = "src/",
                )
            """,
        ),
    ],
}
