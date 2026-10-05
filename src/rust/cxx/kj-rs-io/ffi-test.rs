use super::*;

#[cfg(unix)]
#[test]
fn own_fd_from_raw_rejects_negative_fds() {
    // -1 is OwnedFd's niche: turning it into an OwnedFd would be library UB, so the
    // conversion point must refuse it, as an error (the crate's no-panic policy).
    // Safety: rejected before constructing anything.
    assert!(unsafe { own_fd_from_raw(-1) }.is_err());
}

#[cfg(unix)]
#[test]
fn prepare_socket_rejects_negative_handles() {
    // Safety: rejected before constructing anything.
    assert!(unsafe { prepare_socket(-1, TAKE_OWNERSHIP) }.is_err());
}

#[cfg(unix)]
#[test]
fn prepare_socket_rejects_handles_that_do_not_fit_an_fd() {
    // Safety: rejected before constructing anything.
    assert!(unsafe { prepare_socket(i64::from(i32::MAX) + 1, TAKE_OWNERSHIP) }.is_err());
}

/// The happy path of the conversion point: an fd released by std becomes a socket2 socket
/// that owns it (closes it on drop) and is fully usable.
#[cfg(unix)]
#[test]
fn prepare_socket_takes_ownership_of_a_live_socket() {
    use std::os::fd::AsRawFd;
    use std::os::fd::IntoRawFd;
    let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    let raw = listener.into_raw_fd();
    // Safety: `into_raw_fd` transferred ownership of an open socket to us.
    let socket = unsafe { prepare_socket(i64::from(raw), TAKE_OWNERSHIP) }.unwrap();
    assert_eq!(socket.as_raw_fd(), raw);
    assert_eq!(
        socket.local_addr().unwrap().as_socket().unwrap().port(),
        port
    );
    // `socket` is the sole owner: dropping it closes the fd (socket2::Socket's drop glue).
}

/// `uninit_slice` must not touch the pointer for a zero-length request (KJ callers may pass
/// a null or dangling pointer with `maxBytes == 0`), and must cover exactly `len` bytes
/// otherwise.
#[test]
fn uninit_slice_respects_length() {
    // Safety: a null pointer with len 0 is never dereferenced by contract.
    let empty = unsafe { uninit_slice(std::ptr::null_mut(), 0) };
    assert!(empty.is_empty());
    let mut storage = [MaybeUninit::<u8>::uninit(); 8];
    // Safety: `storage` is live and exclusively ours for the slice's lifetime.
    let view = unsafe { uninit_slice(storage.as_mut_ptr().cast::<u8>(), 5) };
    assert_eq!(view.len(), 5);
    view[0].write(7);
    // Safety: element 0 was just initialized above.
    assert_eq!(unsafe { storage[0].assume_init() }, 7);
}

/// KJ's flag semantics: without `TAKE_OWNERSHIP` the caller's fd is untouched except for
/// `O_NONBLOCK` (shared through the dup) and Rust works on a `CLOEXEC` duplicate; with it
/// the fd itself is owned and gains `CLOEXEC`.
#[cfg(unix)]
#[test]
fn prepare_fd_applies_kj_flags() {
    use std::os::fd::AsRawFd;
    use std::os::fd::IntoRawFd;
    fn fd_flags(fd: i32) -> (i32, i32) {
        // Safety: F_GETFD/F_GETFL only read flags of a live descriptor.
        unsafe {
            (
                libc::fcntl(fd, libc::F_GETFD),
                libc::fcntl(fd, libc::F_GETFL),
            )
        }
    }
    let (a, _b) = std::os::unix::net::UnixStream::pair().unwrap();
    let raw = a.as_raw_fd();
    // Borrowed: a distinct, CLOEXEC dup; the original is now non-blocking too (shared
    // open file description) but still open.
    // Safety: `a` keeps `raw` open for the call.
    let dup = unsafe { prepare_fd(raw, 0) }.unwrap();
    assert_ne!(dup.as_raw_fd(), raw);
    assert_ne!(fd_flags(dup.as_raw_fd()).0 & libc::FD_CLOEXEC, 0);
    assert_ne!(fd_flags(dup.as_raw_fd()).1 & libc::O_NONBLOCK, 0);
    assert_ne!(
        fd_flags(raw).1 & libc::O_NONBLOCK,
        0,
        "O_NONBLOCK is per open file"
    );
    drop(dup);
    assert!(fd_flags(raw).0 >= 0, "the caller's fd is still open");

    // Owned, nothing declared: same fd, CLOEXEC added.
    let (c, d) = std::os::unix::net::UnixStream::pair().unwrap();
    let raw_c = c.into_raw_fd();
    // Safety: `into_raw_fd` transferred ownership.
    let owned = unsafe { prepare_fd(raw_c, TAKE_OWNERSHIP) }.unwrap();
    assert_eq!(owned.as_raw_fd(), raw_c);
    assert_ne!(fd_flags(raw_c).0 & libc::FD_CLOEXEC, 0);
    assert_ne!(fd_flags(raw_c).1 & libc::O_NONBLOCK, 0);
    drop(owned);
    // Observed through the peer, not by probing the released number (another thread could
    // reuse it): the pair's other end reads EOF once the owned end is closed.
    assert_eq!(
        std::io::Read::read(&mut &d, &mut [0u8; 1]).unwrap(),
        0,
        "owned: closed on drop"
    );
}
