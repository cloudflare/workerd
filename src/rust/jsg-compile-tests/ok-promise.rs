// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A method returns a promise, keeps its resolver past the call, and reacts to another promise,
// without `unsafe`.
use std::cell::RefCell;

use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

#[jsg_resource]
pub struct Waiter {
    resolver: RefCell<Option<v8::TracedGlobal<v8::PromiseResolver>>>,
}

#[jsg_resource]
impl Waiter {
    #[jsg_method]
    fn wait<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        input: v8::Local<'s, v8::Promise>,
    ) -> Result<v8::Local<'s, v8::Promise>, jsg::Error> {
        let resolver = v8::PromiseResolver::new(lock)?;
        *self.resolver.borrow_mut() = Some(resolver.to_traced_global(lock));
        input.then(
            lock,
            String::from("data"),
            |lock, data, value| {
                let object = lock.new_object();
                object.set(lock, data, value);
                Ok(object.into())
            },
            |lock, _, reason| Ok(v8::Promise::rejected(lock, reason)?.into()),
        )?;
        Ok(resolver.promise())
    }
}
