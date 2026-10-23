// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// `ToJS` returns a handle of the lock's scope, never a `'static` one.
use jsg::ToJS;
use jsg::v8;

pub fn t(lock: &mut jsg::Lock<'_>) {
    let _: v8::Local<'static, v8::Value> = true.to_js(lock); //~ ERROR lifetime may not live long enough
}
