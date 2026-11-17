// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// TypeScript customizations are string literals. An invalid one is the only error reported:
// the types still implement `TypeInfo`, so describing them adds no missing-trait error.

use jsg::TypeInfo;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_struct;

#[jsg_resource(ts_override = Example)] //~ ERROR expected a string literal of TypeScript
struct Example;

#[jsg_resource]
impl Example {}

#[jsg_struct(ts_define = "type A = 1;", ts_define = "type B = 2;")] //~ ERROR duplicate `ts_define`
struct Data {
    pub value: String,
}

pub fn describe(cx: &mut jsg::rtti::Context) {
    Example::describe(cx);
    Data::describe(cx);
}
