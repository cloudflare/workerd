"""workerd rust crate dependencies analogous to Cargo.toml file.
"""

load("@rules_rust//crate_universe:defs.bzl", "crate")

# We prefer single-digit dependencies to stay up to date as much as possible
PACKAGES = {
    "anyhow": crate.spec(version = "1"),
    "capnp": crate.spec(version = "0"),
    "capnpc": crate.spec(version = "0"),
    "clang-ast": crate.spec(version = "0"),
    # our own cxx fork
    "cxx": crate.spec(git = "https://github.com/cloudflare/workerd-cxx.git"),
    "cxxbridge-cmd": crate.spec(git = "https://github.com/cloudflare/workerd-cxx.git"),
    "flate2": crate.spec(version = "1"),
    "lolhtml": crate.spec(git = "https://github.com/cloudflare/lol-html.git", rev = "4f8becea13a0021c8b71abd2dcc5899384973b66"),
    "nix": crate.spec(version = "0"),
    "pico-args": crate.spec(version = "0"),
    "serde_json": crate.spec(version = "1"),
    "serde": crate.spec(version = "1", features = ["derive"]),
    "tokio": crate.spec(version = "1", features = ["net", "process", "signal", "rt", "rt-multi-thread", "time"]),
    "tracing": crate.spec(version = "0"),
}
