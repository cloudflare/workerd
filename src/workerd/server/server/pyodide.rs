// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The Pyodide bundle a Python worker's compatibility flags select, loaded into the process
//! before the worker compiles (`preloadPython`, factory/worker-factory.c++).
//!
//! A bundle comes from the disk cache (`--pyodide-bundle-disk-cache-dir`) or, failing that, is
//! downloaded and then written to the cache. Either way its bytes are checked against the
//! release's checksum before the factory hands them to its `PyodideBundleManager`. The "dev"
//! bundle is built from the tree and has no published checksum: it is never downloaded or
//! checked, and is found in the cache or not at all.
//!
//! The download is one HTTP/1.1 GET on the calling thread's tokio runtime, over TLS verified by
//! the platform's trust store (kj-hyper's `trust_browser_cas`). The cache is read and written on
//! tokio's blocking pool, as a bundle is tens of MB.

use std::fs;
use std::fs::OpenOptions;
use std::future::poll_fn;
use std::io;
use std::io::Write;
use std::path::Path;
use std::pin::Pin;
use std::pin::pin;

use base64::Engine;
use base64::engine::general_purpose::STANDARD;
use futures::future::Either;
use futures::future::select;
use hyper::body::Body;
use hyper_util::rt::TokioIo;
use kj_hyper::io_kj_error;
use kj_hyper::tls::TlsOptions;
use kj_rs::KjMaybe;
use tokio::io::AsyncRead;
use tokio::io::AsyncWrite;

use crate::Result;

/// Where released bundles are downloaded from: the bundle's file name is appended.
const BUNDLE_URL_PREFIX: &str = "https://pyodide-capnp-bin.edgeworker.net/";

/// The bundle built from the tree, for testing.
const DEV: &str = "dev";

/// The name of bundle `version`'s file, in the disk cache and under [`BUNDLE_URL_PREFIX`].
fn bundle_file_name(version: &str) -> String {
    format!("pyodide_{version}.capnp.bin")
}

/// The subresource-integrity-style checksum (`sha256-<base64>`) of `bytes`.
fn bundle_integrity(bytes: &[u8]) -> String {
    let digest = ring::digest::digest(&ring::digest::SHA256, bytes);
    format!("sha256-{}", STANDARD.encode(digest.as_ref()))
}

/// Checks bundle `version`'s bytes against `expected`, the release's checksum. Every bundle but
/// "dev" must have one.
///
/// # Errors
///
/// A missing or mismatching checksum.
fn verify_bundle_integrity(version: &str, expected: &str, bytes: &[u8]) -> Result<()> {
    if version == DEV {
        return Ok(());
    }
    if expected.is_empty() {
        return Err(kj::failed!(
            "Pyodide bundle is missing an integrity checksum; refusing to use it.; \
             version = {version}"
        ));
    }
    let actual = bundle_integrity(bytes);
    if actual != expected {
        return Err(kj::failed!(
            "Pyodide bundle integrity check failed: the bundle does not match the expected \
             checksum.; version = {version}; expectedIntegrity = {expected}; \
             actualIntegrity = {actual}"
        ));
    }
    Ok(())
}

/// Bundle `version`, verified against `integrity`.
///
/// It is read from the file in `cache_dir`, else (but for "dev", which is then `None`)
/// downloaded from `url_prefix` over TLS configured by `tls` and written to `cache_dir`. A
/// `cache_dir` that is not a directory is no cache. Outside tests `url_prefix` is
/// [`BUNDLE_URL_PREFIX`] and `tls` trusts the platform's store.
///
/// # Errors
///
/// A failed download (a status other than 200 included), a cache that cannot be read or
/// written, or bytes that fail [`verify_bundle_integrity`]. A cached bundle that fails the check
/// is an error, not a reason to download it again.
async fn load_bundle(
    cache_dir: Option<&Path>,
    url_prefix: &str,
    tls: &TlsOptions,
    version: &str,
    integrity: &str,
) -> Result<Option<Vec<u8>>> {
    let name = bundle_file_name(version);
    let cache_dir = cache_dir.filter(|dir| dir.is_dir());
    if let Some(dir) = cache_dir {
        let path = dir.join(&name);
        if let Some(bytes) = blocking(move || read_cached(&path)).await? {
            verify_bundle_integrity(version, integrity, &bytes)?;
            return Ok(Some(bytes));
        }
    }
    if version == DEV {
        return Ok(None);
    }
    let url = format!("{url_prefix}{name}");
    tracing::info!(url = %url, "Loading Pyodide bundle from internet");
    let bytes = download(&url, tls).await?;
    verify_bundle_integrity(version, integrity, &bytes)?;
    let Some(dir) = cache_dir else {
        return Ok(Some(bytes));
    };
    let dir = dir.to_owned();
    let bytes = blocking(move || write_cached(&dir, &name, &bytes).map(|()| bytes)).await?;
    Ok(Some(bytes))
}

/// `f()` on tokio's blocking pool. If the future is dropped first, `f` still runs to the end.
async fn blocking<T: Send + 'static>(f: impl FnOnce() -> Result<T> + Send + 'static) -> Result<T> {
    tokio::task::spawn_blocking(f)
        .await
        .map_err(|e| kj::failed!("Pyodide bundle disk cache: {e}"))?
}

/// The cached file at `path`, `None` if there is none.
fn read_cached(path: &Path) -> Result<Option<Vec<u8>>> {
    match fs::read(path) {
        Ok(bytes) => Ok(Some(bytes)),
        Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(None),
        Err(e) => Err(cache_error(path, &e)),
    }
}

/// Writes `bytes` to the file `name` in `dir`, replacing it at once: a reader sees the old file
/// or the whole new one.
fn write_cached(dir: &Path, name: &str, bytes: &[u8]) -> Result<()> {
    let path = dir.join(name);
    let temp = dir.join(format!(".{name}.{:016x}.tmp", rand::random::<u64>()));
    let written = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&temp)
        .and_then(|mut file| {
            file.write_all(bytes)?;
            file.sync_all()
        })
        .map_err(|e| cache_error(&temp, &e))
        .and_then(|()| fs::rename(&temp, &path).map_err(|e| cache_error(&path, &e)));
    if written.is_err() {
        let _ = fs::remove_file(&temp);
    }
    written
}

fn cache_error(path: &Path, e: &io::Error) -> crate::Error {
    let error = io_kj_error(e);
    cxx::KjError::new(
        error.exception_type(),
        format!(
            "Pyodide bundle disk cache: {}: {}",
            path.display(),
            error.description()
        ),
    )
}

fn request_failed(url: &str, what: impl std::fmt::Display) -> crate::Error {
    kj::failed!("Request for Pyodide bundle failed; url = {url}; {what}")
}

/// `error`, a failed step of the request for `url` (resolving, connecting, the TLS handshake),
/// with the request named and its exception type kept.
fn request_error(url: &str, error: &crate::Error) -> crate::Error {
    cxx::KjError::new(
        error.exception_type(),
        format!(
            "Request for Pyodide bundle failed; url = {url}; {}",
            error.description()
        ),
    )
}

/// The body of a GET of `url`, which must answer 200.
async fn download(url: &str, tls: &TlsOptions) -> Result<Vec<u8>> {
    let uri: http::Uri = url.parse().map_err(|e| request_failed(url, e))?;
    let https = uri.scheme() == Some(&http::uri::Scheme::HTTPS);
    let (Some(authority), Some(host)) = (uri.authority(), uri.host()) else {
        return Err(request_failed(url, "the URL names no host"));
    };
    let host = host.trim_matches(['[', ']']);
    let port = uri.port_u16().unwrap_or(if https { 443 } else { 80 });
    let target = uri.path_and_query().map_or("/", |p| p.as_str());
    let request = http::Request::get(target)
        .header(http::header::HOST, authority.as_str())
        .body(String::new())
        .map_err(|e| request_failed(url, e))?;
    let tcp = kj_hyper::client::connect_allowed(host, port, |_| true)
        .await
        .map_err(|e| request_error(url, &io_kj_error(&e)))?;
    if https {
        let tls = kj_hyper::tls::client_config(tls).map_err(|e| request_error(url, &e))?;
        let stream = kj_hyper::tls::connect(tcp, tls, host)
            .await
            .map_err(|e| request_error(url, &e))?;
        get(url, stream, request).await
    } else {
        get(url, tcp, request).await
    }
}

/// Sends `request` on a new connection over `io` and reads the response, driving the connection
/// until the body has been read.
async fn get<IO>(url: &str, io: IO, request: http::Request<String>) -> Result<Vec<u8>>
where
    IO: AsyncRead + AsyncWrite + Unpin + Send + 'static,
{
    let (mut sender, connection) = hyper::client::conn::http1::handshake(TokioIo::new(io))
        .await
        .map_err(|e| request_failed(url, e))?;
    let exchange = pin!(async move {
        let response = sender
            .send_request(request)
            .await
            .map_err(|e| request_failed(url, e))?;
        let status = response.status().as_u16();
        if status != 200 {
            return Err(request_failed(url, format!("statusCode = {status}")));
        }
        let mut body = response.into_body();
        let mut bytes = Vec::new();
        while let Some(frame) = poll_fn(|cx| Pin::new(&mut body).poll_frame(cx)).await {
            let frame = frame.map_err(|e| request_failed(url, e))?;
            if let Ok(data) = frame.into_data() {
                bytes.extend_from_slice(&data);
            }
        }
        Ok(bytes)
    });
    match select(exchange, pin!(connection)).await {
        Either::Left((result, _)) => result,
        // The connection is done; what it delivered is still to be read.
        Either::Right((Ok(()), exchange)) => exchange.await,
        Either::Right((Err(e), _)) => Err(request_failed(url, e)),
    }
}

/// See the bridge's declaration: [`load_bundle`] from [`BUNDLE_URL_PREFIX`], empty for no
/// bundle.
///
/// # Errors
///
/// As for [`load_bundle`], or an empty bundle, which would read as no bundle.
pub async fn pyodide_bundle_fetch<'a>(
    cache_dir: KjMaybe<&'a str>,
    version: &'a str,
    integrity: &'a str,
) -> Result<Vec<u8>> {
    let cache_dir: Option<&str> = cache_dir.into();
    let tls = TlsOptions {
        trust_browser_cas: true,
        ..TlsOptions::default()
    };
    let bundle = load_bundle(
        cache_dir.map(Path::new),
        BUNDLE_URL_PREFIX,
        &tls,
        version,
        integrity,
    )
    .await?;
    match bundle {
        Some(bytes) if bytes.is_empty() => {
            Err(kj::failed!("Pyodide bundle is empty; version = {version}"))
        }
        bundle => Ok(bundle.unwrap_or_default()),
    }
}

#[cfg(test)]
#[path = "pyodide-test.rs"]
mod tests;
