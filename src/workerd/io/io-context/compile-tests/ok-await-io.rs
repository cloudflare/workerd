// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// An API starts I/O, gives the request a value, and takes the value back, using JavaScript, when
// the I/O completes.
use io_context::IoContext;
use jsg::ToJS;

pub fn start(lock: &mut jsg::Lock<'_>) -> jsg::Result<()> {
    let context = IoContext::current(lock)?;
    let pending = context.add_object(String::from("pending"))?;
    context.await_io(async { 42_u32 }, move |lock, value| {
        let label = pending.take(lock)?.unwrap_or_default();
        let object = lock.new_object();
        let value = f64::from(value).to_js(lock)?;
        object.set(lock, &label, value)?;
        Ok(())
    })
}
