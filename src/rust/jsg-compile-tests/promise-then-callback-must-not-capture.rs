// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A promise reaction is called through a plain function pointer, so its state goes in the
// reaction's data, which the garbage collector can see, rather than in captures.
use jsg::v8;

pub fn t<'s>(
    lock: &mut jsg::Lock<'s>,
    promise: v8::Local<'s, v8::Promise>,
) -> jsg::Result<v8::Local<'s, v8::Promise>> {
    let name = String::from("captured");
    promise.then(lock, (), move |lock, (), _value| {
        //~^ ERROR E0277
        Ok(jsg::ToJS::to_js(name.as_str(), lock))
    })
}
