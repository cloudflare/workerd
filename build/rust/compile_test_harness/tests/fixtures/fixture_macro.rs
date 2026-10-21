// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A proc macro that reads its runtime data while it expands. Two crates are built from this file,
//! each with its own data file, named after the crate.

use proc_macro::TokenStream;

/// Expands to a string literal holding the contents of `<crate name>.txt`, or to a
/// `compile_error!` if the macro cannot read it.
#[proc_macro]
pub fn data_str(_input: TokenStream) -> TokenStream {
    // rustc runs in the execution root, which `file!()` is relative to.
    let path =
        std::path::Path::new(file!()).with_file_name(concat!(env!("CARGO_CRATE_NAME"), ".txt"));
    let expansion = match std::fs::read_to_string(&path) {
        Ok(data) => format!("{data:?}"),
        Err(error) => {
            let message = format!("{}: {error}", path.display());
            format!("compile_error!({message:?})")
        }
    };
    expansion.parse().unwrap_or_default()
}
