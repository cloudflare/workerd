// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The arguments of a property callback live in the callback's scope and cannot leave it.
use jsg::v8;
pub fn getter() -> jsg::RawGetterCallback {
    jsg::getter_callback(|_lock, args| {
        let _this: v8::Local<'static, v8::Value> = args.this();
        //~^ ERROR lifetime may not live long enough
    })
}
pub fn setter() -> jsg::RawSetterCallback {
    jsg::setter_callback(|_lock, args| {
        let _value: v8::Local<'static, v8::Value> = args.get(0);
        //~^ ERROR lifetime may not live long enough
    })
}
