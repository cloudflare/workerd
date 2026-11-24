//! [`PortPace`] and [`PortPaced`]: at most one operation for each time the thread's port runs.

use std::cell::Cell;
use std::cell::RefCell;
use std::future::Future;
use std::io;
use std::pin::Pin;
use std::task::Context;
use std::task::Poll;

use tokio::io::AsyncRead;
use tokio::io::AsyncWrite;
use tokio::io::ReadBuf;
use tokio::task::JoinHandle;

use crate::port::current_local_set;
use crate::port::port_passes;

/// Holds each operation back until this thread's port has run since the one before it.
///
/// KJ turns the events that are ready before it calls its port, and it turns the continuation of
/// an operation that completed without waiting ahead of every event queued earlier. So work that
/// is ready whenever it is asked for keeps a loop thread to itself for as long as it lasts:
/// nothing else on the thread runs, no tokio task, no timer, no event of another thread. Bytes
/// that a thread of another runtime supplies are such work whenever that thread is ahead, where
/// bytes that the loop's own runtime supplies arrive only when the port runs.
///
/// Paced, an operation that follows another without the port having run in between waits for
/// it to run, which it does once KJ has turned everything else that was ready. A loop that
/// passes its port between two operations anyway pays nothing.
#[derive(Default)]
pub struct PortPace {
    /// [`port_passes`] as the last operation completed, until an operation has waited it out.
    completed_at: Cell<Option<u64>>,
    /// A task that ends the next time the port runs, while an operation waits for that.
    waiting: RefCell<Option<JoinHandle<()>>>,
}

impl PortPace {
    /// Ready once the port has run since the last [`completed`](Self::completed), or at once
    /// if nothing completed yet. A thread without a `TokioEventPort` has no port to wait for.
    pub fn poll_ready(&self, cx: &mut Context<'_>) -> Poll<()> {
        let mut waiting = self.waiting.borrow_mut();
        if self.completed_at.get() == Some(port_passes()) {
            if waiting.is_none() {
                // A task of the port's `LocalSet` runs only when the port does.
                *waiting = current_local_set().map(|local| local.spawn_local(async {}));
            }
            if let Some(task) = waiting.as_mut()
                && Pin::new(task).poll(cx).is_pending()
            {
                return Poll::Pending;
            }
        }
        *waiting = None;
        self.completed_at.set(None);
        Poll::Ready(())
    }

    /// An operation completed: the next one waits for the port.
    pub fn completed(&self) {
        self.completed_at.set(Some(port_passes()));
    }
}

/// A byte stream whose reads are paced ([`PortPace`]): for a loop thread to read a stream that
/// a thread of another runtime fills. Writes pass through.
pub struct PortPaced<T> {
    inner: T,
    pace: PortPace,
}

impl<T> PortPaced<T> {
    pub fn new(inner: T) -> Self {
        Self {
            inner,
            pace: PortPace::default(),
        }
    }
}

impl<T: AsyncRead + Unpin> AsyncRead for PortPaced<T> {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<io::Result<()>> {
        let this = self.get_mut();
        std::task::ready!(this.pace.poll_ready(cx));
        let result = std::task::ready!(Pin::new(&mut this.inner).poll_read(cx, buf));
        this.pace.completed();
        Poll::Ready(result)
    }
}

impl<T: AsyncWrite + Unpin> AsyncWrite for PortPaced<T> {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<io::Result<usize>> {
        Pin::new(&mut self.get_mut().inner).poll_write(cx, buf)
    }

    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        Pin::new(&mut self.get_mut().inner).poll_flush(cx)
    }

    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        Pin::new(&mut self.get_mut().inner).poll_shutdown(cx)
    }

    fn poll_write_vectored(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        bufs: &[io::IoSlice<'_>],
    ) -> Poll<io::Result<usize>> {
        Pin::new(&mut self.get_mut().inner).poll_write_vectored(cx, bufs)
    }

    fn is_write_vectored(&self) -> bool {
        self.inner.is_write_vectored()
    }
}

#[cfg(test)]
#[path = "pace-test.rs"]
mod tests;
