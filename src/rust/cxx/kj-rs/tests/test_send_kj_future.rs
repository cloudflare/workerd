//! `SendKjFuture`s of bridged KJ promises, for `send-kj-future-test.c++`.

use std::future::Future;
use std::pin::Pin;
use std::task::Context;
use std::task::Poll;
use std::task::Waker;

use kj_rs::KjFuture;

use crate::Error;
use crate::Result;

type CxxResult<T> = std::result::Result<T, cxx::KjException>;

/// A bridged KJ promise's future, made `Send` with `into_send()`.
pub struct SendKjFutureBox(Pin<Box<dyn Future<Output = CxxResult<()>> + Send>>);

fn boxed(future: impl Future<Output = CxxResult<()>> + Send + 'static) -> Box<SendKjFutureBox> {
    Box::new(SendKjFutureBox(Box::pin(future)))
}

#[expect(
    clippy::unnecessary_box_returns,
    reason = "cxx passes an opaque Rust type to C++ in a Box"
)]
pub fn new_ready_send_kj_future() -> Box<SendKjFutureBox> {
    boxed(crate::ffi::new_ready_promise_void().into_send())
}

#[expect(
    clippy::unnecessary_box_returns,
    reason = "cxx passes an opaque Rust type to C++ in a Box"
)]
pub fn new_pending_send_kj_future() -> Box<SendKjFutureBox> {
    boxed(crate::ffi::new_pending_promise_void().into_send())
}

#[expect(
    clippy::unnecessary_box_returns,
    reason = "cxx passes an opaque Rust type to C++ in a Box"
)]
pub fn new_fulfillable_send_kj_future() -> Box<SendKjFutureBox> {
    boxed(crate::ffi::new_fulfillable_promise_void().into_send())
}

/// Polls `future` once and returns whether it is ready.
pub fn poll_send_kj_future(future: &mut SendKjFutureBox) -> Result<bool> {
    match future
        .0
        .as_mut()
        .poll(&mut Context::from_waker(Waker::noop()))
    {
        Poll::Pending => Ok(false),
        Poll::Ready(result) => result
            .map(|()| true)
            .map_err(|exception| Error::other(exception.what().to_owned())),
    }
}
