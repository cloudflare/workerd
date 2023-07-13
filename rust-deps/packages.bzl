load("@rules_rust//crate_universe:defs.bzl", "crate")

RTTI_PACKAGES = {
    # Crates used for RTTI parameter extraction
    "anyhow": crate.spec(
        features = ["default"],
        version = "1",
    ),
    "clang-ast": crate.spec(
        version = "0.1",
    ),
    "flate2": crate.spec(
        version = "1.0.26",
    ),
    "serde": crate.spec(
        version = "1.0",
        features = ["default", "derive"],
    ),
    "serde_json": crate.spec(
        version = "1.0",
        features = ["default"],
    ),
    "pico-args": crate.spec(
        version = "0.5",
    ),
}

PACKAGES = RTTI_PACKAGES | {
    "lolhtml": crate.spec(
        git = "https://github.com/cloudflare/lol-html.git",
        rev = "2681dcf0b3e6907111565199df8c43cc9aab7fe8",
    ),
}
