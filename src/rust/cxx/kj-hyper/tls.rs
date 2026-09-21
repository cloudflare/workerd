//! rustls configs from workerd's `TlsOptions`, and TLS over a kj stream.

use std::sync::Arc;

use cxx::KjError;
use cxx::KjExceptionType;
use kj_rs::KjOwn;
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

use crate::ffi::AsyncIoStream;
use crate::ffi::TlsOptions;
use crate::io::RustIo;
use crate::io::SharedWakers;
use crate::io::kj_to_tokio;

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
    if options.certificate_chain.is_empty() {
        return Ok(None);
    }
    let chain = certificates(std::slice::from_ref(&options.certificate_chain))?;
    let key =
        PrivateKeyDer::from_pem_slice(options.private_key.as_bytes()).map_err(config_error)?;
    Ok(Some((chain, key)))
}

/// `trustedCertificates` as webpki's anchors.
fn roots(trusted: &[CertificateDer<'static>]) -> RootCertStore {
    let mut roots = RootCertStore::empty();
    for cert in trusted {
        // A certificate webpki can't use as an anchor may still be trusted directly.
        let _ = roots.add(cert.clone());
    }
    roots
}

/// `minVersion`: rustls speaks TLS 1.2 and 1.3, so any floor up to 1.2 allows both.
fn versions(options: &TlsOptions) -> &'static [&'static rustls::SupportedProtocolVersion] {
    static TLS13_ONLY: [&rustls::SupportedProtocolVersion; 1] = [&rustls::version::TLS13];
    if options.min_tls13 {
        &TLS13_ONLY
    } else {
        rustls::DEFAULT_VERSIONS
    }
}

/// The TLS 1.2 suites a `cipherList` may name, by their OpenSSL names: all of rustls' suites,
/// each ECDHE with AEAD.
const TLS12_SUITES: [(&str, rustls::SupportedCipherSuite); 6] = {
    use rustls::crypto::ring::cipher_suite as suite;
    [
        (
            "ECDHE-ECDSA-AES128-GCM-SHA256",
            suite::TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
        ),
        (
            "ECDHE-RSA-AES128-GCM-SHA256",
            suite::TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
        ),
        (
            "ECDHE-ECDSA-AES256-GCM-SHA384",
            suite::TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
        ),
        (
            "ECDHE-RSA-AES256-GCM-SHA384",
            suite::TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
        ),
        (
            "ECDHE-ECDSA-CHACHA20-POLY1305",
            suite::TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
        ),
        (
            "ECDHE-RSA-CHACHA20-POLY1305",
            suite::TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
        ),
    ]
};

/// `cipherList` restricts TLS 1.2 to the suites it names, and as in OpenSSL has no effect on
/// TLS 1.3. Only a colon-separated list of the names above can be represented; anything else
/// (OpenSSL's keywords and exclusions) is refused rather than approximated.
fn provider(options: &TlsOptions) -> crate::Result<Arc<rustls::crypto::CryptoProvider>> {
    let mut provider = rustls::crypto::ring::default_provider();
    if options.cipher_list.is_empty() {
        return Ok(Arc::new(provider));
    }
    let named = options
        .cipher_list
        .split(':')
        .map(|token| {
            TLS12_SUITES
                .iter()
                .find(|(name, _)| *name == token)
                .map(|(_, suite)| *suite)
                .ok_or_else(|| {
                    config_error(format!(
                        "cipherList: '{token}' is not supported; list TLS 1.2 suites by name \
                         from: {}",
                        TLS12_SUITES.map(|(name, _)| name).join(", ")
                    ))
                })
        })
        .collect::<crate::Result<Vec<_>>>()?;
    provider
        .cipher_suites
        .retain(|s| s.version().version != rustls::ProtocolVersion::TLSv1_2 || named.contains(s));
    Ok(Arc::new(provider))
}

pub struct TlsClientConfig(Arc<rustls::ClientConfig>);

pub fn new_tls_client_config(options: &TlsOptions) -> crate::Result<Box<TlsClientConfig>> {
    let provider = provider(options)?;
    let trusted = certificates(&options.trusted_certificates)?;
    // `trustBrowserCas` verifies with the platform itself, as OpenSSL uses the system's store:
    // webpki over the native roots refuses chains ring cannot check (a P-521 root, say).
    let verifier: Option<Arc<dyn ServerCertVerifier>> = if options.trust_system_roots {
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
    Ok(Box::new(TlsClientConfig(Arc::new(config))))
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

pub struct TlsServerConfig(Arc<rustls::ServerConfig>);

pub fn new_tls_server_config(options: &TlsOptions) -> crate::Result<Box<TlsServerConfig>> {
    let (chain, key) =
        keypair(options)?.ok_or_else(|| config_error("a TLS listener needs a keypair"))?;
    let provider = provider(options)?;
    let builder = rustls::ServerConfig::builder_with_provider(provider.clone())
        .with_protocol_versions(versions(options))
        .map_err(config_error)?;
    let builder = if options.require_client_certs {
        // The platform verifies servers only; client certificates are checked against the
        // configured certificates.
        if options.trust_system_roots {
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
    Ok(Box::new(TlsServerConfig(Arc::new(config))))
}

/// A failed handshake, DISCONNECTED when the peer went away during it.
fn handshake_error(e: &std::io::Error) -> KjError {
    let error = crate::io::io_kj_error(e);
    KjError::new(
        error.exception_type(),
        format!("TLS handshake failed: {}", error.description()),
    )
}

/// `kj::SecureNetworkWrapper::wrapClient()`: the handshake completes before the stream is
/// handed back.
pub async fn wrap_tls_client(
    stream: KjOwn<AsyncIoStream>,
    config: &TlsClientConfig,
    hostname: String,
) -> crate::Result<Box<RustIo>> {
    let name = ServerName::try_from(hostname).map_err(config_error)?;
    let (io, hangup) = kj_to_tokio(stream);
    let tls = tokio_rustls::TlsConnector::from(config.0.clone())
        .connect(name, io)
        .await
        .map_err(|e| handshake_error(&e))?;
    Ok(Box::new(RustIo::new(SharedWakers::new(tls), hangup)))
}

/// `kj::SecureNetworkWrapper::wrapServer()`: the handshake, client authentication included,
/// completes before the stream is handed back.
pub async fn wrap_tls_server(
    stream: KjOwn<AsyncIoStream>,
    config: &TlsServerConfig,
) -> crate::Result<Box<RustIo>> {
    let (io, hangup) = kj_to_tokio(stream);
    let tls = tokio_rustls::TlsAcceptor::from(config.0.clone())
        .accept(io)
        .await
        .map_err(|e| handshake_error(&e))?;
    Ok(Box::new(RustIo::new(SharedWakers::new(tls), hangup)))
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

    fn options(min_tls13: bool, cipher_list: &str) -> TlsOptions {
        TlsOptions {
            trust_system_roots: false,
            trusted_certificates: Vec::new(),
            certificate_chain: String::new(),
            private_key: String::new(),
            require_client_certs: false,
            min_tls13,
            cipher_list: cipher_list.to_owned(),
        }
    }

    fn tls12_suites(provider: &rustls::crypto::CryptoProvider) -> Vec<rustls::CipherSuite> {
        provider
            .cipher_suites
            .iter()
            .filter(|s| s.version().version == rustls::ProtocolVersion::TLSv1_2)
            .map(rustls::SupportedCipherSuite::suite)
            .collect()
    }

    #[test]
    fn min_version_tls13_allows_only_tls13() {
        assert_eq!(versions(&options(true, "")), [&rustls::version::TLS13]);
        assert_eq!(versions(&options(false, "")), rustls::DEFAULT_VERSIONS);
    }

    #[test]
    fn cipher_list_names_the_tls12_suites() {
        let default = rustls::crypto::ring::default_provider();
        let named = provider(&options(
            false,
            "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384",
        ))
        .unwrap();
        assert_eq!(
            tls12_suites(&named),
            [
                rustls::CipherSuite::TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
                rustls::CipherSuite::TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384
            ]
        );
        // TLS 1.3 suites are not configured by the list.
        assert_eq!(
            named.cipher_suites.len() - 2,
            default.cipher_suites.len() - tls12_suites(&default).len()
        );
    }

    #[test]
    fn a_cipher_list_it_cannot_represent_is_refused() {
        for list in [
            "HIGH:!aNULL",
            "DEFAULT:!AES128:!CHACHA20",
            "ECDHE-ECDSA-AES256-GCM-SHA384:!ECDHE-RSA-AES256-GCM-SHA384",
            "AES256-SHA",
        ] {
            assert!(provider(&options(false, list)).is_err(), "{list}");
        }
    }

    #[test]
    fn a_trusted_certificate_string_without_a_certificate_is_refused() {
        let mut options = options(false, "");
        options.trusted_certificates = vec!["not a certificate".to_owned()];
        assert!(new_tls_client_config(&options).is_err());
    }

    fn verifier(trusted: &str) -> PinnedVerifier {
        let mut options = options(false, "");
        options.trusted_certificates = vec![trusted.to_owned()];
        let trusted = certificates(&options.trusted_certificates).unwrap();
        let provider = provider(&options).unwrap();
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
        let mut options = options(false, "");
        options.certificate_chain = HOST_CERT.to_owned();
        options.private_key = HOST_KEY.to_owned();
        options.require_client_certs = true;
        options.trust_system_roots = true;
        assert!(new_tls_server_config(&options).is_err());
    }

    async fn handshake(
        server: &TlsServerConfig,
        client: Arc<rustls::ClientConfig>,
    ) -> std::io::Result<rustls::CipherSuite> {
        let (a, b) = tokio::io::duplex(1 << 16);
        let accept = tokio_rustls::TlsAcceptor::from(server.0.clone()).accept(a);
        let connect = tokio_rustls::TlsConnector::from(client)
            .connect(ServerName::try_from("example.com").unwrap(), b);
        let (accepted, connected) = futures::join!(accept, connect);
        accepted?;
        let connected = connected?;
        Ok(connected
            .get_ref()
            .1
            .negotiated_cipher_suite()
            .map(|s| s.suite())
            .unwrap())
    }

    #[test]
    fn cipher_list_restricts_a_tls12_handshake() {
        futures::executor::block_on(async {
            let mut server_options = options(false, "ECDHE-ECDSA-CHACHA20-POLY1305");
            server_options.certificate_chain = HOST_CERT.to_owned();
            server_options.private_key = HOST_KEY.to_owned();
            let server = new_tls_server_config(&server_options).unwrap();
            let client_config = |cipher_list: &str| {
                let options = options(false, cipher_list);
                let mut roots = RootCertStore::empty();
                roots
                    .add(CertificateDer::from_pem_slice(CA_CERT.as_bytes()).unwrap())
                    .unwrap();
                Arc::new(
                    rustls::ClientConfig::builder_with_provider(provider(&options).unwrap())
                        .with_protocol_versions(&[&rustls::version::TLS12])
                        .unwrap()
                        .with_root_certificates(roots)
                        .with_no_client_auth(),
                )
            };
            assert_eq!(
                handshake(&server, client_config("")).await.unwrap(),
                rustls::CipherSuite::TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256
            );
            assert!(
                handshake(&server, client_config("ECDHE-ECDSA-AES128-GCM-SHA256"))
                    .await
                    .is_err()
            );
        });
    }
}
