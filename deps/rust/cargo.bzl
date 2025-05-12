"""workerd rust crate dependencies analogous to Cargo.toml file.
"""

load("@rules_rust//crate_universe:defs.bzl", "crate")
load("@workerd-cxx//third-party:cargo.bzl", WORKERD_CXX_PACKAGES = "PACKAGES")

# We prefer single-digit dependencies to stay up to date as much as possible
PACKAGES = WORKERD_CXX_PACKAGES | {
    # When adding packages here, please only enable features as needed to keep compile times and
    # binary sizes bounded.
    "anyhow": crate.spec(version = "1"),
    "capnp": crate.spec(version = "0"),
    "capnpc": crate.spec(version = "0"),
    "clang-ast": crate.spec(version = "0"),
    "clap": crate.spec(version = "4", default_features = False, features = ["derive", "std", "help"]),
    "codespan-reporting": crate.spec(version = "0"),
    "flate2": crate.spec(version = "1"),
    "lol_html_c_api": crate.spec(git = "https://github.com/cloudflare/lol-html.git", rev = "41960f9bb073e34c476516b878123076f8aae182"),
    "nix": crate.spec(version = "0"),
    "pico-args": crate.spec(version = "0"),
    "quote": crate.spec(version = "1"),
    "ruff_python_ast": crate.spec(git = "https://github.com/astral-sh/ruff.git", tag = "0.11.5"),
    "ruff_python_parser": crate.spec(git = "https://github.com/astral-sh/ruff.git", tag = "0.11.5"),
    "serde_json": crate.spec(version = "1"),
    "serde": crate.spec(version = "1", features = ["derive"]),
    "syn": crate.spec(version = "2"),
    "thiserror": crate.spec(version = "2"),
    # tokio is huge, let's enable only features when we actually need them.
    "tokio": crate.spec(version = "1", default_features = False, features = ["net", "rt", "rt-multi-thread", "time"]),
    "tracing": crate.spec(version = "0", default_features = False, features = ["std"]),
}
