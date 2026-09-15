// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests basic file descriptor cloning behavior in the Rust standard library and verifies that
//! cloned descriptors work correctly when the standard library is instrumented with `TSan`. These
//! tests cover this functionality because workerd patches the standard library's cloning path to
//! make descriptor duplication visible to `TSan`.

use std::io::Read;
use std::io::Write;
use std::os::fd::AsFd;
use std::os::fd::AsRawFd;
use std::os::fd::BorrowedFd;
use std::os::fd::OwnedFd;
use std::os::unix::net::UnixStream as StdUnixStream;

fn clone_fd(fd: BorrowedFd<'_>) -> OwnedFd {
    fd.try_clone_to_owned().expect("OwnedFd::try_clone failed")
}

#[test]
fn stdlib_clone_returns_a_distinct_cloexec_descriptor() {
    let (stream, _peer) = StdUnixStream::pair().expect("failed to create socket pair");
    let cloned = clone_fd(stream.as_fd());

    assert_ne!(cloned.as_raw_fd(), stream.as_raw_fd());
    assert!(
        cloned.as_raw_fd() >= 3,
        "cloned descriptor overlaps standard I/O"
    );

    // SAFETY: `cloned` owns a valid descriptor for the duration of the call.
    let flags = unsafe { libc::fcntl(cloned.as_raw_fd(), libc::F_GETFD) };
    assert!(flags >= 0, "F_GETFD failed");
    assert_ne!(flags & libc::FD_CLOEXEC, 0, "FD_CLOEXEC is not set");
}

#[test]
fn stdlib_clone_remains_usable_after_original_is_dropped() {
    let (stream, mut peer) = StdUnixStream::pair().expect("failed to create socket pair");
    let cloned = clone_fd(stream.as_fd());
    drop(stream);

    let mut cloned = StdUnixStream::from(cloned);
    peer.write_all(&[1]).expect("failed to write through peer");
    let mut byte = [0];
    cloned
        .read_exact(&mut byte)
        .expect("failed to read through cloned descriptor");
    assert_eq!(byte, [1]);

    cloned
        .write_all(&[2])
        .expect("failed to write through cloned descriptor");
    peer.read_exact(&mut byte)
        .expect("failed to read through peer");
    assert_eq!(byte, [2]);
}

#[test]
fn stdlib_clone_does_not_close_original_when_dropped() {
    let (mut stream, mut peer) = StdUnixStream::pair().expect("failed to create socket pair");
    let cloned = clone_fd(stream.as_fd());
    drop(cloned);

    peer.write_all(&[1]).expect("failed to write through peer");
    let mut byte = [0];
    stream
        .read_exact(&mut byte)
        .expect("failed to read through original descriptor");
    assert_eq!(byte, [1]);
}

#[test]
fn stdlib_clone_shares_file_status_flags() {
    let (stream, _peer) = StdUnixStream::pair().expect("failed to create socket pair");
    let cloned = clone_fd(stream.as_fd());

    stream
        .set_nonblocking(true)
        .expect("failed to set O_NONBLOCK");
    // SAFETY: `cloned` owns a valid descriptor for the duration of the call.
    let flags = unsafe { libc::fcntl(cloned.as_raw_fd(), libc::F_GETFL) };
    assert!(flags >= 0, "F_GETFL failed");
    assert_ne!(flags & libc::O_NONBLOCK, 0, "O_NONBLOCK is not shared");
}

#[cfg(target_os = "linux")]
#[test]
fn stdlib_clone_exposes_descriptor_duplication_to_tsan() {
    use std::mem::MaybeUninit;
    use std::os::fd::IntoRawFd;
    use std::sync::Arc;
    use std::sync::Barrier;

    // SAFETY: `EPOLL_CLOEXEC` is a valid `epoll_create1` flag.
    let epoll_fd = unsafe { libc::epoll_create1(libc::EPOLL_CLOEXEC) };
    assert!(epoll_fd >= 0, "epoll_create1 failed");
    // SAFETY: `epoll_fd` was successfully opened above and remains valid until after the waiter
    // thread has joined.
    let registration_fd = unsafe { BorrowedFd::borrow_raw(epoll_fd) }
        .try_clone_to_owned()
        .expect("OwnedFd::try_clone failed")
        .into_raw_fd();

    let (mut reader, mut writer) = StdUnixStream::pair().expect("failed to create socket pair");
    let reader_fd = reader.as_raw_fd();
    let waiter_ready = Arc::new(Barrier::new(2));
    let waiter = {
        let waiter_ready = Arc::clone(&waiter_ready);
        std::thread::spawn(move || {
            waiter_ready.wait();

            let mut event = MaybeUninit::<libc::epoll_event>::uninit();
            // SAFETY: `epoll_fd` remains open until this thread joins, and `event` has space for the
            // single event requested.
            let count = unsafe { libc::epoll_wait(epoll_fd, event.as_mut_ptr(), 1, 5_000) };
            assert_eq!(count, 1, "epoll_wait failed or timed out");

            // SAFETY: A return value of one means `epoll_wait` initialized the event.
            let event = unsafe { event.assume_init() };
            let published = event.u64 as *const u64;
            // SAFETY: The pointer refers to `published` in the parent thread, which remains alive
            // until this thread joins.
            assert_eq!(unsafe { *published }, 0x0123_4567_89ab_cdef);

            let mut byte = [0];
            reader
                .read_exact(&mut byte)
                .expect("failed to drain socket");
        })
    };

    waiter_ready.wait();
    let published = Box::new(0x0123_4567_89ab_cdef_u64);
    let mut event = libc::epoll_event {
        events: libc::EPOLLIN as u32,
        u64: (&raw const *published) as u64,
    };
    // SAFETY: Both descriptors are open, `event` is initialized, and its pointer is valid for the
    // duration of the call.
    let result = unsafe {
        libc::epoll_ctl(
            registration_fd,
            libc::EPOLL_CTL_ADD,
            reader_fd,
            &raw mut event,
        )
    };
    assert_eq!(result, 0, "epoll_ctl failed");
    writer.write_all(&[1]).expect("failed to make socket ready");

    waiter.join().expect("epoll waiter panicked");
    // SAFETY: Both descriptors are open and no longer used after these calls.
    unsafe {
        assert_eq!(libc::close(registration_fd), 0);
        assert_eq!(libc::close(epoll_fd), 0);
    }
}
