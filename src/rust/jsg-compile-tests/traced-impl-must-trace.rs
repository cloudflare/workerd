// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// `Traced::trace` has no default, so an impl can't skip a GC reference by leaving the method
// out. A type with nothing to trace uses `jsg_traced!`.
use jsg_macros::jsg_resource;

#[jsg_resource]
struct Leaf;

#[jsg_resource]
impl Leaf {}

struct Holder {
    leaf: jsg::Rc<Leaf>,
}

impl jsg::Traced for Holder {} //~ ERROR E0046
