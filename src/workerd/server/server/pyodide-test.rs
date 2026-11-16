// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::cell::RefCell;
use std::convert::Infallible;
use std::ops::Deref;
use std::path::PathBuf;
use std::rc::Rc;
use std::sync::Arc;

use hyper::service::service_fn;
use kj_hyper::tls::Keypair;

use super::*;

const VERSION: &str = "0.28.2";
const BUNDLE: &str = "hello pyodide";

fn run<F: Future>(f: F) -> F::Output {
    tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .unwrap()
        .block_on(f)
}

/// An empty directory of its own for each test, removed when dropped.
struct TempDir(PathBuf);

impl Deref for TempDir {
    type Target = Path;
    fn deref(&self) -> &Path {
        &self.0
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

fn temp_dir(test: &str) -> TempDir {
    let root = std::env::var_os("TEST_TMPDIR").map_or_else(std::env::temp_dir, PathBuf::from);
    let dir = root.join(format!("pyodide-{test}-{}", std::process::id()));
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&dir).unwrap();
    TempDir(dir)
}

/// TLS that trusts nothing: plain-HTTP tests never use it.
fn no_tls() -> TlsOptions {
    TlsOptions::default()
}

fn files(dir: &Path) -> Vec<String> {
    let mut names: Vec<String> = fs::read_dir(dir)
        .unwrap()
        .map(|entry| entry.unwrap().file_name().into_string().unwrap())
        .collect();
    names.sort();
    names
}

/// A request the test server received: its path, `Host` header and TLS server name (SNI).
type Seen = Rc<RefCell<Vec<(String, String, Option<String>)>>>;

/// An HTTP server on a local port answering every request with `status` and `body`, one
/// connection at a time: the URL prefix to download from, and the requests it has seen.
async fn serve(status: u16, body: &'static str) -> (String, Seen) {
    serve_on(None, status, body).await
}

/// As [`serve`], over TLS with `config`, at `https://localhost:<port>/`.
async fn serve_tls(config: Arc<rustls::ServerConfig>, body: &'static str) -> (String, Seen) {
    serve_on(Some(config), 200, body).await
}

async fn serve_on(
    tls: Option<Arc<rustls::ServerConfig>>,
    status: u16,
    body: &'static str,
) -> (String, Seen) {
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let port = listener.local_addr().unwrap().port();
    let url = match tls {
        Some(_) => format!("https://localhost:{port}/"),
        None => format!("http://127.0.0.1:{port}/"),
    };
    let seen = Seen::default();
    let log = Rc::clone(&seen);
    tokio::task::spawn_local(async move {
        loop {
            let (stream, _) = listener.accept().await.unwrap();
            let log = Rc::clone(&log);
            match &tls {
                Some(config) => {
                    // A client that refuses the certificate fails the handshake.
                    let Ok(stream) = kj_hyper::tls::accept(stream, Arc::clone(config)).await else {
                        continue;
                    };
                    let sni = stream.get_ref().1.server_name().map(str::to_owned);
                    respond(stream, log, sni, status, body).await;
                }
                None => respond(stream, log, None, status, body).await,
            }
        }
    });
    (url, seen)
}

/// Answers the requests on `io` with `status` and `body`, logging each to `log`.
async fn respond<IO>(io: IO, log: Seen, sni: Option<String>, status: u16, body: &'static str)
where
    IO: AsyncRead + AsyncWrite + Unpin + Send + 'static,
{
    let service = service_fn(move |request: http::Request<hyper::body::Incoming>| {
        let host = request.headers()[http::header::HOST].to_str().unwrap();
        log.borrow_mut().push((
            request.uri().path().to_owned(),
            host.to_owned(),
            sni.clone(),
        ));
        let response = http::Response::builder()
            .status(status)
            .body(body.to_owned())
            .unwrap();
        async move { Ok::<_, Infallible>(response) }
    });
    let _ = hyper::server::conn::http1::Builder::new()
        .serve_connection(TokioIo::new(io), service)
        .await;
}

fn local<F: Future>(f: F) -> F::Output {
    run(tokio::task::LocalSet::new().run_until(f))
}

#[test]
fn integrity_is_a_sha256_subresource_integrity_string() {
    assert_eq!(
        bundle_integrity(b""),
        "sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU="
    );
    assert_eq!(
        bundle_integrity(b"abc"),
        "sha256-ungWv48Bz+pBQUDeXa4iI7ADYaOWF3qctBD/YfIAFa0="
    );
}

#[test]
fn integrity_accepts_a_matching_checksum() {
    let integrity = bundle_integrity(BUNDLE.as_bytes());
    verify_bundle_integrity(VERSION, &integrity, BUNDLE.as_bytes()).unwrap();
}

#[test]
fn integrity_rejects_a_mismatching_checksum() {
    let integrity = bundle_integrity(BUNDLE.as_bytes());
    let error = verify_bundle_integrity(VERSION, &integrity, b"hello pyodide!").unwrap_err();
    assert!(
        error.description().contains("integrity check failed"),
        "{error:?}"
    );
}

#[test]
fn integrity_rejects_a_missing_checksum() {
    let error = verify_bundle_integrity(VERSION, "", BUNDLE.as_bytes()).unwrap_err();
    assert!(
        error
            .description()
            .contains("missing an integrity checksum"),
        "{error:?}"
    );
}

#[test]
fn integrity_is_not_checked_for_dev() {
    let integrity = bundle_integrity(BUNDLE.as_bytes());
    verify_bundle_integrity(DEV, &integrity, b"hello pyodide!").unwrap();
    verify_bundle_integrity(DEV, "", b"hello pyodide!").unwrap();
}

#[test]
fn a_cached_bundle_is_not_downloaded() {
    let dir = temp_dir("cache-hit");
    fs::write(dir.join(bundle_file_name(VERSION)), BUNDLE).unwrap();
    local(async {
        let (url, seen) = serve(200, "downloaded").await;
        let integrity = bundle_integrity(BUNDLE.as_bytes());
        let bytes = load_bundle(Some(&*dir), &url, &no_tls(), VERSION, &integrity)
            .await
            .unwrap();
        assert_eq!(bytes.as_deref(), Some(BUNDLE.as_bytes()));
        assert!(seen.borrow().is_empty());
    });
}

#[test]
fn a_cached_bundle_is_verified() {
    let dir = temp_dir("cache-tampered");
    fs::write(dir.join(bundle_file_name(VERSION)), "tampered").unwrap();
    local(async {
        let (url, seen) = serve(200, BUNDLE).await;
        let integrity = bundle_integrity(BUNDLE.as_bytes());
        let error = load_bundle(Some(&*dir), &url, &no_tls(), VERSION, &integrity)
            .await
            .unwrap_err();
        assert!(
            error.description().contains("integrity check failed"),
            "{error:?}"
        );
        assert!(seen.borrow().is_empty());
    });
}

#[test]
fn a_bundle_is_downloaded_from_its_url() {
    local(async {
        let (url, seen) = serve(200, BUNDLE).await;
        let integrity = bundle_integrity(BUNDLE.as_bytes());
        let bytes = load_bundle(None, &url, &no_tls(), VERSION, &integrity)
            .await
            .unwrap();
        assert_eq!(bytes.as_deref(), Some(BUNDLE.as_bytes()));
        let authority = url.trim_start_matches("http://").trim_end_matches('/');
        assert_eq!(
            *seen.borrow(),
            [(
                "/pyodide_0.28.2.capnp.bin".to_owned(),
                authority.to_owned(),
                None
            )]
        );
    });
}

#[test]
fn a_downloaded_bundle_is_cached() {
    let dir = temp_dir("cache-write");
    local(async {
        let (url, seen) = serve(200, BUNDLE).await;
        let integrity = bundle_integrity(BUNDLE.as_bytes());
        let bytes = load_bundle(Some(&*dir), &url, &no_tls(), VERSION, &integrity)
            .await
            .unwrap();
        assert_eq!(bytes.as_deref(), Some(BUNDLE.as_bytes()));
        assert_eq!(seen.borrow().len(), 1);
    });
    assert_eq!(files(&dir), ["pyodide_0.28.2.capnp.bin"]);
    assert_eq!(
        fs::read(dir.join("pyodide_0.28.2.capnp.bin")).unwrap(),
        BUNDLE.as_bytes()
    );
}

#[test]
fn a_cached_bundle_is_replaced_whole() {
    let dir = temp_dir("cache-replace");
    let name = bundle_file_name(VERSION);
    fs::write(dir.join(&name), "old").unwrap();
    write_cached(&dir, &name, BUNDLE.as_bytes()).unwrap();
    assert_eq!(files(&dir), std::slice::from_ref(&name));
    assert_eq!(fs::read(dir.join(&name)).unwrap(), BUNDLE.as_bytes());
}

#[test]
fn a_missing_cache_directory_is_no_cache() {
    let root = temp_dir("cache-missing");
    let dir = root.join("absent");
    local(async {
        let (url, _) = serve(200, BUNDLE).await;
        let integrity = bundle_integrity(BUNDLE.as_bytes());
        let bytes = load_bundle(Some(&*dir), &url, &no_tls(), VERSION, &integrity)
            .await
            .unwrap();
        assert_eq!(bytes.as_deref(), Some(BUNDLE.as_bytes()));
    });
    assert!(!dir.exists());
}

#[test]
fn a_status_other_than_200_fails() {
    let dir = temp_dir("not-found");
    local(async {
        let (url, seen) = serve(404, BUNDLE).await;
        let integrity = bundle_integrity(BUNDLE.as_bytes());
        let error = load_bundle(Some(&*dir), &url, &no_tls(), VERSION, &integrity)
            .await
            .unwrap_err();
        assert!(
            error
                .description()
                .contains("Request for Pyodide bundle failed"),
            "{error:?}"
        );
        assert!(
            error.description().contains("statusCode = 404"),
            "{error:?}"
        );
        assert_eq!(seen.borrow().len(), 1);
    });
    assert!(files(&dir).is_empty());
}

#[test]
fn a_downloaded_bundle_that_fails_the_check_is_not_cached() {
    let dir = temp_dir("download-tampered");
    local(async {
        let (url, _) = serve(200, "tampered").await;
        let integrity = bundle_integrity(BUNDLE.as_bytes());
        let error = load_bundle(Some(&*dir), &url, &no_tls(), VERSION, &integrity)
            .await
            .unwrap_err();
        assert!(
            error.description().contains("integrity check failed"),
            "{error:?}"
        );
    });
    assert!(files(&dir).is_empty());
}

#[test]
fn dev_is_never_downloaded() {
    let dir = temp_dir("dev-missing");
    local(async {
        let (url, seen) = serve(200, BUNDLE).await;
        assert_eq!(
            load_bundle(Some(&*dir), &url, &no_tls(), DEV, "")
                .await
                .unwrap(),
            None
        );
        assert_eq!(
            load_bundle(None, &url, &no_tls(), DEV, "").await.unwrap(),
            None
        );
        assert!(seen.borrow().is_empty());
    });
    assert!(files(&dir).is_empty());
}

#[test]
fn dev_is_read_from_the_cache_unchecked() {
    let dir = temp_dir("dev-cached");
    fs::write(dir.join(bundle_file_name(DEV)), BUNDLE).unwrap();
    local(async {
        let (url, seen) = serve(200, "downloaded").await;
        let bytes = load_bundle(Some(&*dir), &url, &no_tls(), DEV, "")
            .await
            .unwrap();
        assert_eq!(bytes.as_deref(), Some(BUNDLE.as_bytes()));
        assert!(seen.borrow().is_empty());
    });
}

#[test]
fn bundles_are_downloaded_from_the_release_host() {
    assert_eq!(
        format!("{BUNDLE_URL_PREFIX}{}", bundle_file_name(VERSION)),
        "https://pyodide-capnp-bin.edgeworker.net/pyodide_0.28.2.capnp.bin"
    );
}

#[test]
fn the_bridge_returns_a_cached_bundle() {
    let dir = temp_dir("bridge-cached");
    fs::write(dir.join(bundle_file_name(VERSION)), BUNDLE).unwrap();
    let integrity = bundle_integrity(BUNDLE.as_bytes());
    let bytes = run(pyodide_bundle_fetch(
        KjMaybe::Some(dir.to_str().unwrap()),
        VERSION,
        &integrity,
    ))
    .unwrap();
    assert_eq!(bytes, BUNDLE.as_bytes());
}

#[test]
fn the_bridge_returns_no_bytes_for_an_uncached_dev_bundle() {
    let dir = temp_dir("bridge-dev");
    let bytes = run(pyodide_bundle_fetch(
        KjMaybe::Some(dir.to_str().unwrap()),
        DEV,
        "",
    ))
    .unwrap();
    assert!(bytes.is_empty());
    assert!(
        run(pyodide_bundle_fetch(KjMaybe::None, DEV, ""))
            .unwrap()
            .is_empty()
    );
}

#[test]
fn the_bridge_refuses_an_empty_bundle() {
    // An empty `dev` file passes the (skipped) check, but empty bytes mean no bundle.
    let dir = temp_dir("bridge-empty");
    fs::write(dir.join(bundle_file_name(DEV)), "").unwrap();
    let error = run(pyodide_bundle_fetch(
        KjMaybe::Some(dir.to_str().unwrap()),
        DEV,
        "",
    ))
    .unwrap_err();
    assert!(
        error.description().contains("Pyodide bundle is empty"),
        "{error:?}"
    );
}

#[test]
fn a_refused_connection_names_the_request() {
    local(async {
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let url = format!("http://{}/", listener.local_addr().unwrap());
        drop(listener);
        let error = load_bundle(None, &url, &no_tls(), VERSION, "")
            .await
            .unwrap_err();
        let description = error.description();
        assert!(description.contains("Pyodide"), "{error:?}");
        assert!(
            description.contains(&format!("{url}{}", bundle_file_name(VERSION))),
            "{error:?}"
        );
        assert_eq!(
            error.exception_type(),
            cxx::KjExceptionType::Disconnected,
            "{error:?}"
        );
    });
}

#[test]
fn an_unresolvable_host_names_the_request() {
    local(async {
        let url = "http://pyodide-bundle.invalid/";
        let error = load_bundle(None, url, &no_tls(), VERSION, "")
            .await
            .unwrap_err();
        let description = error.description();
        assert!(description.contains("Pyodide"), "{error:?}");
        assert!(
            description.contains(&format!("{url}{}", bundle_file_name(VERSION))),
            "{error:?}"
        );
    });
}

// An EC P-256 CA and a `localhost` certificate it signed, with the certificate's key. Valid
// from 2026 on.
const TEST_CA: &str = "-----BEGIN CERTIFICATE-----
MIIBXjCCAQSgAwIBAgIJALfB2Y2IEYBBMAoGCCqGSM49BAMCMCExHzAdBgNVBAMM
FlB5b2RpZGUgYnVuZGxlIHRlc3QgQ0EwIBcNMjYxMTA1MDA0NjQyWhgPMjEyNjEw
MTIwMDQ2NDJaMCExHzAdBgNVBAMMFlB5b2RpZGUgYnVuZGxlIHRlc3QgQ0EwWTAT
BgcqhkjOPQIBBggqhkjOPQMBBwNCAATLAPDIaTlWHLq5kIWru3N6WlY2+zEKznEa
VujxrlN6dI5ZiBkgPI5sbRrc4/g62zIT6tejhGKzvJEE2Qvpf/ySoyMwITAPBgNV
HRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwICBDAKBggqhkjOPQQDAgNIADBFAiEA
qFogCAhrDaPCoZRqi+Xr69xn/EoUSJkgCfsba6Z+py4CIEdP3VBZeaEk3luQIQcK
FJQcLzUqG4WY/UE1r6xcxyKy
-----END CERTIFICATE-----
";

const LOCALHOST_CERT: &str = "-----BEGIN CERTIFICATE-----
MIIBeTCCAR+gAwIBAgIJAKlwPv3xjGl2MAoGCCqGSM49BAMCMCExHzAdBgNVBAMM
FlB5b2RpZGUgYnVuZGxlIHRlc3QgQ0EwIBcNMjYxMTA1MDA0NjQyWhgPMjEyNjEw
MTIwMDQ2NDJaMBQxEjAQBgNVBAMMCWxvY2FsaG9zdDBZMBMGByqGSM49AgEGCCqG
SM49AwEHA0IABG58wLDTo3FAqVmjQ3ZRs45H0SneCXv9kh4XG+gfwI/a3JB3Hxew
7nCIOZjgqCNq4Sqzn/qIrz8SSgKLwujXFl6jSzBJMAwGA1UdEwEB/wQCMAAwDgYD
VR0PAQH/BAQDAgeAMBMGA1UdJQQMMAoGCCsGAQUFBwMBMBQGA1UdEQQNMAuCCWxv
Y2FsaG9zdDAKBggqhkjOPQQDAgNIADBFAiEAh7lz9obzdUc/XTfP8Yl7NK55n1rE
kK78/3/JViRseMMCIHFYLIKnM6Hq4ZscnigHWGnFfykY+8grl27aiTnmNbTl
-----END CERTIFICATE-----
";

const LOCALHOST_KEY: &str = "-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgoZf44o8W5ubq25qx
lnRiV9vthUhPruynP37aIRDyBLOhRANCAARufMCw06NxQKlZo0N2UbOOR9Ep3gl7
/ZIeFxvoH8CP2tyQdx8XsO5wiDmY4KgjauEqs5/6iK8/EkoCi8Lo1xZe
-----END PRIVATE KEY-----
";

fn localhost_server() -> Arc<rustls::ServerConfig> {
    kj_hyper::tls::server_config(&TlsOptions {
        keypair: Some(Keypair {
            certificate_chain: LOCALHOST_CERT.to_owned(),
            private_key: LOCALHOST_KEY.to_owned(),
        }),
        ..TlsOptions::default()
    })
    .unwrap()
}

#[test]
fn a_bundle_is_downloaded_over_tls() {
    let dir = temp_dir("tls");
    local(async {
        let (url, seen) = serve_tls(localhost_server(), BUNDLE).await;
        let tls = TlsOptions {
            trusted_certificates: vec![TEST_CA.to_owned()],
            ..TlsOptions::default()
        };
        let integrity = bundle_integrity(BUNDLE.as_bytes());
        let bytes = load_bundle(Some(&*dir), &url, &tls, VERSION, &integrity)
            .await
            .unwrap();
        assert_eq!(bytes.as_deref(), Some(BUNDLE.as_bytes()));
        let authority = url.trim_start_matches("https://").trim_end_matches('/');
        assert_eq!(
            *seen.borrow(),
            [(
                "/pyodide_0.28.2.capnp.bin".to_owned(),
                authority.to_owned(),
                Some("localhost".to_owned())
            )]
        );
    });
    assert_eq!(files(&dir), ["pyodide_0.28.2.capnp.bin"]);
}

#[test]
fn an_untrusted_certificate_fails_the_request() {
    let dir = temp_dir("tls-untrusted");
    local(async {
        let (url, seen) = serve_tls(localhost_server(), BUNDLE).await;
        let integrity = bundle_integrity(BUNDLE.as_bytes());
        let error = load_bundle(Some(&*dir), &url, &no_tls(), VERSION, &integrity)
            .await
            .unwrap_err();
        let description = error.description();
        assert!(description.contains("TLS handshake failed"), "{error:?}");
        assert!(
            description.contains(&format!("{url}{}", bundle_file_name(VERSION))),
            "{error:?}"
        );
        assert!(seen.borrow().is_empty());
    });
    assert!(files(&dir).is_empty());
}

#[test]
fn a_failed_rename_names_the_cache_file() {
    let dir = temp_dir("rename-fails");
    let name = bundle_file_name(VERSION);
    // A non-empty directory where the file goes: the temporary file is written, the rename fails.
    fs::create_dir_all(dir.join(&name).join("occupied")).unwrap();
    let error = write_cached(&dir, &name, BUNDLE.as_bytes()).unwrap_err();
    assert!(
        error
            .description()
            .contains(&format!("{}:", dir.join(&name).display())),
        "{error:?}"
    );
    assert_eq!(files(&dir), std::slice::from_ref(&name));
}
