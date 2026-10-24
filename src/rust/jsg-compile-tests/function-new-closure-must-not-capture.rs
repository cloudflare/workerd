// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A function's closure is called through a plain function pointer, so its state goes in the
// function's data, which the garbage collector can see, rather than in captures.
use jsg::v8;

pub fn t<'s>(lock: &mut jsg::Lock<'s>) -> jsg::Result<v8::Local<'s, v8::Function>> {
    let name = String::from("captured");
    v8::Function::new(lock, (), move |lock, (), _args| {
        //~^ ERROR E0277
        Ok(jsg::ToJS::to_js(name.as_str(), lock))
    })
}
