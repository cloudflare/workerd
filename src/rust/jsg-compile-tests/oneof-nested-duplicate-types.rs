// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A #[jsg_oneof] union may not contain a type that a union nested in it also contains, as C++
// JSG's webidl::FlattenedTypeTraits flattens a nested kj::OneOf before checking for duplicate
// types. These types are in no Web IDL category with a limit, so only their names catch them.

use std::time::SystemTime;

use jsg::Nullable;
use jsg::v8;
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

#[jsg_oneof] //~ ERROR E0080: may not contain the same type twice
pub enum ResourceInNestedUnion {
    Thing(jsg::Rc<Thing>),
    Nested(ThingOrString),
}

#[jsg_oneof]
pub enum NullableThingOrString {
    Thing(Nullable<jsg::Rc<Thing>>),
    String(String),
}

#[jsg_oneof] //~ ERROR E0080: may not contain the same type twice
pub enum ResourceInNullableInNestedUnion {
    Thing(jsg::Rc<Thing>),
    Nested(NullableThingOrString),
}

#[jsg_oneof]
pub enum BytesOrString<'s> {
    Bytes(v8::Local<'s, v8::Uint8Array>),
    String(String),
}

#[jsg_oneof] //~ ERROR E0080: may not contain the same type twice
pub enum HandleInNestedUnion<'s> {
    Bytes(v8::Local<'s, v8::Uint8Array>),
    Nested(BytesOrString<'s>),
}

#[jsg_oneof]
pub enum DateOrString {
    Date(SystemTime),
    String(String),
}

#[jsg_oneof]
pub enum DateOrBool {
    Date(SystemTime),
    Bool(bool),
}

#[jsg_oneof] //~ ERROR E0080: may not contain the same type twice
pub enum DateInTwoNestedUnions {
    First(DateOrString),
    Second(DateOrBool),
}
