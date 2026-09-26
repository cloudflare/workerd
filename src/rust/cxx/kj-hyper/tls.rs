//! rustls configurations from workerd's `TlsOptions`, and the handshakes over tokio streams.
//!
//! What OpenSSL-era options mean here: `trust_browser_cas` verifies servers with the platform's
//! own store; otherwise only `trusted_certificates` are trusted, and a trusted certificate is
//! also trusted directly (as kj's `TlsContext` adds each to its certificate store) with its
//! validity and key usage still checked. `require_client_certs` needs the accepting authorities
//! in `trusted_certificates`: the platform store verifies servers only. Cipher suites are
//! rustls' defaults; there is no cipher list.

use std::sync::Arc;

use cxx::KjError;
use cxx::KjExceptionType;
use rustls::CertificateError;
use rustls::DigitallySignedStruct;
use rustls::RootCertStore;
use rustls::SignatureScheme;
use rustls::client::WebPkiServerVerifier;
use rustls::client::danger::HandshakeSignatureValid;
use rustls::client::danger::ServerCertVerified;
use rustls::client::danger::ServerCertVerifier;
use rustls::pki_types::CertificateDer;
use rustls::pki_types::PrivateKeyDer;
use rustls::pki_types::ServerName;
use rustls::pki_types::UnixTime;
use rustls::pki_types::pem::PemObject;
use tokio::io::AsyncRead;
use tokio::io::AsyncWrite;

/// A certificate chain and its private key, both PEM.
#[derive(Debug, Clone, Default)]
pub struct Keypair {
    pub certificate_chain: String,
    pub private_key: String,
}

/// The lowest protocol version accepted. rustls speaks TLS 1.2 and 1.3, so every floor workerd's
/// config can name below 1.2 is [`MinVersion::Tls12`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum MinVersion {
    #[default]
    Tls12,
    Tls13,
}

/// workerd's `config::TlsOptions`.
#[derive(Debug, Clone, Default)]
pub struct TlsOptions {
    pub keypair: Option<Keypair>,
    /// PEM certificates, each string holding one or more.
    pub trusted_certificates: Vec<String>,
    pub require_client_certs: bool,
    pub trust_browser_cas: bool,
    pub min_version: MinVersion,
}

fn config_error(what: impl std::fmt::Display) -> KjError {
    KjError::new(
        KjExceptionType::Failed,
        format!("TLS configuration: {what}"),
    )
}

/// The certificates in each PEM string; a string holding none is an error.
fn certificates(pems: &[String]) -> crate::Result<Vec<CertificateDer<'static>>> {
    let mut all = Vec::new();
    for pem in pems {
        let certs = CertificateDer::pem_slice_iter(pem.as_bytes())
            .collect::<Result<Vec<_>, _>>()
            .map_err(config_error)?;
        if certs.is_empty() {
            return Err(config_error("no PEM certificate found"));
        }
        all.extend(certs);
    }
    Ok(all)
}

fn keypair(
    options: &TlsOptions,
) -> crate::Result<Option<(Vec<CertificateDer<'static>>, PrivateKeyDer<'static>)>> {
    let Some(keypair) = &options.keypair else {
        return Ok(None);
    };
    let chain = certificates(std::slice::from_ref(&keypair.certificate_chain))?;
    let key =
        PrivateKeyDer::from_pem_slice(keypair.private_key.as_bytes()).map_err(config_error)?;
    Ok(Some((chain, key)))
}

/// `trusted_certificates` as webpki's anchors.
fn roots(trusted: &[CertificateDer<'static>]) -> RootCertStore {
    let mut roots = RootCertStore::empty();
    for cert in trusted {
        // A certificate webpki can't use as an anchor may still be trusted directly.
        let _ = roots.add(cert.clone());
    }
    roots
}

fn versions(options: &TlsOptions) -> &'static [&'static rustls::SupportedProtocolVersion] {
    static TLS13_ONLY: [&rustls::SupportedProtocolVersion; 1] = [&rustls::version::TLS13];
    match options.min_version {
        MinVersion::Tls13 => &TLS13_ONLY,
        MinVersion::Tls12 => rustls::DEFAULT_VERSIONS,
    }
}

fn provider() -> Arc<rustls::crypto::CryptoProvider> {
    Arc::new(rustls::crypto::ring::default_provider())
}

/// A client configuration: `keypair` is the client certificate, if any.
///
/// # Errors
///
/// Options rustls cannot represent (module docs), or malformed PEM.
pub fn client_config(options: &TlsOptions) -> crate::Result<Arc<rustls::ClientConfig>> {
    let provider = provider();
    let trusted = certificates(&options.trusted_certificates)?;
    // `trust_browser_cas` verifies with the platform itself, as OpenSSL uses the system's store:
    // webpki over the native roots refuses chains ring cannot check (a P-521 root, say).
    let verifier: Option<Arc<dyn ServerCertVerifier>> = if options.trust_browser_cas {
        Some(Arc::new(
            rustls_platform_verifier::Verifier::new_with_extra_roots(
                trusted.clone(),
                provider.clone(),
            )
            .map_err(config_error)?,
        ))
    } else {
        let roots = roots(&trusted);
        if roots.is_empty() {
            None
        } else {
            Some(
                WebPkiServerVerifier::builder_with_provider(Arc::new(roots), provider.clone())
                    .build()
                    .map_err(config_error)?,
            )
        }
    };
    let verifier = PinnedVerifier {
        verifier,
        pinned: trusted,
        provider: provider.clone(),
    };
    let builder = rustls::ClientConfig::builder_with_provider(provider)
        .with_protocol_versions(versions(options))
        .map_err(config_error)?
        .dangerous()
        .with_custom_certificate_verifier(Arc::new(verifier));
    let config = match keypair(options)? {
        Some((chain, key)) => builder
            .with_client_auth_cert(chain, key)
            .map_err(config_error)?,
        None => builder.with_no_client_auth(),
    };
    Ok(Arc::new(config))
}

/// A server configuration; `keypair` is required.
///
/// # Errors
///
/// As for [`client_config`], or no keypair.
pub fn server_config(options: &TlsOptions) -> crate::Result<Arc<rustls::ServerConfig>> {
    let (chain, key) =
        keypair(options)?.ok_or_else(|| config_error("a TLS listener needs a keypair"))?;
    let provider = provider();
    let builder = rustls::ServerConfig::builder_with_provider(provider.clone())
        .with_protocol_versions(versions(options))
        .map_err(config_error)?;
    let builder = if options.require_client_certs {
        if options.trust_browser_cas {
            return Err(config_error(
                "requireClientCerts with trustBrowserCas is not supported: list the certificate \
                 authorities to accept client certificates from in trustedCertificates",
            ));
        }
        let trusted = certificates(&options.trusted_certificates)?;
        let verifier = rustls::server::WebPkiClientVerifier::builder_with_provider(
            Arc::new(roots(&trusted)),
            provider,
        )
        .build()
        .map_err(config_error)?;
        builder.with_client_cert_verifier(verifier)
    } else {
        builder.with_no_client_auth()
    };
    let config = builder.with_single_cert(chain, key).map_err(config_error)?;
    Ok(Arc::new(config))
}

/// The configured trust, plus OpenSSL's direct trust: a server certificate byte-identical to a
/// configured trusted certificate is accepted when webpki cannot build a chain for it (it is not a
/// usable anchor, or it is a CA certificate serving as its own end entity). Such a certificate is
/// still checked as OpenSSL checks one for a TLS server ([`directly_trusted`]), and must name the
/// host.
#[derive(Debug)]
struct PinnedVerifier {
    verifier: Option<Arc<dyn ServerCertVerifier>>,
    pinned: Vec<CertificateDer<'static>>,
    provider: Arc<rustls::crypto::CryptoProvider>,
}

/// A chain-building failure direct trust may override.
fn is_untrusted_chain(error: &rustls::Error) -> bool {
    match error {
        rustls::Error::InvalidCertificate(CertificateError::UnknownIssuer) => true,
        rustls::Error::InvalidCertificate(CertificateError::Other(other)) => matches!(
            other.0.downcast_ref::<webpki::Error>(),
            Some(webpki::Error::CaUsedAsEndEntity)
        ),
        _ => false,
    }
}

/// A directly trusted certificate's own policy, as OpenSSL's `X509_PURPOSE_SSL_SERVER` check
/// applies it: within its validity period, and permitted to authenticate a TLS server by its
/// extended key usage and key usage, where it has them.
fn directly_trusted(end_entity: &CertificateDer<'_>, now: UnixTime) -> Result<(), rustls::Error> {
    use x509_cert::der::Decode;
    use x509_cert::der::oid::db::rfc5280;
    use x509_cert::ext::pkix::ExtendedKeyUsage;
    use x509_cert::ext::pkix::KeyUsage;

    let cert = x509_cert::Certificate::from_der(end_entity)
        .map_err(|_| rustls::Error::InvalidCertificate(CertificateError::BadEncoding))?;
    let tbs = &cert.tbs_certificate;
    let now = now.as_secs();
    if now < tbs.validity.not_before.to_unix_duration().as_secs() {
        return Err(CertificateError::NotValidYet.into());
    }
    if now > tbs.validity.not_after.to_unix_duration().as_secs() {
        return Err(CertificateError::Expired.into());
    }
    for extension in tbs.extensions.iter().flatten() {
        let value = extension.extn_value.as_bytes();
        let permitted = if extension.extn_id == rfc5280::ID_CE_EXT_KEY_USAGE {
            let usages = ExtendedKeyUsage::from_der(value)
                .map_err(|_| rustls::Error::InvalidCertificate(CertificateError::BadEncoding))?;
            usages.0.iter().any(|oid| {
                *oid == rfc5280::ID_KP_SERVER_AUTH || *oid == rfc5280::ANY_EXTENDED_KEY_USAGE
            })
        } else if extension.extn_id == rfc5280::ID_CE_KEY_USAGE {
            let usage = KeyUsage::from_der(value)
                .map_err(|_| rustls::Error::InvalidCertificate(CertificateError::BadEncoding))?;
            usage.digital_signature() || usage.key_encipherment() || usage.key_agreement()
        } else {
            true
        };
        if !permitted {
            return Err(CertificateError::InvalidPurpose.into());
        }
    }
    Ok(())
}

impl ServerCertVerifier for PinnedVerifier {
    fn verify_server_cert(
        &self,
        end_entity: &CertificateDer<'_>,
        intermediates: &[CertificateDer<'_>],
        server_name: &ServerName<'_>,
        ocsp_response: &[u8],
        now: UnixTime,
    ) -> Result<ServerCertVerified, rustls::Error> {
        let Some(verifier) = &self.verifier else {
            return Err(CertificateError::UnknownIssuer.into());
        };
        verifier
            .verify_server_cert(end_entity, intermediates, server_name, ocsp_response, now)
            .or_else(|error| {
                let pinned = self
                    .pinned
                    .iter()
                    .any(|c| c.as_ref() == end_entity.as_ref());
                if !pinned || !is_untrusted_chain(&error) {
                    return Err(error);
                }
                directly_trusted(end_entity, now)?;
                let parsed = rustls::server::ParsedCertificate::try_from(end_entity)?;
                rustls::client::verify_server_name(&parsed, server_name)?;
                Ok(ServerCertVerified::assertion())
            })
    }

    fn verify_tls12_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        rustls::crypto::verify_tls12_signature(
            message,
            cert,
            dss,
            &self.provider.signature_verification_algorithms,
        )
    }

    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        rustls::crypto::verify_tls13_signature(
            message,
            cert,
            dss,
            &self.provider.signature_verification_algorithms,
        )
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        self.provider
            .signature_verification_algorithms
            .supported_schemes()
    }
}

// =======================================================================================
// Handshakes

/// A failed handshake, DISCONNECTED when the peer went away during it.
fn handshake_error(e: &std::io::Error) -> KjError {
    let error = crate::io::io_kj_error(e);
    KjError::new(
        error.exception_type(),
        format!("TLS handshake failed: {}", error.description()),
    )
}

/// The server side of a TLS connection over `io`, handshake (client authentication included)
/// complete.
///
/// A peer that later closes without `close_notify` ends reads with an
/// `UnexpectedEof`, which [`crate::io::io_kj_error`] reports as DISCONNECTED.
///
/// # Errors
///
/// A failed or refused handshake.
pub async fn accept<IO>(
    io: IO,
    config: Arc<rustls::ServerConfig>,
) -> crate::Result<tokio_rustls::server::TlsStream<IO>>
where
    IO: AsyncRead + AsyncWrite + Unpin,
{
    tokio_rustls::TlsAcceptor::from(config)
        .accept(io)
        .await
        .map_err(|e| handshake_error(&e))
}

/// The client side of a TLS connection over `io` to `server_name`, handshake complete.
///
/// # Errors
///
/// An invalid server name, or a failed handshake.
pub async fn connect<IO>(
    io: IO,
    config: Arc<rustls::ClientConfig>,
    server_name: &str,
) -> crate::Result<tokio_rustls::client::TlsStream<IO>>
where
    IO: AsyncRead + AsyncWrite + Unpin,
{
    let name = ServerName::try_from(server_name.to_owned()).map_err(config_error)?;
    tokio_rustls::TlsConnector::from(config)
        .connect(name, io)
        .await
        .map_err(|e| handshake_error(&e))
}

/// Whether the client of an accepted connection authenticated with a certificate (a
/// `kj::TlsPeerIdentity` with a certificate, for the request metadata).
pub fn has_client_certificate<IO>(stream: &tokio_rustls::server::TlsStream<IO>) -> bool {
    stream.get_ref().1.peer_certificates().is_some()
}

#[cfg(test)]
mod tests {
    use super::*;

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
        assert_eq!(
            versions(&options(MinVersion::Tls13)),
            [&rustls::version::TLS13]
        );
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
        let before = UnixTime::since_unix_epoch(std::time::Duration::from_secs(0));
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
    ) -> crate::Result<()> {
        let (a, b) = tokio::io::duplex(1 << 16);
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
    fn exchange(
        server: &TlsOptions,
        client: &TlsOptions,
        hostname: &str,
    ) -> (crate::Result<()>, bool) {
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
        futures::executor::block_on(async {
            let (a, b) = tokio::io::duplex(1 << 16);
            let accepting = async {
                let mut stream = accept(a, server).await?;
                let authenticated = has_client_certificate(&stream);
                let mut byte = [0];
                stream
                    .read_exact(&mut byte)
                    .await
                    .map_err(|e| crate::io::io_kj_error(&e))?;
                stream
                    .write_all(&byte)
                    .await
                    .map_err(|e| crate::io::io_kj_error(&e))?;
                Ok::<_, cxx::KjError>(authenticated)
            };
            let pinged = async {
                let mut stream = connect(b, client, hostname).await?;
                stream
                    .write_all(b"x")
                    .await
                    .map_err(|e| crate::io::io_kj_error(&e))?;
                let mut byte = [0];
                stream
                    .read_exact(&mut byte)
                    .await
                    .map_err(|e| crate::io::io_kj_error(&e))?;
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
                .with_protocol_versions(&[&rustls::version::TLS12])
                .unwrap()
                .with_root_certificates(roots)
                .with_no_client_auth(),
        );
        assert!(futures::executor::block_on(handshake(&server, client)).is_err());
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
        futures::executor::block_on(async {
            let (a, b) = tokio::io::duplex(1 << 16);
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
        tokio_rustls::client::TlsStream<tokio::io::DuplexStream>,
        tokio_rustls::server::TlsStream<tokio::io::DuplexStream>,
    ) {
        use tokio::io::AsyncReadExt;
        use tokio::io::AsyncWriteExt;
        let server = server_config(&serving(HOST_CERT)).unwrap();
        let client = client_config(&trusting(CA_CERT)).unwrap();
        let (a, b) = tokio::io::duplex(1 << 16);
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
        futures::executor::block_on(async {
            let (client, mut server) = tls_pair().await;
            let io = crate::io::RustIo::new(client);
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
        futures::executor::block_on(async {
            let (client, server) = tls_pair().await;
            let io = crate::io::RustIo::new(client);
            let mut buffer = [0; 5];
            let mut read = Box::pin(io.read(tokio::io::ReadBuf::new(&mut buffer), 1));
            assert!(futures::poll!(read.as_mut()).is_pending());
            drop(server);
            let error = read.await.unwrap_err();
            assert_eq!(error.exception_type(), KjExceptionType::Disconnected);
        });
    }
}
