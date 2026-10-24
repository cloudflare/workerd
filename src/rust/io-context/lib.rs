// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The `IoContext` of the current request, for APIs implemented in Rust.
//!
//! `IoContext` (`src/workerd/io/io-context.h`) holds the state of the request that JavaScript is
//! running on behalf of. This crate is a separate crate from `jsg` because `//src/workerd/io`
//! depends on `jsg`.

#![allow(
    unsafe_code,
    reason = "dereferences the IoContext pointer that the FFI returns"
)]

mod ffi;

use std::marker::PhantomData;
use std::pin::Pin;
use std::ptr::NonNull;
use std::time::Duration;
use std::time::SystemTime;
use std::time::UNIX_EPOCH;

use ffi::bridge;

/// The `IoContext` of the current request.
///
/// It can be used for as long as the [`jsg::Lock`] it was obtained with. The request ends, and
/// its `IoContext` is destroyed, only once JavaScript stops running on its behalf, which it cannot
/// do while Rust code called from JavaScript holds the lock. An `IoContext` is not `Send`.
// TODO(this PR): these lifetime assumptions need to be verified. Is the `IoContext` alive for all
// of `'s` whenever `current()` succeeds, including a `Lock` that spans re-entry into JavaScript
// (e.g. `Local<Function>::call()`) or one created by C++ outside `IoContext::run()`?
pub struct IoContext<'s> {
    context: NonNull<bridge::IoContext>,
    _lock: PhantomData<&'s ()>,
}

impl<'s> IoContext<'s> {
    /// Returns the `IoContext` of the current request.
    ///
    /// # Errors
    /// Outside a request, such as in a Worker's global scope, fails with the JavaScript error
    /// that C++ `IoContext::current()` throws, which explains that the operation needs a request.
    pub fn current(lock: &mut jsg::Lock<'s>) -> jsg::Result<Self> {
        let isolate = lock.isolate().as_ffi();
        // SAFETY: lock guarantees the isolate is locked, with a context entered. The reference is
        // turned into a pointer straight away, so its lifetime does not matter.
        let context = jsg::v8::catching(lock, |exception| unsafe {
            bridge::io_context_current(isolate, exception)
        })?;
        // SAFETY: the pointer is only used through `as_ffi()`, which keeps the pin's guarantee.
        Ok(Self::from_ffi(NonNull::from(unsafe {
            Pin::into_inner_unchecked(context)
        })))
    }

    /// Returns the `IoContext` of the current request, or `None` outside a request.
    #[must_use]
    pub fn try_current(_lock: &mut jsg::Lock<'s>) -> Option<Self> {
        NonNull::new(bridge::io_context_try_current()).map(Self::from_ffi)
    }

    fn from_ffi(context: NonNull<bridge::IoContext>) -> Self {
        Self {
            context,
            _lock: PhantomData,
        }
    }

    fn as_ffi(&self) -> Pin<&mut bridge::IoContext> {
        // TODO(this PR): these lifetime assumptions need to be verified (see `IoContext`).
        // SAFETY: the IoContext lives as long as the lock (see the type's documentation), and is
        // not borrowed elsewhere in Rust: each method borrows it only for one call into C++,
        // which does not call back into Rust. C++ never moves an IoContext.
        unsafe { Pin::new_unchecked(&mut *self.context.as_ptr()) }
    }

    /// Returns the current time, as the request observes it, like `Date.now()` does in
    /// JavaScript but to the nanosecond. Mitigating Spectre, the clock advances only when
    /// JavaScript is waiting for I/O, unless the `precise_timers` compatibility flag is on.
    ///
    /// # Errors
    /// Fails if the `IoContext` is handling no request, which only happens while it is not
    /// running JavaScript.
    pub fn now(&self) -> jsg::Result<SystemTime> {
        let nanos = bridge::io_context_now(self.as_ffi())?;
        let offset = Duration::from_nanos(nanos.unsigned_abs());
        Ok(if nanos < 0 {
            UNIX_EPOCH - offset
        } else {
            UNIX_EPOCH + offset
        })
    }

    /// Sends `message`, a serialized Cap'n Proto message (as `capnp::serialize::write_message()`
    /// writes it), to logfwdr channel `channel`. The request waits for the write, as it does for
    /// `ctx.waitUntil()`, and the write makes no delivery guarantees.
    ///
    /// # Errors
    /// Fails if `message` is not a valid serialized message.
    pub fn write_logfwdr(&self, channel: u32, message: &[u8]) -> jsg::Result<()> {
        Ok(bridge::io_context_write_logfwdr(
            self.as_ffi(),
            channel,
            message,
        )?)
    }
}
