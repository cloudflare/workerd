// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// `#[jsg_traced]` gives a struct a `trace` that visits nothing, so it requires each field to hold
// no GC references. Otherwise a `TracedGlobal` or `jsg::Rc` in a field would never be visited: it
// would stay strong, and a cycle through it would never be collected.
use jsg::v8;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_traced;

#[jsg_resource]
struct Leaf;

#[jsg_resource]
impl Leaf {}

#[jsg_traced]
struct HoldsGlobal {
    value: v8::TracedGlobal<v8::Value>, //~ ERROR E0277
}

#[jsg_traced]
struct HoldsRc(jsg::Rc<Leaf>); //~ ERROR E0277

// Plain data and other `#[jsg_traced]` types are covered, so the struct can sit in a std cell.
#[jsg_traced]
struct Plain;

#[jsg_traced]
struct Nested {
    name: String,
    plain: Option<Plain>,
    all: Vec<Plain>,
}

#[jsg_resource]
struct NestedInCell {
    nested: std::cell::RefCell<Nested>,
}

#[jsg_resource]
impl NestedInCell {}
