// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The generic form of `jsg_traced!` gives `X<T>` a `trace` that visits nothing, so it covers
// `X<T>` only where `T` holds no GC references. Otherwise a `TracedGlobal` or `jsg::Rc` inside
// it would never be visited: it would stay strong, and a cycle through it would never be
// collected.
use jsg::v8;
use jsg_macros::jsg_resource;

pub struct Slot<T>(T);
jsg::jsg_traced!(<T> Slot<T>);

#[jsg_resource]
struct Leaf;

#[jsg_resource]
impl Leaf {}

#[jsg_resource] //~ ERROR E0277
struct GlobalInSlot {
    slot: Slot<v8::TracedGlobal<v8::Value>>,
}

#[jsg_resource]
impl GlobalInSlot {}

#[jsg_resource] //~ ERROR E0277
struct RcInSlot {
    slot: Slot<jsg::Rc<Leaf>>,
}

#[jsg_resource]
impl RcInSlot {}

// Plain data is still covered, and so can sit in a std cell.
#[jsg_resource]
struct NumberInSlot {
    slot: std::cell::Cell<Option<Slot<u32>>>,
}

#[jsg_resource]
impl NumberInSlot {}
