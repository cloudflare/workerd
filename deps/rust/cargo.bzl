"""workerd rust crate dependencies analogous to Cargo.toml file.
"""

load("@rules_rust//crate_universe:defs.bzl", "crate")

RTTI_PACKAGES = {
    # Crates used for RTTI parameter extraction
    "anyhow": crate.spec(
        version = "1",
    ),
    "clang-ast": crate.spec(
        version = "0.1",
    ),
    "flate2": crate.spec(
        version = "1.0",
    ),
    "serde": crate.spec(
        version = "1.0",
        features = ["derive"],
    ),
    "serde_json": crate.spec(
        version = "1.0",
    ),
    "pico-args": crate.spec(
        version = "0.5",
    ),
}

PACKAGES = RTTI_PACKAGES | {
    "lolhtml": crate.spec(
        git = "https://github.com/cloudflare/lol-html.git",
        rev = "4f8becea13a0021c8b71abd2dcc5899384973b66",
    ),
    "capnp": crate.spec(version = "0.20"),
    "capnpc": crate.spec(version = "0.20"),
}
