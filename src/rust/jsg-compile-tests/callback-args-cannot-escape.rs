// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The arguments of a callback live in the callback's scope and cannot leave it.
use jsg::v8;
pub fn t() -> jsg::RawFunctionCallback {
    jsg::function_callback(|_lock, args| {
        let _this: v8::Local<'static, v8::Value> = args.this();
        //~^ ERROR lifetime may not live long enough
    })
}
