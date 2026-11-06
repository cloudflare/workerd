// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// `#[jsg_struct]` derives `HoldsNoGcReferences` from its fields, so a struct that holds a GC
// reference can't be given the marker by hand to get it into a std cell.
use jsg_macros::jsg_resource;
use jsg_macros::jsg_struct;

#[jsg_resource]
struct Leaf;

#[jsg_resource]
impl Leaf {}

#[jsg_struct] //~ ERROR E0119
pub struct Linked {
    pub leaf: jsg::Rc<Leaf>,
}

impl jsg::HoldsNoGcReferences for Linked {}
