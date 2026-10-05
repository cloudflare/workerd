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
