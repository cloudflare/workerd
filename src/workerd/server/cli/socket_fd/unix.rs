//! `--socket-fd` on unix: the inherited descriptor is kept open (no close-on-exec, so a `--watch`
//! re-exec passes it on under the same number) and the server gets a duplicate to own.

use std::io;

use crate::socket_fd::Error;

pub struct InheritedSocket(std::os::fd::OwnedFd);

impl InheritedSocket {
    /// Takes ownership of `fd`, checking that it is an open socket that is listening (where the
    /// OS can tell).
    pub fn take(fd: u32) -> Result<Self, Error> {
        use std::os::fd::FromRawFd;
        use std::os::fd::IntoRawFd;
        use std::os::fd::OwnedFd;

        let raw = i32::try_from(fd).map_err(|_| Error::NotOpen)?;
        // Safety: `raw` names a descriptor the parent process handed us, which nothing else in
        // this process owns. If it is not open, the checks below find out and give the number back
        // before the `OwnedFd` could close anything.
        let socket = Self(unsafe { OwnedFd::from_raw_fd(raw) });
        if let Err(error) = socket.check() {
            if matches!(error, Error::NotOpen) {
                // Not ours after all: do not close it.
                let _ = socket.0.into_raw_fd();
            }
            return Err(error);
        }
        Ok(socket)
    }

    fn check(&self) -> Result<(), Error> {
        let socket = socket2::SockRef::from(&self.0);
        // SO_TYPE is answered by every socket: it tells an open socket from anything else.
        if let Err(error) = socket.r#type() {
            return Err(match error.raw_os_error() {
                Some(libc::EBADF) => Error::NotOpen,
                Some(libc::ENOTSOCK) => Error::NotSocket,
                _ => Error::Os(error),
            });
        }
        if is_listener(&socket).map_err(Error::Os)? == Some(false) {
            return Err(Error::NotListening);
        }
        Ok(())
    }

    /// A duplicate for the server to own (kj's `wrapListenSocketFd` with `TAKE_OWNERSHIP`), as
    /// the raw descriptor that interface takes.
    pub fn duplicate_for_server(&self) -> io::Result<i64> {
        use std::os::fd::IntoRawFd;
        Ok(i64::from(self.0.try_clone()?.into_raw_fd()))
    }
}

/// Whether the socket is listening, or `None` where the OS cannot say (macOS has no
/// `SO_ACCEPTCONN`; the server finds out at the first `accept()`).
#[cfg(any(target_os = "linux", target_os = "android", target_os = "freebsd"))]
fn is_listener(socket: &socket2::SockRef<'_>) -> io::Result<Option<bool>> {
    socket.is_listener().map(Some)
}

#[cfg(not(any(target_os = "linux", target_os = "android", target_os = "freebsd")))]
#[expect(
    clippy::unnecessary_wraps,
    reason = "same signature as the checking arm"
)]
fn is_listener(_socket: &socket2::SockRef<'_>) -> io::Result<Option<bool>> {
    Ok(None)
}

#[cfg(test)]
mod tests {
    use std::net::TcpListener;
    use std::os::fd::IntoRawFd;

    use super::*;

    #[test]
    fn listening_socket_is_taken_and_duplicated() {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        let fd = listener.into_raw_fd().cast_unsigned();
        let socket = InheritedSocket::take(fd).unwrap();
        let duplicate = socket.duplicate_for_server().unwrap();
        assert_ne!(i64::from(fd), duplicate);
        // Both name the same listening socket.
        let bound = std::net::TcpStream::connect(("127.0.0.1", port)).unwrap();
        drop(bound);
        drop(socket);
    }

    #[cfg(any(target_os = "linux", target_os = "android", target_os = "freebsd"))]
    #[test]
    fn connected_socket_is_not_listening() {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let stream = std::net::TcpStream::connect(listener.local_addr().unwrap()).unwrap();
        let fd = stream.into_raw_fd().cast_unsigned();
        assert!(matches!(
            InheritedSocket::take(fd),
            Err(Error::NotListening)
        ));
    }

    #[test]
    fn regular_file_is_not_a_socket() {
        // `take()` owns the descriptor from here on and closes it on this error.
        let fd = std::fs::File::open("/dev/null").unwrap().into_raw_fd();
        assert!(matches!(
            InheritedSocket::take(fd.cast_unsigned()),
            Err(Error::NotSocket)
        ));
    }
}
