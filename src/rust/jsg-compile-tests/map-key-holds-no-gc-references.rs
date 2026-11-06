// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A map's `trace` visits its values but not its keys, so a `jsg::Rc` in a key would stay strong
// and leak any cycle through it. Keys must hold no GC references.
use std::collections::BTreeMap;
use std::collections::HashMap;

use jsg_macros::jsg_resource;

#[jsg_resource]
struct Leaf;

#[jsg_resource]
impl Leaf {}

/// The bound `#[jsg_resource]` puts on every field.
fn resource_field<T: jsg::Traced>() {}

pub fn keys() {
    resource_field::<HashMap<jsg::Rc<Leaf>, u32>>(); //~ ERROR E0277
    resource_field::<BTreeMap<jsg::Rc<Leaf>, u32>>(); //~ ERROR E0277
    resource_field::<HashMap<String, jsg::Rc<Leaf>>>();
}
