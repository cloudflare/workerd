use std::io;
use std::pin::Pin;

use cxx::KjError;
use kj_rs_io::TokioStream;
use tokio::io::Interest;
use tokio::net::TcpStream;

use crate::ffi::KjAsyncIoStream;
use crate::ffi::PreboundListener;

type Result<T> = std::result::Result<T, KjError>;

fn kj_err(message: impl std::fmt::Display) -> KjError {
    KjError::new(cxx::KjExceptionType::Failed, message.to_string())
}

/// Write-all over the native tokio readiness API.
async fn native_write_all(stream: &TcpStream, data: &[u8]) -> io::Result<()> {
    let mut written = 0;
    while written < data.len() {
        match stream.try_write(&data[written..]) {
            Ok(n) => written += n,
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                stream.ready(Interest::WRITABLE).await?;
            }
            Err(e) => return Err(e),
        }
    }
    Ok(())
}

pub async fn native_write_via_unwrap(stream: Box<TokioStream>, data: Vec<u8>) -> Result<()> {
    let tcp = stream
        .into_tcp_stream()
        .ok_or_else(|| kj_err("expected a TCP stream"))?;
    native_write_all(&tcp, &data).await.map_err(kj_err)?;
    // Dropping `tcp` closes the connection; the C++ side observes data followed by EOF.
    Ok(())
}

pub async fn native_write_via_kj_unwrap(
    stream: Pin<&mut KjAsyncIoStream>,
    data: &[u8],
) -> Result<()> {
    // unwrap_kj_stream is safe now: it detects in-flight I/O and returns an error rather than
    // aliasing (the C++ test also keeps no I/O in flight here).
    let native = kj_rs_io::unwrap_kj_stream(stream).map_err(KjError::from)?;
    let tcp = native
        .into_tcp_stream()
        .ok_or_else(|| kj_err("expected a TCP stream"))?;
    native_write_all(&tcp, data).await.map_err(kj_err)?;
    Ok(())
}

pub fn create_prebound_listener_fd() -> Result<PreboundListener> {
    let listener = std::net::TcpListener::bind("127.0.0.1:0").map_err(kj_err)?;
    let port = listener.local_addr().map_err(kj_err)?.port();
    #[cfg(unix)]
    {
        use std::os::fd::IntoRawFd;
        Ok(PreboundListener {
            fd: listener.into_raw_fd(),
            port,
        })
    }
    #[cfg(not(unix))]
    {
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
