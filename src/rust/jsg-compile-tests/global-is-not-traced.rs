// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A `Global` is always strong, so a cycle through it is never collected. It is not
// `Traced`, so it cannot be a field of a resource that GC traces; a resource holds a
// `TracedGlobal`.
use std::cell::Cell;

use jsg::v8;
use jsg_macros::jsg_resource;

#[jsg_resource] //~ ERROR E0277
struct Holder {
    handle: Cell<Option<v8::Global<v8::Value>>>,
}

#[jsg_resource]
impl Holder {}
