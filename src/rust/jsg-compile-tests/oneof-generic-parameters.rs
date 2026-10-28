// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A #[jsg_oneof] union, like a kj::OneOf, has no type parameters. It may have one lifetime
// parameter, for the scope of the handles it holds.

use jsg::v8;
use jsg_macros::jsg_oneof;

#[jsg_oneof]
pub enum TypeParameter<T> {
    //~^ ERROR only one lifetime parameter
    Value(T),
    String(String),
}

#[jsg_oneof]
pub enum TwoLifetimes<'a, 'b> {
    //~^ ERROR only one lifetime parameter
    First(v8::Local<'a, v8::Object>),
    Second(v8::Local<'b, v8::String>),
}
