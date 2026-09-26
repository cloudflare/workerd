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

    pub fn duplicate_for_server(&self) -> io::Result<i64> {
        Ok(i64::from(self.0))
    }
}

/// Whether the socket is listening, or `None` where the provider cannot say (the server finds out
/// at the first `accept()`). `Error::NotSocket` if `fd` is not a socket, closed handles included.
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
