// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The example of `IoContext::make_reentry_callback_weak()`: an API keeps a JavaScript callback that
// returns a Worker's code, for a C++ `kj::Function` to call later, as `WorkerLoader.get()` does.
// The `await_js` map converts the resolved object in the IoContext, and the future builds an
// output that is not `Send` from it. A Rust future can await a call too.
use std::rc::Rc;

use io_context::IoContext;
use io_context::ReentryCallback;
use jsg::FromJS;
use jsg::v8;
use jsg_macros::jsg_struct;

#[jsg_struct]
pub struct WorkerCode {
    pub main_module: String,
    pub compatibility_date: String,
}

pub fn keep<'s>(
    lock: &mut jsg::Lock<'s>,
    get_code: v8::Local<'s, v8::Function>,
) -> jsg::Result<ReentryCallback<Rc<WorkerCode>>> {
    let get_code = get_code.to_global(lock);
    IoContext::current(lock)?.make_reentry_callback_weak(move |lock, context| {
        let code = get_code.as_local(lock).call(lock, None, &[])?;
        let code = v8::Promise::resolved(lock, code)?;
        let code = context.await_js(lock, code, |lock, value| WorkerCode::from_js(lock, value))?;
        Ok(async move { Ok(Rc::new(code.await?)) })
    })
}

pub async fn fetch(callback: &ReentryCallback<Rc<WorkerCode>>) -> Option<usize> {
    Some(callback.call().await.ok()?.main_module.len())
}
