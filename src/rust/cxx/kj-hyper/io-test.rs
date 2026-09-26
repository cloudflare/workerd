use std::io;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::Ordering;

use futures::executor::block_on;
use tokio::io::AsyncReadExt;
use tokio::io::BufWriter;
use tokio::io::duplex;

use super::*;

#[test]
fn a_write_completes_only_once_flushed() {
    block_on(async {
        let (ours, mut peer) = duplex(1 << 16);
        // A transport that holds what it is written until flushed, as a TLS stream may.
        let io = RustIo::new(BufWriter::new(ours));
        io.write(b"hello").await.unwrap();
        let mut buf = [0; 5];
        peer.read_exact(&mut buf).await.unwrap();
        assert_eq!(&buf, b"hello");
    });
}

#[test]
fn an_operation_owns_its_share_of_the_stream() {
    block_on(async {
        let (ours, mut peer) = duplex(1 << 16);
        let io = RustIo::new(ours);
        let write = io.write(b"hi");
        drop(io);
        write.await.unwrap();
        let mut buf = [0; 2];
        peer.read_exact(&mut buf).await.unwrap();
        assert_eq!(&buf, b"hi");
    });
}

/// A transport that takes nothing: `poll_write` reports zero bytes, as a closed one does.
struct ClosedIo;

impl AsyncRead for ClosedIo {
    fn poll_read(
        self: Pin<&mut Self>,
        _cx: &mut Context<'_>,
        _buf: &mut ReadBuf<'_>,
    ) -> Poll<io::Result<()>> {
        Poll::Ready(Ok(()))
    }
}

impl AsyncWrite for ClosedIo {
    fn poll_write(
        self: Pin<&mut Self>,
        _cx: &mut Context<'_>,
        _buf: &[u8],
    ) -> Poll<io::Result<usize>> {
        Poll::Ready(Ok(0))
    }

    fn poll_flush(self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        Poll::Ready(Ok(()))
    }

    fn poll_shutdown(self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        Poll::Ready(Ok(()))
    }
}

#[test]
fn a_write_the_transport_takes_nothing_of_fails_as_disconnected() {
    block_on(async {
        let io = RustIo::new(ClosedIo);
        let error = io.write(b"hello").await.unwrap_err();
        assert_eq!(error.exception_type(), KjExceptionType::Disconnected);
    });
}

#[test]
fn abort_read_ends_a_parked_read() {
    block_on(async {
        let (ours, _peer) = duplex(16);
        let io = RustIo::new(ours);
        let mut buf = [0; 4];
        let mut read = Box::pin(io.read(ReadBuf::new(&mut buf), 1));
        assert!(futures::poll!(read.as_mut()).is_pending());
        io.abort_read();
        let error = read.await.unwrap_err();
        assert_eq!(error.exception_type(), KjExceptionType::Disconnected);
    });
}

/// A transport with one waker slot for both directions, as a TLS stream whose reads write
/// and whose writes read has in effect.
struct OneSlotIo(Rc<RefCell<Option<Waker>>>);

impl AsyncRead for OneSlotIo {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        _buf: &mut ReadBuf<'_>,
    ) -> Poll<io::Result<()>> {
        *self.0.borrow_mut() = Some(cx.waker().clone());
        Poll::Pending
    }
}

impl AsyncWrite for OneSlotIo {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        _buf: &[u8],
    ) -> Poll<io::Result<usize>> {
        *self.0.borrow_mut() = Some(cx.waker().clone());
        Poll::Pending
    }

    fn poll_flush(self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        Poll::Ready(Ok(()))
    }

    fn poll_shutdown(self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        Poll::Ready(Ok(()))
    }
}

#[derive(Default)]
struct Flag(AtomicBool);

impl Wake for Flag {
    fn wake(self: Arc<Self>) {
        self.0.store(true, Ordering::SeqCst);
    }
}

#[test]
fn a_wake_for_either_direction_reaches_both_tasks() {
    let slot = Rc::default();
    let mut io = SharedWakers::new(OneSlotIo(Rc::clone(&slot)));
    let (reader, writer) = (Arc::new(Flag::default()), Arc::new(Flag::default()));
    let mut buf = [0; 1];
    let mut read_buf = ReadBuf::new(&mut buf);
    let reader_waker = Waker::from(reader.clone());
    let writer_waker = Waker::from(writer.clone());
    assert!(
        Pin::new(&mut io)
            .poll_read(&mut Context::from_waker(&reader_waker), &mut read_buf)
            .is_pending()
    );
    assert!(
        Pin::new(&mut io)
            .poll_write(&mut Context::from_waker(&writer_waker), b"x")
            .is_pending()
    );
    // The transport kept only the writer's registration; waking it reaches the reader too.
    slot.borrow_mut().take().unwrap().wake();
    assert!(reader.0.load(Ordering::SeqCst));
    assert!(writer.0.load(Ordering::SeqCst));
}
