// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A `#[jsg_struct]` without fields converts to an empty object, without a warning.
#![deny(warnings)]

#[jsg_macros::jsg_struct]
pub struct Empty {}

pub fn t<'s>(lock: &mut jsg::Lock<'s>) -> jsg::Result<jsg::v8::Local<'s, jsg::v8::Value>> {
    jsg::ToJS::to_js(Empty {}, lock)
}
