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
    "clap": crate.spec(version = "4", features = ["derive"]),
    "codespan-reporting": crate.spec(version = "0"),
    "cxx": crate.spec(version = "1"),
    "cxxbridge-cmd": crate.spec(version = "1"),
    "flate2": crate.spec(version = "1"),
    "lolhtml": crate.spec(git = "https://github.com/cloudflare/lol-html.git", rev = "4f8becea13a0021c8b71abd2dcc5899384973b66"),
    "nix": crate.spec(version = "0"),
    "pico-args": crate.spec(version = "0"),
    "proc-macro2": crate.spec(version = "1"),
    "quote": crate.spec(version = "1"),
    "serde_json": crate.spec(version = "1"),
    "serde": crate.spec(version = "1", features = ["derive"]),
    "syn": crate.spec(version = "2"),
    "tokio": crate.spec(version = "1", features = ["net", "process", "signal", "rt", "rt-multi-thread", "time"]),
    "tracing": crate.spec(version = "0", default_features = False, features = ["std"]),
}
