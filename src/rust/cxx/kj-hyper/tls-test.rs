use std::time::Duration;

use base64::Engine;
use base64::engine::general_purpose::STANDARD;
use futures::executor::block_on;
use rustls::internal::msgs::codec::Codec;
use rustls::version::TLS12;
use rustls::version::TLS13;
use tokio::io::DuplexStream;
use tokio::io::duplex;
use tokio_rustls::client;
use tokio_rustls::server;

use super::*;
use crate::Result;
use crate::io::RustIo;
use crate::io::io_kj_error;

// As tls-network-test.c++'s: an EC P-256 CA, an example.com certificate it signed, the key
// under both, and a self-signed example.com CA certificate over that key. Valid from 2026 on.
const CA_CERT: &str = "-----BEGIN CERTIFICATE-----
MIIBmzCCAUGgAwIBAgIUFtVfWCEoNJw9tRYkhBCqWkPYQAkwCgYIKoZIzj0EAwIw
GjEYMBYGA1UEAwwPd29ya2VyZCB0ZXN0IENBMCAXDTI2MDkxODEzNDcwMVoYDzIx
MjYwODI1MTM0NzAxWjAaMRgwFgYDVQQDDA93b3JrZXJkIHRlc3QgQ0EwWTATBgcq
hkjOPQIBBggqhkjOPQMBBwNCAAR0c/eq28LGrosC4Jp0m5O6/xS5vvetDh6lDWNG
LfwBXbM3O4yoeSz9pUKY4cChCSL4BMldwTbepDKMCBmVMfJDo2MwYTAdBgNVHQ4E
FgQU1HLxzohU0eGQqgstH3O0chw0cVIwHwYDVR0jBBgwFoAU1HLxzohU0eGQqgst
H3O0chw0cVIwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMCAgQwCgYIKoZI
zj0EAwIDSAAwRQIgYtV4qsw7p1xhZ1OSOWmRmjyc4LzQplblEr4jZmZO6BECIQCx
Al0TgxgxuWjJ4FuakSJ5qfCA2BiIliBop+phth7LKw==
-----END CERTIFICATE-----
";

const HOST_CERT: &str = "-----BEGIN CERTIFICATE-----
MIIBwzCCAWmgAwIBAgIUPPhG/ycBgb1qLtsWGGJiMhxtewAwCgYIKoZIzj0EAwIw
GjEYMBYGA1UEAwwPd29ya2VyZCB0ZXN0IENBMCAXDTI2MDkxODEzNDcwMVoYDzIx
MjYwODI1MTM0NzAxWjAWMRQwEgYDVQQDDAtleGFtcGxlLmNvbTBZMBMGByqGSM49
AgEGCCqGSM49AwEHA0IABCc5+7lyl50H3MHWYyEAgNbxnIhMc6TBtR7Wvpp6XOBg
7CzaOCZFwix4Mj8KXPoyhi7xgNQVKAgE1maTCPVPlB2jgY4wgYswDAYDVR0TAQH/
BAIwADAOBgNVHQ8BAf8EBAMCB4AwEwYDVR0lBAwwCgYIKwYBBQUHAwEwFgYDVR0R
BA8wDYILZXhhbXBsZS5jb20wHQYDVR0OBBYEFOFpqeWqxNaPtlBncznOXK34Slsh
MB8GA1UdIwQYMBaAFNRy8c6IVNHhkKoLLR9ztHIcNHFSMAoGCCqGSM49BAMCA0gA
MEUCIGkJrUe5mthCxcMYy8zUKtbDuURGIS1OqeT9xqypm2nlAiEA5jYVU9d8NUV8
WBIiMYUVDYKLwUf1elA4zib/Qnms8DQ=
-----END CERTIFICATE-----
";

const SELF_SIGNED_CERT: &str = "-----BEGIN CERTIFICATE-----
MIIBnDCCAUGgAwIBAgIUS2xurXFfrYHJlrcRk6lqv44ritUwCgYIKoZIzj0EAwIw
FjEUMBIGA1UEAwwLZXhhbXBsZS5jb20wIBcNMjYwOTE4MTM0OTAwWhgPMjEyNjA4
MjUxMzQ5MDBaMBYxFDASBgNVBAMMC2V4YW1wbGUuY29tMFkwEwYHKoZIzj0CAQYI
KoZIzj0DAQcDQgAEJzn7uXKXnQfcwdZjIQCA1vGciExzpMG1Hta+mnpc4GDsLNo4
JkXCLHgyPwpc+jKGLvGA1BUoCATWZpMI9U+UHaNrMGkwHQYDVR0OBBYEFOFpqeWq
xNaPtlBncznOXK34SlshMB8GA1UdIwQYMBaAFOFpqeWqxNaPtlBncznOXK34Slsh
MA8GA1UdEwEB/wQFMAMBAf8wFgYDVR0RBA8wDYILZXhhbXBsZS5jb20wCgYIKoZI
zj0EAwIDSQAwRgIhAP8xzPMa6tuqL9p3AIKUn1eABTfZv7o/VmiEvtCK3rU2AiEA
9S5OQ1DXe5Nt5ZD5h3qOJF1IN9uR5Jb0kf1bl7lKIGI=
-----END CERTIFICATE-----
";

const HOST_KEY: &str = "-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgb9K+RSaSGrigmGFU
Ucyf1+0zBpj3gEnQ9LCKN9gIFhqhRANCAAQnOfu5cpedB9zB1mMhAIDW8ZyITHOk
wbUe1r6aelzgYOws2jgmRcIseDI/Clz6MoYu8YDUFSgIBNZmkwj1T5Qd
-----END PRIVATE KEY-----
";

fn options(min_version: MinVersion) -> TlsOptions {
    TlsOptions {
        min_version,
        ..TlsOptions::default()
    }
}

fn host_keypair() -> Keypair {
    Keypair {
        certificate_chain: HOST_CERT.to_owned(),
        private_key: HOST_KEY.to_owned(),
    }
}

#[test]
fn min_version_tls13_allows_only_tls13() {
    assert_eq!(versions(&options(MinVersion::Tls13)), [&TLS13]);
    assert_eq!(
        versions(&options(MinVersion::Tls12)),
        rustls::DEFAULT_VERSIONS
    );
}

#[test]
fn a_trusted_certificate_string_without_a_certificate_is_refused() {
    let options = TlsOptions {
        trusted_certificates: vec!["not a certificate".to_owned()],
        ..TlsOptions::default()
    };
    assert!(client_config(&options).is_err());
}

fn verifier(trusted: &str) -> PinnedVerifier {
    let trusted = certificates(&[trusted.to_owned()]).unwrap();
    let provider = provider();
    PinnedVerifier {
        verifier: Some(
            WebPkiServerVerifier::builder_with_provider(
                Arc::new(roots(&trusted)),
                provider.clone(),
            )
            .build()
            .unwrap(),
        ),
        pinned: trusted,
        provider,
    }
}

fn verify(verifier: &PinnedVerifier, cert: &str, now: UnixTime) -> bool {
    let cert = CertificateDer::from_pem_slice(cert.as_bytes()).unwrap();
    let name = ServerName::try_from("example.com").unwrap();
    verifier
        .verify_server_cert(&cert, &[], &name, &[], now)
        .is_ok()
}

#[test]
fn direct_trust_still_checks_validity() {
    let now = UnixTime::now();
    let before = UnixTime::since_unix_epoch(Duration::from_secs(0));
    let pinned = verifier(SELF_SIGNED_CERT);
    assert!(verify(&pinned, SELF_SIGNED_CERT, now));
    assert!(!verify(&pinned, SELF_SIGNED_CERT, before));
    let chained = verifier(CA_CERT);
    assert!(verify(&chained, HOST_CERT, now));
    assert!(!verify(&chained, HOST_CERT, before));
}

// A self-signed example.com CA certificate over HOST_KEY whose extended key usage is client
// authentication only.
const CLIENT_AUTH_ONLY_CERT: &str = "-----BEGIN CERTIFICATE-----
MIIBjzCCATWgAwIBAgIUA485T1izFLPPRZrwPqK3rWKwcH8wCgYIKoZIzj0EAwIw
FjEUMBIGA1UEAwwLZXhhbXBsZS5jb20wIBcNMjYwOTIwMjAwMzU0WhgPMjEyNjA4
MjcyMDAzNTRaMBYxFDASBgNVBAMMC2V4YW1wbGUuY29tMFkwEwYHKoZIzj0CAQYI
KoZIzj0DAQcDQgAEJzn7uXKXnQfcwdZjIQCA1vGciExzpMG1Hta+mnpc4GDsLNo4
JkXCLHgyPwpc+jKGLvGA1BUoCATWZpMI9U+UHaNfMF0wDwYDVR0TAQH/BAUwAwEB
/zATBgNVHSUEDDAKBggrBgEFBQcDAjAWBgNVHREEDzANggtleGFtcGxlLmNvbTAd
BgNVHQ4EFgQU4Wmp5arE1o+2UGdzOc5crfhKWyEwCgYIKoZIzj0EAwIDSAAwRQIh
ANgJVEO6Mnsk2O3XGz89VQ7BYTpOJCl4A1H2N9KESaBSAiADwsuif/CsdvskreTj
Ecy/XDh8/EdD2Yn1+dXV4yG1Hg==
-----END CERTIFICATE-----
";

#[test]
fn direct_trust_requires_a_server_purpose() {
    let pinned = verifier(CLIENT_AUTH_ONLY_CERT);
    assert!(!verify(&pinned, CLIENT_AUTH_ONLY_CERT, UnixTime::now()));
}

#[test]
fn client_certs_from_browser_cas_are_refused_at_configuration() {
    let options = TlsOptions {
        keypair: Some(host_keypair()),
        require_client_certs: true,
        trust_browser_cas: true,
        ..TlsOptions::default()
    };
    assert!(server_config(&options).is_err());
}

/// Both handshakes over a pipe, with rustls configurations built by hand.
async fn handshake(
    server: &Arc<rustls::ServerConfig>,
    client: Arc<rustls::ClientConfig>,
) -> Result<()> {
    let (a, b) = duplex(1 << 16);
    let (accepted, connected) =
        futures::join!(accept(a, server.clone()), connect(b, client, "example.com"));
    accepted?;
    connected?;
    Ok(())
}

// As tls-network-test.c++'s: an unrelated CA; signed by CA_CERT over HOST_KEY, a 127.0.0.1
// server certificate; and a client certificate over its own key.
const OTHER_CA_CERT: &str = "-----BEGIN CERTIFICATE-----
MIIBqDCCAU2gAwIBAgIURKbmFqvXbsPNYNcpsOoC3p9wz24wCgYIKoZIzj0EAwIw
IDEeMBwGA1UEAwwVd29ya2VyZCBvdGhlciB0ZXN0IENBMCAXDTI2MDkxODEzNDcw
MVoYDzIxMjYwODI1MTM0NzAxWjAgMR4wHAYDVQQDDBV3b3JrZXJkIG90aGVyIHRl
c3QgQ0EwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAATjpjzuMfD87oJWO1+dmaR9
WzThSBIax7DRYA1eMPnzYBN4sgjXlYnB3PurAMIRpcAzQwrwe7AqGhQYMoAxF7Lm
o2MwYTAdBgNVHQ4EFgQUWIulx+0h/gDS1PpTmqr2q7zaNygwHwYDVR0jBBgwFoAU
WIulx+0h/gDS1PpTmqr2q7zaNygwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8E
BAMCAgQwCgYIKoZIzj0EAwIDSQAwRgIhAP7lmLtP3Ag3cLs3rA8MN0rFpX1HZhzQ
Dwto7y1IWJnCAiEArBdbUYBM9JmohOEGr7EwLio9fiYclR2rCs54VwpMyWY=
-----END CERTIFICATE-----
";

const IP_CERT: &str = "-----BEGIN CERTIFICATE-----
MIIBuzCCAWCgAwIBAgIUPPhG/ycBgb1qLtsWGGJiMhxtewEwCgYIKoZIzj0EAwIw
GjEYMBYGA1UEAwwPd29ya2VyZCB0ZXN0IENBMCAXDTI2MDkxODE4MTcxNVoYDzIx
MjYwODI1MTgxNzE1WjAUMRIwEAYDVQQDDAkxMjcuMC4wLjEwWTATBgcqhkjOPQIB
BggqhkjOPQMBBwNCAAQnOfu5cpedB9zB1mMhAIDW8ZyITHOkwbUe1r6aelzgYOws
2jgmRcIseDI/Clz6MoYu8YDUFSgIBNZmkwj1T5Qdo4GHMIGEMAwGA1UdEwEB/wQC
MAAwDgYDVR0PAQH/BAQDAgeAMBMGA1UdJQQMMAoGCCsGAQUFBwMBMA8GA1UdEQQI
MAaHBH8AAAEwHQYDVR0OBBYEFOFpqeWqxNaPtlBncznOXK34SlshMB8GA1UdIwQY
MBaAFNRy8c6IVNHhkKoLLR9ztHIcNHFSMAoGCCqGSM49BAMCA0kAMEYCIQD2WsDQ
/ypIhVIOGVqf7figuf4YxauEMOrrZ3kxn4cMPgIhANTmbGb6b3/sKXkyBTrTieJt
aiqt5Ls2/3+/Fzf04N2M
-----END CERTIFICATE-----
";

const CLIENT_CERT: &str = "-----BEGIN CERTIFICATE-----
MIIBsTCCAVegAwIBAgIUPPhG/ycBgb1qLtsWGGJiMhxtewIwCgYIKoZIzj0EAwIw
GjEYMBYGA1UEAwwPd29ya2VyZCB0ZXN0IENBMCAXDTI2MDkxODE4MTcxNVoYDzIx
MjYwODI1MTgxNzE1WjAeMRwwGgYDVQQDDBN3b3JrZXJkIHRlc3QgY2xpZW50MFkw
EwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEf1+6QE6J0A4kaCqmELiGf1WE8ctlQQ3o
O9HMPFuWxwmqyHWx4EgW7p/NVc0cLvpDO+mlq5t3ty4RKjGN6ASs8aN1MHMwDAYD
VR0TAQH/BAIwADAOBgNVHQ8BAf8EBAMCB4AwEwYDVR0lBAwwCgYIKwYBBQUHAwIw
HQYDVR0OBBYEFP5LB3a8ceMElm+R3T7kMQwSgqneMB8GA1UdIwQYMBaAFNRy8c6I
VNHhkKoLLR9ztHIcNHFSMAoGCCqGSM49BAMCA0gAMEUCICCikKtYhV/a121v/+JC
zCTWoIZVpC4+0hwXc9FMKu0lAiEA+Mfwtk957dSWffB+ntSCNsNr9a2UeyAwJY5c
ak5n5qY=
-----END CERTIFICATE-----
";

const CLIENT_KEY: &str = "-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgjxQourjhr8gzNJrQ
GIdkArapVpTBw4yb4ZN0o1DdXBmhRANCAAR/X7pATonQDiRoKqYQuIZ/VYTxy2VB
Deg70cw8W5bHCarIdbHgSBbun81VzRwu+kM76aWrm3e3LhEqMY3oBKzx
-----END PRIVATE KEY-----
";

fn serving(certificate: &str) -> TlsOptions {
    TlsOptions {
        keypair: Some(Keypair {
            certificate_chain: certificate.to_owned(),
            private_key: HOST_KEY.to_owned(),
        }),
        ..TlsOptions::default()
    }
}

fn trusting(certificate: &str) -> TlsOptions {
    TlsOptions {
        trusted_certificates: vec![certificate.to_owned()],
        ..TlsOptions::default()
    }
}

/// Connects a client to a server over a pipe and exchanges a byte each way; the client's
/// view of the outcome, and whether the server saw a client certificate.
fn exchange(server: &TlsOptions, client: &TlsOptions, hostname: &str) -> (Result<()>, bool) {
    use tokio::io::AsyncReadExt;
    use tokio::io::AsyncWriteExt;
    let server = match server_config(server) {
        Ok(config) => config,
        Err(e) => return (Err(e), false),
    };
    let client = match client_config(client) {
        Ok(config) => config,
        Err(e) => return (Err(e), false),
    };
    block_on(async {
        let (a, b) = duplex(1 << 16);
        let accepting = async {
            let mut stream = accept(a, server).await?;
            let authenticated = has_client_certificate(&stream);
            let mut byte = [0];
            stream
                .read_exact(&mut byte)
                .await
                .map_err(|e| io_kj_error(&e))?;
            stream.write_all(&byte).await.map_err(|e| io_kj_error(&e))?;
            Ok::<_, cxx::KjError>(authenticated)
        };
        let pinged = async {
            let mut stream = connect(b, client, hostname).await?;
            stream.write_all(b"x").await.map_err(|e| io_kj_error(&e))?;
            let mut byte = [0];
            stream
                .read_exact(&mut byte)
                .await
                .map_err(|e| io_kj_error(&e))?;
            assert_eq!(byte, *b"x");
            Ok(())
        };
        let (accepting, pinged) = futures::join!(accepting, pinged);
        (pinged, accepting.unwrap_or(false))
    })
}

fn connects(server: &TlsOptions, client: &TlsOptions, hostname: &str) -> bool {
    exchange(server, client, hostname).0.is_ok()
}

#[test]
fn a_certificate_a_trusted_ca_signed_is_accepted() {
    assert!(connects(
        &serving(HOST_CERT),
        &trusting(CA_CERT),
        "example.com"
    ));
}

#[test]
fn with_nothing_trusted_every_certificate_is_refused() {
    let nothing = TlsOptions::default();
    assert!(!connects(&serving(HOST_CERT), &nothing, "example.com"));
    assert!(!connects(
        &serving(SELF_SIGNED_CERT),
        &nothing,
        "example.com"
    ));
}

#[test]
fn a_certificate_an_untrusted_ca_signed_is_refused() {
    assert!(!connects(
        &serving(HOST_CERT),
        &trusting(OTHER_CA_CERT),
        "example.com"
    ));
}

#[test]
fn the_certificate_must_name_the_host() {
    assert!(!connects(
        &serving(HOST_CERT),
        &trusting(CA_CERT),
        "wrong.example.com"
    ));
}

#[test]
fn a_trusted_self_signed_certificate_is_trusted_directly() {
    let server = serving(SELF_SIGNED_CERT);
    let client = trusting(SELF_SIGNED_CERT);
    assert!(connects(&server, &client, "example.com"));
    assert!(!connects(&server, &client, "wrong.example.com"));
}

#[test]
fn min_version_configures_both_sides() {
    let policy = |mut options: TlsOptions| {
        options.min_version = MinVersion::Tls13;
        options
    };
    assert!(connects(
        &policy(serving(HOST_CERT)),
        &policy(trusting(CA_CERT)),
        "example.com"
    ));
    // A TLS 1.2-only client cannot reach a TLS 1.3 floor.
    let server = server_config(&policy(serving(HOST_CERT))).unwrap();
    let mut roots = RootCertStore::empty();
    roots
        .add(CertificateDer::from_pem_slice(CA_CERT.as_bytes()).unwrap())
        .unwrap();
    let client = Arc::new(
        rustls::ClientConfig::builder_with_provider(provider())
            .with_protocol_versions(&[&TLS12])
            .unwrap()
            .with_root_certificates(roots)
            .with_no_client_auth(),
    );
    assert!(block_on(handshake(&server, client)).is_err());
}

#[test]
fn an_ip_address_is_verified_against_the_certificates_ip_names() {
    assert!(connects(&serving(IP_CERT), &trusting(CA_CERT), "127.0.0.1"));
    assert!(!connects(
        &serving(IP_CERT),
        &trusting(CA_CERT),
        "127.0.0.2"
    ));
}

fn requiring_client_certs() -> TlsOptions {
    TlsOptions {
        trusted_certificates: vec![CA_CERT.to_owned()],
        require_client_certs: true,
        ..serving(HOST_CERT)
    }
}

fn authenticated_client() -> TlsOptions {
    TlsOptions {
        keypair: Some(Keypair {
            certificate_chain: CLIENT_CERT.to_owned(),
            private_key: CLIENT_KEY.to_owned(),
        }),
        ..trusting(CA_CERT)
    }
}

#[test]
fn require_client_certs_accepts_a_trusted_client_certificate_and_refuses_none() {
    let server = requiring_client_certs();
    let (result, authenticated) = exchange(&server, &authenticated_client(), "example.com");
    result.unwrap();
    assert!(authenticated);
    let (result, authenticated) = exchange(&server, &trusting(CA_CERT), "example.com");
    assert!(result.is_err());
    assert!(!authenticated);
}

#[test]
fn accept_hands_out_a_connection_only_once_its_client_is_authenticated() {
    // The accepted stream is the handshake's result: an anonymous client against a server
    // requiring client certificates never yields one.
    let server = server_config(&requiring_client_certs()).unwrap();
    let anonymous = client_config(&trusting(CA_CERT)).unwrap();
    block_on(async {
        let (a, b) = duplex(1 << 16);
        let (accepted, connected) = futures::join!(
            accept(a, server.clone()),
            connect(b, anonymous, "example.com")
        );
        assert!(accepted.is_err());
        drop(connected);
    });
    assert!(connects(
        &requiring_client_certs(),
        &authenticated_client(),
        "example.com"
    ));
}

#[test]
fn a_server_without_a_keypair_is_refused_at_configuration() {
    assert!(server_config(&trusting(CA_CERT)).is_err());
    // A client needs no keypair, and one with a server's serves as its client certificate.
    assert!(client_config(&trusting(CA_CERT)).is_ok());
}

#[test]
fn a_malformed_trusted_certificate_is_refused_at_configuration() {
    let bad = trusting("-----BEGIN CERTIFICATE-----\nnot base64\n-----END CERTIFICATE-----\n");
    assert!(client_config(&bad).is_err());
}

/// A handshaken client stream and its server peer, with a byte already exchanged.
async fn tls_pair() -> (
    client::TlsStream<DuplexStream>,
    server::TlsStream<DuplexStream>,
) {
    use tokio::io::AsyncReadExt;
    use tokio::io::AsyncWriteExt;
    let server = server_config(&serving(HOST_CERT)).unwrap();
    let client = client_config(&trusting(CA_CERT)).unwrap();
    let (a, b) = duplex(1 << 16);
    let (accepted, connected) =
        futures::join!(accept(a, server), connect(b, client, "example.com"));
    let (mut server, mut client) = (accepted.unwrap(), connected.unwrap());
    client.write_all(b"x").await.unwrap();
    let mut byte = [0];
    server.read_exact(&mut byte).await.unwrap();
    (client, server)
}

#[test]
fn a_kj_read_and_write_in_flight_at_once_on_a_tls_stream_both_complete() {
    use tokio::io::AsyncReadExt;
    use tokio::io::AsyncWriteExt;
    block_on(async {
        let (client, mut server) = tls_pair().await;
        let io = RustIo::new(client);
        let mut buffer = [0; 5];
        let read = io.read(tokio::io::ReadBuf::new(&mut buffer), 5);
        let write = io.write(b"hello");
        let peer = async {
            let mut got = [0; 5];
            server.read_exact(&mut got).await.unwrap();
            assert_eq!(&got, b"hello");
            server.write_all(b"world").await.unwrap();
        };
        let (read, write, ()) = futures::join!(read, write, peer);
        write.unwrap();
        assert_eq!(read.unwrap(), 5);
        assert_eq!(&buffer, b"world");
    });
}

#[test]
fn a_tls_peer_closing_without_close_notify_is_a_disconnect_not_an_eof() {
    block_on(async {
        let (client, server) = tls_pair().await;
        let io = RustIo::new(client);
        let mut buffer = [0; 5];
        let mut read = Box::pin(io.read(tokio::io::ReadBuf::new(&mut buffer), 1));
        assert!(futures::poll!(read.as_mut()).is_pending());
        drop(server);
        let error = read.await.unwrap_err();
        assert_eq!(error.exception_type(), KjExceptionType::Disconnected);
    });
}

// A P-521 CA, and example.com certificates it signed over HOST_KEY with ECDSA-SHA256 (as a
// TLS-inspecting proxy's root signs) and ECDSA-SHA512. Valid from 2026 on.
const P521_CA_CERT: &str = "-----BEGIN CERTIFICATE-----
MIICDjCCAW+gAwIBAgIUeuX/zfOMhcRuupxQp5ind4JvRnkwCgYIKoZIzj0EAwQw
IDEeMBwGA1UEAwwVd29ya2VyZCB0ZXN0IFAtNTIxIENBMCAXDTI2MDkxODAwMDAw
MFoYDzIxMjYwODI1MDAwMDAwWjAgMR4wHAYDVQQDDBV3b3JrZXJkIHRlc3QgUC01
MjEgQ0EwgZswEAYHKoZIzj0CAQYFK4EEACMDgYYABAFBSR0iNpE24EfJX6F5Mhf0
bfqkJJodx8/g7Z0iE2kbZXZlF+9NaMaDv3VFzARACFr1y1aYwH1O/YjD9xYj09IG
5AFSOYEiyWU+BQwu3mCOIx0b2U/suR/yYHDTjBtM2CWFxIj+MfG8GfOmzdbYsUu4
QDKP8IbNjhDaDNVE2KqCTNLAn6NCMEAwHQYDVR0OBBYEFIawszsczSO3jMIXXGhr
7SZ4HpvMMA8GA1UdEwEB/wQFMAMBAf8wDgYDVR0PAQH/BAQDAgIEMAoGCCqGSM49
BAMEA4GMADCBiAJCAeAVVhKczFaFNZUOn6jGSEmkiDaYeRNimTj27yY1IAjs6kIV
1keWwthaOa7tDscFpl+DK47oTG+3nCrMuwQ54IuGAkIAp+H/DSlsk1rnYLFUnKnQ
JHfCyBm2hRpA4OiD6OcwEeUihfX9Ymqc7Qq2mfseOjESvryRcZsAhyMiW8BVe1xu
GPY=
-----END CERTIFICATE-----
";

const P521_SHA256_HOST_CERT: &str = "-----BEGIN CERTIFICATE-----
MIIBujCCARugAwIBAgICUCEwCgYIKoZIzj0EAwIwIDEeMBwGA1UEAwwVd29ya2Vy
ZCB0ZXN0IFAtNTIxIENBMCAXDTI2MDkxODAwMDAwMFoYDzIxMjYwODI1MDAwMDAw
WjAWMRQwEgYDVQQDDAtleGFtcGxlLmNvbTBZMBMGByqGSM49AgEGCCqGSM49AwEH
A0IABCc5+7lyl50H3MHWYyEAgNbxnIhMc6TBtR7Wvpp6XOBg7CzaOCZFwix4Mj8K
XPoyhi7xgNQVKAgE1maTCPVPlB2jTTBLMAwGA1UdEwEB/wQCMAAwDgYDVR0PAQH/
BAQDAgeAMBMGA1UdJQQMMAoGCCsGAQUFBwMBMBYGA1UdEQQPMA2CC2V4YW1wbGUu
Y29tMAoGCCqGSM49BAMCA4GMADCBiAJCAYz/ze2e+d6rdjZmi32w2H8XMpg1QNE/
v0934lUiEX9eYNs0jlr/GwhCtSu28wDmRfqq/z5PYoJNQPFpNGZ6ZMQqAkIAvGoG
VzlY4yI8iP/vIryby7J9hjn6lZERmELG8NNQ6AJIvY8loLBfps1GoBw0lVfQblFS
9rcuKuw8+Ds5AoiNqv4=
-----END CERTIFICATE-----
";

const P521_SHA512_HOST_CERT: &str = "-----BEGIN CERTIFICATE-----
MIIBuTCCARugAwIBAgICUCIwCgYIKoZIzj0EAwQwIDEeMBwGA1UEAwwVd29ya2Vy
ZCB0ZXN0IFAtNTIxIENBMCAXDTI2MDkxODAwMDAwMFoYDzIxMjYwODI1MDAwMDAw
WjAWMRQwEgYDVQQDDAtleGFtcGxlLmNvbTBZMBMGByqGSM49AgEGCCqGSM49AwEH
A0IABCc5+7lyl50H3MHWYyEAgNbxnIhMc6TBtR7Wvpp6XOBg7CzaOCZFwix4Mj8K
XPoyhi7xgNQVKAgE1maTCPVPlB2jTTBLMAwGA1UdEwEB/wQCMAAwDgYDVR0PAQH/
BAQDAgeAMBMGA1UdJQQMMAoGCCsGAQUFBwMBMBYGA1UdEQQPMA2CC2V4YW1wbGUu
Y29tMAoGCCqGSM49BAMEA4GLADCBhwJBdsslAvQMZnMaAsfPCFJ0yo4BHbHXQrCm
O3N93aWXLrgXSqEI9SupsbxH4cVUYuCftXm8Rk+x5aQSi8FaZuAClYMCQgHX4qpk
B6jsVBHCpRkECwphBjE1d0i2YET5ZnVCQihnfNcFeH7Te/vubsnOGLDp5b/aFjkT
lrraD+bHpdthcDa/xQ==
-----END CERTIFICATE-----
";

#[test]
fn a_certificate_a_p521_ca_signed_is_accepted() {
    let ca = trusting(P521_CA_CERT);
    assert!(connects(
        &serving(P521_SHA256_HOST_CERT),
        &ca,
        "example.com"
    ));
    assert!(connects(
        &serving(P521_SHA512_HOST_CERT),
        &ca,
        "example.com"
    ));
}

// HOST_KEY's public point, and its ECDSA-SHA512 signature of "workerd".
const HOST_POINT: [u8; 65] = [
    0x04, 0x27, 0x39, 0xfb, 0xb9, 0x72, 0x97, 0x9d, 0x07, 0xdc, 0xc1, 0xd6, 0x63, 0x21, 0x00, 0x80,
    0xd6, 0xf1, 0x9c, 0x88, 0x4c, 0x73, 0xa4, 0xc1, 0xb5, 0x1e, 0xd6, 0xbe, 0x9a, 0x7a, 0x5c, 0xe0,
    0x60, 0xec, 0x2c, 0xda, 0x38, 0x26, 0x45, 0xc2, 0x2c, 0x78, 0x32, 0x3f, 0x0a, 0x5c, 0xfa, 0x32,
    0x86, 0x2e, 0xf1, 0x80, 0xd4, 0x15, 0x28, 0x08, 0x04, 0xd6, 0x66, 0x93, 0x08, 0xf5, 0x4f, 0x94,
    0x1d,
];
const HOST_SHA512_SIGNATURE: [u8; 71] = [
    0x30, 0x45, 0x02, 0x21, 0x00, 0xbc, 0x75, 0xf4, 0x5a, 0x32, 0x93, 0xe9, 0x62, 0x74, 0xb5, 0x00,
    0xc0, 0x61, 0x71, 0x3b, 0x5c, 0x61, 0xbc, 0x28, 0xad, 0xe6, 0x60, 0x3c, 0x6e, 0xb4, 0x94, 0x5e,
    0x65, 0xd1, 0x0c, 0xf8, 0x63, 0x02, 0x20, 0x19, 0x42, 0xe1, 0x67, 0x6b, 0xc8, 0x3f, 0xb3, 0xab,
    0x57, 0x46, 0x0e, 0x2b, 0x55, 0x66, 0x8c, 0x0e, 0x46, 0x6a, 0xdc, 0x7d, 0x58, 0x9d, 0xb2, 0xe9,
    0xde, 0xda, 0x33, 0x62, 0x4f, 0x11, 0x2b,
];

#[test]
fn boringssl_verifies_the_ecdsa_signatures_ring_cannot() {
    assert!(
        P256_SHA512
            .verify_signature(&HOST_POINT, b"workerd", &HOST_SHA512_SIGNATURE)
            .is_ok()
    );
    assert!(
        P256_SHA512
            .verify_signature(&HOST_POINT, b"workerD", &HOST_SHA512_SIGNATURE)
            .is_err()
    );
    // The point is not on P-521.
    assert!(
        P521_SHA512
            .verify_signature(&HOST_POINT, b"workerd", &HOST_SHA512_SIGNATURE)
            .is_err()
    );
    assert!(
        provider()
            .signature_verification_algorithms
            .supported_schemes()
            .contains(&SignatureScheme::ECDSA_NISTP521_SHA512)
    );
}

// A self-signed example.com certificate over a P-521 key, and that key's ECDSA signatures of
// "workerd" with SHA-256, SHA-384 and SHA-512 (base64 DER), for the handshake schemes.
const P521_HOST_CERT: &str = "-----BEGIN CERTIFICATE-----
MIICATCCAWOgAwIBAgIUDuZZIddMxIaQ+zZR0EgL/4t4Ok8wCgYIKoZIzj0EAwQw
FjEUMBIGA1UEAwwLZXhhbXBsZS5jb20wIBcNMjYwOTE4MDAwMDAwWhgPMjEyNjA4
MjUwMDAwMDBaMBYxFDASBgNVBAMMC2V4YW1wbGUuY29tMIGbMBAGByqGSM49AgEG
BSuBBAAjA4GGAAQBcmOMtg+KuT/8rq9wAAHcht41cICKybLaGjP6PJ4ZgKEeEy6E
xJ9ZwPbXkE3IcHqrMi4IKwkEttLzkFipO22WqAoBv5nQet9cOsfM4vhqJpYMsYbU
pKZve77l9vyVma6LAyyZJMlFobNsvgp7dyIBn+tO5I2lw8qluSLb3Hpk1cfGBjyj
SjBIMB0GA1UdDgQWBBQk8sXNOqoBnc9Z5Wag1YnPSy1kVjAPBgNVHRMBAf8EBTAD
AQH/MBYGA1UdEQQPMA2CC2V4YW1wbGUuY29tMAoGCCqGSM49BAMEA4GLADCBhwJB
R5dkwRynyno4Kl7pX/v2UIazn+MeZskBMJt7EHEhHGEIHOn/PTjXu7UPRaxD8AVX
TKztMh9wR/wh4VKnJzcWCFMCQgFJYFqbPcbQycevOsY13G+eXF0wdsD8rftSfGwd
W/rz6xIA2+ljQUdxN8X/EgK6HcboURRHl3N1ZPhBR0oyLCW8yw==
-----END CERTIFICATE-----
";
const P521_SHA256_SIGNATURE: &str = "\
    MIGIAkIB3uDJci4TIuMKQO6v3wpoOMkxiz22nuOpOp7vpbtxwzDVqlm3slZo4fPM\
    ElKy4C+cvuudw3ih2CY5xuLCt3lC2HMCQgCVM0ICBeCuv1ZFZIg5T2RvTjebFjWQ\
    zQQ5CnLS6j0WV0qL8diHyBIMFdi62tEF4HqD/Gf8HhAEOQf2/napjGulZA==";
const P521_SHA384_SIGNATURE: &str = "\
    MIGHAkIBGh9hOM8s/RWijoWIjexMTRuJ6RfI8Ml9sYCVBibNPdpxBSQvRFo5YKTu\
    pArRwKpZ7i3jX1IJfo/EYxVWGbCRm4MCQT0eUkZeTPQ5iVE+S4aHRLirLCfZRZIR\
    Dp5g1+9TffQEGokGTFQGmWX4Wqql836Ujel3YibIq/gwLCTEA1n7sYXn";
const P521_SHA512_SIGNATURE: &str = "\
    MIGIAkIAjf7ZvyL0IRas4DeivNahdcaj7tojevlJe1U5zSquHeX2waohgSNwEoR7\
    vXf8aXYev9/6jRLcwU2Do0/YumLr2pMCQgCwJu47GSumwONfzzqwJ3s5ByAj+vgV\
    UPTG/X6IaR5zBcecw8zgxHFKqI7y92vi9n06yxHB9spzT3riMK6sJ0dYRA==";

/// A handshake signature as rustls decodes one off the wire.
fn signed(scheme: SignatureScheme, signature: &str) -> DigitallySignedStruct {
    let signature = STANDARD.decode(signature).unwrap();
    let mut encoded = u16::from(scheme).to_be_bytes().to_vec();
    encoded.extend(u16::try_from(signature.len()).unwrap().to_be_bytes());
    encoded.extend(signature);
    DigitallySignedStruct::read_bytes(&encoded).unwrap()
}

#[test]
fn a_p521_server_keys_handshake_signatures_are_verified() {
    let verifier = verifier(P521_HOST_CERT);
    let cert = CertificateDer::from_pem_slice(P521_HOST_CERT.as_bytes()).unwrap();
    let tls12 = |scheme, signature| {
        verifier
            .verify_tls12_signature(b"workerd", &cert, &signed(scheme, signature))
            .is_ok()
    };
    let tls13 = |message: &[u8], scheme, signature| {
        verifier
            .verify_tls13_signature(message, &cert, &signed(scheme, signature))
            .is_ok()
    };
    // TLS 1.2's ECDSA schemes leave the curve to the key.
    assert!(tls12(
        SignatureScheme::ECDSA_NISTP256_SHA256,
        P521_SHA256_SIGNATURE
    ));
    assert!(tls12(
        SignatureScheme::ECDSA_NISTP384_SHA384,
        P521_SHA384_SIGNATURE
    ));
    assert!(tls12(
        SignatureScheme::ECDSA_NISTP521_SHA512,
        P521_SHA512_SIGNATURE
    ));
    // TLS 1.3's name it.
    assert!(tls13(
        b"workerd",
        SignatureScheme::ECDSA_NISTP521_SHA512,
        P521_SHA512_SIGNATURE
    ));
    assert!(!tls13(
        b"workerd",
        SignatureScheme::ECDSA_NISTP384_SHA384,
        P521_SHA384_SIGNATURE
    ));
    assert!(!tls13(
        b"workerD",
        SignatureScheme::ECDSA_NISTP521_SHA512,
        P521_SHA512_SIGNATURE
    ));
}
