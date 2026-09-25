//! `--socket-fd`: listen sockets inherited from the parent process, named by descriptor on the
//! command line.
//!
//! This process keeps the inherited descriptor itself, exactly as it arrived (no close-on-exec, so
//! a `--watch` re-exec passes it on under the same number), and gives the server a duplicate to
//! own. This is the one place a number from the command line becomes an owned descriptor.

// The platform module turns a descriptor number into an owned handle; `unsafe` is confined to it.
#![allow(unsafe_code)]

use std::fmt;
use std::io;

#[derive(Debug)]
pub enum Error {
    #[cfg_attr(
        windows,
        expect(dead_code, reason = "winsock reports a closed handle as not a socket")
    )]
    NotOpen,
    NotSocket,
    NotListening,
    Os(io::Error),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::NotOpen => f.write_str("File descriptor is not open."),
            Self::NotSocket => f.write_str("File descriptor is not a socket."),
            Self::NotListening => f.write_str("Socket is not listening."),
            Self::Os(error) => write!(f, "{error}"),
        }
    }
}

#[cfg_attr(unix, path = "socket_fd/unix.rs")]
#[cfg_attr(windows, path = "socket_fd/windows.rs")]
mod imp;

pub use imp::InheritedSocket;
