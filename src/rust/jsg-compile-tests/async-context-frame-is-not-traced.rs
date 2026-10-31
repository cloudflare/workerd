// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// An `AsyncContextFrame` holds a strong `Global`, so a cycle through it is never collected. It is
// not `Traced`, so it cannot be a field of a resource that GC traces; a resource holds a
// `TracedAsyncContextFrame`.
use std::cell::Cell;

use jsg_macros::jsg_resource;

#[jsg_resource] //~ ERROR E0277
struct Holder {
    frame: Cell<Option<jsg::AsyncContextFrame>>,
}

#[jsg_resource]
impl Holder {}
