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

    /// A duplicate for the server to own.
    pub fn duplicate_for_server(&self) -> io::Result<socket2::Socket> {
        Ok(socket2::Socket::from(self.0.try_clone()?))
    }
}

/// `--control-fd`: a duplicate of the descriptor, as a file the server writes its events to. The
/// inherited descriptor stays open, as `--socket-fd`'s does.
pub fn control_file(fd: u32) -> io::Result<std::fs::File> {
    use std::os::fd::BorrowedFd;

    let raw = i32::try_from(fd).map_err(|_| io::Error::from_raw_os_error(libc::EBADF))?;
    // SAFETY: borrowed for the duplication only, which fails with EBADF if `raw` is not open.
    let inherited = unsafe { BorrowedFd::borrow_raw(raw) };
    inherited.try_clone_to_owned().map(std::fs::File::from)
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
#[path = "unix-test.rs"]
mod tests;
