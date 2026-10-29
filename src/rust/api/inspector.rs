// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The `node-internal:inspector` module, the native half of the experimental, local-dev-only
//! `node:inspector` module.

#![allow(
    unsafe_code,
    reason = "connecting to the V8 inspector takes the isolate as a raw pointer"
)]

use std::cell::Cell;
use std::cell::RefCell;

use jsg::ToJS;
use jsg::v8;
use jsg_macros::jsg_constructor;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;
use kj_rs::KjOwn;

use crate::ffi;

/// A thin pipe between JavaScript and the isolate's own V8 inspector.
///
/// It forwards opaque Chrome `DevTools` Protocol (CDP) JSON strings, like Node.js's
/// `JSBindingsConnection` (`src/inspector_js_api.cc`). All CDP envelope logic
/// (id/method/params/result/error) lives in the TypeScript layer
/// (`src/node/internal/internal_inspector.ts`).
///
/// A connection connects to the isolate's own V8 inspector in-process, independent of any
/// `DevTools` WebSocket session. It can only be created in the open-source `workerd` binary in
/// local development: the V8 inspector requires the `enable_nodejs_inspector_local_dev`
/// experimental flag, and the connection refuses to work in a multi-tenant process.
#[jsg_resource(custom_trace)]
pub struct InspectorConnection {
    /// Called with each response or notification, as a JSON string.
    callback: v8::TracedGlobal<v8::Function>,
    /// `None` once disconnected, and while `dispatch` passes a message to the session.
    session: RefCell<Option<KjOwn<ffi::InspectorSession>>>,
    /// Set by `disconnect`, so that a `dispatch` whose message ran JavaScript that disconnected
    /// does not reconnect the session afterwards.
    disconnected: Cell<bool>,
    /// Set while `flush` delivers messages, so that a reentrant `dispatch` from `callback` leaves
    /// its messages in the session for the delivery loop instead of starting a nested one.
    draining: Cell<bool>,
}

impl jsg::Traced for InspectorConnection {
    fn trace(&self, visitor: &mut jsg::GcVisitor) {
        jsg::Traced::trace(&self.callback, visitor);
    }
}

#[jsg_resource]
impl InspectorConnection {
    #[jsg_constructor]
    fn constructor<'s>(
        lock: &mut jsg::Lock<'s>,
        callback: v8::Local<'s, v8::Value>,
    ) -> jsg::Result<Self> {
        let Ok(callback) = v8::Local::<v8::Function>::try_from(callback) else {
            return Err(jsg::Error::new_type_error(
                "The callback must be a function.",
            ));
        };
        let session = connect(lock)?;
        Ok(Self {
            callback: callback.to_traced_global(lock),
            session: RefCell::new(Some(session)),
            disconnected: Cell::new(false),
            draining: Cell::new(false),
        })
    }

    /// Dispatches a CDP message (a JSON string) to the V8 inspector session, then delivers the
    /// responses and notifications it produced to the callback.
    ///
    /// # Errors
    ///
    /// Fails if the connection is disconnected, or if called from JavaScript that a message
    /// dispatched by this connection is evaluating (e.g. `Runtime.evaluate`).
    #[jsg_method]
    pub fn dispatch(&self, lock: &mut jsg::Lock, message: String) -> jsg::Result<()> {
        // Take the session out for the dispatch: the message may run JavaScript that calls this
        // connection again, which must not find the session borrowed.
        let mut session = self
            .session
            .borrow_mut()
            .take()
            .ok_or_else(|| jsg::Error::new_error("Inspector connection is not connected."))?;
        let result = ffi::inspector_session_dispatch(session.as_mut(), &message);
        if self.disconnected.get() {
            // Destroying the session discards the messages it buffered.
            drop(session);
            return Ok(result?);
        }
        *self.session.borrow_mut() = Some(session);
        result?;
        self.flush(lock)
    }

    /// Tears down the V8 inspector session. Safe to call multiple times.
    #[jsg_method]
    pub fn disconnect(&self, _lock: &mut jsg::Lock) {
        // The session must be destroyed with the isolate locked, which `_lock` guarantees.
        // Destroying it discards the messages it buffered.
        self.disconnected.set(true);
        self.session.borrow_mut().take();
    }

    /// Delivers the messages the session buffered to the callback in order, including the ones
    /// produced by reentrant `dispatch` calls and the notifications the session sends while the
    /// callback runs (e.g. `Runtime.consoleAPICalled`).
    fn flush(&self, lock: &mut jsg::Lock) -> jsg::Result<()> {
        if self.draining.replace(true) {
            return Ok(());
        }
        let _reset = ResetOnDrop(&self.draining);
        let callback = self.callback.as_local(lock);
        loop {
            // Take the whole buffer, so the session can buffer more messages while the callback
            // runs.
            let batch = match self.session.borrow_mut().as_mut() {
                Some(session) => ffi::inspector_session_take_messages(session.as_mut()),
                None => return Ok(()),
            };
            if batch.is_empty() {
                return Ok(());
            }
            for message in batch {
                let message = message.to_js(lock)?;
                // If the callback throws, its exception propagates from `dispatch()`, and the
                // rest of the batch is dropped.
                callback.call(lock, None, &[message])?;
            }
        }
    }
}

/// Clears a flag when dropped, including when unwinding.
struct ResetOnDrop<'a>(&'a Cell<bool>);

impl Drop for ResetOnDrop<'_> {
    fn drop(&mut self) {
        self.0.set(false);
    }
}

fn connect(lock: &mut jsg::Lock) -> jsg::Result<KjOwn<ffi::InspectorSession>> {
    if ffi::inspector_is_multi_tenant_process() {
        return Err(jsg::Error::new_error(
            "node:inspector Session is not available in this environment.",
        ));
    }
    let isolate = lock.isolate().as_ffi();
    // SAFETY: the lock keeps the isolate locked by this thread; Rust JSG resources only run in
    // isolates that a `Worker::Isolate` created.
    if !unsafe { ffi::inspector_isolate_has_v8_inspector(isolate) }? {
        return Err(jsg::Error::new_error(
            "node:inspector Session requires the experimental \
             'enable_nodejs_inspector_local_dev' compatibility flag and is only available when \
             running workerd locally with --experimental.",
        ));
    }
    // SAFETY: as above, and resource constructors run with a context entered.
    Ok(unsafe { ffi::inspector_session_connect(isolate) }?)
}

/// The `node-internal:inspector` module object.
///
/// It exposes the `Connection` constructor used by the TypeScript `Session` implementation,
/// mirroring the shape of Node.js's `internalBinding('inspector')` minus the pieces the
/// TypeScript layer implements itself.
#[jsg_resource]
pub struct InspectorModule;

#[jsg_resource(nested_types(Connection = InspectorConnection))]
impl InspectorModule {
    pub fn new() -> jsg::Rc<Self> {
        jsg::Rc::new(Self {})
    }
}
