// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! What the TypeScript `HTMLRewriter` gets from the crate: a class with one static method per
//! entry point, whose constructor function C++ gets with `new_utils()` in `ffi.rs`.
//!
//! It is the class, not an instance of it, so that the bootstrap's `utils` holds no resource: a
//! startup snapshot carries a function made from a resource's template, whose callbacks are
//! registered as external references, but no live resource that is neither stateless nor
//! clonable.

use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

use crate::DocumentHandlers;
use crate::ElementHandlers;
use crate::HtmlRewriterHandle;
use crate::HtmlSelector;

/// The entry points of the crate, for the TypeScript `HTMLRewriter`. It has no constructor
/// JavaScript can call, so no instance.
#[jsg_resource]
pub struct HtmlRewriterUtils {}

#[jsg_resource]
impl HtmlRewriterUtils {
    /// See [`crate::parse_selector`].
    #[jsg_method]
    pub fn parse_selector(
        lock: &mut jsg::Lock<'_>,
        selector: String,
    ) -> jsg::Result<jsg::Rc<HtmlSelector>> {
        crate::parse_selector(lock, &selector)
    }

    /// See [`crate::new_rewriter`].
    #[jsg_method]
    pub fn new_rewriter<'s>(
        lock: &mut jsg::Lock<'s>,
        element_handlers: Vec<ElementHandlers>,
        document_handlers: Vec<DocumentHandlers>,
        encoding: String,
        dispatcher: v8::Local<'s, v8::Function>,
    ) -> jsg::Result<jsg::Rc<HtmlRewriterHandle>> {
        crate::new_rewriter(
            lock,
            element_handlers,
            document_handlers,
            &encoding,
            dispatcher,
        )
    }
}
