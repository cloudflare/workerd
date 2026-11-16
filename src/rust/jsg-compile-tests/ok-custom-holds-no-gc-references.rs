// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// `custom_holds_no_gc_references` keeps the generated `Traced` impl and leaves
// `HoldsNoGcReferences` to the author, which breaks the cycle of a struct that contains itself,
// directly or through another.
use jsg_macros::jsg_oneof;
use jsg_macros::jsg_struct;

#[jsg_struct(custom_holds_no_gc_references)]
pub struct TreeNode {
    pub value: f64,
    pub children: Vec<TreeNode>,
}

impl jsg::HoldsNoGcReferences for TreeNode {}

#[jsg_struct(custom_holds_no_gc_references)]
pub struct Parent {
    pub children: Vec<Child>,
}

impl jsg::HoldsNoGcReferences for Parent {}

#[jsg_struct]
pub struct Child {
    pub parents: Vec<Parent>,
}

#[jsg_oneof(custom_holds_no_gc_references)]
pub enum ParentOrNumber {
    Parent(Parent),
    Number(f64),
}

fn holds_no_gc_references<T: jsg::HoldsNoGcReferences>() {}
fn traced<T: jsg::Traced>() {}

pub fn check() {
    holds_no_gc_references::<std::cell::Cell<Option<TreeNode>>>();
    holds_no_gc_references::<std::cell::Cell<Option<Parent>>>();
    holds_no_gc_references::<std::cell::RefCell<Child>>();
    traced::<ParentOrNumber>();
}
