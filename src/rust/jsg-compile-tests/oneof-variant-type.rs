// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A #[jsg_oneof] variant is a type that converts from JavaScript and that C++ JSG's kj::OneOf
// could hold.

use jsg_macros::jsg_oneof;

#[jsg_oneof] //~ ERROR E0277
pub enum FloatOrString {
    Float(f64), //~ ERROR cannot be a `#[jsg_oneof]` variant
    //~| ERROR E0277
    String(String),
}
