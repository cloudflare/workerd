// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A `jsg::Arguments` parameter takes the remaining arguments, so it is the last parameter of a
// method or constructor.

use jsg_macros::jsg_constructor;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

#[jsg_resource]
pub struct Rest;

#[jsg_resource]
impl Rest {
    #[jsg_constructor]
    pub fn constructor(rest: jsg::Arguments<u32>, last: u32) -> Self {
        //~^ ERROR so it is the last parameter
        let _ = (rest, last);
        Self
    }

    #[jsg_method]
    pub fn method(&self, rest: jsg::Arguments<u32>, last: u32) {
        //~^ ERROR so it is the last parameter
        let _ = (rest, last);
    }

    #[jsg_method]
    pub fn twice(rest: jsg::Arguments<u32>, more: jsg::Arguments<u32>) {
        //~^ ERROR so it is the last parameter
        let _ = (rest, more);
    }
}
