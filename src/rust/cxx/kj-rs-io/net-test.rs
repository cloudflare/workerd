use std::net::IpAddr;
use std::net::Ipv4Addr;
use std::net::Ipv6Addr;
use std::net::SocketAddr;
use std::task::Waker;

use cxx::KjError;
use static_assertions::assert_impl_all;

use super::*;

assert_impl_all!(TokioListener: Send, Sync);
assert_impl_all!(TokioAddress: Send, Sync);
assert_impl_all!(TokioDatagram: Send, Sync);

fn parse_once(text: &[u8], port_hint: u16) -> Option<Result<TokioAddress>> {
    parse_once_in(text, port_hint, &LoopbackRegistry::new())
}

fn parse_once_in(
    text: &[u8],
    port_hint: u16,
    loopback: &LoopbackRegistry,
) -> Option<Result<TokioAddress>> {
    let mut fut = std::pin::pin!(TokioAddress::parse(text, port_hint, loopback));
    let mut cx = Context::from_waker(Waker::noop());
    match fut.as_mut().poll(&mut cx) {
        Poll::Ready(result) => Some(result),
        Poll::Pending => None,
    }
}

fn parse_ok(text: &str, port_hint: u16) -> TokioAddress {
    match parse_once(text.as_bytes(), port_hint) {
        Some(Ok(addr)) => addr,
        Some(Err(e)) => panic!(
            "{text:?} failed to parse: {}",
            KjError::from(e).description()
        ),
        None => panic!("{text:?} unexpectedly went to DNS"),
    }
}

fn parse_err(text: &str, port_hint: u16) -> KjError {
    match parse_once(text.as_bytes(), port_hint) {
        Some(Ok(_)) => panic!("{text:?} unexpectedly parsed"),
        Some(Err(e)) => KjError::from(e),
        None => panic!("{text:?} unexpectedly went to DNS"),
    }
}

fn err_of<T>(result: Result<T>) -> KjError {
    match result {
        Ok(_) => panic!("expected an error"),
        Err(e) => KjError::from(e),
    }
}

fn ip_addrs(addr: &TokioAddress) -> (&[SocketAddr], bool) {
    match &addr.spec {
        Spec::Ip { addrs, wildcard } => (addrs, *wildcard),
        _ => panic!("expected an IP address"),
    }
}

/// How many sockets a listener binds (KJ's aggregate receiver: one per resolved address).
fn socket_count(listener: &TokioListener) -> usize {
    match &listener.shared.backend {
        ListenerBackend::Sockets { inners, .. } => inners.len(),
        ListenerBackend::Loopback(_) => panic!("expected a socket listener"),
    }
}

#[test]
fn loopback_addresses_need_the_registry_enabled() {
    let registry = LoopbackRegistry::new();
    // Disabled: "loopback:svc" is a hostname lookup, which this port-less thread refuses.
    let Some(Err(err)) = parse_once_in(b"loopback:svc", 0, &registry) else {
        panic!("expected the hostname lookup to be refused at once")
    };
    assert!(
        KjError::from(err)
            .description()
            .contains("no TokioEventPort")
    );

    registry.enable();
    // Binding and listening register with the loop; parsing a loopback address does not.
    let _port = kj_rs_tokio::TokioPort::new();
    let addr = parse_once_in(b"loopback:svc", 0, &registry)
        .unwrap()
        .unwrap();
    assert_eq!(addr.to_display_bytes(), b"loopback:svc");
    let targets = addr.targets().unwrap();
    assert_eq!(targets, [SocketAddress::loopback(b"svc")]);
    // Same name, same queue; the registry's children (clone_handle) see it too.
    let again = parse_once_in(b"loopback:svc", 0, &registry.clone_handle())
        .unwrap()
        .unwrap();
    match (&addr.spec, &again.spec) {
        (Spec::Loopback(a), Spec::Loopback(b)) => {
            assert!(Arc::ptr_eq(a, b));
        }
        _ => panic!("expected loopback addresses"),
    }
    let Err(err) = addr.bind_datagram() else {
        panic!("expected bind_datagram() to fail")
    };
    assert!(
        KjError::from(err)
            .description()
            .contains("loopback addresses do not support datagrams")
    );
}

#[test]
fn socket_pair_accepts_only_its_own_client() {
    let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let addr = listener.local_addr().unwrap();
    let foreign = std::net::TcpStream::connect(addr).unwrap();
    let client = std::net::TcpStream::connect(addr).unwrap();

    let server = accept_socket_pair_peer(&listener, &client).unwrap();
    assert_eq!(server.peer_addr().unwrap(), client.local_addr().unwrap());

    drop(foreign);
}

#[test]
fn ipv4_literal_with_and_without_port() {
    let addr = parse_ok("1.2.3.4:80", 0);
    let (addrs, wildcard) = ip_addrs(&addr);
    assert_eq!(
        addrs,
        &[SocketAddr::new(IpAddr::V4(Ipv4Addr::new(1, 2, 3, 4)), 80)]
    );
    assert!(!wildcard);

    let addr = parse_ok("1.2.3.4", 8080);
    assert_eq!(ip_addrs(&addr).0[0].port(), 8080);
}

#[test]
fn ipv6_literal_forms() {
    let addr = parse_ok("[::1]:443", 0);
    assert_eq!(
        ip_addrs(&addr).0,
        &[SocketAddr::new(IpAddr::V6(Ipv6Addr::LOCALHOST), 443)]
    );
    let addr = parse_ok("[::1]", 7);
    assert_eq!(ip_addrs(&addr).0[0].port(), 7);
    let addr = parse_ok("fe80::1", 9);
    assert_eq!(
        ip_addrs(&addr).0[0],
        SocketAddr::new("fe80::1".parse::<IpAddr>().unwrap(), 9)
    );
}

#[test]
fn wildcard_forms() {
    let addr = parse_ok("*", 1234);
    let (addrs, wildcard) = ip_addrs(&addr);
    assert!(wildcard);
    assert_eq!(
        addrs,
        &[SocketAddr::new(IpAddr::V6(Ipv6Addr::UNSPECIFIED), 1234)]
    );
    let addr = parse_ok("*:80", 0);
    let (addrs, wildcard) = ip_addrs(&addr);
    assert!(wildcard);
    assert_eq!(addrs[0].port(), 80);
    assert!(
        KjError::from(addr.targets().unwrap_err())
            .description()
            .contains("wildcard")
    );
}

#[test]
fn port_text_is_decimal_or_a_service_name() {
    assert_eq!(ip_addrs(&parse_ok("127.0.0.1:80", 0)).0[0].port(), 80);
    assert_eq!(ip_addrs(&parse_ok("127.0.0.1:0", 5)).0[0].port(), 0);
    assert_eq!(ip_addrs(&parse_ok("127.0.0.1", 5)).0[0].port(), 5);
    for too_large in ["127.0.0.1:65536", "127.0.0.1:99999999999999999999"] {
        assert!(
            parse_err(too_large, 0)
                .description()
                .contains("Port number too large."),
            "{too_large}"
        );
    }
    let _port = kj_rs_tokio::TokioPort::new();
    for service in [
        "127.0.0.1:http",
        "127.0.0.1:0x50",
        "127.0.0.1:-1",
        "127.0.0.1:",
    ] {
        assert!(
            parse_once(service.as_bytes(), 0).is_none(),
            "{service}: not decimal, so a service name for getaddrinfo"
        );
    }
}

/// Every `SocketAddress` kind survives the round trip through a `TokioAddress` (what
/// `getSockaddr` followed by `toString` / `connect` sees) and back to a typed address.
#[test]
fn typed_addresses_round_trip() {
    for text in ["1.2.3.4:80", "[::1]:443", "[fe80::1%7]:0"] {
        let addr: SocketAddr = text.parse().unwrap();
        let typed = SocketAddress::from(addr);
        assert_eq!(ip_socket_addr(&typed).unwrap(), addr, "{text}");
        let back = network_address_from(&typed).unwrap();
        assert_eq!(ip_addrs(&back).0, &[addr]);
        assert_eq!(back.targets().unwrap(), vec![typed]);
    }
    let unnamed = SocketAddress::blank(AddressKind::UnixUnnamed);
    #[cfg(unix)]
    let expected = "unnamed";
    #[cfg(windows)]
    let expected = "not a socket address this platform supports";
    assert!(
        err_of(network_address_from(&unnamed))
            .description()
            .contains(expected),
        "an unnamed peer is not an address one can connect to"
    );
    assert!(ip_socket_addr(&unnamed).is_err());
}

#[cfg(unix)]
#[test]
fn unix_forms() {
    use std::os::unix::ffi::OsStrExt;
    let addr = parse_ok("unix:/tmp/sock", 0);
    assert_eq!(addr.to_display_bytes(), b"unix:/tmp/sock");
    assert!(
        matches!(&addr.spec, Spec::Unix(UnixName::Path(path)) if path == std::path::Path::new("/tmp/sock"))
    );
    let too_long = format!("unix:/{}", "x".repeat(200));
    assert!(
        parse_err(&too_long, 0)
            .description()
            .contains("parseAddress")
    );
    assert!(
        parse_err("unix:/tmp/a\0b", 0)
            .description()
            .contains("contains NULL")
    );
    let raw = b"unix:/tmp/\xff\xfe";
    let Some(Ok(addr)) = parse_once(raw, 0) else {
        panic!("a non-UTF-8 unix path must parse");
    };
    assert!(matches!(
        &addr.spec,
        Spec::Unix(UnixName::Path(path)) if path.as_os_str().as_bytes() == b"/tmp/\xff\xfe"
    ));
    assert_eq!(addr.to_display_bytes(), raw);

    // Typed round trip, including a path that fills sun_path (KJ allows the unterminated
    // form; std refuses to bind or connect it, which is where that surfaces).
    let targets = addr.targets().unwrap();
    assert_eq!(targets.len(), 1);
    assert_eq!(targets[0].kind, AddressKind::UnixPath);
    assert_eq!(targets[0].name, b"/tmp/\xff\xfe");
    let back = network_address_from(&targets[0]).unwrap();
    assert_eq!(back.to_display_bytes(), addr.to_display_bytes());
    let mut full = SocketAddress::blank(AddressKind::UnixPath);
    full.name = vec![b'x'; 108];
    full.name[0] = b'/';
    let whole = network_address_from(&full).unwrap();
    assert_eq!(
        whole.to_display_bytes().len(),
        5 + 108,
        "printed whole, like KJ"
    );
    assert!(
        err_of(
            whole
                .targets()
                .and_then(|t| UnixName::from_socket_address(&t[0])?.to_tokio())
        )
        .description()
        .contains("parseAddress"),
        "108 bytes leaves no room for std's NUL: connect/listen is where it fails"
    );
    let peer = std::os::unix::net::SocketAddr::from_pathname("/tmp/peer").unwrap();
    assert_eq!(
        SocketAddress::from(&peer),
        UnixName::Path(PathBuf::from("/tmp/peer")).to_socket_address()
    );
}

#[cfg(target_os = "linux")]
#[test]
fn abstract_unix_forms() {
    use std::os::linux::net::SocketAddrExt;
    let addr = parse_ok("unix-abstract:kj-rs-io", 0);
    assert_eq!(addr.to_display_bytes(), b"unix-abstract:kj-rs-io");
    let targets = addr.targets().unwrap();
    assert_eq!(targets[0].kind, AddressKind::UnixAbstract);
    assert_eq!(targets[0].name, b"kj-rs-io");
    let back = network_address_from(&targets[0]).unwrap();
    assert_eq!(back.to_display_bytes(), addr.to_display_bytes());
    let too_long = format!("unix-abstract:{}", "x".repeat(200));
    assert!(
        parse_err(&too_long, 0)
            .description()
            .contains("parseAddress")
    );
    let std_addr = std::os::unix::net::SocketAddr::from_abstract_name(b"peer").unwrap();
    assert_eq!(
        SocketAddress::from(&std_addr),
        UnixName::Abstract(b"peer".to_vec()).to_socket_address()
    );
    let unnamed = std::os::unix::net::SocketAddr::from_pathname("").unwrap();
    assert_eq!(SocketAddress::from(&unnamed).kind, AddressKind::UnixUnnamed);
}

#[cfg(all(unix, not(target_os = "linux")))]
#[test]
fn abstract_unix_names_are_linux_only() {
    assert!(
        parse_err("unix-abstract:kj-rs-io", 0)
            .description()
            .contains("only supported on Linux")
    );
    let mut typed = SocketAddress::blank(AddressKind::UnixAbstract);
    typed.name = b"peer".to_vec();
    assert!(
        err_of(network_address_from(&typed))
            .description()
            .contains("only supported on Linux")
    );
}

#[test]
fn address_lists_dedup_in_order() {
    let v4a: SocketAddr = "10.0.0.2:1".parse().unwrap();
    let v6: SocketAddr = "[::2]:2".parse().unwrap();
    assert_eq!(dedup_in_order(vec![v4a, v6, v4a, v6, v4a]), vec![v4a, v6]);
    let addr = TokioAddress::from_socket_addrs(vec![v6, v6, v4a, v6]);
    assert_eq!(ip_addrs(&addr).0, &[v6, v4a]);
    let _port = kj_rs_tokio::TokioPort::new();
    let loopback: SocketAddr = "127.0.0.1:0".parse().unwrap();
    let listener = TokioAddress::from_socket_addrs(vec![loopback, loopback])
        .listen()
        .expect("duplicates collapse to one socket");
    assert_eq!(socket_count(&listener), 1);
}

#[test]
fn rejects_non_utf8_non_unix_text() {
    let Some(Err(e)) = parse_once(b"\xff\xfe:80", 0) else {
        panic!("non-UTF-8 host text must be rejected");
    };
    assert!(KjError::from(e).description().contains("UTF-8"));
}

#[test]
fn empty_address_lists_are_errors() {
    let _port = kj_rs_tokio::TokioPort::new();
    let addr = TokioAddress::from_socket_addrs(Vec::new());
    assert!(
        KjError::from(addr.listen().err().expect("listen on nothing fails"))
            .description()
            .contains("no addresses to bind")
    );
    assert!(addr.targets().unwrap().is_empty());
}

#[test]
fn parse_never_panics_on_random_input_and_literals_round_trip() {
    const ALPHABET: &[u8] = b"0123456789abcdef:.[]*%-/xu";
    // No TokioPort on purpose: a host-like input then fails at `ensure_loop_thread` instead
    // of starting a real getaddrinfo on the blocking pool (deterministic, no DNS traffic).
    let mut state: u64 = 0x2545_f491_4f6c_dd1d;
    let mut next = move || {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        state
    };
    for _ in 0..20_000 {
        let len = usize::try_from(next() % 24).unwrap();
        let text: Vec<u8> = (0..len)
            .map(|_| ALPHABET[usize::try_from(next() % ALPHABET.len() as u64).unwrap()])
            .collect();
        let hint = u16::try_from(next() & 0xffff).unwrap();
        let Some(Ok(addr)) = parse_once(&text, hint) else {
            continue; // rejected, or resolving: both fine
        };
        let shown = String::from_utf8(addr.to_display_bytes()).unwrap();
        let Spec::Ip { addrs, wildcard } = &addr.spec else {
            continue;
        };
        if *wildcard {
            assert!(shown.starts_with("*:"), "{text:?} -> {shown:?}");
            continue;
        }
        let expected = addrs[0];
        let reparsed = parse_ok(&shown, 0);
        assert_eq!(ip_addrs(&reparsed).0[0], expected, "{text:?} -> {shown:?}");
    }
}

#[test]
fn bracket_errors_carry_kj_messages() {
    let err = parse_err("[::1", 0);
    assert!(
        err.description().contains("Unclosed '['"),
        "{}",
        err.description()
    );
    let err = parse_err("[::1]x", 0);
    assert!(
        err.description().contains("Expected port suffix after ']'"),
        "{}",
        err.description()
    );
}

#[cfg(unix)]
#[test]
fn accept_and_connect_error_classification_matches_kj() {
    for errno in [
        libc::ECONNABORTED,
        libc::EPROTO,
        libc::ENETDOWN,
        libc::EHOSTUNREACH,
        libc::ETIMEDOUT,
        libc::EINTR,
    ] {
        assert!(
            is_transient_accept_error(&std::io::Error::from_raw_os_error(errno)),
            "errno {errno} should be retried"
        );
    }
    for errno in [libc::EBADF, libc::EMFILE, libc::ENOTSOCK] {
        assert!(
            !is_transient_accept_error(&std::io::Error::from_raw_os_error(errno)),
            "errno {errno} must surface"
        );
    }

    assert!(is_tolerable_nodelay_error(
        &std::io::Error::from_raw_os_error(libc::EOPNOTSUPP)
    ));
    assert!(is_tolerable_nodelay_error(
        &std::io::Error::from_raw_os_error(libc::ENOPROTOOPT)
    ));
    assert!(!is_tolerable_nodelay_error(
        &std::io::Error::from_raw_os_error(libc::EBADF)
    ));
    assert_eq!(
        is_tolerable_nodelay_error(&std::io::Error::from_raw_os_error(libc::EINVAL)),
        cfg!(any(target_os = "macos", target_os = "freebsd"))
    );
}

/// `accept()` is a bridged operation like any other: on a thread without a `TokioEventPort`
/// it fails with a `kj::Exception` rather than waiting on a driver that never turns.
#[test]
fn accept_off_the_loop_thread_is_refused() {
    let _port = kj_rs_tokio::TokioPort::new();
    let listener = TokioAddress::from_socket_addrs(vec!["127.0.0.1:0".parse().unwrap()])
        .listen()
        .unwrap();
    let accept = listener_accept(&listener);
    let err = std::thread::spawn(move || {
        let mut accept = Box::pin(accept);
        let mut cx = Context::from_waker(Waker::noop());
        match accept.as_mut().poll(&mut cx) {
            Poll::Ready(Err(e)) => KjError::from(e),
            _ => panic!("accept off the loop thread must fail at once"),
        }
    })
    .join()
    .unwrap();
    assert!(err.description().contains("no TokioEventPort"));
}

/// A listener carried to another loop thread (its own port) is refused at accept: its
/// sockets are registered with the creator's driver.
#[test]
fn accept_on_a_different_port_is_refused() {
    let _port = kj_rs_tokio::TokioPort::new();
    let listener = TokioAddress::from_socket_addrs(vec!["127.0.0.1:0".parse().unwrap()])
        .listen()
        .unwrap();
    let accept = listener_accept(&listener);
    let err = std::thread::spawn(move || {
        let _other_port = kj_rs_tokio::TokioPort::new();
        let mut accept = Box::pin(accept);
        let mut cx = Context::from_waker(Waker::noop());
        match accept.as_mut().poll(&mut cx) {
            Poll::Ready(Err(e)) => KjError::from(e),
            _ => panic!("accept on a foreign port must fail at once"),
        }
    })
    .join()
    .unwrap();
    assert!(err.description().contains("different TokioEventPort"));
}

#[test]
fn listen_binds_every_resolved_address_or_fails_as_a_whole() {
    let _port = kj_rs_tokio::TokioPort::new();
    let (v4, v6) = (0..16)
        .find_map(|_| {
            let probe = std::net::TcpListener::bind("127.0.0.1:0").ok()?;
            let port = probe.local_addr().ok()?.port();
            drop(probe);
            let v4: SocketAddr = SocketAddr::new(Ipv4Addr::LOCALHOST.into(), port);
            let v6: SocketAddr = SocketAddr::new(Ipv6Addr::LOCALHOST.into(), port);
            std::net::TcpListener::bind(v6).ok().map(|l| {
                drop(l);
                (v4, v6)
            })
        })
        .expect("a port free on both loopback families");

    let listener = TokioAddress::from_socket_addrs(vec![v6, v4])
        .listen()
        .expect("listen on both addresses");
    assert_eq!(
        socket_count(&listener),
        2,
        "one socket per resolved address"
    );
    assert_eq!(
        listener.port().unwrap(),
        v6.port(),
        "getPort() reports the first socket"
    );
    assert_eq!(
        ip_socket_addr(&listener.local_addr().unwrap()).unwrap(),
        v6,
        "getsockname() reports the first socket"
    );
    std::net::TcpStream::connect(v4).expect("IPv4 connect");
    std::net::TcpStream::connect(v6).expect("IPv6 connect");

    // winsock's SO_REUSEADDR lets a second socket bind a port that is already listened on, so
    // the "one bind fails the whole listen" half is a unix check.
    #[cfg(unix)]
    {
        let taken = v4;
        let free: SocketAddr = "127.0.0.1:0".parse().unwrap();
        let err = TokioAddress::from_socket_addrs(vec![free, taken])
            .listen()
            .err()
            .expect("the second bind must fail the listen");
        assert!(
            KjError::from(err).description().contains("bind()"),
            "reported as the bind failure"
        );
    }
}
