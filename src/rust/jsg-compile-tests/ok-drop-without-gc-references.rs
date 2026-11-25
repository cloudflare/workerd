// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A resource whose fields hold no GC references may implement `Drop`, and so may a field's type,
// as `jsg::Rc` and `Vec` do.
#![deny(warnings)]

use jsg_macros::jsg_resource;
use jsg_macros::jsg_traced;

#[jsg_traced]
pub struct Binding(pub Vec<u8>);

impl Drop for Binding {
    fn drop(&mut self) {}
}

#[jsg_resource]
pub struct Plain {
    name: String,
    count: u32,
    bytes: Vec<u8>,
}

impl Drop for Plain {
    fn drop(&mut self) {}
}

#[jsg_resource]
impl Plain {}

#[jsg_resource]
pub struct Empty;

impl Drop for Empty {
    fn drop(&mut self) {}
}

#[jsg_resource]
impl Empty {}

#[jsg_resource]
pub struct HoldsBinding {
    binding: Binding,
}

#[jsg_resource]
impl HoldsBinding {}

#[jsg_resource]
pub struct HoldsRcs {
    children: Vec<jsg::Rc<Empty>>,
}

#[jsg_resource]
impl HoldsRcs {}
