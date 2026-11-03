// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Every owner of a shared `std::rc::Rc` would trace what it holds, but a GC reference has a
// single parent. So `std::rc::Rc` is `Traced` only for types that hold no GC references
// (`jsg::HoldsNoGcReferences`); state shared with another owner lives in a resource, reached
// through a `jsg::Rc` or `jsg::Weak`.
use jsg::v8;
use jsg_macros::jsg_resource;

#[jsg_resource]
struct Leaf;

#[jsg_resource]
impl Leaf {}

#[jsg_resource] //~ ERROR E0277
struct SharedRc {
    leaf: std::rc::Rc<jsg::Rc<Leaf>>,
}

#[jsg_resource]
impl SharedRc {}

#[jsg_resource] //~ ERROR E0277
struct SharedGlobal {
    callback: std::rc::Rc<v8::TracedGlobal<v8::Value>>,
}

#[jsg_resource]
impl SharedGlobal {}
