// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A resource type has at most one `[Symbol.iterator]()`, a method that takes `&self` or
// `this: jsg::Rc<Self>`, and `#[jsg_iterable]` takes no arguments. `inherit_intrinsic` names
// one `jsg::Intrinsic` variant, and is for automatic registration only.

use jsg_macros::jsg_iterable;
use jsg_macros::jsg_resource;

#[jsg_resource]
pub struct Twice;

#[jsg_resource]
impl Twice {
    #[jsg_iterable]
    pub fn first(&self) {}

    #[jsg_iterable]
    pub fn second(&self) {}
    //~^ ERROR a resource type has at most one #[jsg_iterable]
}

#[jsg_resource]
pub struct Static;

#[jsg_resource]
impl Static {
    #[jsg_iterable]
    pub fn entries() {}
    //~^ ERROR a #[jsg_iterable] method takes `&self`, or `this: jsg::Rc<Self>` in its place
}

#[jsg_resource]
pub struct Mutable;

#[jsg_resource]
impl Mutable {
    #[jsg_iterable]
    pub fn entries(&mut self) {}
    //~^ ERROR a resource method takes `&self`
}

#[jsg_resource]
pub struct Arguments;

#[jsg_resource]
impl Arguments {
    #[jsg_iterable(name = "x")]
    //~^ ERROR #[jsg_iterable] takes no arguments
    pub fn entries(&self) {}
}

#[jsg_resource]
pub struct UnknownIntrinsic;

#[jsg_resource(inherit_intrinsic = ArrayPrototype)]
//~^ ERROR E0599
impl UnknownIntrinsic {}

#[jsg_resource]
pub struct CustomInherit;

#[jsg_resource(custom_members, inherit_intrinsic = IteratorPrototype)]
//~^ ERROR `inherit_intrinsic` cannot be combined with `custom_members`
//~| ERROR `custom_members` given here
impl CustomInherit {}

impl jsg::Resource for CustomInherit {
    fn members(
        _: &mut impl jsg::MemberRegistry,
        _: jsg::feature_flags::compatibility_flags::Reader<'_>,
    ) {
    }
}
