// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A `jsg::Rc` or a `TracedGlobal` moved out of a `std::cell::Cell` or `std::cell::RefCell`
// after GC visited it there is neither strong nor traced. So those cells are `Traced` only for
// types that hold no GC references (`jsg::HoldsNoGcReferences`); a reference that a resource
// takes out of a field lives in a `jsg::Cell` or `jsg::RefCell`, which make it strong on the
// way out.
use std::cell::Cell;
use std::cell::RefCell;

use jsg::v8;
use jsg_macros::jsg_resource;

#[jsg_resource]
struct Leaf;

#[jsg_resource]
impl Leaf {}

#[jsg_resource] //~ ERROR E0277
struct RcInCell {
    pending: Cell<Option<jsg::Rc<Leaf>>>,
}

#[jsg_resource]
impl RcInCell {}

#[jsg_resource] //~ ERROR E0277
struct RcInRefCell {
    queue: RefCell<Vec<jsg::Rc<Leaf>>>,
}

#[jsg_resource]
impl RcInRefCell {}

#[jsg_resource] //~ ERROR E0277
struct GlobalInCell {
    callback: Cell<Option<v8::TracedGlobal<v8::Value>>>,
}

#[jsg_resource]
impl GlobalInCell {}

#[jsg_resource] //~ ERROR E0277
struct GlobalInRefCell {
    handles: RefCell<Vec<v8::TracedGlobal<v8::Value>>>,
}

#[jsg_resource]
impl GlobalInRefCell {}

// A `Lenient` holds a `TracedGlobal` for a value it could not convert.
#[jsg_resource] //~ ERROR E0277
struct LenientInCell {
    value: Cell<jsg::Lenient<String>>,
}

#[jsg_resource]
impl LenientInCell {}

// A `jsg::Cell` holding a `jsg::Rc` would be moved out whole, with its weak contents.
#[jsg_resource] //~ ERROR E0277
struct JsgCellInCell {
    nested: Cell<jsg::Cell<Option<jsg::Rc<Leaf>>>>,
}

#[jsg_resource]
impl JsgCellInCell {}
