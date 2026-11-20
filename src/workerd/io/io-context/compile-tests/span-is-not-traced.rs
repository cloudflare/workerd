// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A trace span belongs to its request's event loop, and the garbage collector may drop a resource
// off it, which would abort the process, so a span is not `Traced`: a resource holds one only
// through `IoContext::add_span()`.
use io_context::SpanBuilder;
use jsg_macros::jsg_resource;

#[jsg_macros::jsg_traced]
pub struct Spanned(SpanBuilder); //~ ERROR E0277

#[jsg_resource] //~ ERROR E0277
pub struct SpanResource {
    span: SpanBuilder,
}

#[jsg_resource]
impl SpanResource {}
