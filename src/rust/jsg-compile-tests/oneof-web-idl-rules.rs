// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A #[jsg_oneof] union follows Web IDL's rules for unions, as C++ JSG's
// webidl::UnionTypeValidator checks a kj::OneOf at compile time: at most one type of each kind
// that a value can't tell apart, counting the types of nested unions and nullable types.

use jsg::Dict;
use jsg::Nullable;
use jsg::v8;
use jsg_macros::jsg_oneof;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_struct;

#[jsg_struct]
pub struct Point {
    pub x: f64,
}

#[jsg_resource]
pub struct Thing;

#[jsg_resource]
impl Thing {}

#[jsg_oneof] //~ ERROR E0080: at most one boolean type
pub enum TwoBooleans {
    Bool(bool),
    Strict(jsg::NonCoercible<bool>),
}

#[jsg_oneof] //~ ERROR E0080: at most one numeric type
pub enum TwoNumerics {
    Number(f64),
    Integer(u32),
}

#[jsg_oneof] //~ ERROR E0080: at most one string type
pub enum TwoStrings<'s> {
    String(String),
    Handle(v8::Local<'s, v8::String>),
}

#[jsg_oneof] //~ ERROR E0080: an object type only if
pub enum ObjectAndResource<'s> {
    Object(v8::Local<'s, v8::Object>),
    Thing(jsg::Rc<Thing>),
}

#[jsg_oneof] //~ ERROR E0080: an object type only if
pub enum ObjectAndRecord<'s> {
    Object(v8::Local<'s, v8::Object>),
    Dict(Dict<f64>),
}

#[jsg_oneof] //~ ERROR E0080: an object type only if
pub enum ObjectAndSequence<'s> {
    Object(v8::Local<'s, v8::Object>),
    Sequence(Vec<f64>),
}

#[jsg_oneof] //~ ERROR E0080: at most one dictionary-like type
pub enum DictionaryAndRecord {
    Point(Point),
    Dict(Dict<f64>),
}

#[jsg_oneof] //~ ERROR E0080: at most one sequence-like type
pub enum TwoSequences {
    Numbers(Vec<f64>),
    Strings(Vec<String>),
}

#[jsg_oneof] //~ ERROR E0080: at most one nullable or dictionary type
pub enum NullableAndDictionary {
    Number(Nullable<f64>),
    Point(Point),
}

#[jsg_oneof] //~ ERROR E0080: at most one nullable or dictionary type
pub enum TwoNullables {
    Number(Nullable<f64>),
    Integer(Nullable<String>),
}

#[jsg_oneof] //~ ERROR E0080: may not contain Option<T> or Lenient<T>
pub enum Optional {
    Number(Option<f64>),
    String(String),
}

#[jsg_oneof]
pub enum StringOrBool {
    String(String),
    Bool(bool),
}

// The nested union's string type counts as the outer union's.
#[jsg_oneof] //~ ERROR E0080: at most one string type
pub enum NestedStrings {
    String(String),
    Nested(StringOrBool),
}
