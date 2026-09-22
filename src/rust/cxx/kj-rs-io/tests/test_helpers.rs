use cxx::KjError;

use crate::ffi::PreboundListener;

type Result<T> = std::result::Result<T, KjError>;

fn kj_err(message: impl std::fmt::Display) -> KjError {
    KjError::new(cxx::KjExceptionType::Failed, message.to_string())
}

pub fn create_prebound_listener_fd() -> Result<PreboundListener> {
    let listener = std::net::TcpListener::bind("127.0.0.1:0").map_err(kj_err)?;
    #[cfg(unix)]
    {
        use std::os::fd::IntoRawFd;
        let port = listener.local_addr().map_err(kj_err)?.port();
        Ok(PreboundListener {
            fd: listener.into_raw_fd(),
            port,
        })
    }
    #[cfg(not(unix))]
    {
        drop(listener);
        Err(kj_err("not supported on this platform"))
    }
}

/// See the bridge doc on `address_from_loopback_ports` (lib.rs).
pub fn address_from_loopback_ports(ports: &[u16]) -> Box<kj_rs_io::TokioAddress> {
    let addrs = ports
        .iter()
        .map(|&port| std::net::SocketAddr::from((std::net::Ipv4Addr::LOCALHOST, port)))
        .collect();
    Box::new(kj_rs_io::TokioAddress::from_socket_addrs(addrs))
}
