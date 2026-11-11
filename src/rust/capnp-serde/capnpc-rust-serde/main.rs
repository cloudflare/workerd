// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `capnpc-rust-serde`: a Cap'n Proto compiler plugin that generates Rust types with serde
//! derives, whose JSON is that of the C++ `capnp::JsonCodec` with `handleByAnnotation()`.
//!
//! The compiler runs it as `capnp compile -o path/to/capnpc-rust-serde:<out-dir> foo.capnp`, and
//! it writes `foo_serde.rs`, which a crate `include!`s and which depends on the `serde`,
//! `serde_json` and `capnp_serde` crates. Bazel's `wd_rust_serde_capnp_library()` does all of
//! this. See `codegen.rs` for the mapping from schema to Rust.

mod codegen;
mod names;

use std::io::Write as _;
use std::path::Path;
use std::process::ExitCode;

use anyhow::Context;
use anyhow::Result;
use capnp::message::ReaderOptions;
use capnp::schema_capnp::code_generator_request;

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            let _ = writeln!(std::io::stderr(), "capnpc-rust-serde: {error:#}");
            ExitCode::FAILURE
        }
    }
}

fn run() -> Result<()> {
    let mut options = ReaderOptions::new();
    // The request comes from the schema compiler, which is trusted.
    options.traversal_limit_in_words(None);
    let message = capnp::serialize::read_message(std::io::stdin().lock(), options)
        .context("reading the CodeGeneratorRequest")?;
    let request = message.get_root::<code_generator_request::Reader>()?;
    for (path, contents) in codegen::generate(request)? {
        let path = Path::new(&path);
        if let Some(dir) = path.parent() {
            std::fs::create_dir_all(dir).with_context(|| format!("creating {}", dir.display()))?;
        }
        std::fs::write(path, contents).with_context(|| format!("writing {}", path.display()))?;
    }
    Ok(())
}
