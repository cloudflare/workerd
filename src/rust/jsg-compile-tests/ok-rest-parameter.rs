// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The shapes a rest parameter can take.

use jsg::Arguments;
use jsg::v8;
use jsg_macros::jsg_constructor;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

#[jsg_resource]
pub struct Rest;

#[jsg_resource]
impl Rest {
    #[jsg_constructor]
    pub fn constructor(_lock: &mut jsg::Lock, rest: Arguments<String>) -> Self {
        let _ = rest;
        Self
    }

    #[jsg_method]
    pub fn handles<'s>(&self, rest: jsg::Arguments<v8::Local<'s, v8::Value>>) -> u32 {
        u32::try_from(rest.len()).unwrap_or(u32::MAX)
    }

    #[jsg_method]
    pub fn after_optional(
        this: jsg::Rc<Self>,
        first: Option<String>,
        rest: jsg::Arguments<jsg::Rc<Self>>,
    ) -> jsg::Result<bool> {
        let _ = first;
        Ok(rest.iter().all(|other| *other == this))
    }
}
