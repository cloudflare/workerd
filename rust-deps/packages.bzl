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
    # TODO: Unpinning this breaks on windows for unknown reasons
    # because the output command is too long.
    "proc-macro-hack": crate.spec(
        version = "=0.5.19"
    )
}

PACKAGES = RTTI_PACKAGES | {
    "lolhtml": crate.spec(
        git = "https://github.com/cloudflare/lol-html.git",
        rev = "7967765ff8db27000845ba0a0a9a025ac908a043",
    ),
}
