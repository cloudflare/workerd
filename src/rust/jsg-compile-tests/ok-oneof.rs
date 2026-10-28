// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The unions that Web IDL allows compile, including several resources, a handle of any type with
// a dictionary, and an object handle with primitives.

use jsg::Nullable;
use jsg::Number;
use jsg::v8;
use jsg_macros::jsg_oneof;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_struct;

#[jsg_struct]
pub struct Point {
    pub x: Number,
}

#[jsg_resource]
pub struct First;

#[jsg_resource]
impl First {}

#[jsg_resource]
pub struct Second;

#[jsg_resource]
impl Second {}

#[jsg_oneof]
pub enum Everything<'s> {
    First(jsg::Rc<First>),
    Second(jsg::Rc<Second>),
    Point(Point),
    Numbers(Vec<Number>),
    Bytes(v8::Local<'s, v8::Uint8Array>),
    String(String),
    Number(i32),
    Bool(bool),
    Date(std::time::SystemTime),
}

#[jsg_oneof]
pub enum Handles<'s> {
    Object(v8::Local<'s, v8::Object>),
    String(Nullable<String>),
    Number(v8::Local<'s, v8::BigInt>),
    Function(v8::Local<'s, v8::Function>),
}

#[jsg_oneof]
pub enum AnyOrPoint<'s> {
    Any(v8::Local<'s, v8::Value>),
    Point(Point),
}
