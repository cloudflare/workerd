// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// `#[jsg_method(fast)]` on a method whose types can't be fast fails to compile.
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

#[jsg_resource]
pub struct Resource {}

#[jsg_resource]
impl Resource {
    #[jsg_method(fast)]
    //~^ ERROR E0277
    pub fn length(&self, s: String) -> jsg::Number {
        jsg::Number::from(u32::try_from(s.len()).unwrap_or(0))
    }

    #[jsg_method(fast)]
    //~^ ERROR E0277
    pub fn unsupported_return(&self, x: i32) -> String {
        x.to_string()
    }
}
