// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// NonCoercible<T> takes only the types a value is otherwise coerced to (String, bool,
// jsg::Number and the integers), as C++ JSG's takes only a CoercibleType. A union or a resource
// would convert a value as it always does, coercion included, once its JavaScript type passed a
// coarse check.

use jsg::FromJS;
use jsg::NonCoercible;
use jsg_macros::jsg_oneof;
use jsg_macros::jsg_resource;

#[jsg_resource]
pub struct Thing;

#[jsg_resource]
impl Thing {}

#[jsg_oneof]
pub enum ThingOrString {
    Thing(jsg::Rc<Thing>),
    String(String),
}

pub fn union_name() -> String {
    <NonCoercible<ThingOrString> as FromJS<'static>>::type_name() //~ ERROR E0277
}

pub fn resource_name() -> String {
    <NonCoercible<jsg::Rc<Thing>> as FromJS<'static>>::type_name() //~ ERROR E0277
}
