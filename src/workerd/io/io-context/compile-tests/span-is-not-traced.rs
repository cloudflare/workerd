// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A trace span belongs to its request's thread: it holds KJ objects whose reference counts are
// not atomic. It is not `Send`, so no `Traced` type, and so no resource, can hold one.
pub struct Spanned(io_context::SpanBuilder);

impl jsg::Traced for Spanned {
    //~^ ERROR E0277
    fn trace(&self, _visitor: &mut jsg::v8::GcVisitor) {}
}
