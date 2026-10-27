// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// JavaScript only ever sees a promise, never the capability to settle it.
use jsg::v8;

pub fn t<'s>(lock: &mut jsg::Lock<'s>) -> jsg::Result<v8::Local<'s, v8::Value>> {
    let resolver = v8::PromiseResolver::new(lock)?;
    jsg::ToJS::to_js(resolver, lock)
    //~^ ERROR E0277
}
