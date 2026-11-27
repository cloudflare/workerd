// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `TestController`, the first argument of a Worker's `test(controller, env, ctx)` handler, and
//! the call of that handler, which the entrypoint crate (`src/workerd/io/entrypoint/test.rs`)
//! makes when it delivers the test event.
//!
//! The handler is C++'s `ExportedHandler::test`, read when the export is loaded, as the other
//! handlers are. It is held untyped, since a `jsg::Function` signature can't name a Rust
//! `TestController`, and `ServiceWorkerGlobalScope::getTestHandler()` looks it up for the
//! entrypoint crate, which calls it through `run()`.
//!
//! The controller has no members: it exists for consistency with the other handlers' first
//! arguments, on the assumption that it will get some someday. Its TypeScript declaration is
//! written by hand, next to `ExecutionContext`'s in `api/global-scope.h`, since C++ RTTI can't
//! describe a Rust type.

use jsg::FromJS;
use jsg::ToJS;
use jsg::v8;
use jsg_macros::jsg_resource;

/// The first argument of a `test()` handler.
#[jsg_resource]
pub struct TestController;

#[jsg_resource]
impl TestController {}

/// Calls `handler`, a `test()` handler, on `receiver` with a new `TestController`, `env` and
/// `ctx`, and returns what it returns as a promise.
///
/// The call is that of a C++ JSG `jsg::Function` with those arguments, and the result is
/// converted as C++ JSG converts a `jsg::Promise<void>`: a promise as is, a thenable followed
/// only under `unwrap_custom_thenables`, and any other value as a promise resolved with
/// `undefined`.
///
/// # Errors
/// What the handler throws, which stays pending.
pub fn run<'s>(
    lock: &mut jsg::Lock<'s>,
    handler: v8::Local<'s, v8::Function>,
    receiver: v8::Local<'s, v8::Value>,
    env: v8::Local<'s, v8::Value>,
    ctx: v8::Local<'s, v8::Value>,
) -> jsg::Result<v8::Local<'s, v8::Promise>> {
    let controller = jsg::Rc::new(lock, TestController).to_js(lock)?;
    let returned = handler.call(lock, Some(receiver), &[controller, env, ctx])?;
    Ok(v8::VoidPromise::from_js(lock, returned)?.into())
}

#[cfg(test)]
#[path = "lib-test.rs"]
mod tests;
