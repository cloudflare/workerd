//! The CXX code generator for constructing and compiling C++ code.
//!
//! This is intended as a mechanism for embedding the `cxx` crate into
//! higher-level code generators. See [dtolnay/cxx#235] and
//! [https://github.com/google/autocxx].
//!
//! [dtolnay/cxx#235]: https://github.com/dtolnay/cxx/issues/235
//! [https://github.com/google/autocxx]: https://github.com/google/autocxx

#![doc(html_root_url = "https://docs.rs/cxx-gen/0.7.151")]
#![deny(missing_docs)]
#![allow(dead_code)]
#![cfg_attr(not(check_cfg), allow(unexpected_cfgs))]
#![allow(
    clippy::cast_sign_loss,
    clippy::default_trait_access,
    clippy::enum_glob_use,
    clippy::inherent_to_string,
    clippy::items_after_statements,
    clippy::match_bool,
    clippy::match_same_arms,
    clippy::missing_errors_doc,
    clippy::must_use_candidate,
    clippy::needless_lifetimes,
    clippy::needless_pass_by_value,
    clippy::nonminimal_bool,
    clippy::redundant_else,
    clippy::ref_option,
    clippy::similar_names,
    clippy::single_match_else,
    clippy::struct_excessive_bools,
    clippy::struct_field_names,
    clippy::too_many_arguments,
    clippy::too_many_lines,
    clippy::toplevel_ref_arg,
    clippy::uninlined_format_args
)]

mod error;

pub use r#gen::CfgEvaluator;
pub use r#gen::CfgResult;
pub use r#gen::GeneratedCode;
pub use r#gen::Opt;
pub use r#gen::include::HEADER;
pub use r#gen::include::Include;
use proc_macro2::TokenStream;
pub use syntax::IncludeKind;

pub use crate::error::Error;

/// Generate C++ bindings code from a Rust token stream. This should be a Rust
/// token stream which somewhere contains a `#[cxx::bridge] mod {}`.
pub fn generate_header_and_cc(rust_source: TokenStream, opt: &Opt) -> Result<GeneratedCode, Error> {
    let syntax = syn::parse2(rust_source)
        .map_err(r#gen::Error::from)
        .map_err(Error::from)?;
    r#gen::generate(syntax, opt).map_err(Error::from)
}
