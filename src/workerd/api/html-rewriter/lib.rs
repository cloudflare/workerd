// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The native handle of the `HTMLRewriter` that the per-isolate bootstrap implements in
//! TypeScript: lol-html, bound through its Rust API.
//!
//! The handle does what needs lol-html, and the facade everything else: the API's classes,
//! argument checks, calling the user's handlers, and streams. The facade gets:
//!
//! - [`HtmlSelector`]s, parsed by [`parse_selector`] when `HTMLRewriter.on()` is called, so that
//!   an invalid selector throws there;
//! - an [`HtmlRewriterHandle`] per `transform()`, from [`new_rewriter`], to which it writes the
//!   document and which returns the output, from the call or, when lol-html runs on a fiber so
//!   that handlers can return promises, from a promise;
//! - a token handle for each handler lol-html runs ([`tokens`]), valid while the handler runs.
//!
//! It gets them from the static methods of [`HtmlRewriterUtils`], whose constructor function C++
//! gets with `new_utils()`. None of these is a global, and none has a constructor JavaScript can
//! call.

mod ffi;
mod fiber;
mod rewriter;
mod slot;
pub mod tokens;
mod utils;

pub use rewriter::DocumentHandlers;
pub use rewriter::ElementHandlers;
pub use rewriter::HandlerKind;
pub use rewriter::HtmlRewriterHandle;
pub use rewriter::HtmlSelector;
pub use rewriter::new_rewriter;
pub use rewriter::parse_selector;
pub use utils::HtmlRewriterUtils;

/// The `TypeError` C++ `HTMLRewriter` throws for an error lol-html reports.
fn parser_error(error: impl std::fmt::Display) -> jsg::Error {
    jsg::Error::new_type_error(format!("Parser error: {error}"))
}
