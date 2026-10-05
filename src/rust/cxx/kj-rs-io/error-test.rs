use super::*;

fn kind_error(kind: std::io::ErrorKind) -> std::io::Error {
    std::io::Error::new(kind, "synthetic")
}

#[test]
fn errorkind_fallback_matches_kj_classes() {
    use std::io::ErrorKind;
    for kind in [
        ErrorKind::ConnectionRefused,
        ErrorKind::ConnectionReset,
        ErrorKind::ConnectionAborted,
        ErrorKind::BrokenPipe,
        ErrorKind::NotConnected,
        ErrorKind::UnexpectedEof,
    ] {
        assert_eq!(
            exception_type(&kind_error(kind)),
            KjExceptionType::Disconnected,
            "{kind:?}"
        );
    }
    assert_eq!(
        exception_type(&kind_error(ErrorKind::TimedOut)),
        KjExceptionType::Overloaded
    );
    assert_eq!(
        exception_type(&kind_error(ErrorKind::OutOfMemory)),
        if cfg!(windows) {
            KjExceptionType::Failed
        } else {
            KjExceptionType::Overloaded
        }
    );
    assert_eq!(
        exception_type(&kind_error(ErrorKind::Unsupported)),
        KjExceptionType::Unimplemented
    );
    assert_eq!(
        exception_type(&kind_error(ErrorKind::Other)),
        KjExceptionType::Failed
    );
    assert_eq!(
        exception_type(&kind_error(ErrorKind::InvalidInput)),
        KjExceptionType::Failed
    );
}

#[cfg(windows)]
#[test]
fn win32_table_matches_kj() {
    let of = |code: i32| exception_type(&std::io::Error::from_raw_os_error(code));

    for code in [win32::WSAETIMEDOUT, win32::ERROR_SEM_TIMEOUT] {
        assert_eq!(of(code), KjExceptionType::Overloaded, "error {code}");
    }
    for code in [
        win32::WSAENOTCONN,
        win32::WSAECONNABORTED,
        win32::WSAECONNREFUSED,
        win32::WSAECONNRESET,
        win32::WSAEHOSTDOWN,
        win32::WSAEHOSTUNREACH,
        win32::WSAENETDOWN,
        win32::WSAENETRESET,
        win32::WSAENETUNREACH,
        win32::WSAESHUTDOWN,
        win32::ERROR_NETNAME_DELETED,
        win32::ERROR_CONNECTION_ABORTED,
        win32::ERROR_CONNECTION_REFUSED,
        win32::ERROR_CONNECTION_INVALID,
        win32::ERROR_HOST_UNREACHABLE,
        win32::ERROR_NETWORK_UNREACHABLE,
        win32::ERROR_PORT_UNREACHABLE,
        win32::ERROR_BROKEN_PIPE,
        win32::ERROR_NO_DATA,
        win32::ERROR_PIPE_NOT_CONNECTED,
    ] {
        assert_eq!(of(code), KjExceptionType::Disconnected, "error {code}");
    }
    for code in [
        win32::WSAEOPNOTSUPP,
        win32::WSAENOPROTOOPT,
        win32::WSAENOTSOCK,
    ] {
        assert_eq!(of(code), KjExceptionType::Unimplemented, "error {code}");
    }
    for code in [0, 5, 8, 87, 10013] {
        assert_eq!(of(code), KjExceptionType::Failed, "error {code}");
    }
}

/// The raw-errno table must mirror KJ's `typeOfErrno()` errno-for-errno, since consumers
/// (kj-http, capnp-rpc) change behavior on the class. Spot-checks one member of every
/// class plus the classic confusables (ETIMEDOUT is OVERLOADED, not DISCONNECTED).
#[cfg(unix)]
#[test]
fn errno_table_matches_kj() {
    let of = |errno: i32| exception_type(&std::io::Error::from_raw_os_error(errno));
    // OVERLOADED
    for errno in [
        libc::EMFILE,
        libc::ENFILE,
        libc::ENOBUFS,
        libc::ENOMEM,
        libc::ETIMEDOUT,
    ] {
        assert_eq!(of(errno), KjExceptionType::Overloaded, "errno {errno}");
    }
    // DISCONNECTED
    for errno in [
        libc::ECONNREFUSED,
        libc::ECONNRESET,
        libc::EPIPE,
        libc::ENOTCONN,
        libc::EHOSTUNREACH,
        libc::ENETDOWN,
    ] {
        assert_eq!(of(errno), KjExceptionType::Disconnected, "errno {errno}");
    }
    // UNIMPLEMENTED
    for errno in [
        libc::ENOSYS,
        libc::ENOTSUP,
        libc::ENOPROTOOPT,
        libc::ENOTSOCK,
    ] {
        assert_eq!(of(errno), KjExceptionType::Unimplemented, "errno {errno}");
    }
    // FAILED (everything else)
    for errno in [libc::EINVAL, libc::EACCES, libc::EBADF, libc::EEXIST] {
        assert_eq!(of(errno), KjExceptionType::Failed, "errno {errno}");
    }
    // Platform-conditional entries mirror KJ's #ifdefs.
    #[cfg(any(target_os = "linux", target_os = "android"))]
    assert_eq!(of(libc::ENONET), KjExceptionType::Disconnected);
    #[cfg(not(any(target_os = "linux", target_os = "android")))]
    assert_eq!(of(libc::EOPNOTSUPP), KjExceptionType::Unimplemented);
}

#[test]
fn description_is_op_colon_message() {
    let err = KjIoError::other("connect()", "boom");
    let kj = KjError::from(err);
    assert_eq!(kj.description(), "connect(): boom");
    assert_eq!(kj.exception_type(), KjExceptionType::Failed);

    let err = op("read()")(kind_error(std::io::ErrorKind::ConnectionReset));
    let kj = KjError::from(err);
    assert!(
        kj.description().starts_with("read(): "),
        "{}",
        kj.description()
    );
    assert_eq!(kj.exception_type(), KjExceptionType::Disconnected);
}
