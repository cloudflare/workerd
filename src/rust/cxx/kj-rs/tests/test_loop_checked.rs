//! Helpers for `loop-checked-test.c++`.

use std::future::Future;
use std::future::IntoFuture;
use std::pin::Pin;
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;
use std::task::Context;
use std::task::Poll;
use std::task::Waker;

use kj_rs::LoopChecked;

use crate::Error;
use crate::Result;

static DROPS: AtomicUsize = AtomicUsize::new(0);

/// Counts its drops in `DROPS`.
struct Counted(u32);

impl Drop for Counted {
    fn drop(&mut self) {
        DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

/// A `LoopChecked` value, or none once it is taken.
pub struct LoopCheckedSlot(Option<LoopChecked<Counted>>);

/// A bridged KJ promise's future, wrapped to be `Send`.
pub struct LoopCheckedFuture(
    Pin<Box<dyn Future<Output = std::result::Result<i32, cxx::KjException>> + Send>>,
);

fn io_error(exception: &cxx::KjException) -> Error {
    Error::other(exception.what().to_owned())
}

#[expect(
    clippy::unnecessary_box_returns,
    reason = "cxx passes an opaque Rust type to C++ in a Box"
)]
pub fn new_loop_checked_slot(value: u32) -> Box<LoopCheckedSlot> {
    // SAFETY: `Counted` has no affinity to the OS thread.
    Box::new(LoopCheckedSlot(Some(unsafe {
        LoopChecked::new(Counted(value))
    })))
}

pub fn loop_checked_get(slot: &LoopCheckedSlot) -> Result<u32> {
    let checked = slot.0.as_ref().ok_or_else(|| Error::other("taken"))?;
    Ok(checked.get().map_err(|e| io_error(&e))?.0)
}

pub fn loop_checked_set(slot: &mut LoopCheckedSlot, value: u32) -> Result<()> {
    let checked = slot.0.as_mut().ok_or_else(|| Error::other("taken"))?;
    checked.get_mut().map_err(|e| io_error(&e))?.0 = value;
    Ok(())
}

/// Takes the value out of its `LoopChecked`, leaving it in `slot` if that fails.
pub fn loop_checked_take(slot: &mut LoopCheckedSlot) -> Result<u32> {
    let checked = slot.0.take().ok_or_else(|| Error::other("taken"))?;
    match checked.into_inner() {
        Ok(counted) => Ok(counted.0),
        Err(checked) => {
            slot.0 = Some(checked);
            Err(Error::other("off its event loop"))
        }
    }
}

pub fn loop_checked_drops() -> usize {
    DROPS.load(Ordering::SeqCst)
}

#[expect(
    clippy::unnecessary_box_returns,
    reason = "cxx passes an opaque Rust type to C++ in a Box"
)]
pub fn new_loop_checked_future(value: i32) -> Box<LoopCheckedFuture> {
    let future = crate::ffi::new_ready_promise_i32(value).into_future();
    // SAFETY: a KJ promise depends on its event loop, not on the OS thread.
    Box::new(LoopCheckedFuture(Box::pin(unsafe {
        LoopChecked::new(future)
    })))
}

/// Polls `future` once, returning its value, or -1 if it is pending.
pub fn poll_loop_checked_future(future: &mut LoopCheckedFuture) -> Result<i32> {
    match future
        .0
        .as_mut()
        .poll(&mut Context::from_waker(Waker::noop()))
    {
        Poll::Pending => Ok(-1),
        Poll::Ready(result) => result.map_err(|e| io_error(&e)),
    }
}
