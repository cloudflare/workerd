// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// An API returns a promise for the result of I/O, converted with JavaScript when it completes,
// or for its output as is.
use io_context::IoContext;
use jsg::ToJS;
use jsg::v8;

pub fn label<'s>(lock: &mut jsg::Lock<'s>) -> jsg::Result<v8::Local<'s, v8::Promise>> {
    let context = IoContext::current(lock)?;
    context.await_io(lock, async { 42_u32 }, |lock, value| {
        let object = lock.new_object();
        let value = f64::from(value).to_js(lock)?;
        object.set(lock, "value", value)?;
        Ok(object.into())
    })
}

pub fn value<'s>(lock: &mut jsg::Lock<'s>) -> jsg::Result<v8::Local<'s, v8::Promise>> {
    let context = IoContext::current(lock)?;
    context.await_io_value(lock, async { Ok::<_, jsg::Error>(String::from("done")) })
}
