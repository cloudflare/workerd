//! `--socket-fd` on Windows: there is no re-exec to keep the socket for, so the server simply
//! takes it.

use std::io;

use crate::socket_fd::Error;

pub struct InheritedSocket(u32);

impl InheritedSocket {
    /// Checks that `fd` is a socket that is listening (where the provider can tell).
    pub fn take(fd: u32) -> Result<Self, Error> {
        if is_listener(fd)? == Some(false) {
            return Err(Error::NotListening);
        }
        Ok(Self(fd))
    }

    pub fn duplicate_for_server(&self) -> io::Result<socket2::Socket> {
        use std::os::windows::io::FromRawSocket;
        use std::os::windows::io::IntoRawSocket;
        use std::os::windows::io::OwnedSocket;

        // SAFETY: the handle was checked to be a socket; it is released again below, so this
        // process keeps it as inherited.
        let owned = unsafe { OwnedSocket::from_raw_socket(self.0.into()) };
        let duplicate = owned.try_clone();
        let _ = owned.into_raw_socket();
        Ok(socket2::Socket::from(duplicate?))
    }
}

/// `--control-fd`: a duplicate of the C runtime descriptor's handle (a parent passes extra
/// descriptors through the C runtime's handle table, as Node.js does), as a file the server writes
/// its events to. The inherited descriptor stays open, as on Unix.
pub fn control_file(fd: u32) -> io::Result<std::fs::File> {
    use std::os::windows::io::BorrowedHandle;
    use std::os::windows::io::RawHandle;

    let not_open = || io::Error::new(io::ErrorKind::InvalidInput, "File descriptor is not open.");
    let raw = i32::try_from(fd).map_err(|_| not_open())?;
    // SAFETY: the C runtime checks the descriptor itself, returning -1 for one that is not open,
    // or -2 for a standard stream with no handle.
    let handle = unsafe { libc::get_osfhandle(raw) };
    if handle == -1 || handle == -2 {
        return Err(not_open());
    }
    // SAFETY: borrowed for the duplication only; the C runtime keeps owning the handle.
    let inherited = unsafe { BorrowedHandle::borrow_raw(handle as RawHandle) };
    inherited.try_clone_to_owned().map(std::fs::File::from)
}

/// Whether the socket is listening, or `None` where the provider cannot say (the server finds out
/// at the first `accept()`). `Error::NotSocket` if `fd` is not a socket, closed handles included.
#[expect(clippy::expect_used, reason = "the size of an i32 fits an i32")]
fn is_listener(fd: u32) -> Result<Option<bool>, Error> {
    use windows_sys::Win32::Networking::WinSock as winsock;

    let mut acceptcon: i32 = 0;
    let mut optlen = i32::try_from(size_of::<i32>()).expect("the size of an i32 fits an i32");
    // SAFETY: winsock validates the handle itself and reports a bad one through the return value;
    // the pointers name `acceptcon` and `optlen`, which outlive the call.
    let result = unsafe {
        winsock::getsockopt(
            fd as winsock::SOCKET,
            winsock::SOL_SOCKET,
            winsock::SO_ACCEPTCONN,
            (&raw mut acceptcon).cast(),
            &raw mut optlen,
        )
    };
    if result == winsock::SOCKET_ERROR {
        // SAFETY: reads this thread's last winsock error, set by the call above.
        return match unsafe { winsock::WSAGetLastError() } {
            winsock::WSAENOTSOCK => Err(Error::NotSocket),
            winsock::WSAENOPROTOOPT => Ok(None),
            error => Err(Error::Os(io::Error::from_raw_os_error(error))),
        };
    }
    Ok(Some(acceptcon != 0))
}
