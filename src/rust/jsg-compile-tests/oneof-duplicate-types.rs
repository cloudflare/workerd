// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A #[jsg_oneof] union may not contain the same type twice, as C++ JSG's
// webidl::UnionTypeValidator requires of a kj::OneOf, even of a kind it may have more than one
// of, such as resources. As there, a nullable type counts as its inner type.

use jsg::Nullable;
use jsg_macros::jsg_oneof;
use jsg_macros::jsg_resource;

#[jsg_resource]
pub struct Thing;

#[jsg_resource]
impl Thing {}

type SameThing = jsg::Rc<Thing>;

#[jsg_oneof]
pub enum SameResourceTwice {
    First(jsg::Rc<Thing>),
    Second(SameThing), //~ ERROR E0119
}

#[jsg_oneof]
pub enum ResourceAndNullableResource {
    Thing(jsg::Rc<Thing>),
    Nullable(Nullable<jsg::Rc<Thing>>), //~ ERROR E0119
}
