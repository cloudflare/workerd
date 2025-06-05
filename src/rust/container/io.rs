use std::collections::VecDeque;
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
    unread_data: VecDeque<u8>,
}

impl MpscReader {
    #[must_use]
    pub fn new(receiver: mpsc::Receiver<Vec<u8>>) -> Self {
        Self {
            receiver,
            unread_data: VecDeque::new(),
        }
    }

    fn read_unread(
        mut self: Pin<&mut Self>,
        buf: &mut [u8],
    ) -> Poll<Result<usize, std::io::Error>> {
        let len = std::cmp::min(buf.len(), self.unread_data.len());
        let data = self.unread_data.drain(..len);
        for (i, byte) in data.enumerate() {
            buf[i] = byte;
        }
        Poll::Ready(Ok(len))
    }
}

impl AsyncRead for MpscReader {
    fn poll_read(
        mut self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut [u8],
    ) -> Poll<std::io::Result<usize>> {
        if !self.unread_data.is_empty() {
            return self.read_unread(buf);
        }

        match self.receiver.try_recv() {
            Ok(data) => {
                self.unread_data.extend(data.iter());
                self.read_unread(buf)
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
        _: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<std::io::Result<usize>> {
        match self.sender.try_send(buf.to_vec()) {
            Ok(()) => Poll::Ready(Ok(buf.len())),
            Err(mpsc::TrySendError::Full(_)) => Poll::Ready(Err(std::io::Error::new(
                std::io::ErrorKind::StorageFull,
                "Channel is full",
            ))),
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
