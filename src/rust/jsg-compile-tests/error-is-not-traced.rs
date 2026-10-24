// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A `jsg::Error` may hold what JavaScript threw in a strong `Global`, so like a `Global` it
// is not `Traced`: a resource that kept one would keep a cycle through the thrown value alive.
use std::cell::Cell;

use jsg_macros::jsg_resource;

#[jsg_resource] //~ ERROR E0277
struct Holder {
    last_error: Cell<Option<jsg::Error>>,
}

#[jsg_resource]
impl Holder {}
