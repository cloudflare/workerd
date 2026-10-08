//! [`Runtime`]: the tokio-backed KJ event loop, owned by a Rust `main`.

use std::cell::Cell;
use std::cell::RefCell;
use std::future::Future;
use std::future::poll_fn;
use std::panic::resume_unwind;
use std::pin::Pin;
use std::rc::Rc;
use std::task::Poll;

use cxx::KjError;
use cxx::KjExceptionType;
use kj_rs::KjOwn;

use crate::ffi::TokioAsyncIoContext;

/// What [`Runtime::block_on`] has the KJ loop wait for: the end of the task it spawned.
pub struct BlockOnTask(Pin<Box<dyn Future<Output = ()>>>);

pub async fn block_on_task_join(task: Box<BlockOnTask>) {
    task.0.await;
}

/// A thread's KJ event loop on the tokio-backed port (tokio-event-port.h), with its tokio
/// `current_thread` runtime, for a Rust `main` to own and block on.
///
/// One per thread. The KJ loop schedules the thread, and tokio runs whenever that loop sleeps;
/// the thread stays inside the tokio runtime's context for the Runtime's whole life, so tokio
/// resources created anywhere on it -- including by C++ inside KJ turns -- register with its
/// drivers.
pub struct Runtime {
    context: KjOwn<TokioAsyncIoContext>,
}

impl Runtime {
    /// Builds the tokio runtime, enters it on this thread, and builds the KJ loop on it.
    ///
    /// # Errors
    ///
    /// This thread already has a `TokioEventPort` (another `Runtime`) or a current KJ event loop.
    pub fn new() -> Result<Self, KjError> {
        Ok(Self {
            context: crate::ffi::new_tokio_async_io_context()?,
        })
    }

    /// The C++ context: the loop's `kj::Timer`, `kj::EventLoop` and `kj::WaitScope`, for the C++
    /// that builds on them.
    pub fn context(&mut self) -> Pin<&mut TokioAsyncIoContext> {
        self.context.as_mut()
    }

    /// Runs `future` to completion, blocking this thread in `promise.wait()` on the loop's
    /// `WaitScope` meanwhile. `future` is polled by a task on the loop's `LocalSet` (see
    /// [`crate::spawn`]): KJ events run, and whenever none is left, tokio does. Tasks still
    /// pending when `future` completes run whenever the loop next waits (a later `block_on`, or a
    /// wait on [`context()`](Self::context)), until the Runtime is dropped.
    ///
    /// However this returns, `future` has been dropped by then.
    ///
    /// # Errors
    ///
    /// The port could not sleep (its `wait_*` panicked, surfaced as a `kj::Exception`). Or the
    /// task was cancelled, which only `TokioEventPort::cancelSpawnedTasks()` during the wait
    /// does; the Runtime cannot `block_on` again after that (it panics).
    ///
    /// # Panics
    ///
    /// Resumes a panic of `future`, as tokio's `block_on` lets one through.
    pub fn block_on<F>(&mut self, future: F) -> Result<F::Output, KjError>
    where
        F: Future + 'static,
        F::Output: 'static,
    {
        // This frame owns `future`, so it is dropped when `block_on` returns, however it returns;
        // the task only reaches it through a `Weak` to poll it, and tasks are polled only inside
        // the wait.
        let future = Rc::new(RefCell::new(Box::pin(future)));
        let task = crate::spawn(poll_fn({
            let future = Rc::downgrade(&future);
            move |cx| match future.upgrade() {
                Some(future) => future.borrow_mut().as_mut().poll(cx),
                // `block_on` has returned, and aborted this task.
                None => Poll::Pending,
            }
        }));
        let abort = task.abort_handle();
        let joined = Rc::new(Cell::new(None));
        let join = {
            let joined = Rc::clone(&joined);
            async move { joined.set(Some(task.await)) }
        };
        let waited = self
            .context
            .as_mut()
            .block_on(Box::new(BlockOnTask(Box::pin(join))));
        // After a failed wait the task is still on the `LocalSet`; once this frame drops `future`
        // it has nothing left to poll.
        abort.abort();
        waited?;
        match joined.take() {
            Some(Ok(output)) => Ok(output),
            Some(Err(error)) => match error.try_into_panic() {
                Ok(panic) => resume_unwind(panic),
                Err(error) => Err(KjError::new(KjExceptionType::Failed, error.to_string())),
            },
            None => unreachable!("the wait returned before the task ended"),
        }
    }
}

#[cfg(test)]
#[path = "runtime-test.rs"]
mod tests;
