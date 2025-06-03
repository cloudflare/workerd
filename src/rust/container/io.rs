use std::ffi::CStr;
use std::pin::Pin;
use std::sync::mpsc;
use std::task::Context;
use std::task::Poll;

use futures::AsyncRead;
use futures::AsyncWrite;

unsafe extern "C" {
    fn strsignal(sig: u32) -> *const std::os::raw::c_char;
}

#[must_use]
pub fn signo_as_string(signo: u32) -> Option<String> {
    unsafe {
        let ptr = strsignal(signo);
        if ptr.is_null() {
            None
        } else {
            CStr::from_ptr(ptr).to_string_lossy().into_owned().into()
        }
    }
}

pub struct MpscReader {
    pub receiver: mpsc::Receiver<Vec<u8>>,
}

impl MpscReader {
    #[must_use]
    pub fn new(receiver: mpsc::Receiver<Vec<u8>>) -> Self {
        Self { receiver }
    }
}

impl AsyncRead for MpscReader {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut [u8],
    ) -> Poll<std::io::Result<usize>> {
        match self.receiver.try_recv() {
            Ok(data) => {
                let len = std::cmp::min(buf.len(), data.len());
                buf[..len].copy_from_slice(&data[..len]);
                Poll::Ready(Ok(len))
            }
            Err(mpsc::TryRecvError::Empty) => {
                cx.waker().wake_by_ref();
                Poll::Pending
            }
            Err(mpsc::TryRecvError::Disconnected) => {
                Poll::Ready(Ok(0)) // EOF
            }
        }
    }
}

pub struct MpscWriter {
    pub sender: mpsc::SyncSender<Vec<u8>>,
}

impl MpscWriter {
    #[must_use]
    pub fn new(sender: mpsc::SyncSender<Vec<u8>>) -> Self {
        Self { sender }
    }
}

impl AsyncWrite for MpscWriter {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<std::io::Result<usize>> {
        match self.sender.try_send(buf.to_vec()) {
            Ok(()) => Poll::Ready(Ok(buf.len())),
            Err(mpsc::TrySendError::Full(_)) => {
                cx.waker().wake_by_ref();
                Poll::Pending
            }
            Err(mpsc::TrySendError::Disconnected(_)) => Poll::Ready(Err(std::io::Error::new(
                std::io::ErrorKind::BrokenPipe,
                "Channel disconnected",
            ))),
        }
    }

    fn poll_flush(self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        // MPSC channels don't need explicit flushing
        Poll::Ready(Ok(()))
    }

    fn poll_close(self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        // Closing is handled by dropping the sender
        Poll::Ready(Ok(()))
    }
}
