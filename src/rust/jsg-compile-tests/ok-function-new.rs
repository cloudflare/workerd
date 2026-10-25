// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A function created from a closure reads its data and arguments and returns a value of its own
// scope, without `unsafe`.
use jsg::v8;

pub fn t<'s>(lock: &mut jsg::Lock<'s>) -> jsg::Result<v8::Local<'s, v8::Function>> {
    v8::Function::new(lock, String::from("data"), |lock, data, args| {
        let object = lock.new_object();
        object.set(lock, data, args.get(0));
        Ok(object.into())
    })
}
