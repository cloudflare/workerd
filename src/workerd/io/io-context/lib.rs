// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The `IoContext` of the current request, for APIs implemented in Rust.
//!
//! `IoContext` (`src/workerd/io/io-context.h`) holds the state of the request that JavaScript is
//! running on behalf of. This crate is a package of its own because `//src/workerd/io` depends on
//! `jsg`.

mod ffi;

use std::marker::PhantomData;
use std::time::Duration;
use std::time::SystemTime;
use std::time::UNIX_EPOCH;

use ffi::bridge;

/// The message of the error C++ `IoContext::current()` throws outside a request.
// A copy of `kAsyncIoErrorMessage` in src/workerd/io/io-context.c++; update both together.
const NO_REQUEST_MESSAGE: &str = "Disallowed operation called within global scope. Asynchronous \
     I/O (ex: fetch() or connect()), setting a timeout, and generating random values are not \
     allowed within global scope. To fix this error, perform this operation within a handler. \
     https://developers.cloudflare.com/workers/runtime-apis/handlers/";

/// The `IoContext` of the current request.
///
/// It holds the process-unique id of the C++ `IoContext` (`IoContext::getId()`), not a reference
/// to it. Each method uses the current `IoContext`, and fails unless it is the one this handle
/// identifies. Ids are never reused, so a handle can never reach another request's `IoContext`,
/// or one that has been destroyed. A handle can be used only with the [`jsg::Lock`] it was
/// obtained with, and is neither `Send` nor `Sync`.
pub struct IoContext<'s> {
    id: u64,
    _lock: PhantomData<(&'s (), *const ())>,
}

impl<'s> IoContext<'s> {
    /// Returns the `IoContext` of the current request.
    ///
    /// The closure that `jsg::modules::add_builtin` runs to create a builtin module's exports
    /// should not call it. That closure normally runs outside any request, but the legacy module
    /// registry can run it during one (for example, the first time a request reads the `process`
    /// global), and the closure would then see that request's `IoContext`.
    ///
    /// # Errors
    /// Outside a request, such as in a Worker's global scope, fails with the error that C++
    /// `IoContext::current()` throws, which explains that the operation needs a request.
    pub fn current(lock: &mut jsg::Lock<'s>) -> jsg::Result<Self> {
        Self::try_current(lock).ok_or_else(|| jsg::Error::new_error(NO_REQUEST_MESSAGE))
    }

    /// Returns the `IoContext` of the current request, or `None` outside a request.
    #[must_use]
    pub fn try_current(_lock: &mut jsg::Lock<'s>) -> Option<Self> {
        ffi::current_id().map(|id| Self {
            id,
            _lock: PhantomData,
        })
    }

    /// Returns the current time, as the request observes it, like `Date.now()` does in
    /// JavaScript but to the nanosecond. Mitigating Spectre, the clock advances only when
    /// JavaScript is waiting for I/O, unless the `precise_timers` compatibility flag is on.
    ///
    /// # Errors
    /// Fails with an internal error if this `IoContext` is no longer the current one, or is
    /// handling no request, which only happens while it is not running JavaScript.
    pub fn now(&self) -> jsg::Result<SystemTime> {
        let nanos = bridge::io_context_now(self.id)?;
        let offset = Duration::from_nanos(nanos.unsigned_abs());
        Ok(if nanos < 0 {
            UNIX_EPOCH - offset
        } else {
            UNIX_EPOCH + offset
        })
    }

    /// Sends `message`, one serialized Cap'n Proto message (as `capnp::serialize::write_message()`
    /// writes it), to logfwdr channel `channel`. The request waits for the write, as it does for
    /// `ctx.waitUntil()`, and the write makes no delivery guarantees. In a Durable Object, the
    /// message is sent only once the output gate opens, that is, once the storage writes made
    /// before it are confirmed.
    ///
    /// # Errors
    /// Fails if `message` is not exactly one valid serialized message, and with an internal error
    /// if this `IoContext` is no longer the current one.
    pub fn write_logfwdr(&self, channel: u32, message: &[u8]) -> jsg::Result<()> {
        Ok(bridge::io_context_write_logfwdr(self.id, channel, message)?)
    }
}
