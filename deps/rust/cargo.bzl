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
    # Commit hash refers to lol-html v2.1.0. We then access the nested lol_html_c_api crate within.
    # TODO(npaun): The next release of lol-html could change the way we access the nested crate.
    # Check once https://github.com/cloudflare/lol-html/pull/247 is in a release.
    "lol_html_c_api": crate.spec(git = "https://github.com/cloudflare/lol-html.git", rev = "cac9f2f59aea8ad803286b0aae0d667926f441c7"),
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
}
